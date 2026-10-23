#ifndef CORE_SHADER_RECOMPILER_IMAGETABLEABI_HPP
#define CORE_SHADER_RECOMPILER_IMAGETABLEABI_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ShaderRecompiler::ImageTableAbi {

inline constexpr std::uint32_t Version = 4;

inline constexpr std::uint32_t HeaderVersion = 0;
inline constexpr std::uint32_t HeaderElementBase = 1;
inline constexpr std::uint32_t HeaderElementCount = 2;
inline constexpr std::uint32_t HeaderSamplerCount = 3;
inline constexpr std::uint32_t HeaderPoisonCount = 4;
inline constexpr std::uint32_t HeaderElementFaults = 5;
inline constexpr std::uint32_t HeaderSamplerFaults = 6;
inline constexpr std::uint32_t HeaderTableCount = 7;
inline constexpr std::uint32_t HeaderGuestElements = 8;
inline constexpr std::uint32_t HeaderGuestState = 9;
inline constexpr std::uint32_t HeaderGuestUsed = 10;
inline constexpr std::uint32_t HeaderGuestSamplers = 11;
inline constexpr std::uint32_t HeaderGuestFault = 12;
inline constexpr std::uint32_t HeaderGuestMemory = 13;
inline constexpr std::uint32_t HeaderWords = 14;

inline constexpr std::uint32_t TableMapStart = 0;
inline constexpr std::uint32_t TableKeyCount = 1;
inline constexpr std::uint32_t TableSizeLow = 2;
inline constexpr std::uint32_t TableSizeHigh = 3;
inline constexpr std::uint32_t TableBaseLow = 4;
inline constexpr std::uint32_t TableBaseHigh = 5;
inline constexpr std::uint32_t TableOutsideCode = 6;
inline constexpr std::uint32_t TableFault = 7;
inline constexpr std::uint32_t TableWords = 8;

inline constexpr std::uint32_t ImageRecordFlags = 0;
inline constexpr std::uint32_t ImageRecordDword3 = 1;
inline constexpr std::uint32_t ImageRecordElement = 2;
inline constexpr std::uint32_t ImageRecordWords = 3;

inline constexpr std::uint32_t SamplerRecordFlags = 0;
inline constexpr std::uint32_t SamplerRecordElement = 1;
inline constexpr std::uint32_t SamplerRecordWords = 2;
inline constexpr std::uint32_t SamplerEntryElements = 4;

inline constexpr std::uint32_t NullCode = 0;
inline constexpr std::uint32_t PoisonFlag = 0x80000000u;
inline constexpr std::uint32_t ModeShift = 1;
inline constexpr std::uint32_t ModeMask = 0xffu;

inline constexpr std::uint32_t OutsideSnapshotPoison = 0;
inline constexpr std::uint32_t OutsideDomainPoison = 1;
inline constexpr std::uint32_t FirstEntryPoison = 2;

inline constexpr std::uint32_t MaxKeys = 65536;
inline constexpr std::uint32_t MaxModes = 256;
inline constexpr std::uint32_t ImageElementBudget = 65536;

inline constexpr std::uint32_t GuestElementWords = 8;
inline constexpr std::uint32_t GuestSamplerWords = 4;
inline constexpr std::uint32_t GuestMemoryWords = 4;
inline constexpr std::uint32_t GuestStateHost = 0;
inline constexpr std::uint32_t GuestStateSampled = 1;
inline constexpr std::uint32_t GuestStateCapable = 2;
inline constexpr std::uint32_t SamplerElementBudget = 1024;

enum class PoisonReason : std::uint32_t {
    OutsideSnapshot,
    OutsideDomain,
    Unmapped,
    Inactive,
    NotBuffer,
    NotImage,
    InvalidFormat,
    Dimension,
    Multisampled,
    Fmask,
    ColorCompare,
    DepthBits16,
    Conversion,
    SrgbUnsupported,
    NumericClass,
    Budget,
    Reduction,
    Unnormalized,
    HeapWritten,
    DriverRejected,
    UnsupportedOperation,
    OffsetCarry,
    GuestSampler,
    Count
};

constexpr std::string_view PoisonReasonName(PoisonReason reason) {
    switch (reason) {
        case PoisonReason::OutsideSnapshot: return "outside the snapshot";
        case PoisonReason::OutsideDomain: return "outside the key domain";
        case PoisonReason::Unmapped: return "unmapped";
        case PoisonReason::Inactive: return "inactive source";
        case PoisonReason::NotBuffer: return "table V# is not a buffer";
        case PoisonReason::NotImage: return "not an image";
        case PoisonReason::InvalidFormat: return "invalid format";
        case PoisonReason::Dimension: return "dimension";
        case PoisonReason::Multisampled: return "multisampled";
        case PoisonReason::Fmask: return "FMASK";
        case PoisonReason::ColorCompare: return "color texture under comparison";
        case PoisonReason::DepthBits16: return "depth bits with 16-bit results";
        case PoisonReason::Conversion: return "format conversion";
        case PoisonReason::SrgbUnsupported: return "sRGB decode";
        case PoisonReason::NumericClass: return "numeric class";
        case PoisonReason::Budget: return "over budget";
        case PoisonReason::Reduction: return "min or max reduction";
        case PoisonReason::Unnormalized: return "unnormalized coordinates";
        case PoisonReason::HeapWritten: return "table written by its dispatch";
        case PoisonReason::DriverRejected: return "rejected by the driver";
        case PoisonReason::UnsupportedOperation: return "unsupported operation";
        case PoisonReason::OffsetCarry: return "record offset carries past 32 bits";
        case PoisonReason::GuestSampler: return "the guest sampler met a texel, coordinate or LOD it does not implement";
        case PoisonReason::Count: break;
    }
    return "unknown";
}

constexpr std::uint32_t PoisonCode(std::uint32_t poison) {
    return PoisonFlag | poison;
}

constexpr std::uint32_t TableHeader(std::uint32_t table) {
    return HeaderWords + TableWords * table;
}

constexpr bool GuestGatherSupported(const std::array<std::uint32_t, 8>& image) {
    return ((image[3] >> 16u) & 0xfu) == 0u && ((image[5] >> 4u) & 0xfu) == 0u;
}

constexpr bool GuestLoadSupported(const std::array<std::uint32_t, 8>& image) {
    const std::uint32_t format = (image[1] >> 20u) & 0x1ffu;
    const std::uint32_t baseLevel = (image[3] >> 12u) & 0xfu;
    const std::uint32_t lastLevel = (image[3] >> 16u) & 0xfu;
    const std::uint32_t swizzle = (image[3] >> 20u) & 0x1fu;
    const std::uint32_t type = (image[3] >> 28u) & 0xfu;
    const std::uint32_t maxMip = (image[5] >> 4u) & 0xfu;
    const bool rgba8 = format == 56u || format == 130u;
    const bool single = format == 7u || format == 22u;
    const bool plain = format == 1u || format == 13u || format == 36u || format == 71u;
    const bool bc = format == 169u || format == 170u || format == 175u || format == 177u || format == 179u || format == 181u || format == 182u;
    if (!rgba8 && !single && !plain && !bc) return false;
    if (type != 9u || baseLevel != 0u || maxMip != lastLevel) return false;
    if (rgba8 ? (swizzle != 0u && swizzle != 1u && swizzle != 9u && swizzle != 24u && swizzle != 27u) || lastLevel != 0u : single ? swizzle != 0u && swizzle != 1u : plain ? (swizzle != 0u && swizzle != 1u && swizzle != 24u && swizzle != 27u) || lastLevel != 0u : swizzle != 0u && swizzle != 1u && swizzle != 5u && swizzle != 9u) return false;
    for (std::uint32_t channel = 0; channel < 4u; ++channel) {
        const std::uint32_t selector = (image[3] >> (channel * 3u)) & 7u;
        if (selector == 2u || selector == 3u) return false;
    }
    return swizzle != 0u || image[4] == 0u;
}

constexpr bool GuestSampleSupported(const std::array<std::uint32_t, 8>& image, const std::array<std::uint32_t, 4>& sampler) {
    const std::uint32_t format = (image[1] >> 20u) & 0x1ffu;
    const std::uint32_t textureMinLod = (image[1] >> 8u) & 0xfffu;
    const std::uint32_t baseLevel = (image[3] >> 12u) & 0xfu;
    const std::uint32_t lastLevel = (image[3] >> 16u) & 0xfu;
    const std::uint32_t swizzle = (image[3] >> 20u) & 0x1fu;
    const std::uint32_t type = (image[3] >> 28u) & 0xfu;
    const std::uint32_t maxMip = (image[5] >> 4u) & 0xfu;
    const std::uint32_t clampX = sampler[0] & 7u;
    const std::uint32_t clampY = (sampler[0] >> 3u) & 7u;
    const std::uint32_t samplerMinLod = sampler[1] & 0xfffu;
    const std::uint32_t magFilter = (sampler[2] >> 20u) & 3u;
    const std::uint32_t minFilter = (sampler[2] >> 22u) & 3u;
    const std::uint32_t mipFilter = (sampler[2] >> 26u) & 3u;
    const bool rgba8 = format == 56u || format == 130u;
    const bool single = format == 7u || format == 22u;
    const bool plain = format == 1u || format == 13u || format == 36u || format == 71u;
    const bool bc = format == 169u || format == 170u || format == 175u || format == 177u || format == 179u || format == 181u || format == 182u;
    if (!rgba8 && !single && !plain && !bc) return false;
    if ((type != 9u && (type != 13u || (image[4] >> 16u) != 0u)) || baseLevel != 0u || maxMip != lastLevel || std::max(textureMinLod, samplerMinLod) > (lastLevel << 8u)) return false;
    if (rgba8 ? (swizzle != 0u && swizzle != 1u && swizzle != 9u && swizzle != 24u && swizzle != 27u) || lastLevel != 0u : single ? swizzle != 0u && swizzle != 1u : plain ? (swizzle != 0u && swizzle != 1u && swizzle != 24u && swizzle != 27u) || lastLevel != 0u : swizzle != 0u && swizzle != 1u && swizzle != 5u && swizzle != 9u) return false;
    for (std::uint32_t channel = 0; channel < 4u; ++channel) {
        const std::uint32_t selector = (image[3] >> (channel * 3u)) & 7u;
        if (selector == 2u || selector == 3u) return false;
    }
    if (swizzle == 0u && image[4] != 0u) return false;
    const std::uint32_t anisoRatio = (sampler[0] >> 9u) & 7u;
    const std::uint32_t anisoThreshold = (sampler[0] >> 16u) & 7u;
    const bool anisoTaps = magFilter == 3u && minFilter == 3u && anisoRatio == 1u;
    if ((clampX != 0u && clampX != 2u) || (clampY != 0u && clampY != 2u)) return false;
    if (!anisoTaps && (magFilter != minFilter || magFilter > 1u || anisoRatio > 1u)) return false;
    if ((sampler[0] & 0xe00u) != (anisoRatio << 9u) || ((sampler[0] >> 12u) & ~0x70u) != 0u || (anisoThreshold != 0u && anisoThreshold != 7u) || (sampler[2] & 0xfc000u) != 0u) return false;
    if (anisoTaps) {
        const std::uint32_t samplerMaxLod = (sampler[1] >> 12u) & 0xfffu;
        if ((format != 56u && format != 130u && format != 169u && format != 170u && format != 175u && format != 181u && format != 182u) || samplerMinLod != 0u || samplerMaxLod < (lastLevel << 8u)) return false;
    }
    if (lastLevel != 0u && mipFilter != 1u && mipFilter != 2u) return false;
    if (single && lastLevel != 0u && mipFilter == 2u) return false;
    return true;
}

constexpr std::optional<std::uint32_t> ScalarBufferDword(std::uint32_t offset, std::uint32_t immediate, std::uint64_t size) {
    const std::uint64_t address = static_cast<std::uint64_t>(offset) + immediate;
    if (address > 0xffffffffull) return std::nullopt;
    const std::uint32_t dword = static_cast<std::uint32_t>(address) & ~3u;
    if (static_cast<std::uint64_t>(dword) + 4u > size) return std::nullopt;
    return dword;
}

constexpr std::optional<std::uint64_t> ScalarAddressDword(std::uint64_t base, std::uint32_t offset, std::uint32_t immediate) {
    const std::uint64_t first = base + (offset & ~3u);
    if (first < base) return std::nullopt;
    const std::int64_t delta = static_cast<std::int32_t>(immediate & ~3u);
    const std::uint64_t magnitude = delta < 0 ? static_cast<std::uint64_t>(-delta) : static_cast<std::uint64_t>(delta);
    if (delta < 0) return magnitude > first ? std::nullopt : std::optional<std::uint64_t>(first - magnitude);
    return first + magnitude < first ? std::nullopt : std::optional<std::uint64_t>(first + magnitude);
}

constexpr std::uint32_t ScalarAddressRecords(std::uint32_t stride) {
    return stride <= 1u ? 0xffffffffu * stride : 0xffffffffu / stride + 1u;
}

constexpr std::uint32_t KeyRecords(std::uint32_t maxKey, std::uint32_t stride) {
    return maxKey != 0xffffffffu && static_cast<std::uint64_t>(maxKey) * stride <= 0xffffffffull ? maxKey + 1u : 0xffffffffu;
}

constexpr std::uint32_t ScalarBufferRecords(std::uint32_t addend, std::uint32_t stride, std::uint32_t immediate, std::uint64_t size) {
    const std::uint64_t start = static_cast<std::uint64_t>(addend) + immediate;
    const std::uint64_t rounded = size & ~std::uint64_t{3u};
    const std::uint64_t limit = rounded < 0x100000000ull ? rounded : 0x100000000ull;
    if (stride == 0u || start >= limit) return 0u;
    const std::uint64_t records = (limit - start + stride - 1u) / stride;
    return records > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(records);
}

}

#endif
