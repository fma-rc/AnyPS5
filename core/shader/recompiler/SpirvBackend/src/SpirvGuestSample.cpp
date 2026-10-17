#include "SpirvBackend/SpirvGuestSample.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include "GuestSampleLibrary_spv.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace ShaderRecompiler {
namespace {

constexpr std::uint32_t HeaderWords = 5;

std::vector<std::uint32_t> NameWords(std::string_view name) {
    std::vector<std::uint32_t> words(name.size() / 4u + 1u, 0u);
    std::memcpy(words.data(), name.data(), name.size());
    return words;
}

std::string_view LiteralString(std::span<const std::uint32_t> words) {
    const auto* text = reinterpret_cast<const char*>(words.data());
    return std::string_view(text, strnlen(text, words.size() * sizeof(std::uint32_t)));
}

void Append(std::vector<std::uint32_t>& out, std::uint32_t opcode, std::initializer_list<std::span<const std::uint32_t>> parts) {
    std::size_t count = 1u;
    for (const auto part : parts) count += part.size();
    out.push_back(static_cast<std::uint32_t>(count << 16u) | opcode);
    for (const auto part : parts) out.insert(out.end(), part.begin(), part.end());
}

bool Preamble(std::uint32_t opcode) {
    switch (opcode) {
        case spv::OpCapability: case spv::OpExtension: case spv::OpExtInstImport: case spv::OpMemoryModel: case spv::OpEntryPoint:
        case spv::OpExecutionMode: case spv::OpExecutionModeId: case spv::OpString: case spv::OpSourceExtension: case spv::OpSource:
        case spv::OpSourceContinued: case spv::OpName: case spv::OpMemberName: case spv::OpModuleProcessed: case spv::OpLine: case spv::OpNoLine:
        case spv::OpDecorate: case spv::OpMemberDecorate: case spv::OpDecorationGroup: case spv::OpGroupDecorate: case spv::OpGroupMemberDecorate:
        case spv::OpDecorateId: case spv::OpDecorateString: case spv::OpMemberDecorateString:
            return true;
        default:
            return false;
    }
}

template<typename TVisit>
void ForEachInstruction(std::span<const std::uint32_t> module, TVisit&& visit) {
    if (module.size() < HeaderWords || module[0] != spv::MagicNumber) throw std::runtime_error("guest sampler: not a SPIR-V module");
    for (std::size_t at = HeaderWords; at < module.size();) {
        const auto count = module[at] >> 16u;
        if (count == 0u || at + count > module.size()) throw std::runtime_error("guest sampler: truncated SPIR-V instruction");
        visit(module[at] & 0xffffu, module.subspan(at, count));
        at += count;
    }
}

std::vector<std::uint32_t> PrepareLibrary(std::uint32_t addressing, std::uint32_t memoryModel) {
    const std::span<const std::uint32_t> library(GUEST_SAMPLE_LIBRARY_SPV, std::size(GUEST_SAMPLE_LIBRARY_SPV));
    std::uint32_t sampleId = 0;
    std::uint32_t loadId = 0;
    std::uint32_t mainId = 0;
    ForEachInstruction(library, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (opcode != spv::OpName) return;
        const auto name = LiteralString(words.subspan(2));
        if (name.starts_with("sampleGuest(")) sampleId = words[1];
        else if (name.starts_with("guestLoad32(")) loadId = words[1];
        else if (name == "main") mainId = words[1];
    });
    if (sampleId == 0u || loadId == 0u || mainId == 0u) throw std::runtime_error("guest sampler: the library lacks sampleGuest, guestLoad32 or main");
    std::vector<std::uint32_t> out(library.begin(), library.begin() + HeaderWords);
    const std::array<std::uint32_t, 1> linkage{spv::CapabilityLinkage};
    Append(out, spv::OpCapability, {linkage});
    bool decorated = false;
    bool skipping = false;
    bool body = false;
    ForEachInstruction(library, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (!decorated && !Preamble(opcode)) {
            for (const auto& [id, name, type] : {std::tuple{sampleId, std::string_view("sampleGuest"), spv::LinkageTypeExport}, std::tuple{loadId, std::string_view("guestLoad32"), spv::LinkageTypeImport}}) {
                const std::array<std::uint32_t, 2> head{id, spv::DecorationLinkageAttributes};
                const auto text = NameWords(name);
                const std::array<std::uint32_t, 1> kind{static_cast<std::uint32_t>(type)};
                Append(out, spv::OpDecorate, {head, text, kind});
            }
            decorated = true;
        }
        if (opcode == spv::OpEntryPoint || opcode == spv::OpExecutionMode || opcode == spv::OpExecutionModeId) return;
        // glslang decorates its gl_WorkGroupSize constant (1, 1, 1), which would override the
        // shader's LocalSize once linked in.
        if (opcode == spv::OpDecorate && words[2] == spv::DecorationBuiltIn) return;
        if (opcode == spv::OpName && words[1] == mainId) return;
        if (opcode == spv::OpMemoryModel) {
            const std::array<std::uint32_t, 2> model{addressing, memoryModel};
            Append(out, spv::OpMemoryModel, {model});
            return;
        }
        if (opcode == spv::OpFunction) {
            skipping = words[2] == mainId;
            body = words[2] == loadId;
        }
        const bool end = opcode == spv::OpFunctionEnd;
        const bool keep = !skipping && (!body || opcode == spv::OpFunction || opcode == spv::OpFunctionParameter || end);
        if (keep) out.insert(out.end(), words.begin(), words.end());
        if (end) {
            skipping = false;
            body = false;
        }
    });
    return out;
}

void DefineGuestSampleFunctions(SpirvEmitterState& state) {
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto vec4 = TypeU32Vector(state, 4);
    const auto vec4Pointer = TypePointer(state, spv::StorageClassFunction, vec4);
    const auto u32Pointer = TypePointer(state, spv::StorageClassFunction, u32);
    const auto u64Pointer = TypePointer(state, spv::StorageClassFunction, u64);
    const auto sampleType = state.module.Type(spv::OpTypeFunction, vec4, vec4Pointer, vec4Pointer, vec4Pointer, vec4Pointer, u32Pointer);
    const auto loadType = state.module.Type(spv::OpTypeFunction, u32, u64Pointer);
    state.module.EmitCapability(spv::CapabilityLinkage);
    const auto label = state.currentLabel;
    state.module.BeginHelperFunction();
    state.guestSampleFunction = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, vec4, state.guestSampleFunction, spv::FunctionControlMaskNone, sampleType);
    for (std::uint32_t parameter = 0; parameter < 5u; ++parameter) state.module.AddFunction(spv::OpFunctionParameter, parameter < 4u ? vec4Pointer : u32Pointer, state.module.AllocateId());
    state.module.AddFunction(spv::OpFunctionEnd);
    const auto load = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, u32, load, spv::FunctionControlMaskNone, loadType);
    const auto parameter = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionParameter, u64Pointer, parameter);
    EmitLabel(state, state.module.AllocateId());
    const auto address = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, u64, address, parameter);
    const auto bytes = [&] {
        const auto pair = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionCall, TypeU32Vector(state, 2), pair, state.bdaDwordReadFunctions[0][0], address, ConstantU32(state, 0u));
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, u32, value, pair, 0u);
        return value;
    };
    std::uint32_t value = 0;
    if (state.bdaProbeFunction != 0u) {
        const auto physical = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionCall, u64, physical, state.bdaProbeFunction, address, ConstantU32(state, 4u), ConstantU32(state, 0u));
        const auto usable = Binary(state, spv::OpLogicalAnd, TypeBool(state), Binary(state, spv::OpINotEqual, TypeBool(state), physical, BdaConstant(state, 0u)), Binary(state, spv::OpIEqual, TypeBool(state), Binary(state, spv::OpBitwiseAnd, u64, physical, BdaConstant(state, 3u)), BdaConstant(state, 0u)));
        value = EmitValueIfElse(state, usable, u32, [&] {
            const auto pointer = state.module.AllocateId();
            state.module.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer, physical);
            const auto loaded = state.module.AllocateId();
            state.module.AddFunction(spv::OpLoad, u32, loaded, pointer, spv::MemoryAccessAlignedMask, 4u);
            return loaded;
        }, bytes);
    } else {
        value = bytes();
    }
    state.module.AddFunction(spv::OpReturnValue, value);
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.EndHelperFunction();
    state.currentLabel = label;
    const auto sampleName = NameWords("sampleGuest");
    const auto loadName = NameWords("guestLoad32");
    state.module.AddAnnotation(spv::OpDecorate, state.guestSampleFunction, spv::DecorationLinkageAttributes, std::span<const std::uint32_t>(sampleName), spv::LinkageTypeImport);
    state.module.AddAnnotation(spv::OpDecorate, load, spv::DecorationLinkageAttributes, std::span<const std::uint32_t>(loadName), spv::LinkageTypeExport);
}

}

bool ProgramSamplesGuest(const IrProgram& program) {
    const auto& images = program.Info().images;
    return std::any_of(images.begin(), images.end(), [](const ImageResource& image) { return image.guestSample; });
}

void DeclareGuestSampleVariables(SpirvEmitterState& state) {
    if (!ProgramSamplesGuest(state.program)) return;
    for (std::uint32_t index = 0; index < state.guestSampleVariables.size(); ++index) {
        state.guestSampleVariables[index] = state.module.AllocateId();
        state.module.AddFunction(spv::OpVariable, TypePointer(state, spv::StorageClassFunction, index < 4u ? TypeU32Vector(state, 4) : TypeU32(state)), state.guestSampleVariables[index], spv::StorageClassFunction);
    }
}

std::uint32_t EmitGuestSampleCall(SpirvEmitterState& state, std::uint32_t tlow, std::uint32_t thigh, std::uint32_t sampler, std::uint32_t row, std::uint32_t& fault) {
    if (state.guestSampleVariables[0] == 0u) throw std::runtime_error("guest sampler: the program declared no guest sample variables");
    if (state.bdaDwordReadFunctions[0][0] == 0u) throw std::runtime_error("guest sampler: the program has no BDA dword reads");
    if (state.guestSampleFunction == 0u) DefineGuestSampleFunctions(state);
    const std::array<std::uint32_t, 4> arguments{tlow, thigh, sampler, row};
    for (std::uint32_t index = 0; index < arguments.size(); ++index) state.module.AddFunction(spv::OpStore, state.guestSampleVariables[index], arguments[index]);
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionCall, TypeU32Vector(state, 4), result, state.guestSampleFunction, state.guestSampleVariables[0], state.guestSampleVariables[1], state.guestSampleVariables[2], state.guestSampleVariables[3], state.guestSampleVariables[4]);
    fault = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32(state), fault, state.guestSampleVariables[4]);
    return result;
}

std::vector<std::uint32_t> LinkGuestSampleLibrary(const std::vector<std::uint32_t>& module) {
    std::uint32_t addressing = spv::AddressingModelLogical;
    std::uint32_t memoryModel = spv::MemoryModelGLSL450;
    ForEachInstruction(module, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (opcode != spv::OpMemoryModel) return;
        addressing = words[1];
        memoryModel = words[2];
    });
    spvtools::Context context(SPV_ENV_UNIVERSAL_1_6);
    std::string messages;
    context.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        messages += message;
        messages += '\n';
    });
    spvtools::LinkerOptions options;
    options.SetUseHighestVersion(true);
    std::vector<std::uint32_t> linked;
    if (spvtools::Link(context, {module, PrepareLibrary(addressing, memoryModel)}, &linked, options) != SPV_SUCCESS) throw std::runtime_error("guest sampler: linking the library failed: " + messages);
    return linked;
}

}
