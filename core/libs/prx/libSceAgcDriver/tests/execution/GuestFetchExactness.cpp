#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t MaxGroups = 64;
constexpr std::uint32_t Stride = 48;
constexpr std::uint32_t Surfaces = 32;
constexpr std::size_t SurfaceBytes = 0x10000;
constexpr std::size_t SurfaceBase = 0x10000;
constexpr std::size_t SrtOffset = SurfaceBase + Surfaces * SurfaceBytes;
constexpr std::size_t TableOffset = SrtOffset + 0x100;
constexpr std::size_t KeysOffset = TableOffset + Surfaces * Stride + 0x100;
constexpr std::size_t RowsOffset = KeysOffset + 0x1000;
constexpr std::size_t OutputOffset = RowsOffset + MaxGroups * Lanes * 16u;
constexpr std::size_t BlockBytes = (OutputOffset + MaxGroups * Lanes * 16u + 0xffffu) & ~std::size_t{0xffff};

// Per group: record = Keys[group]; T# = the record's eight words at +16; each lane reads the group's
// row with image_sample_l (S# at Srt+48) or image_load_mip (integer x, y, level), storing four words.
alignas(256) constexpr std::array<std::uint32_t, 29> SampleCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0900f08u,
    0x0109040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 29> LoadCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0041f08u,
    0x0009040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

std::array<std::uint32_t, 4> PointSampler() {
    constexpr std::uint32_t clampLastTexel = 2;
    constexpr std::uint32_t mipPoint = 1;
    return {clampLastTexel | (clampLastTexel << 3u) | (clampLastTexel << 6u), 0xfffu << 12u, mipPoint << 26u, 0u};
}

std::array<std::uint32_t, 8> TextureWords(const void* base, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t swizzle, std::uint32_t lastLevel) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    constexpr std::uint32_t type2D = 9;
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((width - 1u) & 3u) << 30u), ((width - 1u) >> 2u) | ((height - 1u) << 14u), 0xfacu | (lastLevel << 16u) | (swizzle << 20u) | (type2D << 28u), 0u, lastLevel << 4u, 0u, 0u};
}

std::array<std::uint32_t, 4> Buffer(const void* base, std::uint32_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::uint32_t UnormFilteredToFloat(std::uint32_t filtered) {
    const std::uint64_t r = filtered;
    const std::uint64_t sum = ((r << 16u) & ~std::uint64_t{7}) + ((r << 8u) & ~std::uint64_t{7}) + (r & ~std::uint64_t{7}) + ((r >> 8u) & ~std::uint64_t{7}) + ((r >> 16u) & ~std::uint64_t{7});
    if (sum == 0u) return 0u;
    int leading = 63 - __builtin_clzll(sum);
    std::uint32_t mantissa = 0;
    if (leading > 23) {
        const auto shift = static_cast<unsigned>(leading - 23);
        auto quotient = sum >> shift;
        const auto remainder = sum & ((std::uint64_t{1} << shift) - 1u);
        if (remainder >= (std::uint64_t{1} << (shift - 1u))) quotient += 1u;
        if (quotient == (std::uint64_t{1} << 24u)) {
            quotient >>= 1u;
            leading += 1;
        }
        mantissa = static_cast<std::uint32_t>(quotient);
    } else {
        mantissa = static_cast<std::uint32_t>(sum << static_cast<unsigned>(23 - leading));
    }
    return (static_cast<std::uint32_t>(leading - 37 + 127) << 23u) | (mantissa & 0x7fffffu);
}

float BitsFloat(std::uint32_t bits) {
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "guest fetch exactness: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    template <typename T>
    T* At(std::size_t offset) { return reinterpret_cast<T*>(block + offset); }

private:
    std::uint8_t* block = nullptr;
};

std::vector<std::uint32_t> Run(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<const std::uint32_t> code, std::uint32_t record, std::span<const std::uint32_t> rows) {
    const auto count = static_cast<std::uint32_t>(rows.size() / 4u);
    const auto groups = (count + Lanes - 1u) / Lanes;
    Require(groups <= MaxGroups, "guest fetch exactness: too many rows");
    auto* srt = block.At<std::uint32_t>(SrtOffset);
    auto* keys = block.At<std::uint32_t>(KeysOffset);
    auto* rowWords = block.At<std::uint32_t>(RowsOffset);
    auto* output = block.At<std::uint32_t>(OutputOffset);
    const auto table = Buffer(block.At<std::uint32_t>(TableOffset), Surfaces * Stride);
    const auto keyBuffer = Buffer(keys, MaxGroups * 4u);
    const auto outputBuffer = Buffer(output, MaxGroups * Lanes * 16u);
    const auto rowBuffer = Buffer(rowWords, MaxGroups * Lanes * 16u);
    const auto sampler = PointSampler();
    std::copy(table.begin(), table.end(), srt);
    std::copy(keyBuffer.begin(), keyBuffer.end(), srt + 4);
    std::copy(outputBuffer.begin(), outputBuffer.end(), srt + 8);
    std::copy(sampler.begin(), sampler.end(), srt + 12);
    std::copy(rowBuffer.begin(), rowBuffer.end(), srt + 16);
    std::fill(keys, keys + MaxGroups, 0u);
    std::fill(keys, keys + groups, record);
    std::fill(rowWords, rowWords + MaxGroups * Lanes * 4u, 0u);
    std::copy(rows.begin(), rows.end(), rowWords);
    std::fill(output, output + MaxGroups * Lanes * 4u, 0xdeadbeefu);
    const std::array<ShaderRecompiler::MemoryRegion, 4> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)},
        {reinterpret_cast<std::uintptr_t>(srt), std::as_bytes(std::span(srt, 32))},
        {reinterpret_cast<std::uintptr_t>(block.At<std::uint32_t>(TableOffset)), std::as_bytes(std::span(block.At<std::uint32_t>(TableOffset), Surfaces * Stride / 4u))},
        {reinterpret_cast<std::uintptr_t>(keys), std::as_bytes(std::span(keys, MaxGroups))},
    }};
    const auto rootAddress = reinterpret_cast<std::uintptr_t>(srt);
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(rootAddress), static_cast<std::uint32_t>(rootAddress >> 32u)};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    return std::vector<std::uint32_t>(output, output + count * 4u);
}

struct Format {
    const char* name;
    std::uint32_t format;
    std::uint32_t swizzle;
};

void RunTests(AgcDriver::VulkanDevice& device) {
    GuestBlock block;
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    const auto next = [&] {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    };
    constexpr std::uint32_t swizzle64KbS = 9;
    constexpr std::uint32_t swizzle4KbS = 5;
    const std::array<Format, 8> formats{{
        {"rgba8", 56, swizzle64KbS}, {"bc1", 169, swizzle4KbS}, {"bc1-srgb", 170, swizzle4KbS}, {"bc4", 175, swizzle4KbS},
        {"bc5", 177, swizzle4KbS}, {"bc6h-uf16", 179, swizzle4KbS}, {"bc7", 181, swizzle4KbS}, {"bc7-srgb", 182, swizzle4KbS},
    }};
    struct Shape {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t lastLevel;
    };
    const std::array<Shape, 2> shapes{{{64, 64, 0}, {32, 32, 5}}};
    auto* table = block.At<std::uint32_t>(TableOffset);
    std::uint32_t record = 0;
    std::size_t totalFaults = 0;
    for (const auto& format : formats) {
        for (const auto& shape : shapes) {
            Require(record < Surfaces, "guest fetch exactness: too many surfaces");
            auto* bytes = block.At<std::uint8_t>(SurfaceBase + record * SurfaceBytes);
            for (std::size_t at = 0; at < SurfaceBytes; at += 8) {
                const auto word = next();
                std::memcpy(bytes + at, &word, sizeof(word));
            }
            if (format.format == 181u || format.format == 182u) {
                for (std::size_t at = 0; at < SurfaceBytes; at += 16) bytes[at] = static_cast<std::uint8_t>(1u << (bytes[at] & 7u));
            }
            if (format.format == 179u) {
                for (std::size_t at = 0; at < SurfaceBytes; at += 16) {
                    const auto mode = bytes[at] & 0x1fu;
                    if ((mode & 3u) >= 2u && (mode == 0x13u || mode == 0x17u || mode == 0x1bu || mode == 0x1fu)) bytes[at] = static_cast<std::uint8_t>((bytes[at] & 0xe0u) | 0x03u);
                }
            }
            const auto words = TextureWords(bytes, format.format, shape.width, shape.height, format.swizzle, shape.lastLevel);
            std::copy(words.begin(), words.end(), table + record * (Stride / 4u) + 4u);
            std::vector<std::uint32_t> sampleRows;
            std::vector<std::uint32_t> loadRows;
            for (std::uint32_t level = 0; level <= shape.lastLevel; ++level) {
                const auto width = std::max(shape.width >> level, 1u);
                const auto height = std::max(shape.height >> level, 1u);
                for (std::uint32_t y = 0; y < height; ++y) {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        sampleRows.insert(sampleRows.end(), {FloatBits((static_cast<float>(x) + 0.5f) / static_cast<float>(width)), FloatBits((static_cast<float>(y) + 0.5f) / static_cast<float>(height)), FloatBits(static_cast<float>(level)), 0u});
                        loadRows.insert(loadRows.end(), {x, y, level, 0u});
                    }
                }
            }
            AgcDriver::GuestMemory::BumpCollectEpoch();
            const auto before = AgcDriver::Graphics::GuestTableElements();
            const auto guest = Run(device, block, SampleCode, record, sampleRows);
            const auto afterGuest = AgcDriver::Graphics::GuestTableElements();
            const auto fetched = Run(device, block, LoadCode, record, loadRows);
            Require(afterGuest.bound == before.bound + 1u, std::string(format.name) + ": the reference read did not take the guest sampler");
            const bool bc45 = format.format == 175u || format.format == 177u;
            const bool floatFormat = format.format == 179u;
            const bool srgb = format.format == 170u || format.format == 182u;
            const double scale = bc45 ? 255.0 * 64.0 : 255.0;
            std::size_t texelMisses = 0;
            std::size_t wordMisses = 0;
            std::size_t decodeMisses = 0;
            std::size_t offGrid = 0;
            std::array<std::size_t, 4> channelMisses{};
            for (std::size_t row = 0; row < guest.size() / 4u; ++row) {
                bool miss = false;
                bool decodeMiss = false;
                for (std::uint32_t c = 0; c < 4u; ++c) {
                    const auto a = guest[row * 4u + c];
                    const auto b = fetched[row * 4u + c];
                    std::uint32_t mapped = b;
                    if (srgb && c < 3u) {
                        const auto code = [](std::uint32_t bits) {
                            const double linear = static_cast<double>(BitsFloat(bits));
                            const double encoded = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
                            return static_cast<std::uint32_t>(std::nearbyint(encoded * 255.0));
                        };
                        mapped = code(b) == code(a) ? a : a ^ 1u;
                        if (code(b) == code(a) && a != b) ++offGrid;
                    } else if (!floatFormat) {
                        const double scaled = static_cast<double>(BitsFloat(b)) * scale;
                        const double nearest = std::nearbyint(scaled);
                        if (std::fabs(scaled - nearest) > 1e-3) ++offGrid;
                        const auto integer = static_cast<std::uint32_t>(nearest);
                        mapped = UnormFilteredToFloat(bc45 ? integer * 128u : integer * 8192u);
                    }
                    if (a != b) {
                        miss = true;
                        ++wordMisses;
                    }
                    if (a != mapped) {
                        decodeMiss = true;
                        ++decodeMisses;
                        ++channelMisses[c];
                        if (decodeMisses <= 3u) std::fprintf(stderr, "%s %ux%u: texel %zu word %u fetch 0x%08x (%.9g) guest 0x%08x (%.9g)\n", format.name, shape.width, shape.height, row, c, b, static_cast<double>(BitsFloat(b)), a, static_cast<double>(BitsFloat(a)));
                    }
                }
                texelMisses += decodeMiss ? 1u : 0u;
                (void)miss;
            }
            std::printf("%-10s %2ux%-2u levels %u: texels %5zu: raw float words differ %zu; decoded texels differ %zu (%zu words); fetch values off the %s grid (sRGB: same code, other float) %zu; per channel %zu %zu %zu %zu\n", format.name, shape.width, shape.height, shape.lastLevel + 1u, guest.size() / 4u, wordMisses, texelMisses, decodeMisses, bc45 ? "1/16320" : "1/255", offGrid, channelMisses[0], channelMisses[1], channelMisses[2], channelMisses[3]);
            totalFaults += decodeMisses;
            ++record;
        }
    }
    std::printf("total decoded words differing %zu\n", totalFaults);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        RunTests(*device);
        std::puts("guest fetch exactness measured");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
