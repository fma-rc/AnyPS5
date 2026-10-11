#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Words = 8;
alignas(256) std::array<std::uint32_t, Threads> F32Input{
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
    0x00800001u, 0x80800001u, 0x3f000000u, 0xbf000000u, 0x3effffffu, 0x3f000001u, 0xc0800000u, 0xc07fffffu,
    0xc0800001u, 0x3f800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00001u, 0x7f800001u, 0x7f7fffffu,
    0xff7fffffu, 0x00400000u, 0x80400000u, 0x0000ffffu, 0x3e800000u, 0xbe800000u, 0x80000000u, 0x01000000u,
};
constexpr std::array<std::uint32_t, Threads> HalfBits{
    0x0000u, 0x8000u, 0x0001u, 0x8001u, 0x03ffu, 0x83ffu, 0x0400u, 0x8400u, 0x0401u, 0x8401u, 0x3800u, 0xb800u, 0x3bffu, 0x3c01u, 0xc400u, 0xc3ffu,
    0xc401u, 0x3c00u, 0x7c00u, 0xfc00u, 0x7e00u, 0xfe01u, 0x7c01u, 0x7bffu, 0xfbffu, 0x0200u, 0x8200u, 0x00ffu, 0x3400u, 0xb400u, 0x8000u, 0x0800u,
};
alignas(256) std::array<std::uint32_t, Threads> Input{};
alignas(256) std::array<std::uint32_t, Threads * Words> Output{};

enum class Kind { Half, Computed };

struct Constant {
    std::uint32_t bits;
};

constexpr std::array<Constant, 2> HalfConstants{{{0x7c00u}, {0xfc00u}}};
constexpr std::array<Constant, 2> ComputedConstants{{{0x7f7fffffu}, {0xff7fffffu}}};
constexpr std::array<std::uint32_t, 2> HalfOpcodes{0xebu, 0xeeu};
constexpr std::array<std::uint32_t, 2> F32Opcodes{0x0bu, 0x0eu};
constexpr std::array<const char*, 2> PredicateNames{"ngt", "nlt"};

std::vector<std::uint32_t> Build(Kind kind, bool registerOperand) {
    std::vector<std::uint32_t> code{0x34020082u, 0x34060085u, 0xe0301000u, 0x80000401u, 0xbf8c3f70u, 0x7e2c0281u};
    for (std::uint32_t word = 0; word < Words; ++word) code.push_back(0x7e000280u | ((30u + word) << 17));
    const auto& constants = kind == Kind::Half ? HalfConstants : ComputedConstants;
    const auto& opcodes = kind == Kind::Half ? HalfOpcodes : F32Opcodes;
    std::uint32_t bit = 0;
    for (const auto& constant : constants) {
        if (registerOperand) {
            code.insert(code.end(), {0x7e2a02ffu, constant.bits});
            if (kind == Kind::Computed) code.push_back((3u << 25) | (21u << 17) | (21u << 9) | (256u + 21u));
        }
        const std::uint32_t source = registerOperand ? 256u + 21u : 0xffu;
        const bool literal = !registerOperand;
        for (const std::uint32_t opcode : opcodes) {
            for (const bool constantFirst : {true, false}) {
                if (constantFirst) {
                    code.push_back(0x7c000000u | (opcode << 17) | (4u << 9) | source);
                } else {
                    code.push_back(0xd4000000u | (opcode << 16) | 106u);
                    code.push_back((source << 9) | (256u + 4u));
                }
                if (literal) code.push_back(constant.bits);
                code.push_back((0x01u << 25) | (20u << 17) | (22u << 9) | 0x80u);
                code.push_back(0xd4000000u | (0x36fu << 16) | 30u);
                code.push_back(((256u + 30u) << 18) | ((0x80u + bit) << 9) | (256u + 20u));
                ++bit;
            }
        }
    }
    for (std::uint32_t word = 0; word < Words; ++word) code.insert(code.end(), {0xe0701000u | (word * 4u), 0x80010003u | ((30u + word) << 8)});
    code.push_back(0xbf810000u);
    return code;
}

float HalfToFloat(std::uint32_t half) {
    const std::uint32_t sign = (half & 0x8000u) << 16u;
    const std::uint32_t exponent = (half >> 10u) & 0x1fu;
    const std::uint32_t mantissa = half & 0x3ffu;
    if (exponent == 0x1fu) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13u));
    if (exponent == 0u) {
        const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);
        return sign != 0u ? -magnitude : magnitude;
    }
    return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
}

bool Compare(std::size_t predicate, float lhs, float rhs) {
    return predicate == 0u ? !(lhs > rhs) : !(lhs < rhs);
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Run(AgcDriver::VulkanDevice& device, Kind kind, bool registerOperand) {
    Output.fill(0xdeadbeefu);
    const auto code = Build(kind, registerOperand);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), words, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(Kind kind, bool registerOperand) {
    const std::string mode = std::string(kind == Kind::Half ? "f16" : "computed f32 infinity") + (registerOperand ? ", register operand" : ", constant operand");
    const auto& constants = kind == Kind::Half ? HalfConstants : ComputedConstants;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const float value = kind == Kind::Half ? HalfToFloat(Input[tid] & 0xffffu) : std::bit_cast<float>(Input[tid]);
        std::uint32_t bit = 0;
        for (const auto& constant : constants) {
            const float infinity = kind == Kind::Half ? HalfToFloat(constant.bits) : std::bit_cast<float>(constant.bits) * 2.0f;
            for (std::size_t predicate = 0; predicate < PredicateNames.size(); ++predicate) {
                for (const bool constantFirst : {true, false}) {
                    const bool expected = constantFirst ? Compare(predicate, infinity, value) : Compare(predicate, value, infinity);
                    const bool actual = ((Output[tid * Words] >> bit) & 1u) != 0u;
                    Require(actual == expected, "float compare infinity: " + mode + " lane " + std::to_string(tid) + " v_cmp_" + PredicateNames[predicate] + " " +
                        (constantFirst ? Hex(constant.bits) + ", " + Hex(Input[tid]) : Hex(Input[tid]) + ", " + Hex(constant.bits)) + " is " + (actual ? "1" : "0") + ", expected " + (expected ? "1" : "0"));
                    ++bit;
                }
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const Kind kind : {Kind::Half, Kind::Computed}) {
            for (const bool registerOperand : {true, false}) {
                if (kind == Kind::Computed && !registerOperand) continue;
                for (std::uint32_t lane = 0; lane < Threads; ++lane) Input[lane] = kind == Kind::Half ? HalfBits[lane] | 0x3c000000u : F32Input[lane];
                Run(*device, kind, registerOperand);
                Check(kind, registerOperand);
            }
        }
        std::puts("float compare infinity tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
