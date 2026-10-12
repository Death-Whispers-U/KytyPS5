#ifndef KYTY_GRAPHICS_HOST_GPU_GPU_CHECKPOINTS_H_
#define KYTY_GRAPHICS_HOST_GPU_GPU_CHECKPOINTS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <string>

namespace Libs::Graphics {

struct GraphicContext;

// GPU progress markers (--gpu-checkpoints, VK_NV_device_diagnostic_checkpoints). Every guest draw
// and dispatch records a marker; when the device is lost, ReportGpuCheckpoints prints the last
// markers the GPU reached, naming the guest shaders, so a hang or fault can be tied to one draw.
// Recording and reporting do nothing unless the extension was enabled on the device.

enum class GpuCheckpointKind : uint32_t {
	Draw,
	DrawMesh,
	Dispatch,
	DispatchIndirect,
	Internal,
};

struct GpuCheckpointInfo {
	GpuCheckpointKind kind      = GpuCheckpointKind::Internal;
	uint64_t          submit_id = 0;
	// ShaderProgram ids (0 = unused): vertex stages then pixel for draws, [0] for dispatches.
	uint64_t    shader_ids[4] {};
	uint32_t    counts[3] {};       // draws: vertex/index count, instance count; dispatch: groups
	const char* label = nullptr;    // Internal: a static string naming the emulator's own pass
};

// Called once at device creation when the extension was enabled.
void EnableGpuCheckpoints();
[[nodiscard]] bool GpuCheckpointsEnabled();

// Records a marker in the command buffer just before the command the info describes.
void GpuCheckpoint(vk::CommandBuffer command, const GpuCheckpointInfo& info);
void GpuCheckpoint(vk::CommandBuffer command, const char* label);

// Remembers the printable name of a shader program ("ps_4aa5db68601dcd19") for reports.
void RegisterGpuShaderName(uint64_t shader_id, const char* stage_name, uint64_t shader_hash);
// The registered name of a shader program, or "shader#<id>" when it has none.
[[nodiscard]] std::string GpuShaderName(uint64_t shader_id);
// A program recompiled under a new id (shader override reload) keeps its name.
void CopyGpuShaderName(uint64_t old_id, uint64_t new_id);

// Prints "GpuCheckpoint: ..." lines for the markers the GPU reached on the graphics queue.
void ReportGpuCheckpoints(GraphicContext& graphics);

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_HOST_GPU_GPU_CHECKPOINTS_H_ */
