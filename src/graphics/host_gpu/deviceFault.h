#ifndef KYTY_GRAPHICS_HOST_GPU_DEVICE_FAULT_H_
#define KYTY_GRAPHICS_HOST_GPU_DEVICE_FAULT_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct GraphicContext;

// Remembers the device whose faults ReportDeviceFailure can query (VK_EXT_device_fault).
void RegisterDeviceFaultContext(GraphicContext* context);

// Call when a Vulkan call fails, before stopping. On VK_ERROR_DEVICE_LOST it asks the driver what
// went wrong and prints it as "DeviceFault: ..." lines; every failure prints a "DeviceLost:" or
// "VulkanFailure:" line naming the call and the result. Reports once per process.
void ReportDeviceFailure(const char* where, vk::Result result);

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_HOST_GPU_DEVICE_FAULT_H_ */
