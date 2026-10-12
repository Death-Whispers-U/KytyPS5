#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADER_OVERRIDE_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADER_OVERRIDE_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Development aid: replaces the translator's SPIR-V for individual shaders while a game runs.
//
// Set KYTY_SHADER_OVERRIDE to a directory. Every shader the translator produces is written to
//   <dir>/dump/<stage>_<guest hash>_<spirv hash>.spv      (binary)
//   <dir>/dump/<stage>_<guest hash>_<spirv hash>.spvasm   (SPIR-V assembly, editable)
// Copy one of them into <dir>/override/ and edit it. The emulator assembles and validates it,
// then uses it instead of the translator's output; deleting it restores the translator's output.
// Changes are picked up while the game runs, and pipelines that use the shader are rebuilt on
// their next draw.
//
// The name ends in a hash of the translator's output, so an override only applies to exactly
// the SPIR-V it was made from. An override must keep the shader's interface (bindings, push
// constants, inputs and outputs) or pipeline creation fails.

namespace Libs::Graphics::ShaderOverride {

[[nodiscard]] bool IsEnabled();

// "<stage>_<guest hash>_<hash of spirv>", e.g. "ps_4aa5db68601dcd19_9c1f04d27e3b55a0".
[[nodiscard]] std::string VariantName(const char* stage_name, uint64_t shader_hash,
                                      std::span<const uint32_t> spirv);

// Reports the variant ("ShaderOverride: translated <name>") and writes the translator's output
// to the dump folder if it is not there yet.
void Dump(const std::string& name, std::span<const uint32_t> spirv);

// Modification stamp of the override file for this name (.spvasm preferred over .spv);
// 0 when there is none.
[[nodiscard]] int64_t OverrideStamp(const std::string& name);

struct LoadResult {
	bool                  present  = false; // an override file exists
	bool                  ok       = false; // it assembled, validated and kept the interface
	bool                  readable = true;  // false: the file could not be read (try again later)
	std::vector<uint32_t> spirv;
	std::string           error;
};

// Reads, assembles (.spvasm) and validates the override for this name, and checks that it keeps
// the translated shader's interface (see InterfaceMismatch).
[[nodiscard]] LoadResult Load(const std::string& name, std::span<const uint32_t> translated);

// Compares what a pipeline depends on: declared capabilities (the override may not add any) and,
// per storage class, the Location, Binding, DescriptorSet and BuiltIn decorations of variables.
// Returns an empty string when they match, else what differs. Validation alone cannot catch
// these: a mismatch passes spirv-val but fails pipeline creation, which stops the emulator.
[[nodiscard]] std::string InterfaceMismatch(std::span<const uint32_t> translated,
                                            std::span<const uint32_t> override_spirv);

// Validation used for overrides; returns an empty string when the module is valid.
[[nodiscard]] std::string Validate(std::span<const uint32_t> spirv);

} // namespace Libs::Graphics::ShaderOverride

#endif /* KYTY_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADER_OVERRIDE_H_ */
