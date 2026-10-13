#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_RESOURCEPLANENCODING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_RESOURCEPLANENCODING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Byte encodings of the translated-shader metadata kept by the persistent shader cache. The
// encoding belongs to one emulator build: the cache file is discarded when the build changes,
// so there is no versioning inside the bytes.

// Appends the plan to `out`. Returns false (and leaves `out` unchanged) when the plan holds a
// value the encoding cannot express, such as an instruction outside the plan's own storage.
[[nodiscard]] bool EncodeResourcePlan(const ResourcePlan& plan, std::vector<uint8_t>& out);
// Rebuilds a plan, including its SRT walker. Returns false on malformed or trailing bytes.
[[nodiscard]] bool DecodeResourcePlan(std::span<const uint8_t> bytes, ResourcePlan& plan);

void               EncodeCompiledShaderInfo(const CompiledShaderInfo& info, std::vector<uint8_t>& out);
[[nodiscard]] bool DecodeCompiledShaderInfo(std::span<const uint8_t> bytes, CompiledShaderInfo& info);

// Specializations are only ever encoded, as part of a cache key.
void EncodeSpecialization(const ResourceSpecialization& specialization, std::vector<uint8_t>& out);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_RESOURCEPLANENCODING_H_ */
