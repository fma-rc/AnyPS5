#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#ifndef GUEST_SAMPLE_CHECK
layout(local_size_x = 1) in;
#endif

#include "GuestSampleTables.glsl"

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GuestWord {
    uint value;
};

uint64_t guestOrigin = 0ul;
uint64_t guestDevice = 0ul;
uint64_t guestBytes = 0ul;
bool guestOutside = false;

uint guestLoad32(uint64_t address) {
    if (address < guestOrigin || ((address - guestOrigin) & 3ul) != 0ul || address - guestOrigin >= guestBytes || guestBytes - (address - guestOrigin) < 4ul) {
        guestOutside = true;
        return 0u;
    }
    return GuestWord(guestDevice + (address - guestOrigin)).value;
}

#define RoundU64(value, shift) packUint2x32(roundHalfEvenShiftUnsigned(value, shift))
#define WideShift64(high, low, shift) packUint2x32(wideShiftRight(high, low, shift))

const uint FaultCode = 0xfa017fa0u;
const uint Bc7Weights2[4] = uint[](0u, 21u, 43u, 64u);
const uint Bc7Weights3[8] = uint[](0u, 9u, 18u, 27u, 37u, 46u, 55u, 64u);
const uint Bc7Weights4[16] = uint[](0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u, 34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u);
const uint Bc7Modes[8] = uint[](0x03104043u, 0x03206062u, 0x02005063u, 0x02107062u, 0x32065601u, 0x22087201u, 0x04177001u, 0x02155062u);

bool faulted = false;

uint standardOffset4(uint x, uint y) {
    uint o = ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
    o ^= ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^ ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
    return o & 0xffffu;
}

uint standardOffset16(uint x, uint y) {
    uint o = ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
    o ^= ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^ ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
    return o;
}

uint standardOffset8(uint x, uint y) {
    return ((x & 1u) << 3) | ((y & 3u) << 4) | ((x & 6u) << 5) | ((y & 4u) << 6) | ((x & 8u) << 6) | ((y & 8u) << 7) | ((x & 16u) << 7) | ((y & 16u) << 8) | ((x & 32u) << 8) | ((y & 32u) << 9) | ((x & 64u) << 9);
}

ivec2 roundHalfEvenShift(int64_t value, uint shift) {
    if (shift == 0u) return unpackInt2x32(value);
    if (shift >= 63u) return unpackInt2x32(int64_t(0));
    bool negative = value < int64_t(0);
    uint64_t magnitude = uint64_t(negative ? -value : value);
    uint64_t quotient = magnitude >> shift;
    uint64_t remainder = magnitude & ((uint64_t(1) << shift) - uint64_t(1));
    uint64_t midpoint = uint64_t(1) << (shift - 1u);
    if (remainder > midpoint || (remainder == midpoint && (quotient & uint64_t(1)) != uint64_t(0))) quotient += uint64_t(1);
    return unpackInt2x32(negative ? -int64_t(quotient) : int64_t(quotient));
}

uvec2 roundHalfEvenShiftUnsigned(uint64_t value, int shift) {
    if (shift <= 0) return unpackUint2x32(value << uint(-shift));
    uint64_t quotient = value >> uint(shift);
    uint64_t remainder = value & ((uint64_t(1) << uint(shift)) - uint64_t(1));
    uint64_t midpoint = uint64_t(1) << uint(shift - 1);
    if (remainder > midpoint || (remainder == midpoint && (quotient & uint64_t(1)) != uint64_t(0))) quotient += uint64_t(1);
    return unpackUint2x32(quotient);
}

int msb64(uint64_t value) {
    uint high = uint(value >> 32);
    return high != 0u ? 32 + findMSB(high) : findMSB(uint(value));
}

ivec2 fixedTexelCoordinate(uint bits, uint size) {
    uint exponent = (bits >> 23) & 0xffu;
    if (exponent == 0u) return unpackInt2x32(int64_t(0));
    int64_t mantissa = int64_t((bits & 0x7fffffu) | 0x800000u) * int64_t(size);
    if ((bits & 0x80000000u) != 0u) mantissa = -mantissa;
    int shift = 142 - int(exponent);
    if (shift <= 0) return unpackInt2x32(mantissa << uint(-shift));
    return roundHalfEvenShift(mantissa, uint(shift));
}

int wrapIndex(int index, int extent, uint mode) {
    if (mode == 0u) {
        uint magnitude = uint(index < 0 ? -index : index) % uint(extent);
        return index < 0 && magnitude != 0u ? extent - int(magnitude) : int(magnitude);
    }
    return clamp(index, 0, extent - 1);
}

uint elementByte(uint ex, uint ey, uint widthElements, uint bytesPerElement, uint swizzle) {
    if (swizzle == 0u) {
        uint align = 256u / bytesPerElement;
        uint pitch = (widthElements + align - 1u) / align * align;
        return (ey * pitch + ex) * bytesPerElement;
    }
    if (bytesPerElement == 2u && swizzle == 1u) return (((ey >> 3) * ((widthElements + 15u) >> 4) + (ex >> 4)) << 8) + (((ex & 7u) << 1) | ((ey & 7u) << 4) | (((ex >> 3) & 1u) << 7));
    if (bytesPerElement == 4u && swizzle == 1u) return (((ey >> 3) * ((widthElements + 7u) >> 3) + (ex >> 3)) << 8) + (standardOffset4(ex & 7u, ey & 7u) & 0xffu);
    if (bytesPerElement == 4u && swizzle == 9u) {
        uint blocksPerRow = (widthElements + 127u) >> 7;
        return (((ey >> 7) * blocksPerRow + (ex >> 7)) << 16) + standardOffset4(ex & 127u, ey & 127u);
    }
    if (bytesPerElement == 8u) {
        if (swizzle == 1u) return (((ey >> 2) * ((widthElements + 7u) >> 3) + (ex >> 3)) << 8) + standardOffset8(ex & 7u, ey & 3u);
        if (swizzle == 5u) return (((ey >> 4) * ((widthElements + 31u) >> 5) + (ex >> 5)) << 12) + standardOffset8(ex & 31u, ey & 15u);
        if (swizzle == 9u) return (((ey >> 6) * ((widthElements + 127u) >> 7) + (ex >> 7)) << 16) + standardOffset8(ex & 127u, ey & 63u);
    }
    if (bytesPerElement == 16u) {
        if (swizzle == 1u) return (((ey >> 2) * ((widthElements + 3u) >> 2) + (ex >> 2)) << 8) + (standardOffset16(ex & 3u, ey & 3u) & 0xffu);
        if (swizzle == 5u) return (((ey >> 4) * ((widthElements + 15u) >> 4) + (ex >> 4)) << 12) + (standardOffset16(ex & 15u, ey & 15u) & 0xfffu);
        if (swizzle == 9u) return (((ey >> 6) * ((widthElements + 63u) >> 6) + (ex >> 6)) << 16) + standardOffset16(ex & 63u, ey & 63u);
    }
    faulted = true;
    return 0u;
}

uint blockBits(uvec4 block, uint position, uint count) {
    if (count == 0u) return 0u;
    uint word = position >> 5;
    uint shift = position & 31u;
    uint low = block[word] >> shift;
    if (shift + count > 32u) low |= block[word + 1u] << (32u - shift);
    return low & ((1u << count) - 1u);
}

uint bc7Expand(uint value, uint bits) {
    if (bits >= 8u) return value;
    return ((value << (8u - bits)) | (value >> (2u * bits - 8u))) & 0xffu;
}

uint bc7Weight(uint bits, uint index) {
    if (bits == 2u) return Bc7Weights2[index];
    if (bits == 3u) return Bc7Weights3[index];
    return Bc7Weights4[index];
}

uint bc7IndexOffset(uint pixel, uint bits, uint anchor1, uint anchor2) {
    if (pixel == 0u) return 0u;
    uint offset = pixel * bits - 1u;
    if (anchor1 < pixel) offset -= 1u;
    if (anchor2 < pixel) offset -= 1u;
    return offset;
}

uint bc7IndexLength(uint pixel, uint bits, uint anchor1, uint anchor2) {
    return (pixel == 0u || pixel == anchor1 || pixel == anchor2) ? bits - 1u : bits;
}

uvec4 decodeBc7Texel(uvec4 block, uint pixel) {
    uint firstByte = block.x & 0xffu;
    if (firstByte == 0u) return uvec4(0u);
    uint mode = uint(findLSB(firstByte));
    uint desc = Bc7Modes[mode];
    uint subsets = desc & 3u;
    uint partitionBits = (desc >> 4) & 0xfu;
    uint rotationBits = (desc >> 8) & 3u;
    uint selectionBits = (desc >> 10) & 1u;
    uint colorBits = (desc >> 12) & 0xfu;
    uint alphaBits = (desc >> 16) & 0xfu;
    uint endpointPBits = (desc >> 20) & 1u;
    uint sharedPBits = (desc >> 21) & 1u;
    uint indexBits = (desc >> 24) & 7u;
    uint secondaryBits = (desc >> 28) & 7u;
    uint position = mode + 1u;
    uint partitionIndex = blockBits(block, position, partitionBits);
    position += partitionBits;
    uint rotation = blockBits(block, position, rotationBits);
    position += rotationBits;
    uint selection = blockBits(block, position, selectionBits);
    position += selectionBits;
    uint subset = 0u;
    uint anchor1 = 16u;
    uint anchor2 = 16u;
    if (subsets == 2u) {
        subset = (Bc7Partition2[partitionIndex] >> pixel) & 1u;
        anchor1 = Bc7Anchor2[partitionIndex];
    } else if (subsets == 3u) {
        subset = (Bc7Partition3[partitionIndex] >> (2u * pixel)) & 3u;
        anchor1 = Bc7Anchor3a[partitionIndex];
        anchor2 = Bc7Anchor3b[partitionIndex];
    }
    uint colorStart = position;
    uint alphaStart = colorStart + 6u * subsets * colorBits;
    uint pStart = alphaStart + 2u * subsets * alphaBits;
    uint indexStart = pStart + (endpointPBits != 0u ? 2u * subsets : (sharedPBits != 0u ? subsets : 0u));
    uvec4 e0;
    uvec4 e1;
    uint precisionColor = colorBits + endpointPBits + sharedPBits;
    uint precisionAlpha = alphaBits != 0u ? alphaBits + endpointPBits + sharedPBits : 0u;
    for (uint e = 0u; e < 2u; ++e) {
        uint pbit = 0u;
        if (endpointPBits != 0u) pbit = blockBits(block, pStart + subset * 2u + e, 1u);
        if (sharedPBits != 0u) pbit = blockBits(block, pStart + subset, 1u);
        uvec4 endpoint;
        for (uint c = 0u; c < 3u; ++c) {
            uint v = blockBits(block, colorStart + (c * subsets * 2u + subset * 2u + e) * colorBits, colorBits);
            if (endpointPBits != 0u || sharedPBits != 0u) v = (v << 1) | pbit;
            endpoint[c] = bc7Expand(v, precisionColor);
        }
        if (alphaBits != 0u) {
            uint v = blockBits(block, alphaStart + (subset * 2u + e) * alphaBits, alphaBits);
            if (endpointPBits != 0u || sharedPBits != 0u) v = (v << 1) | pbit;
            endpoint.w = bc7Expand(v, precisionAlpha);
        } else {
            endpoint.w = 255u;
        }
        if (e == 0u) e0 = endpoint; else e1 = endpoint;
    }
    uint primary = blockBits(block, indexStart + bc7IndexOffset(pixel, indexBits, anchor1, anchor2), bc7IndexLength(pixel, indexBits, anchor1, anchor2));
    uint colorWeight = bc7Weight(indexBits, primary);
    uint alphaWeight = colorWeight;
    if (secondaryBits != 0u) {
        uint secondaryStart = indexStart + 16u * indexBits - 1u;
        uint secondary = blockBits(block, secondaryStart + bc7IndexOffset(pixel, secondaryBits, 16u, 16u), bc7IndexLength(pixel, secondaryBits, 16u, 16u));
        if (selection == 0u) {
            alphaWeight = bc7Weight(secondaryBits, secondary);
        } else {
            colorWeight = bc7Weight(secondaryBits, secondary);
            alphaWeight = bc7Weight(indexBits, primary);
        }
    }
    uvec4 texel;
    for (uint c = 0u; c < 3u; ++c) texel[c] = ((64u - colorWeight) * e0[c] + colorWeight * e1[c] + 32u) >> 6;
    texel.w = ((64u - alphaWeight) * e0.w + alphaWeight * e1.w + 32u) >> 6;
    if (rotation == 1u) texel = texel.wyzx;
    else if (rotation == 2u) texel = texel.xwzy;
    else if (rotation == 3u) texel = texel.xywz;
    return texel;
}

const uint Tail4KB16[8] = uint[](0x0008u, 0x0804u, 0x0c00u, 0x0800u, 0x0404u, 0x0004u, 0x0400u, 0x0000u);
const uint Tail64KB16[12] = uint[](0x0020u, 0x2000u, 0x0010u, 0x1000u, 0x0008u, 0x0804u, 0x0c00u, 0x0800u, 0x0404u, 0x0004u, 0x0400u, 0x0000u);
const uint Tail4KB8[8] = uint[](0x0010u, 0x0808u, 0x0c00u, 0x0800u, 0x0408u, 0x0008u, 0x0400u, 0x0000u);
const uint Tail64KB8[12] = uint[](0x0040u, 0x2000u, 0x0020u, 0x1000u, 0x0010u, 0x0808u, 0x0c00u, 0x0800u, 0x0408u, 0x0008u, 0x0400u, 0x0000u);
const uint Bc4Weights7[8] = uint[](0u, 64u, 9u, 18u, 27u, 37u, 46u, 55u);
const uint Bc4Weights5[6] = uint[](0u, 64u, 13u, 26u, 38u, 51u);

struct Level {
    uint width;
    uint height;
    uint widthElements;
    uint offset;
    uint tailX;
    uint tailY;
};

uint shiftCeil(uint value, uint shift) {
    return (value + (1u << shift) - 1u) >> shift;
}

uint alignUp(uint value, uint alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

Level levelLayout(uint width, uint height, uint levelCount, uint swizzle, uint bytesPerElement, uint level) {
    Level result;
    result.width = max(width >> level, 1u);
    result.height = max(height >> level, 1u);
    result.tailX = 0u;
    result.tailY = 0u;
    result.offset = 0u;
    if (bytesPerElement <= 4u) {
        result.widthElements = result.width;
        if (levelCount == 1u) return result;
        if (swizzle == 0u) {
            for (uint j = level + 1u; j < levelCount; ++j) result.offset += alignUp(max(width >> j, 1u), 256u / bytesPerElement) * max(height >> j, 1u) * bytesPerElement;
        } else if (swizzle == 1u) {
            uint blockWidth = bytesPerElement == 4u ? 8u : 16u;
            for (uint j = level + 1u; j < levelCount; ++j) result.offset += alignUp(max(width >> j, 1u), blockWidth) * alignUp(max(height >> j, 1u), 8u) * bytesPerElement;
        } else {
            faulted = true;
        }
        return result;
    }
    uint widthElements0 = (width + 3u) >> 2;
    uint heightElements0 = (height + 3u) >> 2;
    result.widthElements = max(shiftCeil(widthElements0, level), 1u);
    if (swizzle == 0u) {
        uint pitchAlign = 256u / bytesPerElement;
        for (uint j = level + 1u; j < levelCount; ++j) {
            result.offset += alignUp(max(shiftCeil(widthElements0, j), 1u), pitchAlign) * max(shiftCeil(heightElements0, j), 1u) * bytesPerElement;
        }
        return result;
    }
    uint widthScale = bytesPerElement == 8u ? 2u : 1u;
    uint blockSize;
    uint blockWidth;
    uint blockHeight;
    uint tailLength = 0u;
    if (swizzle == 1u) {
        blockSize = 256u; blockWidth = 4u * widthScale; blockHeight = 4u;
    } else if (swizzle == 5u) {
        blockSize = 4096u; blockWidth = 16u * widthScale; blockHeight = 16u; tailLength = 8u;
    } else if (swizzle == 9u) {
        blockSize = 65536u; blockWidth = 64u * widthScale; blockHeight = 64u; tailLength = 12u;
    } else {
        faulted = true;
        return result;
    }
    uint firstTail = levelCount;
    if (tailLength != 0u && levelCount > 1u) {
        for (uint j = 0u; j < levelCount; ++j) {
            if (shiftCeil(widthElements0, j) <= blockWidth / 2u && shiftCeil(heightElements0, j) <= blockHeight && levelCount - j <= tailLength) {
                firstTail = j;
                break;
            }
        }
    }
    if (level >= firstTail) {
        uint index = level - firstTail;
        uint entry;
        if (bytesPerElement == 8u) entry = swizzle == 5u ? Tail4KB8[index] : Tail64KB8[index];
        else entry = swizzle == 5u ? Tail4KB16[index] : Tail64KB16[index];
        result.tailX = entry & 0xffu;
        result.tailY = entry >> 8;
        return result;
    }
    result.offset = firstTail < levelCount ? blockSize : 0u;
    for (uint j = level + 1u; j < firstTail; ++j) {
        result.offset += alignUp(max(shiftCeil(widthElements0, j), 1u), blockWidth) * alignUp(max(shiftCeil(heightElements0, j), 1u), blockHeight) * bytesPerElement;
    }
    return result;
}

uint bytesPerElementOf(uint format) {
    if (format == 56u || format == 22u) return 4u;
    if (format == 7u) return 2u;
    if (format == 169u || format == 170u || format == 175u) return 8u;
    return 16u;
}

uint bc4Value64(uvec2 block, uint pixel) {
    uint r0 = block.x & 0xffu;
    uint r1 = (block.x >> 8) & 0xffu;
    uint64_t bits = (uint64_t(block.y) << 32) | uint64_t(block.x);
    uint index = uint(bits >> (16u + pixel * 3u)) & 7u;
    uint weight;
    if (r0 > r1) {
        weight = Bc4Weights7[index];
    } else {
        if (index == 6u) return 0u;
        if (index == 7u) return 255u * 64u;
        weight = Bc4Weights5[index];
    }
    return r0 * (64u - weight) + r1 * weight;
}

uvec3 rgb565(uint value) {
    uint r = value >> 11;
    uint g = (value >> 5) & 63u;
    uint b = value & 31u;
    return uvec3((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

uvec4 decodeBc1Texel(uvec2 block, uint pixel) {
    uint c0 = block.x & 0xffffu;
    uint c1 = block.x >> 16;
    uvec3 e0 = rgb565(c0);
    uvec3 e1 = rgb565(c1);
    uint index = (block.y >> (2u * pixel)) & 3u;
    uint weight;
    if (c0 > c1) {
        weight = index == 0u ? 0u : (index == 1u ? 64u : (index == 2u ? 21u : 43u));
    } else {
        if (index == 3u) return uvec4(0u);
        weight = index == 0u ? 0u : (index == 1u ? 64u : 32u);
    }
    return uvec4((e0 * (64u - weight) + e1 * weight + 32u) >> 6, 255u);
}

uint bc6hCode5(uint code) {
    switch (code) {
        case 2u: return 2u;
        case 6u: return 3u;
        case 10u: return 4u;
        case 14u: return 5u;
        case 18u: return 6u;
        case 22u: return 7u;
        case 26u: return 8u;
        case 30u: return 9u;
        case 3u: return 10u;
        case 7u: return 11u;
        case 11u: return 12u;
        case 15u: return 13u;
    }
    return 14u;
}

uint bc6hUnquantize(uint value, uint bits) {
    if (bits >= 15u) return value;
    if (value == 0u) return 0u;
    if (value == (1u << bits) - 1u) return 0xffffu;
    return ((value << 15) + 0x4000u) >> (bits - 1u);
}

uint halfToFloatBits(uint halfBits) {
    uint exponent = (halfBits >> 10) & 31u;
    uint mantissa = halfBits & 1023u;
    if (exponent == 0u) {
        if (mantissa == 0u) return 0u;
        int top = findMSB(mantissa);
        return (uint(top - 24 + 127) << 23) | ((mantissa << uint(23 - top)) & 0x7fffffu);
    }
    return ((exponent - 15u + 127u) << 23) | (mantissa << 13);
}

uvec4 decodeBc6hTexel(uvec4 block, uint pixel) {
    uint mode = (block.x & 3u) < 2u ? (block.x & 3u) : bc6hCode5(block.x & 31u);
    if (mode >= 14u) return uvec4(0u);
    uint desc = Bc6hModes[mode];
    uint regions = desc & 3u;
    bool transformed = ((desc >> 2) & 1u) != 0u;
    uint endpointBits = (desc >> 3) & 31u;
    uvec3 deltaBits = uvec3((desc >> 8) & 15u, (desc >> 12) & 15u, (desc >> 16) & 15u);
    uint start = desc >> 20;
    uint position = (block.x & 3u) < 2u ? 2u : 5u;
    uint fields[12] = uint[](0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    for (uint i = 0u; i < Bc6hFieldCounts[mode]; ++i) {
        uint entryIndex = start + i;
        uint entry = (Bc6hLayout[entryIndex >> 2] >> ((entryIndex & 3u) * 8u)) & 0xffu;
        fields[entry >> 4] |= blockBits(block, position, 1u) << (entry & 15u);
        position += 1u;
    }
    uint partitionIndex = 0u;
    if (regions == 2u) {
        partitionIndex = blockBits(block, position, 5u);
        position += 5u;
    }
    uint endpointCount = regions * 2u;
    uint mask = (1u << endpointBits) - 1u;
    if (transformed) {
        for (uint e = 1u; e < endpointCount; ++e) {
            for (uint c = 0u; c < 3u; ++c) {
                uint bits = deltaBits[c];
                int delta = int(fields[e * 3u + c] << (32u - bits)) >> (32u - bits);
                fields[e * 3u + c] = uint(int(fields[c]) + delta) & mask;
            }
        }
    }
    uint indexBits = regions == 2u ? 3u : 4u;
    uint anchor = regions == 2u ? Bc7Anchor2[partitionIndex] : 16u;
    uint subset = regions == 2u ? (Bc7Partition2[partitionIndex] >> pixel) & 1u : 0u;
    uint indexOffset = 0u;
    for (uint p = 0u; p < pixel; ++p) indexOffset += (p == 0u || p == anchor) ? indexBits - 1u : indexBits;
    uint index = blockBits(block, position + indexOffset, (pixel == 0u || pixel == anchor) ? indexBits - 1u : indexBits);
    uint weight = indexBits == 3u ? Bc7Weights3[index] : Bc7Weights4[index];
    uvec4 texel;
    for (uint c = 0u; c < 3u; ++c) {
        uint a = bc6hUnquantize(fields[subset * 6u + c], endpointBits);
        uint b = bc6hUnquantize(fields[subset * 6u + 3u + c], endpointBits);
        uint interpolated = (a * (64u - weight) + b * weight + 32u) >> 6;
        texel[c] = halfToFloatBits((interpolated * 31u) >> 6);
    }
    texel.w = texel.x;
    return texel;
}

uvec4 fetchTexel(uint64_t base, int x, int y, uint format, Level level, uint swizzle) {
    if (format == 22u || format == 7u) {
        uint elementOffset = level.offset + elementByte(uint(x), uint(y), level.widthElements, bytesPerElementOf(format), swizzle);
        uint word = guestLoad32(base + uint64_t(elementOffset & ~3u));
        if (format == 7u) word = (elementOffset & 2u) != 0u ? word >> 16 : word & 0xffffu;
        return uvec4(word);
    }
    if (format == 56u) {
        uint word = guestLoad32(base + uint64_t(elementByte(uint(x), uint(y), level.width, 4u, swizzle)));
        return uvec4(word & 0xffu, (word >> 8) & 0xffu, (word >> 16) & 0xffu, word >> 24);
    }
    uint bytesPerElement = bytesPerElementOf(format);
    uint byteOffset = level.offset + elementByte((uint(x) >> 2) + level.tailX, (uint(y) >> 2) + level.tailY, level.widthElements, bytesPerElement, swizzle);
    uint64_t first = base + uint64_t(byteOffset);
    uint pixel = (uint(y) & 3u) * 4u + (uint(x) & 3u);
    if (bytesPerElement == 8u) {
        uvec2 half0 = uvec2(guestLoad32(first), guestLoad32(first + uint64_t(4)));
        if (format == 175u) return uvec4(bc4Value64(half0, pixel));
        return decodeBc1Texel(half0, pixel);
    }
    uvec4 block = uvec4(guestLoad32(first), guestLoad32(first + uint64_t(4)), guestLoad32(first + uint64_t(8)), guestLoad32(first + uint64_t(12)));
    if (format == 177u) {
        uint red = bc4Value64(block.xy, pixel);
        uint green = bc4Value64(block.zw, pixel);
        return uvec4(red, green, red, green);
    }
    if (format == 179u) return decodeBc6hTexel(block, pixel);
    return decodeBc7Texel(block, pixel);
}
const uint Float32GridBits = 19u;

uint fixedToFloatTiesAway(uint64_t sum, int fractionBits) {
    if (sum == uint64_t(0)) return 0u;
    int leading = msb64(sum);
    uint mantissa;
    if (leading > 23) {
        uint shift = uint(leading - 23);
        uint64_t quotient = sum >> shift;
        uint64_t remainder = sum & ((uint64_t(1) << shift) - uint64_t(1));
        if (remainder >= (uint64_t(1) << (shift - 1u))) quotient += uint64_t(1);
        if (quotient == (uint64_t(1) << 24)) {
            quotient >>= 1;
            leading += 1;
        }
        mantissa = uint(quotient);
    } else {
        mantissa = uint(sum << uint(23 - leading));
    }
    uint exponent = uint(leading - fractionBits + 127);
    return (exponent << 23) | (mantissa & 0x7fffffu);
}

uint unormFilteredToFloat(uint filtered) {
    uint64_t r = uint64_t(filtered);
    return fixedToFloatTiesAway(((r << 16) & ~uint64_t(7)) + ((r << 8) & ~uint64_t(7)) + (r & ~uint64_t(7)) + ((r >> 8) & ~uint64_t(7)) + ((r >> 16) & ~uint64_t(7)), 37);
}

uint unorm16FilteredToFloat(uint64_t r) {
    return fixedToFloatTiesAway(((r << 8) & ~uint64_t(7)) + ((r >> 8) & ~uint64_t(7)) + ((r >> 24) & ~uint64_t(7)), 37);
}

uint srgbAlphaFilter(uint texels[8], uint tapWeights[8], uint tapLevels[8], uint tapCount, uint levelWeights[2]) {
    uint64_t levelSums[2] = uint64_t[](uint64_t(0), uint64_t(0));
    for (uint i = 0u; i < tapCount; ++i) levelSums[tapLevels[i]] += uint64_t(texels[i] * 257u) * uint64_t(tapWeights[i]);
    uint64_t blended = RoundU64(levelSums[0] * uint64_t(levelWeights[0]), 5) + RoundU64(levelSums[1] * uint64_t(levelWeights[1]), 5);
    return unorm16FilteredToFloat(RoundU64(blended, 6));
}

uint unorm16Filter(uint texels[8], uint tapWeights[8], uint tapCount) {
    uint64_t sum = uint64_t(0);
    for (uint i = 0u; i < tapCount; ++i) sum += uint64_t(texels[i]) * uint64_t(tapWeights[i]);
    return unorm16FilteredToFloat(RoundU64(sum, 3));
}

uint float32Point(uint value) {
    uint biased = (value >> 23) & 0xffu;
    if (biased == 0xffu && (value & 0x7fffffu) != 0u) return 0xffc00000u;
    if (biased == 0u) return value & 0x80000000u;
    return value;
}

uint unormFilter(uint texels[8], uint tapWeights[8], uint tapLevels[8], uint tapCount, uint levelWeights[2]) {
    uint64_t levelSums[2] = uint64_t[](uint64_t(0), uint64_t(0));
    for (uint i = 0u; i < tapCount; ++i) levelSums[tapLevels[i]] += uint64_t(texels[i] * tapWeights[i]);
    uint64_t blended = RoundU64(levelSums[0] * uint64_t(levelWeights[0]), 5) + RoundU64(levelSums[1] * uint64_t(levelWeights[1]), 5);
    return unormFilteredToFloat(uint(RoundU64(blended, 6)));
}

uint bc45Filter(uint texels[8], uint tapWeights[8], uint tapLevels[8], uint tapCount, uint levelWeights[2]) {
    uint64_t sum = uint64_t(0);
    for (uint i = 0u; i < tapCount; ++i) sum += uint64_t(texels[i]) * uint64_t(tapWeights[i]) * uint64_t(levelWeights[tapLevels[i]]);
    return unormFilteredToFloat(uint((sum + uint64_t(256)) >> 17));
}

uint wideBit(uint64_t high, uint64_t low, int index) {
    if (index < 0) return 0u;
    if (index >= 64) return uint((high >> uint(index - 64)) & uint64_t(1));
    return uint((low >> uint(index)) & uint64_t(1));
}

bool wideAnyBelow(uint64_t high, uint64_t low, int index) {
    if (index <= 0) return false;
    if (index >= 64) return low != uint64_t(0) || (index > 64 && (high & ((uint64_t(1) << uint(min(index - 64, 63))) - uint64_t(1))) != uint64_t(0));
    return (low & ((uint64_t(1) << uint(index)) - uint64_t(1))) != uint64_t(0);
}

uvec2 wideShiftRight(uint64_t high, uint64_t low, int shift) {
    if (shift <= 0) return unpackUint2x32(low << uint(-shift));
    if (shift >= 64) return unpackUint2x32(high >> uint(shift - 64));
    return unpackUint2x32((low >> uint(shift)) | (high << uint(64 - shift)));
}

uint roundFilterSum(uint64_t high, uint64_t low, bool negative, int unitExponent) {
    int top = high != uint64_t(0) ? 64 + msb64(high) : msb64(low);
    int keep = top - 34;
    if (keep >= 64) {
        low = uint64_t(0);
        high &= ~((uint64_t(1) << uint(keep - 64)) - uint64_t(1));
    } else if (keep > 0) {
        low &= ~((uint64_t(1) << uint(keep)) - uint64_t(1));
    }
    int firstShift = top - 28;
    uint64_t wide = WideShift64(high, low, firstShift);
    if (firstShift > 0 && wideBit(high, low, firstShift - 1) != 0u && (wideAnyBelow(high, low, firstShift - 1) || (wide & uint64_t(1)) != uint64_t(0))) wide += uint64_t(1);
    uint64_t mantissa = RoundU64(wide, 5);
    int exponent = top + unitExponent;
    if (mantissa >= (uint64_t(1) << 24)) {
        mantissa >>= 1;
        exponent += 1;
    }
    if (exponent < -126 || exponent > 127) {
        faulted = true;
        return FaultCode;
    }
    return (negative ? 0x80000000u : 0u) | (uint(exponent + 127) << 23) | (uint(mantissa) & 0x7fffffu);
}

uint float32Filter(uint values[8], uint weights[8], uint tapCount, bool singleTexel) {
    int largest = -1000;
    bool allNegativeZero = true;
    for (uint i = 0u; i < tapCount; ++i) {
        if (weights[i] == 0u) continue;
        uint biased = (values[i] >> 23) & 0xffu;
        if (biased == 0xffu || (biased == 0u && (values[i] & 0x7fffffu) != 0u)) {
            faulted = true;
            return FaultCode;
        }
        if (values[i] != 0x80000000u) allNegativeZero = false;
        if (biased != 0u) largest = max(largest, int(biased) - 127);
    }
    if (largest == -1000) return allNegativeZero && singleTexel ? 0x80000000u : 0u;
    int64_t sum = int64_t(0);
    for (uint i = 0u; i < tapCount; ++i) {
        uint biased = (values[i] >> 23) & 0xffu;
        if (weights[i] == 0u || biased == 0u) continue;
        uint shift = uint(largest - (int(biased) - 127));
        if (shift >= 24u + Float32GridBits) continue;
        int64_t gridValue = int64_t((uint64_t((values[i] & 0x7fffffu) | 0x800000u) << Float32GridBits) >> shift);
        if ((values[i] & 0x80000000u) != 0u) gridValue = -gridValue;
        sum += gridValue * int64_t(weights[i]);
    }
    if (sum == int64_t(0)) return 0u;
    bool negative = sum < int64_t(0);
    uint64_t magnitude = uint64_t(negative ? -sum : sum);
    int keep = msb64(magnitude) - 34;
    if (negative && keep > 0 && (magnitude & ((uint64_t(1) << uint(keep)) - uint64_t(1))) != uint64_t(0)) magnitude = (magnitude & ~((uint64_t(1) << uint(keep)) - uint64_t(1))) + (uint64_t(1) << uint(keep));
    return roundFilterSum(uint64_t(0), magnitude, negative, largest - 39 - int(Float32GridBits));
}

uint floatFilter(uint values[8], uint weights[8], uint tapLevels[8], uint tapCount) {
    int largest[2] = int[](-1000, -1000);
    bool allNegativeZero = true;
    for (uint i = 0u; i < tapCount; ++i) {
        if (weights[i] == 0u) continue;
        uint biased = (values[i] >> 23) & 0xffu;
        if (biased == 0xffu || (biased == 0u && (values[i] & 0x7fffffu) != 0u)) {
            faulted = true;
            return FaultCode;
        }
        if (values[i] != 0x80000000u) allNegativeZero = false;
        if (biased != 0u) largest[tapLevels[i]] = max(largest[tapLevels[i]], int(biased) - 127);
    }
    if (largest[0] == -1000 && largest[1] == -1000) return allNegativeZero ? 0x80000000u : 0u;
    int64_t levelSums[2] = int64_t[](int64_t(0), int64_t(0));
    bool truncatedToZero = false;
    for (uint i = 0u; i < tapCount; ++i) {
        if (weights[i] == 0u) continue;
        uint biased = (values[i] >> 23) & 0xffu;
        if (biased == 0u) continue;
        uint shift = uint(largest[tapLevels[i]] - (int(biased) - 127) + 3);
        uint mantissa = (values[i] & 0x7fffffu) | 0x800000u;
        int64_t gridValue = shift >= 32u ? int64_t(0) : int64_t(mantissa >> shift);
        if (gridValue == int64_t(0)) truncatedToZero = true;
        if ((values[i] & 0x80000000u) != 0u) gridValue = -gridValue;
        levelSums[tapLevels[i]] += gridValue * int64_t(weights[i]);
    }
    uint upper = largest[1] > largest[0] ? 1u : 0u;
    int lowest = largest[1 - upper] == -1000 ? largest[upper] : largest[1 - upper];
    int gap = largest[upper] - lowest;
    uint64_t high = uint64_t(0);
    uint64_t low = uint64_t(0);
    bool negative = false;
    if (gap <= 15) {
        int64_t sum = (levelSums[upper] << uint(gap)) + levelSums[1 - upper];
        negative = sum < int64_t(0);
        low = uint64_t(negative ? -sum : sum);
    } else {
        if (levelSums[0] < int64_t(0) || levelSums[1] < int64_t(0)) {
            faulted = true;
            return FaultCode;
        }
        uint64_t upperSum = uint64_t(levelSums[upper]);
        high = gap >= 64 ? upperSum << uint(gap - 64) : upperSum >> uint(64 - gap);
        low = gap >= 64 ? uint64_t(0) : upperSum << uint(gap);
        uint64_t added = low + uint64_t(levelSums[1 - upper]);
        if (added < low) high += uint64_t(1);
        low = added;
    }
    if (high == uint64_t(0) && low == uint64_t(0)) {
        if (truncatedToZero) faulted = true;
        return 0u;
    }
    return roundFilterSum(high, low, negative, lowest - 44);
}

uint srgbAnisoFilter(uint packed[16], uint fractions[4], uint levelSlots, uint levelWeights[2], uint channel) {
    int largest[2] = int[](-1000, -1000);
    for (uint i = 0u; i < levelSlots * 8u; ++i) {
        uint value = SrgbToLinear[(packed[i] >> (channel * 8u)) & 0xffu];
        uint biased = (value >> 23) & 0xffu;
        if (biased != 0u) largest[i >> 3] = max(largest[i >> 3], int(biased) - 127);
    }
    if (largest[0] == -1000 && largest[1] == -1000) return 0u;
    int64_t levelSums[2] = int64_t[](int64_t(0), int64_t(0));
    for (uint i = 0u; i < levelSlots * 8u; ++i) {
        uint value = SrgbToLinear[(packed[i] >> (channel * 8u)) & 0xffu];
        uint biased = (value >> 23) & 0xffu;
        if (biased == 0u) continue;
        uint fraction = fractions[i >> 2];
        uint wx = fraction & 0xffu;
        uint wy = fraction >> 8;
        uint weight = ((i & 1u) != 0u ? wx : 256u - wx) * ((i & 2u) != 0u ? wy : 256u - wy) * levelWeights[i >> 3];
        if (weight == 0u) continue;
        uint shift = uint(largest[i >> 3] - (int(biased) - 127) + 3);
        uint mantissa = (value & 0x7fffffu) | 0x800000u;
        levelSums[i >> 3] += int64_t(shift >= 32u ? 0u : mantissa >> shift) * int64_t(weight);
    }
    uint upper = largest[1] > largest[0] ? 1u : 0u;
    int lowest = largest[1 - upper] == -1000 ? largest[upper] : largest[1 - upper];
    int gap = largest[upper] - lowest;
    uint64_t high = uint64_t(0);
    uint64_t low = uint64_t(0);
    if (gap <= 15) {
        low = uint64_t((levelSums[upper] << uint(gap)) + levelSums[1 - upper]);
    } else {
        uint64_t upperSum = uint64_t(levelSums[upper]);
        high = gap >= 64 ? upperSum << uint(gap - 64) : upperSum >> uint(64 - gap);
        low = gap >= 64 ? uint64_t(0) : upperSum << uint(gap);
        uint64_t added = low + uint64_t(levelSums[1 - upper]);
        if (added < low) high += uint64_t(1);
        low = added;
    }
    if (high == uint64_t(0) && low == uint64_t(0)) return 0u;
    return roundFilterSum(high, low, false, lowest - 45);
}

uint selectChannel(uint selector, uvec4 value) {
    switch (selector) {
        case 0u: return 0u;
        case 1u: return 0x3f800000u;
        case 4u: return value.x;
        case 5u: return value.y;
        case 6u: return value.z;
        case 7u: return value.w;
    }
    faulted = true;
    return FaultCode;
}

const int AbsentExponent = -100000;
const ivec2 LengthLines[5] = ivec2[](ivec2(1024, 64), ivec2(1000, 224), ivec2(952, 384), ivec2(884, 520), ivec2(788, 656));

bool decodeDerivative(uint bits, out uint mantissa, out int exponent) {
    uint biased = (bits >> 23) & 0xffu;
    mantissa = 0u;
    exponent = AbsentExponent;
    if (biased == 0xffu || (biased == 0u && (bits & 0x7fffffu) != 0u)) {
        faulted = true;
        return false;
    }
    if (biased == 0u) return false;
    mantissa = (bits & 0x7fffffu) | 0x800000u;
    exponent = int(biased) - 127;
    return true;
}

int alignDerivative(uint bits, uint mantissa, int shift) {
    uint magnitude = shift >= 32 ? 0u : min((mantissa + (1u << uint(shift - 1))) >> uint(shift), 2047u);
    return (bits >> 31) != 0u ? -int(magnitude) : int(magnitude);
}

int roundSignedHalfUp(int value, int shift) {
    if (shift <= 0) return value;
    if (shift > 30) return 0;
    return (value + (1 << (shift - 1))) >> shift;
}

int sizeSignificand(uint size, out int log2Size) {
    log2Size = findMSB(size);
    uint fraction = log2Size <= 10 ? ((size << 10) >> uint(log2Size)) & 1023u : (size >> uint(log2Size - 10)) & 1023u;
    return int(1024u + fraction);
}

uint polygonLength(int x, int y) {
    int major = max(abs(x), abs(y));
    int minor = min(abs(x), abs(y));
    int best = 0;
    for (uint i = 0u; i < 5u; ++i) best = max(best, (LengthLines[i].x * major + LengthLines[i].y * minor + 512) >> 10);
    return uint(best);
}

uint derivativeLod(uvec4 derivatives, uint width, uint height, uint lastLevel, int bias) {
    uint mantissas[4];
    int exponents[4];
    bool present[4];
    for (uint i = 0u; i < 4u; ++i) present[i] = decodeDerivative(derivatives[i], mantissas[i], exponents[i]);
    if (faulted) return 0u;
    int sizeLog2[2];
    int sizeSignificands[2] = int[](sizeSignificand(width, sizeLog2[0]), sizeSignificand(height, sizeLog2[1]));
    int pairExponents[2] = int[](AbsentExponent, AbsentExponent);
    for (uint i = 0u; i < 4u; ++i) {
        if (present[i]) pairExponents[i & 1u] = max(pairExponents[i & 1u], exponents[i]);
    }
    int pairScales[2];
    for (uint pair = 0u; pair < 2u; ++pair) pairScales[pair] = pairExponents[pair] == AbsentExponent ? AbsentExponent : pairExponents[pair] + sizeLog2[pair];
    int scale = max(pairScales[0], pairScales[1]);
    if (scale == AbsentExponent) {
        faulted = true;
        return 0u;
    }
    int products[4];
    for (uint i = 0u; i < 4u; ++i) {
        products[i] = 0;
        if (!present[i]) continue;
        uint pair = i & 1u;
        int aligned = alignDerivative(derivatives[i], mantissas[i], 13 + pairExponents[pair] - exponents[i]);
        int product = (aligned * sizeSignificands[pair] + 512) >> 10;
        products[i] = roundSignedHalfUp(product, scale - pairScales[pair]);
    }
    uint first = polygonLength(products[0] + products[3], products[2] - products[1]);
    uint second = polygonLength(products[0] - products[3], products[2] + products[1]);
    uint t = ((first + second) >> 1) << 1;
    if (t < 2048u || t >= 16384u) {
        faulted = true;
        return 0u;
    }
    uint entry = (DerivativeLodByT[(t - 2048u) >> 2] >> (((t >> 1) & 1u) * 16u)) & 0xffffu;
    if (entry == 0xffffu) {
        faulted = true;
        return 0u;
    }
    return uint(clamp(256 * scale + int(entry) + bias, 0, int(lastLevel) * 256));
}

bool derivativeProducts(uvec4 derivatives, uint width, uint height, out int products[4], out int scale) {
    uint mantissas[4];
    int exponents[4];
    bool present[4];
    for (uint i = 0u; i < 4u; ++i) {
        products[i] = 0;
        present[i] = decodeDerivative(derivatives[i], mantissas[i], exponents[i]);
    }
    scale = AbsentExponent;
    if (faulted) return false;
    int sizeLog2[2];
    int sizeSignificands[2] = int[](sizeSignificand(width, sizeLog2[0]), sizeSignificand(height, sizeLog2[1]));
    int pairExponents[2] = int[](AbsentExponent, AbsentExponent);
    for (uint i = 0u; i < 4u; ++i) {
        if (present[i]) pairExponents[i & 1u] = max(pairExponents[i & 1u], exponents[i]);
    }
    int pairScales[2];
    for (uint pair = 0u; pair < 2u; ++pair) pairScales[pair] = pairExponents[pair] == AbsentExponent ? AbsentExponent : pairExponents[pair] + sizeLog2[pair];
    scale = max(pairScales[0], pairScales[1]);
    if (scale == AbsentExponent) return false;
    for (uint i = 0u; i < 4u; ++i) {
        if (!present[i]) continue;
        uint pair = i & 1u;
        int aligned = alignDerivative(derivatives[i], mantissas[i], 13 + pairExponents[pair] - exponents[i]);
        int product = (aligned * sizeSignificands[pair] + 512) >> 10;
        products[i] = roundSignedHalfUp(product, scale - pairScales[pair]);
    }
    return true;
}

int lodOfLength(uint t, int scale) {
    int shift = 0;
    while (t < 2048u) {
        t <<= 1;
        shift += 1;
    }
    while (t >= 16384u) {
        t >>= 1;
        shift -= 1;
    }
    t = (t >> 1) << 1;
    uint entry = (DerivativeLodByT[(t - 2048u) >> 2] >> (((t >> 1) & 1u) * 16u)) & 0xffffu;
    if (entry == 0xffffu) faulted = true;
    return 256 * (scale - shift) + int(entry);
}

int halfUpShift(int value, int shift) {
    return (value + (1 << (shift - 1))) >> shift;
}

int roundedShift(int value, int shift, bool down) {
    return down ? -((-value + (1 << (shift - 1))) >> shift) : halfUpShift(value, shift);
}

int anisoReciprocal(uint length) {
    uint index = length - 1024u;
    return int((AnisoReciprocal[index >> 1] >> ((index & 1u) * 16u)) & 0xffffu);
}

ivec2 scaledUnit(ivec2 z, int reciprocal, int shift, bool down) {
    return ivec2(halfUpShift(z.x * reciprocal, shift), roundedShift(z.y * reciprocal, shift, down));
}

ivec2 halvedUnit(ivec2 u, int shift, bool down) {
    if (shift == 0) return u;
    return ivec2(halfUpShift(u.x, shift), roundedShift(u.y, shift, down));
}

uint doublingsDown(uint length) {
    return uint(max(findMSB(length) - 10, 0));
}

uint doublingsUp(uint length) {
    return uint(max(10 - findMSB(length), 0));
}

ivec2 longUnit(ivec2 z, uint length, uint extra, bool down) {
    uint k = doublingsDown(length);
    return halvedUnit(scaledUnit(z, anisoReciprocal(length >> k), 10, down), int(k + extra), down);
}

uint polygonRaw(int x, int y) {
    int major = max(abs(x), abs(y));
    int minor = min(abs(x), abs(y));
    int best = 0;
    for (uint i = 0u; i < 5u; ++i) best = max(best, LengthLines[i].x * major + LengthLines[i].y * minor);
    return uint(best);
}

uint truncateMantissa11(uint value) {
    if (value == 0u) return 0u;
    int top = findMSB(value);
    return top > 10 ? value & ~((1u << uint(top - 10)) - 1u) : value;
}

uvec2 anisoTapVector(ivec2 z1, ivec2 z2, uint length1, uint length2) {
    uint shortest = min(length1, length2);
    if (shortest == 0u) {
        faulted = true;
        return uvec2(0u);
    }
    ivec2 n1;
    ivec2 n2;
    if (shortest < 1024u) {
        uint shift = doublingsUp(shortest);
        n1 = length1 < 1024u ? scaledUnit(z1, anisoReciprocal(length1 << shift), 10, true) : longUnit(z1, length1, shift, true);
        n2 = length2 < 1024u ? scaledUnit(z2, anisoReciprocal(length2 << shift), 10, false) : longUnit(z2, length2, shift, false);
        uint factor = 2u << shift;
        return uvec2(factor * polygonLength(n1.x + n2.x, n1.y - n2.y), factor * polygonLength(n1.x - n2.x, n1.y + n2.y));
    }
    if (length1 >= 2048u && length2 >= 2048u) {
        n1 = halvedUnit(scaledUnit(z1, anisoReciprocal(length1 >> doublingsDown(length1)), 10, true), int(doublingsDown(length1)) - 1, true);
        n2 = halvedUnit(scaledUnit(z2, anisoReciprocal(length2 >> doublingsDown(length2)), 10, false), int(doublingsDown(length2)) - 1, false);
        return uvec2(truncateMantissa11((polygonRaw(n1.x + n2.x, n1.y - n2.y) + 512u) >> 10), truncateMantissa11((polygonRaw(n1.x - n2.x, n1.y + n2.y) + 512u) >> 10));
    }
    n1 = longUnit(z1, length1, 0u, true);
    n2 = longUnit(z2, length2, 0u, false);
    return uvec2(2u * polygonLength(n1.x + n2.x, n1.y - n2.y), 2u * polygonLength(n1.x - n2.x, n1.y + n2.y));
}

ivec2 levelZeroCoordinate(uint bits, uint size) {
    uint exponent = (bits >> 23) & 0xffu;
    if (exponent == 0u) return unpackInt2x32(int64_t(0));
    int64_t mantissa = int64_t((bits & 0x7fffffu) | 0x800000u) * int64_t(size);
    if ((bits & 0x80000000u) != 0u) mantissa = -mantissa;
    int shift = 134 - int(exponent);
    if (shift <= 0) return unpackInt2x32(mantissa << uint(-shift));
    return roundHalfEvenShift(mantissa, uint(shift));
}

uint anisoScale(uint table[64], uint index) {
    return (table[index >> 2] >> ((index & 3u) * 8u)) & 0xffu;
}

uvec4 sampleGuest(uvec4 tlow, uvec4 thigh, uvec4 ssharp, uvec4 row, uvec4 derivatives, uvec4 memory, out uint fault) {
    faulted = false;
    guestOutside = false;
    fault = 1u;
    uint64_t base = (uint64_t(tlow.x) | (uint64_t(tlow.y & 0xffu) << 32)) << 8;
    guestOrigin = base;
    guestDevice = uint64_t(memory.x) | (uint64_t(memory.y) << 32);
    guestBytes = uint64_t(memory.z);
    uint t1 = tlow.y;
    uint t2 = tlow.z;
    uint t3 = tlow.w;
    uint format = (t1 >> 20) & 0x1ffu;
    uint width = (((t1 >> 30) & 3u) | ((t2 & 0xfffu) << 2)) + 1u;
    uint height = ((t2 >> 14) & 0x3fffu) + 1u;
    uint baseLevel = (t3 >> 12) & 0xfu;
    uint lastLevel = (t3 >> 16) & 0xfu;
    uint swizzle = (t3 >> 20) & 0x1fu;
    uint type = (t3 >> 28) & 0xfu;
    uint maxMip = (thigh.y >> 4) & 0xfu;
    uint textureMinLod = (t1 >> 8) & 0xfffu;
    uint clampX = ssharp.x & 7u;
    uint clampY = (ssharp.x >> 3) & 7u;
    uint anisoRatio = (ssharp.x >> 9) & 7u;
    uint samplerMinLod = ssharp.y & 0xfffu;
    uint samplerMaxLod = (ssharp.y >> 12) & 0xfffu;
    uint magFilter = (ssharp.z >> 20) & 3u;
    uint minFilter = (ssharp.z >> 22) & 3u;
    uint mipFilter = (ssharp.z >> 26) & 3u;
    bool knownFormat = format == 7u || format == 22u || format == 56u || format == 169u || format == 170u || format == 175u || format == 177u || format == 179u || format == 181u || format == 182u;
    bool srgb = format == 170u || format == 182u;
    bool anisoTaps = magFilter == 3u && minFilter == 3u && anisoRatio == 1u;
    bool supported = knownFormat && type == 9u && (clampX == 0u || clampX == 2u) && (clampY == 0u || clampY == 2u);
    supported = supported && (anisoTaps || (magFilter == minFilter && magFilter <= 1u && anisoRatio <= 1u));
    supported = supported && baseLevel == 0u && maxMip == lastLevel && max(textureMinLod, samplerMinLod) <= (lastLevel << 8);
    uint anisoThreshold = (ssharp.x >> 16) & 7u;
    supported = supported && ((ssharp.x >> 12) & ~0x70u) == 0u && (anisoThreshold == 0u || anisoThreshold == 7u) && (ssharp.z & 0xfffffu) == 0u;
    supported = supported && (lastLevel == 0u || (clampX == 2u && clampY == 2u && (mipFilter == 1u || mipFilter == 2u)));
    if (anisoTaps) {
        bool unormFormat = format == 169u || format == 181u || format == 182u;
        supported = supported && unormFormat && row.w == 1u && lastLevel != 0u && textureMinLod == 0u && samplerMinLod == 0u && samplerMaxLod >= (lastLevel << 8);
        supported = supported && bitCount(width) == 1u && bitCount(height) == 1u;
    }
    if (!supported) {
        return uvec4(0u);
    }
    if (row.w > 2u) return uvec4(0u);
    int bias = 0;
    if (row.w == 2u) {
        float value = uintBitsToFloat(row.z);
        if (isnan(value) || abs(value) >= 16.0) return uvec4(0u);
        bias = int(roundEven(value * 256.0));
    }
    uint quantizedLod;
    int tapLod = 0;
    uvec2 tapVector = uvec2(0u);
    int tapExponent = 0;
    bool tapSignPositive = true;
    if (anisoTaps) {
        int products[4];
        int scale;
        if (!derivativeProducts(derivatives, width, height, products, scale) || faulted) return uvec4(0u);
        ivec2 z1 = ivec2(products[0] + products[3], products[1] - products[2]);
        ivec2 z2 = ivec2(products[0] - products[3], products[1] + products[2]);
        uint length1 = polygonLength(z1.x, z1.y);
        uint length2 = polygonLength(z2.x, z2.y);
        int lmax = lodOfLength(length1 + length2, scale);
        uint gap = (uint(abs(int(length1) - int(length2))) >> 1) << 1;
        int lmin = gap == 0u ? 0 : max(lodOfLength(gap, scale), 0);
        if (faulted) return uvec4(0u);
        quantizedLod = uint(clamp(max(lmax - 256, lmin), 0, int(lastLevel) * 256));
        tapLod = max(max(lmax - 256, lmin), 0);
        if (lmax > lmin) {
            tapVector = anisoTapVector(z1, z2, length1, length2);
            tapExponent = lmax - lmin < 16 ? 1 : findMSB(uint(min(lmax - lmin, 128)));
            if (faulted) return uvec4(0u);
        }
        tapSignPositive = products[0] * products[1] + products[2] * products[3] > 0;
    } else if (row.w != 0u && lastLevel == 0u) {
        quantizedLod = 0u;
    } else if (row.w != 0u) {
        quantizedLod = derivativeLod(derivatives, width, height, lastLevel, bias);
        if (faulted) {
            return uvec4(0u);
        }
    } else {
        float lod = uintBitsToFloat(row.z);
        if (isnan(lod)) {
            return uvec4(0u);
        }
        quantizedLod = uint(roundEven(clamp(lod, 0.0, float(lastLevel)) * 256.0));
    }
    quantizedLod = max(max(textureMinLod, samplerMinLod), min(quantizedLod, min(samplerMaxLod, lastLevel << 8)));
    uint firstLevel = 0u;
    uint fraction = 0u;
    if (mipFilter == 1u) {
        firstLevel = min((quantizedLod + 128u) >> 8, lastLevel);
    } else if (mipFilter == 2u) {
        firstLevel = quantizedLod >> 8;
        fraction = quantizedLod & 255u;
        if (firstLevel >= lastLevel) {
            firstLevel = lastLevel;
            fraction = 0u;
        }
    }
    if ((format == 22u || format == 7u) && fraction != 0u) {
        return uvec4(0u);
    }
    uvec2 tapNumerator = uvec2(0u);
    if (anisoTaps) {
        int scaleIndex = (tapLod & ~3) - 256 * int(firstLevel);
        int scaleLevels = mipFilter == 1u ? (scaleIndex + 128) >> 8 : scaleIndex >> 8;
        uint scaleFraction = uint(scaleIndex - 256 * scaleLevels);
        uint tapScale = (mipFilter == 1u ? anisoScale(AnisoPointScale, (scaleFraction + 128u) & 255u) : 128u + anisoScale(AnisoTrilinearScale, scaleFraction)) << uint(scaleLevels);
        tapNumerator = uvec2(truncateMantissa11(tapVector.x * tapScale), truncateMantissa11(tapVector.y * tapScale));
    }
    uint levelWeights[2] = uint[](256u - fraction, fraction);
    uint levelSlots = fraction != 0u ? 2u : 1u;
    uvec4 tapTexels[8];
    bool singleTexel = true;
    uint tapWeights[8];
    uint tapLevels[8];
    uint tapCount = 0u;
    uvec4 anisoSums[2] = uvec4[](uvec4(0u), uvec4(0u));
    uint anisoPacked[16];
    uint anisoFractions[4];
    for (uint slot = 0u; slot < levelSlots; ++slot) {
        Level level = levelLayout(width, height, lastLevel + 1u, swizzle, bytesPerElementOf(format), firstLevel + slot);
        if (anisoTaps) {
            uint levelIndex = firstLevel + slot;
            int64_t centreX = (packInt2x32(levelZeroCoordinate(row.x, width)) << (16u - levelIndex)) - (int64_t(128) << 24);
            int64_t centreY = (packInt2x32(levelZeroCoordinate(row.y, height)) << (16u - levelIndex)) - (int64_t(128) << 24);
            uint offsetShift = uint(5 + tapExponent) - slot;
            int64_t offsetX = int64_t(tapNumerator.x) << offsetShift;
            int64_t offsetY = int64_t(tapNumerator.y) << offsetShift;
            if (!tapSignPositive) offsetY = -offsetY;
            for (uint side = 0u; side < 2u; ++side) {
                int64_t fx = (side == 0u ? centreX + offsetX : centreX - offsetX) + (int64_t(1) << 23);
                int64_t fy = (side == 0u ? centreY + offsetY : centreY - offsetY) + (int64_t(1) << 23);
                fx >>= 24;
                fy >>= 24;
                int x0 = int(fx >> 8);
                int y0 = int(fy >> 8);
                uint wx = uint(fx & int64_t(255));
                uint wy = uint(fy & int64_t(255));
                int xa = wrapIndex(x0, int(level.width), clampX);
                int xb = wrapIndex(x0 + 1, int(level.width), clampX);
                int ya = wrapIndex(y0, int(level.height), clampY);
                int yb = wrapIndex(y0 + 1, int(level.height), clampY);
                uint tap = slot * 2u + side;
                anisoFractions[tap] = wx | (wy << 8);
                for (uint corner = 0u; corner < 4u; ++corner) {
                    uvec4 texel = fetchTexel(base, (corner & 1u) != 0u ? xb : xa, (corner & 2u) != 0u ? yb : ya, format, level, swizzle);
                    anisoSums[slot] += texel * (((corner & 1u) != 0u ? wx : 256u - wx) * ((corner & 2u) != 0u ? wy : 256u - wy));
                    anisoPacked[tap * 4u + corner] = texel.x | (texel.y << 8) | (texel.z << 16) | (texel.w << 24);
                }
            }
            continue;
        }
        int64_t fx = packInt2x32(fixedTexelCoordinate(row.x, level.width));
        int64_t fy = packInt2x32(fixedTexelCoordinate(row.y, level.height));
        if (magFilter == 0u) {
            int x = wrapIndex(int(fx >> 8), int(level.width), clampX);
            int y = wrapIndex(int(fy >> 8), int(level.height), clampY);
            tapTexels[tapCount] = fetchTexel(base, x, y, format, level, swizzle);
            tapWeights[tapCount] = 65536u;
            tapLevels[tapCount] = slot;
            tapCount += 1u;
            continue;
        }
        fx -= int64_t(128);
        fy -= int64_t(128);
        int x0 = int(fx >> 8);
        int y0 = int(fy >> 8);
        uint wx = uint(fx & int64_t(255));
        uint wy = uint(fy & int64_t(255));
        int xa = wrapIndex(x0, int(level.width), clampX);
        int xb = wrapIndex(x0 + 1, int(level.width), clampX);
        int ya = wrapIndex(y0, int(level.height), clampY);
        int yb = wrapIndex(y0 + 1, int(level.height), clampY);
        tapTexels[tapCount] = fetchTexel(base, xa, ya, format, level, swizzle);
        tapTexels[tapCount + 1u] = fetchTexel(base, xb, ya, format, level, swizzle);
        tapTexels[tapCount + 2u] = fetchTexel(base, xa, yb, format, level, swizzle);
        tapTexels[tapCount + 3u] = fetchTexel(base, xb, yb, format, level, swizzle);
        singleTexel = singleTexel && (xa == xb || wx == 0u) && (ya == yb || wy == 0u);
        tapWeights[tapCount] = (256u - wx) * (256u - wy);
        tapWeights[tapCount + 1u] = wx * (256u - wy);
        tapWeights[tapCount + 2u] = (256u - wx) * wy;
        tapWeights[tapCount + 3u] = wx * wy;
        for (uint i = 0u; i < 4u; ++i) tapLevels[tapCount + i] = slot;
        tapCount += 4u;
    }
    uvec4 value;
    for (uint c = 0u; c < 4u; ++c) {
        if (anisoTaps) {
            if (srgb && c < 3u) {
                value[c] = srgbAnisoFilter(anisoPacked, anisoFractions, levelSlots, levelWeights, c);
                continue;
            }
            uint scale = srgb ? 257u : 1u;
            uint64_t blended = RoundU64(uint64_t(anisoSums[0][c]) * uint64_t(scale) * uint64_t(levelWeights[0]), 5) + RoundU64(uint64_t(anisoSums[1][c]) * uint64_t(scale) * uint64_t(levelWeights[1]), 5);
            uint64_t filtered = tapVector == uvec2(0u) ? RoundU64(blended, 7) : (blended + uint64_t(32)) >> 7;
            value[c] = srgb ? unorm16FilteredToFloat(filtered) : unormFilteredToFloat(uint(filtered));
            continue;
        }
        uint texels[8];
        for (uint i = 0u; i < 8u; ++i) texels[i] = i < tapCount ? tapTexels[i][c] : 0u;
        if (format == 22u && magFilter == 0u) {
            value[c] = float32Point(texels[0]);
        } else if (format == 7u) {
            value[c] = unorm16Filter(texels, tapWeights, tapCount);
        } else if (format == 22u) {
            value[c] = float32Filter(texels, tapWeights, tapCount, singleTexel);
        } else if (format == 179u) {
            uint values[8];
            uint weights[8];
            for (uint i = 0u; i < 8u; ++i) {
                values[i] = i < tapCount ? texels[i] : 0u;
                weights[i] = i < tapCount ? tapWeights[i] * levelWeights[tapLevels[i]] : 0u;
            }
            value[c] = floatFilter(values, weights, tapLevels, tapCount);
        } else if (srgb && c < 3u) {
            uint values[8];
            uint weights[8];
            for (uint i = 0u; i < 8u; ++i) {
                values[i] = i < tapCount ? SrgbToLinear[texels[i]] : 0u;
                weights[i] = i < tapCount ? tapWeights[i] * levelWeights[tapLevels[i]] : 0u;
            }
            value[c] = floatFilter(values, weights, tapLevels, tapCount);
        } else if (srgb) {
            value[c] = srgbAlphaFilter(texels, tapWeights, tapLevels, tapCount, levelWeights);
        } else {
            value[c] = (format == 175u || format == 177u) ? bc45Filter(texels, tapWeights, tapLevels, tapCount, levelWeights) : unormFilter(texels, tapWeights, tapLevels, tapCount, levelWeights);
        }
    }
    uvec4 selected = uvec4(selectChannel(t3 & 7u, value), selectChannel((t3 >> 3) & 7u, value), selectChannel((t3 >> 6) & 7u, value), selectChannel((t3 >> 9) & 7u, value));
    if (faulted || guestOutside) return uvec4(0u);
    fault = 0u;
    return selected;
}

#ifndef GUEST_SAMPLE_CHECK
void main() {
}
#endif
