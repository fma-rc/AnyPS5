#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include "GuestSampledTableData.hpp"
#include "GuestSampledTableDerivativeData.hpp"
#include "GuestSampledTableImplicitData.hpp"
#include "GuestSampledTableMinLodData.hpp"
#include "GuestSampledTableAnisoData.hpp"
#include "GuestSampledTableAnisoSrgbData.hpp"
#include "GuestSampledTableAnisoFormatsData.hpp"
#include "GuestSampledTableSamplerBiasData.hpp"
#include "GuestSampledTableWrapData.hpp"
#include "GuestSampledTableNpotAnisoData.hpp"
#include "GuestSampledTableSrgb8Data.hpp"
#include "GuestSampledTableFloatData.hpp"
#include "GuestSampledTableArrayData.hpp"
#include "GuestSampledTableAnisoRgba8Data.hpp"
#include "GuestSampledTableAnisoSingleData.hpp"
#include "GuestSampledTableAnisoFloatData.hpp"
#include "GuestSampledTableAnisoGapData.hpp"
#include "GuestSampledTableAnisoMipsData.hpp"
#include <algorithm>
#include <array>
#include <bit>
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
constexpr std::size_t BlockBytes = 0x550000;
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
constexpr std::size_t Bc7MinLodTrilinearOffset = 0x8a000;
constexpr std::size_t Bc7MinLodPointOffset = 0x8f000;
constexpr std::size_t Bc7MinLodBiasOffset = 0x94000;
constexpr std::size_t Bc7AnisoTrilinearOffset = 0x99000;
constexpr std::size_t Bc7AnisoPointOffset = 0x9e000;
constexpr std::size_t Bc7AnisoBilinearOffset = 0xa3000;
constexpr std::size_t Bc7SrgbAnisoTrilinearOffset = 0xa8000;
constexpr std::size_t Bc7SrgbAnisoPointOffset = 0xad000;
constexpr std::size_t Bc7SrgbAnisoBilinearOffset = 0xb2000;
constexpr std::size_t Bc1AnisoTrilinearOffset = 0xb7000;
constexpr std::size_t Bc1SrgbAnisoTrilinearOffset = 0xbc000;
constexpr std::size_t Bc1SrgbAnisoPointOffset = 0xc1000;
constexpr std::size_t Bc4AnisoTrilinearOffset = 0xc6000;
constexpr std::size_t Bc5AnisoTrilinearOffset = 0xcb000;
constexpr std::size_t Bc7SrgbAnisoOneLevelOffset = 0xd0000;
constexpr std::size_t Bc7SrgbAnisoNpotOffset = 0xd5000;
constexpr std::size_t Bc7AnisoMinLodOffset = 0xda000;
constexpr std::size_t Bc7AnisoBiasOffset = 0xdf000;
constexpr std::size_t R32fLoadOffset = 0xe8000;
constexpr std::size_t Rgba8LoadOffset = 0xf0000;
constexpr std::size_t Bc7LoadOffset = 0x100000;
constexpr std::size_t Bc7CentreOffset = 0x105000;
constexpr std::size_t Bc7LoadMinLodOffset = 0x10a000;
constexpr std::size_t R32fCentreOffset = 0x10f000;
constexpr std::size_t Bc7GatherOffset = 0x114000;
constexpr std::size_t Bc7GatherCentreOffset = 0x118000;
constexpr std::size_t Rgba8GatherOffset = 0x120000;
constexpr std::size_t Rgba8GatherCentreOffset = 0x130000;
constexpr std::size_t Bc7SamplerBiasPointOffset = 0x140000;
constexpr std::size_t Bc7SamplerBiasTrilinearOffset = 0x145000;
constexpr std::size_t Bc7SamplerBiasLodOffset = 0x14a000;
constexpr std::size_t Bc7SamplerBiasZeroLodOffset = 0x14f000;
constexpr std::size_t Bc7WrapImplicitOffset = 0x154000;
constexpr std::size_t Bc7WrapPointMipOffset = 0x159000;
constexpr std::size_t Bc7WrapLodOffset = 0x15e000;
constexpr std::size_t Bc7WrapClampLodOffset = 0x163000;
constexpr std::size_t Bc7WrapAnisoOffset = 0x168000;
constexpr std::size_t Bc7Npot96x40TrilinearOffset = 0x16d000;
constexpr std::size_t Bc7Npot96x40PointOffset = 0x175000;
constexpr std::size_t Bc7Npot100x52TrilinearOffset = 0x17d000;
constexpr std::size_t Rgba8RenderTargetOffset = 0x190000;
constexpr std::size_t Rgba8DepthOffset = 0x1a0000;
constexpr std::uint32_t Rgba8PipeBankXor = 0x5a00;
constexpr std::size_t Rgba8SrgbBilinearOffset = 0x1b0000;
constexpr std::size_t Rgba8SrgbPointOffset = 0x1c0000;
constexpr std::size_t Rgba8Srgb256BOffset = 0x1d0000;
constexpr std::size_t PlainOffset = 0x1e0000;
constexpr std::size_t PlainSlotBytes = 0x20000;
constexpr std::uint32_t PlainRecord = 55;
constexpr std::size_t Bc7ArrayOffset = 0x2e0000;
constexpr std::size_t Bc7ArrayLayerBytes = 0x4000;
constexpr std::size_t Rgba8ArrayOffset = 0x2f0000;
constexpr std::size_t Rgba8ArrayLayerBytes = 0x10000;
constexpr std::uint32_t ArrayLayers = 3;
constexpr std::size_t Rgba8AnisoOffset = 0x320000;
constexpr std::size_t Rgba8AnisoSlotBytes = 0x10000;
constexpr std::uint32_t Rgba8AnisoRecord = 65;
constexpr std::uint32_t Rgba8AnisoCases = 7;
constexpr std::size_t SingleXorOffset = 0x390000;
constexpr std::size_t SingleXorSlotBytes = 0x10000;
constexpr std::uint32_t SingleXorRecord = 72;
constexpr std::size_t SingleAnisoOffset = 0x3d0000;
constexpr std::size_t SingleAnisoSlotBytes = 0x10000;
constexpr std::uint32_t SingleAnisoRecord = 76;
constexpr std::uint32_t SingleAnisoCases = 5;
constexpr std::size_t FloatAnisoOffset = 0x420000;
constexpr std::size_t FloatAnisoSlotBytes = 0x10000;
constexpr std::uint32_t FloatAnisoRecord = 81;
constexpr std::size_t GapAnisoOffset = 0x490000;
constexpr std::size_t GapAnisoSlotBytes = 0x10000;
constexpr std::uint32_t GapAnisoRecord = 88;
constexpr std::size_t Bc6hMipsAnisoOffset = 0x4f0000;
constexpr std::uint32_t Bc6hMipsAnisoRecord = 94;
constexpr std::size_t R16MipsAnisoOffset = 0x510000;
constexpr std::uint32_t R16MipsAnisoRecord = 95;
constexpr std::size_t R32fMipsAnisoOffset = 0x530000;
constexpr std::uint32_t R32fMipsAnisoRecord = 96;
constexpr std::uint32_t Records = 97;
constexpr std::size_t SrtOffset = 0x30000;
constexpr std::size_t TableOffset = 0x30100;
constexpr std::size_t KeysOffset = 0x31400;
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

// As SampleCode, but sampling a 2D array (rows: u, v, slice, lod).
alignas(256) constexpr std::array<std::uint32_t, 29> SampleArrayCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0900f28u,
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

// As SampleCode, but with image_sample_lz (rows: u, v).
alignas(256) constexpr std::array<std::uint32_t, 29> SampleZeroLodCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf09c0f08u,
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

// As SampleCode, but with image_load_mip (rows: x, y, level as integers).
alignas(256) constexpr std::array<std::uint32_t, 29> LoadCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf0040f08u,
    0x0009040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

// As SampleCode, but with image_gather4_l of the green channel (dmask 0x2; rows: u, v, lod).
alignas(256) constexpr std::array<std::uint32_t, 29> GatherGreenCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf1100208u,
    0x0109040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

// As GatherGreenCode, but of the red channel (dmask 0x1).
alignas(256) constexpr std::array<std::uint32_t, 29> GatherRedCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0xf4080800u, 0xfa000030u,
    0xf4080500u, 0xfa000040u, 0x8f108202u, 0xf4200444u, 0x20000000u, 0x9312b011u, 0xf42c0902u, 0x24000010u,
    0x7e120202u, 0x34101286u, 0x4a101100u, 0x34101084u, 0xe0381000u, 0x80050a08u, 0xbf8c3f70u, 0xf1100108u,
    0x0109040au, 0xbf8c3f70u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

std::array<std::uint32_t, 4> Sampler(std::uint32_t mag, std::uint32_t min, std::uint32_t mip) {
    constexpr std::uint32_t clampLastTexel = 2;
    return {clampLastTexel | (clampLastTexel << 3u) | (clampLastTexel << 6u), 0xfffu << 12u, (mag << 20u) | (min << 22u) | (mip << 26u), 0u};
}

std::array<std::uint32_t, 4> Sampler(std::uint32_t mag, std::uint32_t min, std::uint32_t mip, std::uint32_t minLod, std::uint32_t maxLod) {
    auto words = Sampler(mag, min, mip);
    words[1] = minLod | (maxLod << 12u);
    return words;
}

std::array<std::uint32_t, 4> BiasedSampler(std::uint32_t mag, std::uint32_t min, std::uint32_t mip, std::int32_t bias) {
    auto words = Sampler(mag, min, mip);
    words[2] |= static_cast<std::uint32_t>(bias) & 0x3fffu;
    return words;
}

std::array<std::uint32_t, 4> AddressedSampler(std::uint32_t clampX, std::uint32_t clampY, std::uint32_t mag, std::uint32_t min, std::uint32_t mip, std::uint32_t anisoRatio) {
    auto words = Sampler(mag, min, mip);
    words[0] = clampX | (clampY << 3u) | (2u << 6u) | (anisoRatio << 9u);
    return words;
}

std::array<std::uint32_t, 4> AnisoSampler(std::uint32_t mag, std::uint32_t min, std::uint32_t mip) {
    auto words = Sampler(mag, min, mip);
    words[0] |= 1u << 9u;
    return words;
}

std::array<std::uint32_t, 4> AnisoSampler(std::uint32_t mag, std::uint32_t min, std::uint32_t mip, std::uint32_t bias) {
    auto words = AnisoSampler(mag, min, mip);
    words[2] |= bias;
    return words;
}

std::array<std::uint32_t, 8> TextureWords(const void* base, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t swizzle, std::uint32_t lastLevel) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    constexpr std::uint32_t type2D = 9;
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((width - 1u) & 3u) << 30u), ((width - 1u) >> 2u) | ((height - 1u) << 14u), 0xfacu | (lastLevel << 16u) | (swizzle << 20u) | (type2D << 28u), 0u, lastLevel << 4u, 0u, 0u};
}

std::array<std::uint32_t, 8> ArrayTextureWords(const void* base, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t swizzle, std::uint32_t lastLevel, std::uint32_t layers) {
    constexpr std::uint32_t type2DArray = 13;
    auto words = TextureWords(base, format, width, height, swizzle, lastLevel);
    words[3] = (words[3] & 0x0fffffffu) | (type2DArray << 28u);
    words[4] = layers - 1u;
    return words;
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

std::uint32_t StandardOffset256(std::uint32_t bytes, std::uint32_t x, std::uint32_t y) {
    switch (bytes) {
        case 1u: return ((y << 4u) & 0xf0u) | (x & 0xfu);
        case 2u: return ((y << 4u) & 0x70u) | ((x << 1u) & 0xeu) | ((x << 4u) & 0x80u);
        case 4u: return ((y << 4u) & 0x70u) | ((x << 2u) & 0xcu) | ((x << 5u) & 0x80u);
        default: return ((y << 4u) & 0x30u) | ((x << 3u) & 0x8u) | ((x << 5u) & 0xc0u);
    }
}

std::uint32_t SurfaceOffset(std::uint32_t swizzle, std::uint32_t bytes, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
    const auto log2Bytes = static_cast<std::uint32_t>(std::countr_zero(bytes));
    if (swizzle == 0u) {
        const auto align = 256u / bytes;
        return (y * ((width + align - 1u) / align * align) + x) * bytes;
    }
    const auto log2Elements = (swizzle == 1u ? 8u : 16u) - log2Bytes;
    const auto blockWidth = 1u << ((log2Elements + 1u) / 2u);
    const auto blockHeight = 1u << (log2Elements / 2u);
    const auto block = (y / blockHeight) * ((width + blockWidth - 1u) / blockWidth) + x / blockWidth;
    if (swizzle == 1u) return block * 256u + StandardOffset256(bytes, x % blockWidth, y % blockHeight);
    const auto* equation = AgcDriver::Graphics::FindTextureSwizzleEquation(swizzle, bytes);
    Require(equation != nullptr, "guest sampled table: no XOR swizzle equation for the element size");
    const auto coordinate = (x & 0xfffu) | ((y & 0xfffu) << 12u);
    std::uint32_t offset = 0;
    for (std::uint32_t bit = 0; bit < 16u; ++bit) offset |= static_cast<std::uint32_t>(std::popcount(coordinate & equation->bits[bit]) & 1) << bit;
    return block * 65536u + (offset ^ Rgba8PipeBankXor);
}

struct PlainCase {
    const char* name;
    std::uint32_t format;
    std::uint32_t bytes;
    std::uint32_t swizzle;
    bool bilinear;
    const std::uint32_t* linear;
    std::span<const std::uint32_t> expected;
};

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
    const bool compressed = std::string_view(test.name).starts_with("bc");
    const bool watched = AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(block.At<std::uint8_t>(0)), BlockBytes);
    std::printf("%-16s surface write-watched after its import: %s, promoted copy detiled from the import: %s\n", test.name, watched ? "yes" : "no", afterHost.directUploads > afterGuest.directUploads ? "yes" : "no");
    Require(afterHost.directUploads == afterGuest.directUploads + (compressed && watched ? 1u : 0u), std::string(test.name) + ": the promoted BC element of a write-watched surface was not detiled from its import on the GPU");
    Require(afterReuse.bound == afterHost.bound && afterReuse.tableRevalidations > afterHost.tableRevalidations, std::string(test.name) + ": the use after the promoted build did not reuse it");
    Require(reused == host, std::string(test.name) + ": the reused promoted build disagrees with its first use");
    Require(guestOracle == 0u, std::string(test.name) + ": the guest sampler disagrees with the hardware");
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::vector<std::uint32_t> RunLoad(AgcDriver::VulkanDevice& device, GuestBlock& block, const char* name, std::uint32_t record, std::span<const std::uint32_t> rows, std::vector<std::uint32_t>* host) {
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::GuestTableElements();
    const Case test{name, record, Sampler(0, 0, 1), rows, {}, 4, LoadCode};
    auto guest = Sample(device, block, test);
    const auto afterGuest = AgcDriver::Graphics::GuestTableElements();
    Require(afterGuest.bound == before.bound + 1u && afterGuest.promotions == before.promotions + 1u, std::string(name) + ": the first load did not read the element on the guest and promote it");
    if (host != nullptr) {
        *host = Sample(device, block, test);
        const auto afterHost = AgcDriver::Graphics::GuestTableElements();
        Require(afterHost.bound == afterGuest.bound && afterHost.promotions == afterGuest.promotions, std::string(name) + ": the load after the promotion did not take the host texture");
    }
    return guest;
}

void RunGather(AgcDriver::VulkanDevice& device, GuestBlock& block, const char* name, std::uint32_t record, std::uint32_t centreRecord, std::span<const std::uint32_t> code, std::uint32_t width, std::uint32_t height, std::uint32_t component) {
    std::vector<std::uint32_t> rows;
    std::vector<std::uint32_t> centres;
    std::uint32_t seed = 0x9e3779b9u;
    const auto fraction = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(1u + (seed >> 29u)) / 9.0f;
    };
    const auto centre = [&](std::uint32_t x, std::uint32_t y) {
        centres.insert(centres.end(), {FloatBits((static_cast<float>(x) + 0.5f) / static_cast<float>(width)), FloatBits((static_cast<float>(y) + 0.5f) / static_cast<float>(height)), 0u, 0u});
    };
    for (std::uint32_t y = 0; y + 1u < height; y += 5u) {
        for (std::uint32_t x = 0; x + 1u < width; x += 4u) {
            rows.insert(rows.end(), {FloatBits((static_cast<float>(x) + 0.5f + fraction()) / static_cast<float>(width)), FloatBits((static_cast<float>(y) + 0.5f + fraction()) / static_cast<float>(height)), 0u, 0u});
            centre(x, y + 1u);
            centre(x + 1u, y + 1u);
            centre(x + 1u, y);
            centre(x, y);
        }
    }
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::GuestTableElements();
    const Case test{name, record, Sampler(1, 1, 0), rows, {}, 4, code};
    const auto guest = Sample(device, block, test);
    const auto afterGuest = AgcDriver::Graphics::GuestTableElements();
    Require(afterGuest.bound == before.bound + 1u && afterGuest.promotions == before.promotions + 1u, std::string(name) + ": the first gather did not read the element on the guest and promote it");
    const auto host = Sample(device, block, test);
    const auto afterHost = AgcDriver::Graphics::GuestTableElements();
    Require(afterHost.bound == afterGuest.bound && afterHost.promotions == afterGuest.promotions, std::string(name) + ": the gather after the promotion did not take the host texture");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto point = Sample(device, block, {name, centreRecord, Sampler(0, 0, 0), centres, {}});
    std::vector<std::uint32_t> expected;
    for (std::size_t texel = 0; texel < centres.size() / 4u; ++texel) expected.push_back(point[texel * 4u + component]);
    const auto guestPoint = Mismatches(guest, expected, std::string(name) + " guest gather vs guest point samples");
    const auto hostGuest = Mismatches(host, guest, std::string(name) + " host vs guest");
    std::printf("%-16s rows %4zu: mismatches guest gather/guest point samples at the four texel centres %zu, host/guest %zu\n", name, rows.size() / 4u, guestPoint, hostGuest);
    Require(guestPoint == 0u, std::string(name) + ": the guest gather disagrees with the guest point samples of its four texels");
}

void RunLoadTests(AgcDriver::VulkanDevice& device, GuestBlock& block) {
    std::vector<std::uint32_t> rows;
    std::vector<std::uint32_t> expected;
    const auto* linear = reinterpret_cast<const std::uint8_t*>(GuestSampledTableData::Rgba8Linear.data());
    for (std::uint32_t y = 0; y < 34u; ++y) {
        for (std::uint32_t x = 0; x < 120u; ++x) {
            rows.insert(rows.end(), {x, y, 0u, 0u});
            for (std::uint32_t c = 0; c < 4u; ++c) expected.push_back(FloatBits(static_cast<float>(linear[(y * 120u + x) * 4u + c]) / 255.0f));
        }
    }
    const auto inside = rows.size();
    for (std::uint32_t x = 120u; x < 128u; ++x) rows.insert(rows.end(), {x, 0u, 0u, 0u});
    for (std::uint32_t x = 0; x < 8u; ++x) rows.insert(rows.end(), {x, 0u, 1u, 0u});
    expected.resize(rows.size(), 0u);
    std::vector<std::uint32_t> host;
    const auto rgba8 = RunLoad(device, block, "rgba8/load", 29, rows, &host);
    const auto rgba8Expected = Mismatches(rgba8, expected, "rgba8/load guest");
    const auto rgba8Host = Mismatches(std::span(host).first(inside), std::span(rgba8).first(inside), "rgba8/load host vs guest");
    std::printf("%-16s rows %4zu: mismatches guest/expected %zu, host/guest %zu\n", "rgba8/load", rows.size() / 4u, rgba8Expected, rgba8Host);
    Require(rgba8Expected == 0u, "rgba8/load: the guest texel load disagrees with the texels");

    rows.clear();
    std::vector<std::uint32_t> r32fCentres;
    for (std::uint32_t y = 0; y < 64u; ++y) {
        for (std::uint32_t x = 0; x < 64u; ++x) {
            rows.insert(rows.end(), {x, y, 0u, 0u});
            r32fCentres.insert(r32fCentres.end(), {FloatBits((static_cast<float>(x) + 0.5f) / 64.0f), FloatBits((static_cast<float>(y) + 0.5f) / 64.0f), 0u, 0u});
        }
    }
    const auto r32f = RunLoad(device, block, "r32f/load", 28, rows, &host);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto r32fPoint = Sample(device, block, {"r32f/centre", 33, Sampler(0, 0, 1), r32fCentres, {}});
    const auto r32fGuest = Mismatches(r32f, r32fPoint, "r32f/load guest vs guest point sample");
    const auto r32fHost = Mismatches(host, r32f, "r32f/load host vs guest");
    std::printf("%-16s rows %4zu: mismatches guest load/guest point sample at the texel centre %zu, host/guest %zu\n", "r32f/load", rows.size() / 4u, r32fGuest, r32fHost);
    Require(r32fGuest == 0u, "r32f/load: the guest texel load disagrees with the guest point sample at the texel centre");

    rows.clear();
    std::vector<std::uint32_t> centres;
    for (std::uint32_t level = 0; level < 6u; ++level) {
        const auto width = 128u >> level;
        const auto height = 64u >> level;
        for (std::uint32_t y = 0; y < (level == 0u ? 10u : height); ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                rows.insert(rows.end(), {x, y, level, 0u});
                centres.insert(centres.end(), {FloatBits((static_cast<float>(x) + 0.5f) / static_cast<float>(width)), FloatBits((static_cast<float>(y) + 0.5f) / static_cast<float>(height)), FloatBits(static_cast<float>(level)), 0u});
            }
        }
    }
    const auto bc7 = RunLoad(device, block, "bc7-mips/load", 30, rows, nullptr);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto point = Sample(device, block, {"bc7-mips/centre", 31, Sampler(0, 0, 1), centres, {}});
    const auto bc7Point = Mismatches(bc7, point, "bc7-mips/load guest vs guest point sample");
    std::printf("%-16s rows %4zu: mismatches guest load/guest point sample at the texel centre %zu\n", "bc7-mips/load", rows.size() / 4u, bc7Point);
    Require(bc7Point == 0u, "bc7-mips/load: the guest texel load disagrees with the guest point sample at the texel centre");

    std::vector<std::uint32_t> clamped = bc7;
    for (std::size_t row = 0; row < rows.size() / 4u; ++row) {
        if (rows[row * 4u + 2u] < 2u) std::fill_n(clamped.begin() + static_cast<std::ptrdiff_t>(row * 4u), 4, 0u);
    }
    RunGather(device, block, "rgba8/gather", 34, 35, GatherGreenCode, 120, 68, 1);
    RunGather(device, block, "bc7/gather", 36, 37, GatherRedCode, 72, 40, 0);

    const auto minLod = RunLoad(device, block, "bc7-minlod/load", 32, rows, &host);
    const auto minLodGuest = Mismatches(minLod, clamped, "bc7-minlod/load guest");
    const auto minLodHost = Mismatches(host, minLod, "bc7-minlod/load host vs guest");
    std::printf("%-16s rows %4zu: mismatches guest/expected %zu, host/guest %zu\n", "bc7-minlod/load", rows.size() / 4u, minLodGuest, minLodHost);
    Require(minLodGuest == 0u, "bc7-minlod/load: a load below the texture MIN_LOD level did not return zero");
}

void RunTests(AgcDriver::VulkanDevice& device) {
    GuestBlock block;
    auto* rgba8 = block.At<std::uint8_t>(Rgba8Offset);
    const auto* linear = reinterpret_cast<const std::uint8_t*>(GuestSampledTableData::Rgba8Linear.data());
    const auto withMinLod = [](std::array<std::uint32_t, 8> words, std::uint32_t minLod) {
        words[1] |= minLod << 8u;
        return words;
    };
    for (std::uint32_t y = 0; y < 68u; ++y) {
        for (std::uint32_t x = 0; x < 120u; ++x) {
            std::memcpy(rgba8 + StandardOffset4(x, y), linear + (y * 120u + x) * 4u, 4u);
            std::memcpy(block.At<std::uint8_t>(Rgba8RenderTargetOffset) + SurfaceOffset(27u, 4u, 120u, x, y), linear + (y * 120u + x) * 4u, 4u);
            std::memcpy(block.At<std::uint8_t>(Rgba8DepthOffset) + SurfaceOffset(24u, 4u, 120u, x, y), linear + (y * 120u + x) * 4u, 4u);
            for (std::uint32_t layer = 0; layer < ArrayLayers; ++layer) {
                for (std::uint32_t byte = 0; byte < 4u; ++byte) block.At<std::uint8_t>(Rgba8ArrayOffset + layer * Rgba8ArrayLayerBytes)[StandardOffset4(x, y) + byte] = static_cast<std::uint8_t>(linear[(y * 120u + x) * 4u + byte] ^ ((0x5bu * layer) & 0xffu));
            }
            std::memcpy(block.At<std::uint8_t>(Rgba8Srgb256BOffset) + (((y >> 3u) * 15u + (x >> 3u)) << 8u) + (StandardOffset4(x & 7u, y & 7u) & 0xffu), linear + (y * 120u + x) * 4u, 4u);
        }
    }
    std::memcpy(block.At<std::uint8_t>(Rgba8CopyOffset), rgba8, 0x10000);
    for (std::uint32_t layer = 0; layer < ArrayLayers; ++layer) {
        const auto* chain = reinterpret_cast<const std::uint8_t*>(GuestSampledTableData::Bc7MipMemory.data());
        const auto rotation = layer * 16u;
        auto* target = block.At<std::uint8_t>(Bc7ArrayOffset + layer * Bc7ArrayLayerBytes);
        for (std::size_t byte = 0; byte < Bc7ArrayLayerBytes; ++byte) target[byte] = chain[(byte + rotation) % sizeof(GuestSampledTableData::Bc7MipMemory)];
    }
    std::memcpy(block.At<std::uint8_t>(Rgba8SrgbBilinearOffset), rgba8, 0x10000);
    std::memcpy(block.At<std::uint8_t>(Rgba8SrgbPointOffset), rgba8, 0x10000);
    std::memcpy(block.At<std::uint8_t>(Bc7Offset), GuestSampledTableData::Bc7Memory.data(), sizeof(GuestSampledTableData::Bc7Memory));
    std::memcpy(block.At<std::uint8_t>(Bc7CopyOffset), GuestSampledTableData::Bc7Memory.data(), sizeof(GuestSampledTableData::Bc7Memory));
    std::memcpy(block.At<std::uint8_t>(Bc7MipOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7DerivativeOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7ImplicitOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7BiasOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    for (const auto offset : {Bc7MinLodTrilinearOffset, Bc7MinLodPointOffset, Bc7MinLodBiasOffset, Bc7AnisoTrilinearOffset, Bc7AnisoPointOffset, Bc7AnisoBilinearOffset, Bc7SrgbAnisoTrilinearOffset, Bc7SrgbAnisoPointOffset, Bc7SrgbAnisoBilinearOffset}) std::memcpy(block.At<std::uint8_t>(offset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    for (const auto offset : {Bc7WrapImplicitOffset, Bc7WrapPointMipOffset, Bc7WrapLodOffset, Bc7WrapClampLodOffset, Bc7WrapAnisoOffset, Bc7Npot96x40TrilinearOffset, Bc7Npot96x40PointOffset, Bc7Npot100x52TrilinearOffset}) std::memcpy(block.At<std::uint8_t>(offset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    for (const auto offset : {Bc7SamplerBiasPointOffset, Bc7SamplerBiasTrilinearOffset, Bc7SamplerBiasLodOffset, Bc7SamplerBiasZeroLodOffset}) std::memcpy(block.At<std::uint8_t>(offset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    for (const auto offset : {Bc7SrgbAnisoOneLevelOffset, Bc7SrgbAnisoNpotOffset, Bc7AnisoMinLodOffset, Bc7AnisoBiasOffset}) std::memcpy(block.At<std::uint8_t>(offset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    for (const auto offset : {Bc1AnisoTrilinearOffset, Bc1SrgbAnisoTrilinearOffset, Bc1SrgbAnisoPointOffset, Bc4AnisoTrilinearOffset, Bc5AnisoTrilinearOffset}) std::memcpy(block.At<std::uint8_t>(offset), GuestSampledTableAnisoFormatsData::AnisoNoiseMemory.data(), sizeof(GuestSampledTableAnisoFormatsData::AnisoNoiseMemory));
    for (std::uint32_t slot = 0; slot < Rgba8AnisoCases; ++slot) {
        if (slot == 3u || slot == 4u) continue;
        std::memcpy(block.At<std::uint8_t>(Rgba8AnisoOffset + slot * Rgba8AnisoSlotBytes), GuestSampledTableAnisoRgba8Data::Rgba8AnisoNoise.data(), sizeof(GuestSampledTableAnisoRgba8Data::Rgba8AnisoNoise));
    }
    {
        const auto* noise = reinterpret_cast<const std::uint8_t*>(GuestSampledTableAnisoRgba8Data::Rgba8AnisoNoise.data());
        for (std::uint32_t y = 0; y < 64u; ++y) {
            for (std::uint32_t x = 0; x < 128u; ++x) {
                for (const auto slot : {3u, 4u}) std::memcpy(block.At<std::uint8_t>(Rgba8AnisoOffset + slot * Rgba8AnisoSlotBytes) + SurfaceOffset(27u, 4u, 128u, x, y), noise + SurfaceOffset(1u, 4u, 128u, x, y), 4u);
            }
        }
    }
    for (std::uint32_t y = 0; y < 64u; ++y) {
        for (std::uint32_t x = 0; x < 64u; ++x) {
            const auto* r16 = reinterpret_cast<const std::uint8_t*>(GuestSampledTableDerivativeData::R16Memory.data());
            const auto* r32f = reinterpret_cast<const std::uint8_t*>(GuestSampledTableDerivativeData::R32fMemory.data());
            std::memcpy(block.At<std::uint8_t>(SingleXorOffset) + SurfaceOffset(27u, 2u, 64u, x, y), r16 + SurfaceOffset(1u, 2u, 64u, x, y), 2u);
            std::memcpy(block.At<std::uint8_t>(SingleXorOffset + SingleXorSlotBytes) + SurfaceOffset(24u, 2u, 64u, x, y), r16 + SurfaceOffset(1u, 2u, 64u, x, y), 2u);
            std::memcpy(block.At<std::uint8_t>(SingleXorOffset + 2u * SingleXorSlotBytes) + SurfaceOffset(27u, 4u, 64u, x, y), r32f + SurfaceOffset(1u, 4u, 64u, x, y), 4u);
            std::memcpy(block.At<std::uint8_t>(SingleXorOffset + 3u * SingleXorSlotBytes) + SurfaceOffset(24u, 4u, 64u, x, y), r32f + SurfaceOffset(1u, 4u, 64u, x, y), 4u);
        }
    }
    for (std::uint32_t slot = 0; slot < SingleAnisoCases; ++slot) std::memcpy(block.At<std::uint8_t>(SingleAnisoOffset + slot * SingleAnisoSlotBytes), GuestSampledTableAnisoRgba8Data::Rgba8AnisoNoise.data(), sizeof(GuestSampledTableAnisoRgba8Data::Rgba8AnisoNoise));
    {
        namespace AnisoFloat = GuestSampledTableAnisoFloatData;
        const std::array<const std::array<std::uint32_t, 16384>*, 7> floatMemories{&AnisoFloat::Half16Memory, &AnisoFloat::Half16Memory, &AnisoFloat::Half16Memory, &AnisoFloat::R11g11b10fMemory, &AnisoFloat::Float32Memory, &AnisoFloat::Float32Memory, &AnisoFloat::Bc6hMemory};
        for (std::size_t slot = 0; slot < floatMemories.size(); ++slot) std::memcpy(block.At<std::uint8_t>(FloatAnisoOffset + slot * FloatAnisoSlotBytes), floatMemories[slot]->data(), sizeof(*floatMemories[slot]));
    }
    {
        namespace AnisoGap = GuestSampledTableAnisoGapData;
        const std::array<const std::array<std::uint32_t, 16384>*, 6> gapMemories{&AnisoGap::WideHalf16Memory, &AnisoGap::WideHalf16Memory, &AnisoGap::WideSignedHalf16Memory, &AnisoGap::WideHalf16Memory, &AnisoGap::WideFloat32Memory, &AnisoGap::WideSignedFloat32Memory};
        for (std::size_t slot = 0; slot < gapMemories.size(); ++slot) std::memcpy(block.At<std::uint8_t>(GapAnisoOffset + slot * GapAnisoSlotBytes), gapMemories[slot]->data(), sizeof(*gapMemories[slot]));
    }
    std::memcpy(block.At<std::uint8_t>(Bc6hMipsAnisoOffset), GuestSampledTableAnisoMipsData::Bc6hMipsMemory.data(), sizeof(GuestSampledTableAnisoMipsData::Bc6hMipsMemory));
    std::memcpy(block.At<std::uint8_t>(R16MipsAnisoOffset), GuestSampledTableAnisoMipsData::R16MipsMemory.data(), sizeof(GuestSampledTableAnisoMipsData::R16MipsMemory));
    std::memcpy(block.At<std::uint8_t>(R32fMipsAnisoOffset), GuestSampledTableAnisoMipsData::R32fMipsMemory.data(), sizeof(GuestSampledTableAnisoMipsData::R32fMipsMemory));
    std::memcpy(block.At<std::uint8_t>(R16Offset), GuestSampledTableDerivativeData::R16Memory.data(), sizeof(GuestSampledTableDerivativeData::R16Memory));
    std::memcpy(block.At<std::uint8_t>(R32fOffset), GuestSampledTableDerivativeData::R32fMemory.data(), sizeof(GuestSampledTableDerivativeData::R32fMemory));
    std::memcpy(block.At<std::uint8_t>(R32fLoadOffset), GuestSampledTableDerivativeData::R32fMemory.data(), sizeof(GuestSampledTableDerivativeData::R32fMemory));
    std::memcpy(block.At<std::uint8_t>(Rgba8GatherOffset), rgba8, 0x10000);
    std::memcpy(block.At<std::uint8_t>(Rgba8GatherCentreOffset), rgba8, 0x10000);
    std::memcpy(block.At<std::uint8_t>(Bc7GatherOffset), GuestSampledTableData::Bc7Memory.data(), sizeof(GuestSampledTableData::Bc7Memory));
    std::memcpy(block.At<std::uint8_t>(Bc7GatherCentreOffset), GuestSampledTableData::Bc7Memory.data(), sizeof(GuestSampledTableData::Bc7Memory));
    std::memcpy(block.At<std::uint8_t>(R32fCentreOffset), GuestSampledTableDerivativeData::R32fMemory.data(), sizeof(GuestSampledTableDerivativeData::R32fMemory));
    std::memcpy(block.At<std::uint8_t>(Rgba8LoadOffset), rgba8, 0x10000);
    std::memcpy(block.At<std::uint8_t>(Bc7LoadOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7CentreOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    std::memcpy(block.At<std::uint8_t>(Bc7LoadMinLodOffset), GuestSampledTableData::Bc7MipMemory.data(), sizeof(GuestSampledTableData::Bc7MipMemory));
    constexpr std::uint32_t formatRgba8 = 56;
    constexpr std::uint32_t formatRgba8Srgb = 130;
    constexpr std::uint32_t formatBc7 = 181;
    constexpr std::uint32_t formatBc7Srgb = 182;
    constexpr std::uint32_t swizzle64KbS = 9;
    constexpr std::uint32_t swizzle4KbS = 5;
    constexpr std::uint32_t swizzle256BS = 1;
    constexpr std::uint32_t formatR16 = 7;
    constexpr std::uint32_t formatR32f = 22;
    constexpr std::uint32_t formatBc1 = 169;
    constexpr std::uint32_t formatBc1Srgb = 170;
    constexpr std::uint32_t formatBc4 = 175;
    constexpr std::uint32_t formatBc5 = 177;
    namespace Float = GuestSampledTableFloatData;
    const std::array<PlainCase, 8> plainCases{{
        {"rgba16f/bilinear", 71u, 8u, 0u, true, Float::Rgba16fLinear.data(), Float::Rgba16fBilinear},
        {"rgba16f-256b/point", 71u, 8u, 1u, false, Float::Rgba16fLinear.data(), Float::Rgba16fPoint},
        {"r16f-rx/bilinear", 13u, 2u, 27u, true, Float::R16fLinear.data(), Float::R16fBilinear},
        {"r16f/point", 13u, 2u, 0u, false, Float::R16fLinear.data(), Float::R16fPoint},
        {"r11g11b10f-rx/bilinear", 36u, 4u, 27u, true, Float::R11g11b10fLinear.data(), Float::R11g11b10fBilinear},
        {"r11g11b10f-256b/point", 36u, 4u, 1u, false, Float::R11g11b10fLinear.data(), Float::R11g11b10fPoint},
        {"r8-256b/bilinear", 1u, 1u, 1u, true, Float::R8Linear.data(), Float::R8Bilinear},
        {"r8-rx/point", 1u, 1u, 27u, false, Float::R8Linear.data(), Float::R8Point},
    }};
    for (std::size_t index = 0; index < plainCases.size(); ++index) {
        const auto& plain = plainCases[index];
        const auto* source = reinterpret_cast<const std::uint8_t*>(plain.linear);
        auto* target = block.At<std::uint8_t>(PlainOffset + index * PlainSlotBytes);
        for (std::uint32_t y = 0; y < 68u; ++y) {
            for (std::uint32_t x = 0; x < 120u; ++x) std::memcpy(target + SurfaceOffset(plain.swizzle, plain.bytes, 120u, x, y), source + SurfaceOffset(0u, plain.bytes, 120u, x, y), plain.bytes);
        }
    }
    const auto plainWords = [&](std::size_t index) {
        const auto& plain = plainCases[index];
        const auto pipeBankXor = plain.swizzle == 27u ? Rgba8PipeBankXor : 0u;
        return TextureWords(block.At<std::uint8_t>(PlainOffset + index * PlainSlotBytes + pipeBankXor), plain.format, 120, 68, plain.swizzle, 0);
    };
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
        withMinLod(TextureWords(block.At<std::uint8_t>(Bc7MinLodTrilinearOffset), formatBc7, 128, 64, swizzle4KbS, 5), 0x200u),
        withMinLod(TextureWords(block.At<std::uint8_t>(Bc7MinLodPointOffset), formatBc7, 128, 64, swizzle4KbS, 5), 0x140u),
        withMinLod(TextureWords(block.At<std::uint8_t>(Bc7MinLodBiasOffset), formatBc7, 128, 64, swizzle4KbS, 5), 0x180u),
        TextureWords(block.At<std::uint8_t>(Bc7AnisoTrilinearOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7AnisoPointOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7AnisoBilinearOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SrgbAnisoTrilinearOffset), formatBc7Srgb, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SrgbAnisoPointOffset), formatBc7Srgb, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SrgbAnisoBilinearOffset), formatBc7Srgb, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc1AnisoTrilinearOffset), formatBc1, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc1SrgbAnisoTrilinearOffset), formatBc1Srgb, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc1SrgbAnisoPointOffset), formatBc1Srgb, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc4AnisoTrilinearOffset), formatBc4, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc5AnisoTrilinearOffset), formatBc5, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SrgbAnisoOneLevelOffset), formatBc7Srgb, 128, 64, swizzle4KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7SrgbAnisoNpotOffset), formatBc7Srgb, 96, 48, swizzle4KbS, 0),
        withMinLod(TextureWords(block.At<std::uint8_t>(Bc7AnisoMinLodOffset), formatBc7, 128, 64, swizzle4KbS, 5), 0x80u),
        TextureWords(block.At<std::uint8_t>(Bc7AnisoBiasOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(R32fLoadOffset), formatR32f, 64, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8LoadOffset), formatRgba8, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7LoadOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7CentreOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        withMinLod(TextureWords(block.At<std::uint8_t>(Bc7LoadMinLodOffset), formatBc7, 128, 64, swizzle4KbS, 5), 0x2c0u),
        TextureWords(block.At<std::uint8_t>(R32fCentreOffset), formatR32f, 64, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8GatherOffset), formatRgba8, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8GatherCentreOffset), formatRgba8, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7GatherOffset), formatBc7, 72, 40, swizzle4KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7GatherCentreOffset), formatBc7, 72, 40, swizzle4KbS, 0),
        TextureWords(block.At<std::uint8_t>(Bc7SamplerBiasPointOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SamplerBiasTrilinearOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SamplerBiasLodOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7SamplerBiasZeroLodOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7WrapImplicitOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7WrapPointMipOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7WrapLodOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7WrapClampLodOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7WrapAnisoOffset), formatBc7, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(Bc7Npot96x40TrilinearOffset), formatBc7, 96, 40, swizzle4KbS, 3),
        TextureWords(block.At<std::uint8_t>(Bc7Npot96x40PointOffset), formatBc7, 96, 40, swizzle4KbS, 3),
        TextureWords(block.At<std::uint8_t>(Bc7Npot100x52TrilinearOffset), formatBc7, 100, 52, swizzle4KbS, 3),
        TextureWords(block.At<std::uint8_t>(Rgba8RenderTargetOffset + Rgba8PipeBankXor), formatRgba8, 120, 68, 27u, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8DepthOffset + Rgba8PipeBankXor), formatRgba8, 120, 68, 24u, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8SrgbBilinearOffset), formatRgba8Srgb, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8SrgbPointOffset), formatRgba8Srgb, 120, 68, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8Srgb256BOffset), formatRgba8Srgb, 120, 68, swizzle256BS, 0),
        plainWords(0), plainWords(1), plainWords(2), plainWords(3), plainWords(4), plainWords(5), plainWords(6), plainWords(7),
        ArrayTextureWords(block.At<std::uint8_t>(Bc7ArrayOffset), formatBc7, 128, 64, swizzle4KbS, 4, ArrayLayers),
        ArrayTextureWords(block.At<std::uint8_t>(Rgba8ArrayOffset), formatRgba8, 120, 68, swizzle64KbS, 0, ArrayLayers),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset), formatRgba8, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset + Rgba8AnisoSlotBytes), formatRgba8Srgb, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset + 2u * Rgba8AnisoSlotBytes), formatRgba8, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset + 3u * Rgba8AnisoSlotBytes + Rgba8PipeBankXor), formatRgba8, 128, 64, 27u, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset + 4u * Rgba8AnisoSlotBytes + Rgba8PipeBankXor), formatRgba8Srgb, 128, 64, 27u, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset + 5u * Rgba8AnisoSlotBytes), formatRgba8, 128, 64, swizzle64KbS, 0),
        TextureWords(block.At<std::uint8_t>(Rgba8AnisoOffset + 6u * Rgba8AnisoSlotBytes), formatRgba8, 120, 68, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(SingleXorOffset + Rgba8PipeBankXor), formatR16, 64, 64, 27u, 0),
        TextureWords(block.At<std::uint8_t>(SingleXorOffset + SingleXorSlotBytes + Rgba8PipeBankXor), formatR16, 64, 64, 24u, 0),
        TextureWords(block.At<std::uint8_t>(SingleXorOffset + 2u * SingleXorSlotBytes + Rgba8PipeBankXor), formatR32f, 64, 64, 27u, 0),
        TextureWords(block.At<std::uint8_t>(SingleXorOffset + 3u * SingleXorSlotBytes + Rgba8PipeBankXor), formatR32f, 64, 64, 24u, 0),
        TextureWords(block.At<std::uint8_t>(SingleAnisoOffset), 1u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(SingleAnisoOffset + SingleAnisoSlotBytes), 1u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(SingleAnisoOffset + 2u * SingleAnisoSlotBytes), formatR16, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(SingleAnisoOffset + 3u * SingleAnisoSlotBytes), formatR16, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(SingleAnisoOffset + 4u * SingleAnisoSlotBytes), formatR16, 120, 68, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset), 71u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset + FloatAnisoSlotBytes), 71u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset + 2u * FloatAnisoSlotBytes), 13u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset + 3u * FloatAnisoSlotBytes), 36u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset + 4u * FloatAnisoSlotBytes), formatR32f, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset + 5u * FloatAnisoSlotBytes), formatR32f, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(FloatAnisoOffset + 6u * FloatAnisoSlotBytes), 179u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(GapAnisoOffset), 13u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(GapAnisoOffset + GapAnisoSlotBytes), 13u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(GapAnisoOffset + 2u * GapAnisoSlotBytes), 13u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(GapAnisoOffset + 3u * GapAnisoSlotBytes), 71u, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(GapAnisoOffset + 4u * GapAnisoSlotBytes), formatR32f, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(GapAnisoOffset + 5u * GapAnisoSlotBytes), formatR32f, 128, 64, swizzle256BS, 0),
        TextureWords(block.At<std::uint8_t>(Bc6hMipsAnisoOffset), 179u, 128, 64, swizzle4KbS, 5),
        TextureWords(block.At<std::uint8_t>(R16MipsAnisoOffset), formatR16, 128, 64, swizzle256BS, 5),
        TextureWords(block.At<std::uint8_t>(R32fMipsAnisoOffset), formatR32f, 128, 64, swizzle256BS, 5),
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
    RunCase(device, block, {"bc7-minlod/trilinear", 10, Sampler(1, 1, 2, 0u, 0x480u), GuestSampledTableMinLodData::Bc7MinLodRows, GuestSampledTableMinLodData::Bc7MinLodTrilinear});
    RunCase(device, block, {"bc7-minlod/point", 11, Sampler(0, 0, 1, 0x60u, 0xfffu), GuestSampledTableMinLodData::Bc7MinLodRows, GuestSampledTableMinLodData::Bc7MinLodPoint});
    RunCase(device, block, {"bc7-minlod/bias", 12, Sampler(1, 1, 2, 0x60u, 0x3c0u), GuestSampledTableMinLodData::Bc7MinLodBiasRows, GuestSampledTableMinLodData::Bc7MinLodBias, 4, SampleBiasCode});
    RunCase(device, block, {"bc7-aniso/trilinear", 13, AnisoSampler(3, 3, 2), GuestSampledTableAnisoData::Bc7AnisoRows, GuestSampledTableAnisoData::Bc7AnisoTrilinear, 8});
    RunCase(device, block, {"bc7-aniso/point", 14, AnisoSampler(3, 3, 1), GuestSampledTableAnisoData::Bc7AnisoRows, GuestSampledTableAnisoData::Bc7AnisoPoint, 8});
    RunCase(device, block, {"bc7-aniso/bilinear", 15, AnisoSampler(1, 1, 2), GuestSampledTableAnisoData::Bc7AnisoRows, GuestSampledTableAnisoData::Bc7AnisoBilinearFilterTrilinear, 8});
    RunCase(device, block, {"bc7-srgb-aniso/bilinear", 18, AnisoSampler(1, 1, 2), GuestSampledTableAnisoSrgbData::Bc7SrgbAnisoRows, GuestSampledTableAnisoSrgbData::Bc7SrgbAnisoBilinearFilterTrilinear, 8});
    RunCase(device, block, {"bc7-srgb-aniso/point", 17, AnisoSampler(3, 3, 1), GuestSampledTableAnisoSrgbData::Bc7SrgbAnisoRows, GuestSampledTableAnisoSrgbData::Bc7SrgbAnisoPoint, 8});
    RunCase(device, block, {"bc7-srgb-aniso/trilinear", 16, AnisoSampler(3, 3, 2), GuestSampledTableAnisoSrgbData::Bc7SrgbAnisoRows, GuestSampledTableAnisoSrgbData::Bc7SrgbAnisoTrilinear, 8});
    namespace Formats = GuestSampledTableAnisoFormatsData;
    RunCase(device, block, {"bc1-aniso/trilinear", 19, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Formats::Bc1AnisoTrilinear, 8});
    RunCase(device, block, {"bc1-srgb-aniso/trilinear", 20, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Formats::Bc1SrgbAnisoTrilinear, 8});
    RunCase(device, block, {"bc1-srgb-aniso/point", 21, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, Formats::Bc1SrgbAnisoPoint, 8});
    RunCase(device, block, {"bc4-aniso/trilinear", 22, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Formats::Bc4AnisoTrilinear, 8});
    RunCase(device, block, {"bc7-srgb-aniso/one-level", 24, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Formats::Bc7SrgbAnisoOneLevel, 8});
    RunCase(device, block, {"bc7-srgb-aniso/npot", 25, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Formats::Bc7SrgbAnisoNpotOneLevel, 8});
    RunCase(device, block, {"bc7-aniso/minlod", 26, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Formats::Bc7AnisoMinLodHalf, 8});
    RunCase(device, block, {"bc7-aniso/bias", 27, AnisoSampler(3, 3, 2, 0x80u), Formats::AnisoFormatsRows, Formats::Bc7AnisoBiasHalf, 8});
    namespace SamplerBias = GuestSampledTableSamplerBiasData;
    RunCase(device, block, {"bc7-sbias/implicit-point", 38, BiasedSampler(1, 1, 1, 77), SamplerBias::Bc7SamplerBiasImplicitRows, SamplerBias::Bc7SamplerBiasImplicitPoint, 4, SampleImplicitCode});
    RunCase(device, block, {"bc7-sbias/implicit-trilinear", 39, BiasedSampler(1, 1, 2, -200), SamplerBias::Bc7SamplerBiasImplicitTrilinearRows, SamplerBias::Bc7SamplerBiasImplicitTrilinear, 4, SampleImplicitCode});
    RunCase(device, block, {"bc7-sbias/lod", 40, BiasedSampler(1, 1, 2, 383), SamplerBias::Bc7SamplerBiasLodRows, SamplerBias::Bc7SamplerBiasLodTrilinear});
    RunCase(device, block, {"bc7-sbias/lz", 41, BiasedSampler(1, 1, 2, 300), SamplerBias::Bc7SamplerBiasZeroLodRows, SamplerBias::Bc7SamplerBiasZeroLodTrilinear, 4, SampleZeroLodCode});
    namespace Wrap = GuestSampledTableWrapData;
    RunCase(device, block, {"bc7-wrap/implicit", 42, AddressedSampler(0, 0, 1, 1, 2, 0), Wrap::Bc7WrapImplicitRows, Wrap::Bc7WrapImplicitTrilinear, 4, SampleImplicitCode});
    RunCase(device, block, {"bc7-wrap/point-mip", 43, AddressedSampler(0, 0, 1, 1, 1, 0), Wrap::Bc7WrapPointMipRows, Wrap::Bc7WrapImplicitPointMip, 4, SampleImplicitCode});
    RunCase(device, block, {"bc7-wrap/lod", 44, AddressedSampler(0, 0, 1, 1, 2, 0), Wrap::Bc7WrapLodRows, Wrap::Bc7WrapLodTrilinear});
    RunCase(device, block, {"bc7-wrap-x/lod", 45, AddressedSampler(0, 2, 1, 1, 2, 0), Wrap::Bc7WrapLodRows, Wrap::Bc7WrapXClampYLodTrilinear});
    RunCase(device, block, {"bc7-wrap/aniso", 46, AddressedSampler(0, 0, 3, 3, 2, 1), Wrap::Bc7WrapAnisoRows, Wrap::Bc7WrapAnisoTrilinear, 8});
    namespace Npot = GuestSampledTableNpotAnisoData;
    RunCase(device, block, {"bc7-npot/aniso-trilinear", 47, AnisoSampler(3, 3, 2), Npot::Bc7Npot96x40AnisoRows, Npot::Bc7Npot96x40AnisoTrilinear, 8});
    RunCase(device, block, {"bc7-npot/aniso-point", 48, AnisoSampler(3, 3, 1), Npot::Bc7Npot96x40AnisoRows, Npot::Bc7Npot96x40AnisoPoint, 8});
    RunCase(device, block, {"bc7-npot-uneven/aniso-trilinear", 49, AnisoSampler(3, 3, 2), Npot::Bc7Npot100x52AnisoRows, Npot::Bc7Npot100x52AnisoTrilinear, 8});
    RunCase(device, block, {"rgba8-rx/bilinear", 50, Sampler(1, 1, 0), Rgba8Rows, Rgba8Bilinear});
    RunCase(device, block, {"rgba8-zx/point", 51, Sampler(0, 0, 0), Rgba8Rows, Rgba8Point});
    RunCase(device, block, {"rgba8-srgb/bilinear", 52, Sampler(1, 1, 0), Rgba8Rows, GuestSampledTableSrgb8Data::Rgba8SrgbBilinear});
    RunCase(device, block, {"rgba8-srgb/point", 53, Sampler(0, 0, 0), Rgba8Rows, GuestSampledTableSrgb8Data::Rgba8SrgbPoint});
    RunCase(device, block, {"rgba8-srgb-256b/bilinear", 54, Sampler(1, 1, 0), Rgba8Rows, GuestSampledTableSrgb8Data::Rgba8SrgbBilinear});
    for (std::size_t index = 0; index < plainCases.size(); ++index) {
        const auto& plain = plainCases[index];
        RunCase(device, block, {plain.name, static_cast<std::uint32_t>(PlainRecord + index), plain.bilinear ? Sampler(1, 1, 0) : Sampler(0, 0, 0), Rgba8Rows, plain.expected});
    }
    namespace Arrays = GuestSampledTableArrayData;
    RunCase(device, block, {"bc7-array/trilinear", 63, Sampler(1, 1, 2), Arrays::Bc7ArrayRows, Arrays::Bc7ArrayTrilinear, 4, SampleArrayCode});
    RunCase(device, block, {"rgba8-array/bilinear", 64, Sampler(1, 1, 0), Arrays::Rgba8ArrayRows, Arrays::Rgba8ArrayBilinear, 4, SampleArrayCode});
    {
        namespace Aniso8 = GuestSampledTableAnisoRgba8Data;
        RunCase(device, block, {"rgba8-aniso/trilinear", Rgba8AnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Aniso8::Rgba8AnisoTrilinear, 8});
        RunCase(device, block, {"rgba8-srgb-aniso/trilinear", Rgba8AnisoRecord + 1u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Aniso8::Rgba8SrgbAnisoTrilinear, 8});
        RunCase(device, block, {"rgba8-aniso/point", Rgba8AnisoRecord + 2u, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, Aniso8::Rgba8AnisoPoint, 8});
        RunCase(device, block, {"rgba8-rx-aniso/trilinear", Rgba8AnisoRecord + 3u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Aniso8::Rgba8AnisoTrilinear, 8});
        RunCase(device, block, {"rgba8-srgb-rx-aniso/trilinear", Rgba8AnisoRecord + 4u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Aniso8::Rgba8SrgbAnisoTrilinear, 8});
        RunCase(device, block, {"rgba8-s64-aniso/trilinear", Rgba8AnisoRecord + 5u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Aniso8::Rgba8AnisoS64, 8});
        RunCase(device, block, {"rgba8-npot-aniso/trilinear", Rgba8AnisoRecord + 6u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, Aniso8::Rgba8AnisoNpot, 8});
    }
    RunCase(device, block, {"r16-rx/bilinear", SingleXorRecord, Sampler(1, 1, 0), GuestSampledTableDerivativeData::SingleChannelRows, GuestSampledTableDerivativeData::R16Bilinear});
    RunCase(device, block, {"r16-zx/bilinear", SingleXorRecord + 1u, Sampler(1, 1, 0), GuestSampledTableDerivativeData::SingleChannelRows, GuestSampledTableDerivativeData::R16Bilinear});
    RunCase(device, block, {"r32f-rx/bilinear", SingleXorRecord + 2u, Sampler(1, 1, 0), GuestSampledTableDerivativeData::SingleChannelRows, GuestSampledTableDerivativeData::R32fBilinear});
    RunCase(device, block, {"r32f-zx/bilinear", SingleXorRecord + 3u, Sampler(1, 1, 0), GuestSampledTableDerivativeData::SingleChannelRows, GuestSampledTableDerivativeData::R32fBilinear});
    {
        namespace AnisoSingle = GuestSampledTableAnisoSingleData;
        RunCase(device, block, {"r8-aniso/trilinear", SingleAnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoSingle::R8AnisoTrilinear, 8});
        RunCase(device, block, {"r8-aniso/point", SingleAnisoRecord + 1u, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, AnisoSingle::R8AnisoPoint, 8});
        RunCase(device, block, {"r16-aniso/trilinear", SingleAnisoRecord + 2u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoSingle::R16AnisoTrilinear, 8});
        RunCase(device, block, {"r16-aniso/point", SingleAnisoRecord + 3u, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, AnisoSingle::R16AnisoPoint, 8});
        RunCase(device, block, {"r16-npot-aniso/trilinear", SingleAnisoRecord + 4u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoSingle::R16AnisoNpot, 8});
    }
    {
        namespace AnisoFloat = GuestSampledTableAnisoFloatData;
        RunCase(device, block, {"rgba16f-aniso/trilinear", FloatAnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoFloat::Rgba16fAnisoTrilinear, 8});
        RunCase(device, block, {"rgba16f-aniso/point", FloatAnisoRecord + 1u, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, AnisoFloat::Rgba16fAnisoPoint, 8});
        RunCase(device, block, {"r16f-aniso/trilinear", FloatAnisoRecord + 2u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoFloat::R16fAnisoTrilinear, 8});
        RunCase(device, block, {"r11g11b10f-aniso/trilinear", FloatAnisoRecord + 3u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoFloat::R11g11b10fAnisoTrilinear, 8});
        RunCase(device, block, {"r32f-aniso/trilinear", FloatAnisoRecord + 4u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoFloat::R32fAnisoTrilinear, 8});
        RunCase(device, block, {"r32f-aniso/point", FloatAnisoRecord + 5u, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, AnisoFloat::R32fAnisoPoint, 8});
        RunCase(device, block, {"bc6h-aniso/trilinear", FloatAnisoRecord + 6u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoFloat::Bc6hAnisoTrilinear, 8});
        RunCase(device, block, {"bc6h-aniso-mips/trilinear", Bc6hMipsAnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, GuestSampledTableAnisoMipsData::Bc6hAnisoMips, 8});
        RunCase(device, block, {"r16-aniso-mips/trilinear", R16MipsAnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, GuestSampledTableAnisoMipsData::R16AnisoMips, 8});
        RunCase(device, block, {"r32f-aniso-mips/trilinear", R32fMipsAnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, GuestSampledTableAnisoMipsData::R32fAnisoMips, 8});
    }
    {
        namespace AnisoGap = GuestSampledTableAnisoGapData;
        RunCase(device, block, {"r16f-wide-aniso/trilinear", GapAnisoRecord, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoGap::R16fWideTrilinear, 8});
        RunCase(device, block, {"r16f-wide-aniso/point", GapAnisoRecord + 1u, AnisoSampler(3, 3, 1), Formats::AnisoFormatsRows, AnisoGap::R16fWidePoint, 8});
        RunCase(device, block, {"r16f-wide-signed-aniso/trilinear", GapAnisoRecord + 2u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoGap::R16fWideSignedTrilinear, 8});
        RunCase(device, block, {"rgba16f-wide-aniso/trilinear", GapAnisoRecord + 3u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoGap::Rgba16fWideTrilinear, 8});
        RunCase(device, block, {"r32f-wide-aniso/trilinear", GapAnisoRecord + 4u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoGap::R32fWideTrilinear, 8});
        RunCase(device, block, {"r32f-wide-signed-aniso/trilinear", GapAnisoRecord + 5u, AnisoSampler(3, 3, 2), Formats::AnisoFormatsRows, AnisoGap::R32fWideSignedTrilinear, 8});
    }
    RunCase(device, block, {"bc7/point", 1, Sampler(0, 0, 0), Bc7PointRows, Bc7Point});
    RunCase(device, block, {"bc7-mips/point", 2, Sampler(0, 0, 1), Bc7MipRows, Bc7MipPoint});
    RunCase(device, block, {"rgba8/point", 3, Sampler(0, 0, 0), Rgba8Rows, Rgba8Point});
    RunCase(device, block, {"bc7/bilinear", 4, Sampler(1, 1, 0), Bc7BilinearRows, Bc7Bilinear});
    RunLoadTests(device, block);
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
