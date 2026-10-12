#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_SHADER_TRACE_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_SHADER_TRACE_H_

#include <cstdint>

namespace Libs::Graphics {

struct ShaderStageRuntime;
class RenderContext;

// Development aid for one guest shader (--trace-shader <hash>). Every draw, dispatch, buffer
// fill and buffer copy records the guest memory it writes. When the traced shader is
// dispatched, the trace prints its buffer and image bindings and the most recent commands that
// wrote each of them ("ShaderTrace: ..." lines). With --trace-list <buffer>:<stride>:<next>,
// it also reads that buffer as an array of linked-list nodes (a 32-bit next index at byte
// <next> of each <stride>-byte node, 0 ending a list) and reports any cycle in the links.
// Everything is a no-op unless --trace-shader is given. GPU thread only.
namespace ShaderTrace {

[[nodiscard]] bool Enabled();

// Records the buffers and storage images the stage writes. `kind` is a static string.
void RecordStageWrites(const char* kind, uint64_t submit_id, const ShaderStageRuntime& stage);
// Records a write the emulator makes itself (fills, copies). `kind` is a static string.
void RecordRangeWrite(const char* kind, uint64_t address, uint64_t size);

// Call before the dispatch's bindings are prepared: it may read guest memory back from the GPU.
void OnDispatch(RenderContext& context, uint64_t submit_id, const ShaderStageRuntime& stage,
                uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

} // namespace ShaderTrace

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_HOST_GPU_RENDERER_SHADER_TRACE_H_ */
