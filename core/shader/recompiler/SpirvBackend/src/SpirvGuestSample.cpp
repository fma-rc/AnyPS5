#include "SpirvBackend/SpirvGuestSample.hpp"
#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include "GuestSampleLibrary_spv.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    std::vector<std::uint32_t> noInline;
    constexpr std::array<std::string_view, 8> separate{"sampleGuest", "fetchTexel", "decodeBc7Texel", "decodeBc6hTexel", "decodeBc1Texel", "unormFilter", "bc45Filter", "floatFilter"};
    ForEachInstruction(library, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (opcode != spv::OpName) return;
        const auto name = LiteralString(words.subspan(2));
        const auto paren = name.find('(');
        if (paren != std::string_view::npos) {
            const auto function = name.substr(0, paren);
            bool mark = std::find(separate.begin(), separate.end(), function) != separate.end();
            if (const char* mode = std::getenv("APS5_X_GUEST_NOINLINE"); mode != nullptr) {
                const std::string_view text(mode);
                const auto listed = [&](std::string_view list) {
                    for (std::size_t at = 0; at <= list.size();) {
                        const auto end = std::min(list.find(',', at), list.size());
                        if (list.substr(at, end - at) == function) return true;
                        at = end + 1u;
                    }
                    return false;
                };
                if (text == "all") mark = function != "main";
                else if (text.starts_with("all-but:")) mark = function != "main" && !listed(text.substr(8));
                else if (text.starts_with("only:")) mark = listed(text.substr(5));
            }
            if (mark) noInline.push_back(words[1]);
        }
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
    ForEachInstruction(library, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (!decorated && !Preamble(opcode)) {
            const std::array<std::uint32_t, 2> head{sampleId, spv::DecorationLinkageAttributes};
            const auto text = NameWords("sampleGuest");
            const std::array<std::uint32_t, 1> kind{static_cast<std::uint32_t>(spv::LinkageTypeExport)};
            Append(out, spv::OpDecorate, {head, text, kind});
            decorated = true;
        }
        if (opcode == spv::OpEntryPoint || opcode == spv::OpExecutionMode || opcode == spv::OpExecutionModeId) return;
        if (opcode == spv::OpDecorate && words[2] == spv::DecorationBuiltIn) return;
        if (opcode == spv::OpName && words[1] == mainId) return;
        if (opcode == spv::OpMemoryModel) {
            const std::array<std::uint32_t, 2> model{addressing, memoryModel};
            Append(out, spv::OpMemoryModel, {model});
            return;
        }
        if (opcode == spv::OpFunction) skipping = words[2] == mainId;
        if (opcode == spv::OpFunction && std::find(noInline.begin(), noInline.end(), words[2]) != noInline.end()) {
            const std::array<std::uint32_t, 4> function{words[1], words[2], (words[3] & ~static_cast<std::uint32_t>(spv::FunctionControlInlineMask)) | spv::FunctionControlDontInlineMask, words[4]};
            Append(out, spv::OpFunction, {function});
            return;
        }
        if (!skipping) out.insert(out.end(), words.begin(), words.end());
        if (opcode == spv::OpFunctionEnd) skipping = false;
    });
    return out;
}

void DeclareGuestSampleFunction(SpirvEmitterState& state) {
    const auto u32 = TypeU32(state);
    const auto vec4 = TypeU32Vector(state, 4);
    const auto vec4Pointer = TypePointer(state, spv::StorageClassFunction, vec4);
    const auto u32Pointer = TypePointer(state, spv::StorageClassFunction, u32);
    const auto sampleType = state.module.Type(spv::OpTypeFunction, vec4, vec4Pointer, vec4Pointer, vec4Pointer, vec4Pointer, vec4Pointer, u32Pointer);
    state.module.EmitCapability(spv::CapabilityLinkage);
    const auto label = state.currentLabel;
    state.module.BeginHelperFunction();
    state.guestSampleFunction = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, vec4, state.guestSampleFunction, spv::FunctionControlMaskNone, sampleType);
    for (std::uint32_t parameter = 0; parameter < 6u; ++parameter) state.module.AddFunction(spv::OpFunctionParameter, parameter < 5u ? vec4Pointer : u32Pointer, state.module.AllocateId());
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.EndHelperFunction();
    state.currentLabel = label;
    const auto sampleName = NameWords("sampleGuest");
    state.module.AddAnnotation(spv::OpDecorate, state.guestSampleFunction, spv::DecorationLinkageAttributes, std::span<const std::uint32_t>(sampleName), spv::LinkageTypeImport);
}

std::vector<std::uint32_t> CompleteEntryInterfaces(const std::vector<std::uint32_t>& module) {
    if (module.size() < HeaderWords || module[1] < 0x00010400u) return module;
    std::vector<std::uint32_t> globals;
    ForEachInstruction(module, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (opcode == spv::OpVariable && words.size() > 3u && words[3] != spv::StorageClassFunction) globals.push_back(words[2]);
    });
    std::vector<std::uint32_t> out(module.begin(), module.begin() + HeaderWords);
    ForEachInstruction(module, [&](std::uint32_t opcode, std::span<const std::uint32_t> words) {
        if (opcode != spv::OpEntryPoint) {
            out.insert(out.end(), words.begin(), words.end());
            return;
        }
        std::size_t nameEnd = 3u;
        while (nameEnd < words.size()) {
            const auto word = words[nameEnd++];
            if ((word & 0xffu) == 0u || (word & 0xff00u) == 0u || (word & 0xff0000u) == 0u || (word & 0xff000000u) == 0u) break;
        }
        std::vector<std::uint32_t> entry(words.begin() + 1, words.end());
        for (const auto id : globals) {
            if (std::find(words.begin() + static_cast<std::ptrdiff_t>(nameEnd), words.end(), id) == words.end()) entry.push_back(id);
        }
        out.push_back(static_cast<std::uint32_t>((entry.size() + 1u) << 16u) | spv::OpEntryPoint);
        out.insert(out.end(), entry.begin(), entry.end());
    });
    return out;
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
        state.module.AddFunction(spv::OpVariable, TypePointer(state, spv::StorageClassFunction, index < 5u ? TypeU32Vector(state, 4) : TypeU32(state)), state.guestSampleVariables[index], spv::StorageClassFunction);
    }
}

std::uint32_t EmitGuestSampleCall(SpirvEmitterState& state, std::uint32_t tlow, std::uint32_t thigh, std::uint32_t sampler, std::uint32_t row, std::uint32_t memory, std::uint32_t& fault) {
    if (state.guestSampleVariables[0] == 0u) throw std::runtime_error("guest sampler: the program declared no guest sample variables");
    if (state.guestSampleFunction == 0u) DeclareGuestSampleFunction(state);
    const std::array<std::uint32_t, 5> arguments{tlow, thigh, sampler, row, memory};
    for (std::uint32_t index = 0; index < arguments.size(); ++index) state.module.AddFunction(spv::OpStore, state.guestSampleVariables[index], arguments[index]);
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionCall, TypeU32Vector(state, 4), result, state.guestSampleFunction, state.guestSampleVariables[0], state.guestSampleVariables[1], state.guestSampleVariables[2], state.guestSampleVariables[3], state.guestSampleVariables[4], state.guestSampleVariables[5]);
    fault = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32(state), fault, state.guestSampleVariables[5]);
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
    auto complete = CompleteEntryInterfaces(linked);
    if (const char* dir = std::getenv("APS5_X_DUMP_LINKED"); dir != nullptr) {
        static std::atomic<int> serial{0};
        const auto path = std::string(dir) + "/linked-" + std::to_string(serial.fetch_add(1)) + ".spv";
        if (FILE* file = std::fopen(path.c_str(), "wb"); file != nullptr) {
            std::fwrite(complete.data(), sizeof(std::uint32_t), complete.size(), file);
            std::fclose(file);
        }
    }
    return complete;
}

}
