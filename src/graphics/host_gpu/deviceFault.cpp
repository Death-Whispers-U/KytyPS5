#include "graphics/host_gpu/deviceFault.h"

#include "graphics/host_gpu/gpuCheckpoints.h"
#include "graphics/host_gpu/graphicContext.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <vector>

// A macro keeps every format string a literal the compiler can check.
#define KYTY_FAULT_PRINT(...)                                                                      \
	do {                                                                                           \
		std::printf(__VA_ARGS__);                                                                  \
		std::fflush(stdout);                                                                       \
	} while (false)

namespace Libs::Graphics {

namespace {

std::atomic<GraphicContext*> g_context {nullptr};
std::mutex                   g_report_mutex;
std::condition_variable      g_report_cv;
bool                         g_report_started = false;
bool                         g_report_done    = false;

void QueryFaults(GraphicContext& graphics) {
	if (!graphics.device_fault_enabled) {
		KYTY_FAULT_PRINT("DeviceFault: not available (VK_EXT_device_fault is not enabled on this device)\n");
		return;
	}
	KYTY_FAULT_PRINT("DeviceFault: asking the driver what faulted\n");
	vk::DeviceFaultCountsEXT counts {};
	if (graphics.device.getFaultInfoEXT(&counts, nullptr) != vk::Result::eSuccess) {
		KYTY_FAULT_PRINT("DeviceFault: the driver did not return fault counts\n");
		return;
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	counts.vendorBinarySize = 0; // the vendor binary dump is not requested
	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos  = vendors.empty() ? nullptr : vendors.data();
	const auto result  = graphics.device.getFaultInfoEXT(&counts, &info);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		KYTY_FAULT_PRINT("DeviceFault: the driver did not return fault details (%s)\n",
		      vk::to_string(result).c_str());
		return;
	}
	KYTY_FAULT_PRINT("DeviceFault: description=%s\n", info.description.data());
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& a = addresses[i];
		KYTY_FAULT_PRINT("DeviceFault: address type=%s address=0x%016" PRIx64 " precision=0x%" PRIx64 "\n",
		      vk::to_string(a.addressType).c_str(), static_cast<uint64_t>(a.reportedAddress),
		      static_cast<uint64_t>(a.addressPrecision));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& v = vendors[i];
		KYTY_FAULT_PRINT("DeviceFault: vendor code=0x%016" PRIx64 " data=0x%016" PRIx64 " description=%s\n",
		      static_cast<uint64_t>(v.vendorFaultCode), static_cast<uint64_t>(v.vendorFaultData),
		      v.description.data());
	}
	if (counts.addressInfoCount == 0 && counts.vendorInfoCount == 0) {
		KYTY_FAULT_PRINT("DeviceFault: the driver reported no fault addresses\n");
	}
}

} // namespace

void RegisterDeviceFaultContext(GraphicContext* context) {
	g_context = context;
}

void ReportDeviceFailure(const char* where, vk::Result result) {
	{
		std::unique_lock lock(g_report_mutex);
		if (g_report_started) {
			// Another thread is already reporting. Returning now would let this thread's EXIT end the
			// process before that report is printed, so wait for it (bounded, in case the driver hangs).
			g_report_cv.wait_for(lock, std::chrono::seconds(10), [] { return g_report_done; });
			return;
		}
		g_report_started = true;
	}
	const bool lost = result == vk::Result::eErrorDeviceLost;
	KYTY_FAULT_PRINT("%s: %s failed with %s\n", lost ? "DeviceLost" : "VulkanFailure", where,
	      vk::to_string(result).c_str());
	if (auto* graphics = g_context.load(); lost && graphics != nullptr) {
		QueryFaults(*graphics);
		ReportGpuCheckpoints(*graphics);
	}
	{
		std::lock_guard lock(g_report_mutex);
		g_report_done = true;
	}
	g_report_cv.notify_all();
}

} // namespace Libs::Graphics
