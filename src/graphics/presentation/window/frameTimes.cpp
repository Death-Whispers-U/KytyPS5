#include "graphics/presentation/window/frameTimes.h"

#include "common/emulatorConfig.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics::FrameTimes {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex         g_mutex;
std::vector<float> g_intervals_ms;
Clock::time_point  g_last_present {};

} // namespace

void Presented() {
	if (!Config::FrameTimesEnabled()) {
		return;
	}
	const auto      now = Clock::now();
	std::lock_guard lock(g_mutex);
	if (g_last_present != Clock::time_point {}) {
		g_intervals_ms.push_back(
		    std::chrono::duration<float, std::milli>(now - g_last_present).count());
	}
	g_last_present = now;
}

void Report() {
	if (!Config::FrameTimesEnabled()) {
		return;
	}
	std::vector<float> intervals;
	{
		std::lock_guard lock(g_mutex);
		intervals.swap(g_intervals_ms);
	}
	std::string line = "FrameTimes:";
	char        number[16];
	for (const auto ms: intervals) {
		std::snprintf(number, sizeof(number), " %.1f", ms);
		line += number;
	}
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
}

} // namespace Libs::Graphics::FrameTimes
