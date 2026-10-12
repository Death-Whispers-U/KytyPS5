#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITEBACK_CHECK_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITEBACK_CHECK_H_

#include <cstdint>

namespace Libs::Graphics::WritebackCheck {

// Asynchronous writebacks copy GPU data into guest memory some time after they are recorded. If
// the game writes that memory in between (for example after freeing and reusing it), the
// writeback would overwrite newer data. --writeback-check compares the guest bytes at record
// time and just before the write:
//   off  - no check;
//   log  - report "WritebackConflict: ..." and write anyway (default);
//   skip - report and leave the newer guest data in place. A diagnostic for testing whether
//          such conflicts cause a fault; it can lose GPU results the game expected to read.
enum class Mode : uint32_t { Off, Log, Skip };

[[nodiscard]] Mode GetMode();

// Hash of the guest bytes now in [address, address + size); 0 when the check is off or the range
// cannot be read.
[[nodiscard]] uint64_t Record(uint64_t address, uint64_t size);

// Call right before writing [address, address + size). Returns whether to write.
[[nodiscard]] bool BeforeWrite(const char* kind, uint64_t address, uint64_t size, uint64_t recorded);

} // namespace Libs::Graphics::WritebackCheck

#endif /* KYTY_GRAPHICS_HOST_GPU_RENDERER_CACHE_WRITEBACK_CHECK_H_ */
