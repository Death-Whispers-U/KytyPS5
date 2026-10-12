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
	Clock::time_point  last_present;
	{
		std::lock_guard lock(g_mutex);
		intervals.swap(g_intervals_ms);
		last_present = g_last_present;
	}
	std::string line = "FrameTimes:";
	char        number[24];
	for (const auto ms: intervals) {
		std::snprintf(number, sizeof(number), " %.1f", ms);
		line += number;
	}
	if (last_present != Clock::time_point {}) {
		// How long ago the newest frame was presented, so a reader can place every frame in time.
		std::snprintf(number, sizeof(number), " @%.1f",
		              std::chrono::duration<float, std::milli>(Clock::now() - last_present).count());
		line += number;
	}
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
}

} // namespace Libs::Graphics::FrameTimes
