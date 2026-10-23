#ifndef CORE_SHADER_RECOMPILIER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVGUESTSAMPLE_HPP
#define CORE_SHADER_RECOMPILIER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVGUESTSAMPLE_HPP

#include "SpirvBackend/SpirvEmitterState.hpp"
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

bool ProgramSamplesGuest(const IrProgram& program);
void DeclareGuestSampleVariables(SpirvEmitterState& state);
std::uint32_t EmitGuestSampleCall(SpirvEmitterState& state, std::uint32_t tlow, std::uint32_t thigh, std::uint32_t sampler, std::uint32_t row, std::uint32_t derivatives, std::uint32_t memory, std::uint32_t layer, std::uint32_t& fault);
std::uint32_t EmitGuestLoadCall(SpirvEmitterState& state, std::uint32_t tlow, std::uint32_t thigh, std::uint32_t coord, std::uint32_t memory, std::uint32_t& fault);
std::vector<std::uint32_t> LinkGuestSampleLibrary(const std::vector<std::uint32_t>& module);

}

#endif
