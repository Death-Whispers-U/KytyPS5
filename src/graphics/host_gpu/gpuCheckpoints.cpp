#include "graphics/host_gpu/gpuCheckpoints.h"

#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace {

// Markers are sequence numbers; the driver hands them back untouched. The ring keeps the details
// of the newest ones, far more than one frame records.
constexpr uint64_t RingSize = 1u << 16u;

struct Entry {
	uint64_t          seq = 0;
	GpuCheckpointInfo info;
};

std::atomic_bool                       g_enabled {false};
std::mutex                             g_ring_mutex;
std::vector<Entry>                     g_ring;
uint64_t                               g_last_seq = 0;
std::mutex                             g_names_mutex;
std::unordered_map<uint64_t, std::string> g_names;

const char* KindName(GpuCheckpointKind kind) {
	switch (kind) {
		case GpuCheckpointKind::Draw: return "draw";
		case GpuCheckpointKind::DrawMesh: return "mesh draw";
		case GpuCheckpointKind::Dispatch: return "dispatch";
		case GpuCheckpointKind::DispatchIndirect: return "indirect dispatch";
		case GpuCheckpointKind::Internal: return "emulator pass";
	}
	return "unknown";
}

std::string ShaderName(uint64_t id) {
	std::lock_guard lock(g_names_mutex);
	if (auto it = g_names.find(id); it != g_names.end()) {
		return it->second;
	}
	return "shader#" + std::to_string(id);
}

// Caller holds g_ring_mutex.
std::string Describe(uint64_t seq) {
	const auto& e = g_ring[seq % RingSize];
	if (seq == 0 || e.seq != seq) {
		return "(details no longer kept)";
	}
	const auto& info = e.info;
	std::string text = KindName(info.kind);
	if (info.kind == GpuCheckpointKind::Internal) {
		return text + " " + (info.label != nullptr ? info.label : "?");
	}
	text += " submit=" + std::to_string(info.submit_id);
	for (const auto id: info.shader_ids) {
		if (id != 0) {
			text += " " + ShaderName(id);
		}
	}
	if (info.kind == GpuCheckpointKind::Dispatch) {
		text += " groups=" + std::to_string(info.counts[0]) + "x" + std::to_string(info.counts[1]) +
		        "x" + std::to_string(info.counts[2]);
	} else if (info.kind == GpuCheckpointKind::Draw || info.kind == GpuCheckpointKind::DrawMesh) {
		text += " count=" + std::to_string(info.counts[0]) +
		        " instances=" + std::to_string(info.counts[1]);
	}
	return text;
}

} // namespace

void EnableGpuCheckpoints() {
	{
		std::lock_guard lock(g_ring_mutex);
		g_ring.assign(RingSize, Entry {});
	}
	g_enabled = true;
}

bool GpuCheckpointsEnabled() {
	return g_enabled.load(std::memory_order_relaxed);
}

void GpuCheckpoint(vk::CommandBuffer command, const GpuCheckpointInfo& info) {
	if (!GpuCheckpointsEnabled()) {
		return;
	}
	uint64_t seq = 0;
	{
		std::lock_guard lock(g_ring_mutex);
		seq                    = ++g_last_seq;
		g_ring[seq % RingSize] = {seq, info};
	}
	command.setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(seq)));
}

void GpuCheckpoint(vk::CommandBuffer command, const char* label) {
	if (!GpuCheckpointsEnabled()) {
		return;
	}
	GpuCheckpointInfo info {};
	info.kind  = GpuCheckpointKind::Internal;
	info.label = label;
	GpuCheckpoint(command, info);
}

void RegisterGpuShaderName(uint64_t shader_id, const char* stage_name, uint64_t shader_hash) {
	char name[64];
	std::snprintf(name, sizeof(name), "%s_%016" PRIx64, stage_name, shader_hash);
	std::lock_guard lock(g_names_mutex);
	g_names[shader_id] = name;
}

void CopyGpuShaderName(uint64_t old_id, uint64_t new_id) {
	std::lock_guard lock(g_names_mutex);
	if (auto it = g_names.find(old_id); it != g_names.end()) {
		g_names[new_id] = it->second;
	}
}

void ReportGpuCheckpoints(GraphicContext& graphics) {
	if (!GpuCheckpointsEnabled()) {
		std::printf("GpuCheckpoint: off (run with --gpu-checkpoints to see how far the GPU got)\n");
		std::fflush(stdout);
		return;
	}
	uint32_t count = 0;
	graphics.queue.getCheckpointDataNV(&count, nullptr);
	std::vector<vk::CheckpointDataNV> data(count);
	if (count != 0) {
		graphics.queue.getCheckpointDataNV(&count, data.data());
		data.resize(count);
	}

	std::lock_guard lock(g_ring_mutex);
	std::printf("GpuCheckpoint: the GPU reached %u markers; the CPU recorded up to #%" PRIu64 "\n",
	            count, g_last_seq);
	uint64_t newest = 0;
	for (const auto& d: data) {
		const auto seq = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.pCheckpointMarker));
		newest         = std::max(newest, seq);
		std::printf("GpuCheckpoint: stage=%s #%" PRIu64 " %s\n", vk::to_string(d.stage).c_str(),
		            seq, Describe(seq).c_str());
	}
	// What the GPU had queued right after the newest marker it reached.
	for (uint64_t seq = newest + 1; newest != 0 && seq <= std::min(newest + 2, g_last_seq); seq++) {
		std::printf("GpuCheckpoint: next recorded #%" PRIu64 " %s\n", seq, Describe(seq).c_str());
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics
