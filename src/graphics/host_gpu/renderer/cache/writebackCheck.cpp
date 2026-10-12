#include "graphics/host_gpu/renderer/cache/writebackCheck.h"

#include "common/emulatorConfig.h"
#include "kernel/memory.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics::WritebackCheck {

namespace {

constexpr uint32_t ReportedConflicts = 64;  // then only a running count

std::atomic<uint32_t> g_conflicts {0};

uint64_t HashGuest(uint64_t address, uint64_t size) {
	std::vector<uint8_t> bytes(size);
	if (size == 0 || !LibKernel::Memory::TryReadBacking(address, bytes.data(), size)) {
		return 0;
	}
	// Never 0, so 0 can mean "not recorded".
	return XXH3_64bits(bytes.data(), bytes.size()) | 1u;
}

} // namespace

Mode GetMode() {
	return static_cast<Mode>(Config::WritebackCheckMode());
}

uint64_t Record(uint64_t address, uint64_t size) {
	return GetMode() == Mode::Off ? 0 : HashGuest(address, size);
}

bool BeforeWrite(const char* kind, uint64_t address, uint64_t size, uint64_t recorded) {
	const auto mode = GetMode();
	if (mode == Mode::Off || recorded == 0 || HashGuest(address, size) == recorded) {
		return true;
	}
	const auto n    = g_conflicts.fetch_add(1) + 1;
	const bool skip = mode == Mode::Skip;
	if (n <= ReportedConflicts || n % 1000 == 0) {
		std::printf("WritebackConflict: %s addr=0x%010" PRIx64 " size=0x%" PRIx64
		            " guest memory changed since the writeback was recorded; %s (conflict #%u)\n",
		            kind, address, size, skip ? "skipped" : "written", n);
		std::fflush(stdout);
	}
	return !skip;
}

} // namespace Libs::Graphics::WritebackCheck
