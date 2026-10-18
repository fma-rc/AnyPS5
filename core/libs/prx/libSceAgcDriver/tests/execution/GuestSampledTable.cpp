#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include "GuestSampledTableData.hpp"
#include "GuestSampledTableDerivativeData.hpp"
#include "GuestSampledTableImplicitData.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
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
constexpr std::size_t BlockBytes = 0x90000;
constexpr std::size_t Rgba8Offset = 0x00000;
constexpr std::size_t Bc7Offset = 0x10000;
constexpr std::size_t Bc7MipOffset = 0x20000;
constexpr std::size_t Bc7CopyOffset = 0x14000;
constexpr std::size_t Rgba8CopyOffset = 0x60000;
constexpr std::size_t R16Offset = 0x70000;
constexpr std::size_t R32fOffset = 0x72000;
constexpr std::size_t Bc7DerivativeOffset = 0x78000;
constexpr std::size_t Bc7ImplicitOffset = 0x80000;
constexpr std::size_t Bc7BiasOffset = 0x85000;
constexpr std::uint32_t Records = 10;
constexpr std::size_t SrtOffset = 0x30000;
constexpr std::size_t TableOffset = 0x30100;
constexpr std::size_t KeysOffset = 0x30400;
constexpr std::size_t RowsOffset = 0x40000;
constexpr std::size_t OutputOffset = 0x50000;
constexpr std::size_t RowsBytes = OutputOffset - RowsOffset;

// Per group: record = Keys[group]; T# = the record's eight words at +16; each lane samples the
// group's row (u, v, lod) with image_sample_l and the S# at Srt+48, storing the four words.
alignas(256) constexpr std::array<std::uint32_t, 29> SampleCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0900f08u,
    0x0109040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

// As SampleCode, but with image_sample (derivatives from each quad of lanes; rows: u, v).
alignas(256) constexpr std::array<std::uint32_t, 29> SampleImplicitCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0800f08u,
    0x0109040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

// As SampleImplicitCode, but with image_sample_b (rows: bias, u, v).
alignas(256) constexpr std::array<std::uint32_t, 29> SampleBiasCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0940f08u,
    0x0109040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

// As SampleCode, but each lane's row is du/dx, dv/dx, du/dy, dv/dy, u, v (32 bytes) and it samples with image_sample_d.
alignas(256) constexpr std::array<std::uint32_t, 34> SampleDerivativeCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xbf8cc07fu, 0xf4200444u, 0x20000000u, 0xbf8cc07fu, 0x9312b011u,
    0xf42c0902u, 0x24000010u, 0x7e120202u, 0x34101286u, 0x4a101100u, 0x34121085u, 0x34101084u, 0xe0381000u,
    0x80050a09u, 0xe0341010u, 0x80050e09u, 0xbf8c0070u, 0xf0880f08u, 0x0109040au, 0xbf8c3f70u, 0xe0781000u,
    0x80070408u, 0xbf810000u,
};

std::array<std::uint32_t, 4> Sampler(std::uint32_t mag, std::uint32_t min, std::uint32_t mip) {
    constexpr std::uint32_t clampLastTexel = 2;
    return {clampLastTexel | (clampLastTexel << 3u) | (clampLastTexel << 6u), 0xfffu << 12u, (mag << 20u) | (min << 22u) | (mip << 26u), 0u};
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

std::uint32_t StandardOffset4(std::uint32_t x, std::uint32_t y) {
    auto offset = ((y << 4u) & 0x070u) ^ ((y << 5u) & 0x100u) ^ ((y << 6u) & 0x400u) ^ ((x << 2u) & 0x00cu) ^ ((x << 5u) & 0x080u) ^ ((x << 6u) & 0x200u) ^ ((x << 7u) & 0x800u);
    offset ^= ((x << 8u) & 0x2000u) ^ ((x << 9u) & 0x8000u) ^ ((y << 7u) & 0x1000u) ^ ((y << 8u) & 0x4000u);
    return offset & 0xffffu;
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
        Require(block != nullptr, "guest sampled table: cannot allocate the guest block");
        Require(AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(block), BlockBytes), "guest sampled table: the guest block is not write-watched");
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

struct Case {
    const char* name;
    std::uint32_t record;
    std::array<std::uint32_t, 4> sampler;
    std::span<const std::uint32_t> rows;
    std::span<const std::uint32_t> expected;
    std::uint32_t rowWords = 4;
    std::span<const std::uint32_t> code = SampleCode;
};

std::vector<std::uint32_t> Sample(AgcDriver::VulkanDevice& device, GuestBlock& block, const Case& test) {
    const auto rows = static_cast<std::uint32_t>(test.rows.size() / test.rowWords);
    const auto groups = (rows + Lanes - 1u) / Lanes;
    Require(groups <= MaxGroups && test.rows.size() * sizeof(std::uint32_t) <= RowsBytes, "guest sampled table: too many rows");
    auto* srt = block.At<std::uint32_t>(SrtOffset);
    auto* keys = block.At<std::uint32_t>(KeysOffset);
    auto* rowWords = block.At<std::uint32_t>(RowsOffset);
    auto* output = block.At<std::uint32_t>(OutputOffset);
    const auto table = Buffer(block.At<std::uint32_t>(TableOffset), Records * Stride);
    const auto keyBuffer = Buffer(keys, MaxGroups * 4u);
    const auto outputBuffer = Buffer(output, MaxGroups * Lanes * 16u);
    const auto rowBuffer = Buffer(rowWords, static_cast<std::uint32_t>(RowsBytes));
    std::copy(table.begin(), table.end(), srt);
    std::copy(keyBuffer.begin(), keyBuffer.end(), srt + 4);
    std::copy(outputBuffer.begin(), outputBuffer.end(), srt + 8);
    std::copy(test.sampler.begin(), test.sampler.end(), srt + 12);
    std::copy(rowBuffer.begin(), rowBuffer.end(), srt + 16);
    std::fill(keys, keys + MaxGroups, 0u);
    std::fill(keys, keys + groups, test.record);
    std::fill(rowWords, rowWords + RowsBytes / sizeof(std::uint32_t), 0u);
    std::copy(test.rows.begin(), test.rows.end(), rowWords);
    std::fill(output, output + MaxGroups * Lanes * 4u, 0xdeadbeefu);
    const std::span<const std::uint32_t> span = test.rowWords == 8u ? std::span<const std::uint32_t>(SampleDerivativeCode) : test.code;
    const std::array<ShaderRecompiler::MemoryRegion, 4> memory{{
        {reinterpret_cast<std::uintptr_t>(span.data()), std::as_bytes(span)},
        {reinterpret_cast<std::uintptr_t>(srt), std::as_bytes(std::span(srt, 32))},
        {reinterpret_cast<std::uintptr_t>(block.At<std::uint32_t>(TableOffset)), std::as_bytes(std::span(block.At<std::uint32_t>(TableOffset), Records * Stride / 4u))},
        {reinterpret_cast<std::uintptr_t>(keys), std::as_bytes(std::span(keys, MaxGroups))},
    }};
    const auto rootAddress = reinterpret_cast<std::uintptr_t>(srt);
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(rootAddress), static_cast<std::uint32_t>(rootAddress >> 32u)};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(span.data()), span, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(span.data()));
    device.WaitIdle();
    return std::vector<std::uint32_t>(output, output + rows * 4u);
}

std::size_t Mismatches(std::span<const std::uint32_t> actual, std::span<const std::uint32_t> expected, const std::string& what) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] == expected[index]) continue;
        if (count < 4) std::fprintf(stderr, "%s: row %zu word %zu is 0x%08x, expected 0x%08x\n", what.c_str(), index / 4u, index % 4u, actual[index], expected[index]);
        ++count;
    }
    return count;
}

void RunCase(AgcDriver::VulkanDevice& device, GuestBlock& block, const Case& test) {
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::GuestTableElements();
    const auto guest = Sample(device, block, test);
    const auto afterGuest = AgcDriver::Graphics::GuestTableElements();
    const auto host = Sample(device, block, test);
    const auto afterHost = AgcDriver::Graphics::GuestTableElements();
    const auto reused = Sample(device, block, test);
    const auto afterReuse = AgcDriver::Graphics::GuestTableElements();
    // The host texture's results are counted, not required: its sampler's filter weights, its texel
    // selection within rounding of a texel edge and lavapipe's BC7 decode differ from the hardware.
    const auto guestOracle = Mismatches(guest, test.expected, std::string(test.name) + " guest");
    const auto hostOracle = Mismatches(host, test.expected, std::string(test.name) + " host");
    const auto guestHost = Mismatches(host, guest, std::string(test.name) + " host vs guest");
    std::printf("%-16s rows %4zu: run 1 bound %llu promoted %llu, run 2 bound %llu promoted %llu; mismatches guest/oracle %zu, host/oracle %zu, host/guest %zu\n", test.name, test.rows.size() / 4u,
        static_cast<unsigned long long>(afterGuest.bound - before.bound), static_cast<unsigned long long>(afterGuest.promotions - before.promotions),
        static_cast<unsigned long long>(afterHost.bound - afterGuest.bound), static_cast<unsigned long long>(afterHost.promotions - afterGuest.promotions), guestOracle, hostOracle, guestHost);
    Require(afterGuest.bound == before.bound + 1u && afterGuest.promotions == before.promotions + 1u, std::string(test.name) + ": the first use did not sample the element on the guest and promote it");
    Require(afterHost.bound == afterGuest.bound && afterHost.promotions == afterGuest.promotions, std::string(test.name) + ": the use after the promotion did not take the host texture");
    const bool compressed = std::string_view(test.name).starts_with("bc7");
    const bool watched = AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(block.At<std::uint8_t>(0)), BlockBytes);
    std::printf("%-16s surface write-watched after its import: %s, promoted copy detiled from the import: %s\n", test.name, watched ? "yes" : "no", afterHost.directUploads > afterGuest.directUploads ? "yes" : "no");
    Require(afterHost.directUploads == afterGuest.directUploads + (compressed && watched ? 1u : 0u), std::string(test.name) + ": the promoted BC element of a write-watched surface was not detiled from its import on the GPU");
    Require(afterReuse.bound == afterHost.bound && afterReuse.tableRevalidations > afterHost.tableRevalidations, std::string(test.name) + ": the use after the promoted build did not reuse it");
    Require(reused == host, std::string(test.name) + ": the reused promoted build disagrees with its first use");
    Require(guestOracle == 0u, std::string(test.name) + ": the guest sampler disagrees with the hardware");
}

void RunTests(AgcDriver::VulkanDevice& device) {
    GuestBlock block;
    auto* rgba8 = block.At<std::uint8_t>(Rgba8Offset);
    const auto* linear = reinterpret_cast<const std::uint8_t*>(GuestSampledTableData::Rgba8Linear.data());
    for (std::uint32_t y = 0; y < 68u; ++y) {
        for (std::uint32_t x = 0; x < 120u; ++x) std::memcpy(rgba8 + StandardOffset4(x, y), linear + (y * 120u + x) * 4u, 4u);
    }
    std::memcpy(block.At<std::uint8_t>(Rgba8CopyOffset), rgba8, 0x10000);
    std::memcpy(block.At<std::uint8_t>(Bc7Offset), GuestSampledTableData::Bc7Memory.data(), sizeof(GuestSampledTableData::Bc7Memory));
    std::memcpy(block.At<std::uint8_t>(Bc7CopyOffset), GuestSampledTableData::Bc7Memory.data(), sizeof(GuestSampledTableData::Bc7Memory));
    std::memcpy(block.At<std::uint8_t>(Bc7MipOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7DerivativeOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7ImplicitOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7BiasOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(R16Offset), GuestSampledTableDerivativeData::R16Memory.data(), sizeof(GuestSampledTableDerivativeData::R16Memory));
    std::memcpy(block.At<std::uint8_t>(R32fOffset), GuestSampledTableDerivativeData::R32fMemory.data(), sizeof(GuestSampledTableDerivativeData::R32fMemory));
    constexpr std::uint32_t formatRgba8 = 56;
    constexpr std::uint32_t formatBc7 = 181;
    constexpr std::uint32_t swizzle64KbS = 9;
    constexpr std::uint32_t swizzle4KbS = 5;
    constexpr std::uint32_t swizzle256BS = 1;
    constexpr std::uint32_t formatR16 = 7;
    constexpr std::uint32_t formatR32f = 22;
    const std::array<std::array<std::uint32_t, 8>, Records> records{
        TextureWords(block.At<std::uint8_t>(Rgba8Offset), formatRgba8, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7Offset), formatBc7, 72, 40, swizzle4KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7MipOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Rgba8CopyOffset), formatRgba8, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7CopyOffset), formatBc7, 72, 40, swizzle4KbS, 0),
        TextureWords(block.At<std::uint8_t>(R16Offset), formatR16, 64, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(R32fOffset), formatR32f, 64, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7DerivativeOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7ImplicitOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7BiasOffset), formatBc7, 128, 64, swizzle4KbS, 5),
    };
    auto* table = block.At<std::uint32_t>(TableOffset);
    for (std::size_t record = 0; record < records.size(); ++record) std::copy(records[record].begin(), records[record].end(), table + record * (Stride / 4u) + 4u);
    using namespace GuestSampledTableData;
    // Promotion is per T#: each case samples its own copy of the surface.
    RunCase(device, block, {"rgba8/bilinear", 0, Sampler(1, 1, 0), Rgba8Rows, Rgba8Bilinear});
    RunCase(device, block, {"r16/bilinear", 5, Sampler(1, 1, 0), GuestSampledTableDerivativeData::SingleChannelRows, GuestSampledTableDerivativeData::R16Bilinear});
    RunCase(device, block, {"r32f/bilinear", 6, Sampler(1, 1, 0), GuestSampledTableDerivativeData::SingleChannelRows, GuestSampledTableDerivativeData::R32fBilinear});
    RunCase(device, block, {"bc7-derivative", 7, Sampler(1, 1, 2), GuestSampledTableDerivativeData::Bc7DerivativeRows, GuestSampledTableDerivativeData::Bc7DerivativeTrilinear, 8});
    RunCase(device, block, {"bc7-implicit", 8, Sampler(1, 1, 2), GuestSampledTableImplicitData::Bc7ImplicitRows, GuestSampledTableImplicitData::Bc7ImplicitTrilinear, 4, SampleImplicitCode});
    RunCase(device, block, {"bc7-bias", 9, Sampler(1, 1, 2), GuestSampledTableImplicitData::Bc7BiasRows, GuestSampledTableImplicitData::Bc7BiasTrilinear, 4, SampleBiasCode});
    RunCase(device, block, {"bc7/point", 1, Sampler(0, 0, 0), Bc7PointRows, Bc7Point});
    RunCase(device, block, {"bc7-mips/point", 2, Sampler(0, 0, 1), Bc7MipRows, Bc7MipPoint});
    RunCase(device, block, {"rgba8/point", 3, Sampler(0, 0, 0), Rgba8Rows, Rgba8Point});
    RunCase(device, block, {"bc7/bilinear", 4, Sampler(1, 1, 0), Bc7BilinearRows, Bc7Bilinear});
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        RunTests(*device);
        std::puts("guest sampled table tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
