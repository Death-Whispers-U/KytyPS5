#include "graphics/host_gpu/renderer/shaderTrace.h"

#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderTrace {

namespace {

// Enough history to cover the frames between a list being built and being read.
constexpr size_t   WriteHistory          = 16384;
constexpr uint32_t DetailedDispatches    = 3;  // bindings and writers for the first dispatches
constexpr uint32_t DetailedCycles        = 3;  // and for the first dispatches that see a cycle
constexpr uint32_t SummaryEvery          = 200;
constexpr uint32_t WritersPerBinding     = 4;
constexpr uint32_t CycleNodesShown       = 16;
constexpr uint64_t MaxListBytes          = uint64_t {512} << 20u;

struct Write {
	uint64_t    seq       = 0;
	const char* kind      = nullptr;
	char        name[24]  = {};
	uint64_t    submit_id = 0;
	uint64_t    address   = 0;
	uint64_t    size      = 0; // 0: an image, matched by its base address
};

std::mutex         g_mutex; // fills and copies may come from other threads
std::vector<Write> g_writes;
uint64_t           g_write_seq      = 0;
uint32_t           g_dispatches     = 0;
uint32_t           g_cycles_seen    = 0;

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vs";
		case ShaderType::Pixel: return "ps";
		case ShaderType::Fetch: return "fs";
		case ShaderType::Compute: return "cs";
		case ShaderType::Mesh: return "ms";
		case ShaderType::Local: return "ls";
		case ShaderType::TessellationControl: return "hs";
		case ShaderType::TessellationEvaluation: return "ds";
		default: return "xs";
	}
}

std::string ShaderName(const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	char name[24];
	std::snprintf(name, sizeof(name), "%s_%016" PRIx64, StageName(program.stage), program.shader_hash);
	return name;
}

void Record(const char* kind, const char* name, uint64_t submit_id, uint64_t address, uint64_t size) {
	if (address == 0) {
		return;
	}
	std::lock_guard lock(g_mutex);
	if (g_writes.empty()) {
		g_writes.resize(WriteHistory);
	}
	auto& w     = g_writes[g_write_seq % WriteHistory];
	w.seq       = ++g_write_seq;
	w.kind      = kind;
	w.submit_id = submit_id;
	w.address   = address;
	w.size      = size;
	std::snprintf(w.name, sizeof(w.name), "%s", name);
}

bool Overlaps(const Write& w, uint64_t address, uint64_t size) {
	if (w.size == 0 || size == 0) {
		return w.address == address;
	}
	return w.address < address + size && address < w.address + w.size;
}

// The newest writes to [address, address + size), at most one per writer.
void PrintWriters(uint64_t address, uint64_t size) {
	std::vector<const Write*> found;
	for (uint64_t back = 0; back < std::min<uint64_t>(g_write_seq, WriteHistory); back++) {
		const auto& w = g_writes[(g_write_seq - back) % WriteHistory];
		if (w.seq == 0 || !Overlaps(w, address, size)) {
			continue;
		}
		const bool same = std::any_of(found.begin(), found.end(), [&](const Write* f) {
			return f->kind == w.kind && std::strcmp(f->name, w.name) == 0;
		});
		if (!same) {
			found.push_back(&w);
			if (found.size() == WritersPerBinding) {
				break;
			}
		}
	}
	if (found.empty()) {
		std::printf("ShaderTrace:     no recorded GPU writes (CPU-written, or older than the history)\n");
	}
	for (const auto* w: found) {
		std::printf("ShaderTrace:     written by %s %s submit=%" PRIu64 " (%" PRIu64
		            " writes ago) at 0x%012" PRIx64 "+0x%" PRIx64 "\n",
		            w->kind, w->name[0] != '\0' ? w->name : "-", w->submit_id, g_write_seq - w->seq,
		            w->address, w->size);
	}
}

void PrintBindings(const ShaderRecompiler::IR::CompiledShaderInfo&  program,
                   const ShaderRecompiler::IR::ResourceSnapshot&    resources) {
	for (size_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& b = program.info.buffers[i];
		const auto  r = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		std::printf("ShaderTrace:   buffer[%zu] %s%s addr=0x%012" PRIx64 " size=0x%" PRIx64
		            " stride=%u\n",
		            i, b.written ? "read-write" : "read-only", b.atomic ? " atomic" : "", r.Base48(),
		            r.GetSize(), static_cast<uint32_t>(r.Stride()));
		PrintWriters(r.Base48(), r.GetSize());
	}
	for (size_t i = 0; i < program.info.images.size(); i++) {
		const auto& image = program.info.images[i];
		const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
		std::printf("ShaderTrace:   image[%zu] %s%s%s addr=0x%010" PRIx64 " %ux%u\n", i,
		            image.written ? "read-write" : "read-only", image.atomic ? " atomic" : "",
		            image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
		                ? " sampled"
		                : "",
		            r.Base40(), static_cast<uint32_t>(r.Width5()) + 1u,
		            static_cast<uint32_t>(r.Height5()) + 1u);
		PrintWriters(r.Base40(), 0);
	}
}

struct ListReport {
	uint64_t              nodes  = 0;
	uint64_t              linked = 0;
	uint64_t              cycles = 0;
	std::vector<uint32_t> first_cycle;
};

// Follows every node's next link once; a link back into the path being walked is a cycle.
ListReport CheckList(const uint8_t* data, uint64_t size, uint32_t stride, uint32_t next_offset) {
	ListReport report;
	report.nodes = size / stride;
	if (report.nodes > UINT32_MAX) {
		report.nodes = UINT32_MAX;
	}
	const auto next = [&](uint64_t node) -> uint64_t {
		uint32_t value = 0;
		std::memcpy(&value, data + node * stride + next_offset, sizeof(value));
		return value;
	};
	const auto valid = [&](uint64_t node) { return node != 0 && node < report.nodes; };
	std::vector<uint8_t>  state(report.nodes, 0); // 0 unseen, 1 on the current path, 2 done
	std::vector<uint32_t> path;
	for (uint64_t start = 1; start < report.nodes; start++) {
		if (valid(next(start))) {
			report.linked++;
		}
		if (state[start] != 0) {
			continue;
		}
		path.clear();
		uint64_t node = start;
		while (valid(node) && state[node] == 0) {
			state[node] = 1;
			path.push_back(static_cast<uint32_t>(node));
			node = next(node);
		}
		if (valid(node) && state[node] == 1) {
			report.cycles++;
			if (report.first_cycle.empty()) {
				const auto at = std::find(path.begin(), path.end(), static_cast<uint32_t>(node));
				report.first_cycle.assign(at, path.end());
			}
		}
		for (const auto n: path) {
			state[n] = 2;
		}
	}
	return report;
}

std::string SaveDump(const std::string& name, uint32_t dispatch, uint32_t buffer, const uint8_t* data,
                     uint64_t size) {
	std::error_code ec;
	const std::filesystem::path dir = "_ShaderTrace";
	std::filesystem::create_directories(dir, ec);
	const auto path =
	    dir / (name + "_dispatch" + std::to_string(dispatch) + "_buffer" + std::to_string(buffer) + ".bin");
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	return out ? std::filesystem::absolute(path, ec).string() : std::string("(could not write)");
}

} // namespace

bool Enabled() {
	return Config::TraceShaderHash() != 0;
}

void RecordStageWrites(const char* kind, uint64_t submit_id, const ShaderStageRuntime& stage) {
	if (!Enabled() || !stage) {
		return;
	}
	const auto& program   = *stage.program;
	const auto& resources = *stage.resources;
	const auto  name      = ShaderName(program);
	for (size_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& b = program.info.buffers[i];
		if (b.written || b.atomic) {
			const auto r = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			Record(kind, name.c_str(), submit_id, r.Base48(), r.GetSize());
		}
	}
	for (size_t i = 0; i < program.info.images.size(); i++) {
		const auto& image = program.info.images[i];
		if (image.written || image.atomic) {
			const auto r = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			Record(kind, name.c_str(), submit_id, r.Base40(), 0);
		}
	}
}

void RecordRangeWrite(const char* kind, uint64_t address, uint64_t size) {
	if (Enabled()) {
		Record(kind, "", 0, address, size);
	}
}

void OnDispatch(RenderContext& context, uint64_t submit_id, const ShaderStageRuntime& stage,
                uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) {
	if (!Enabled() || !stage || stage.program->shader_hash != Config::TraceShaderHash()) {
		return;
	}
	const auto& program   = *stage.program;
	const auto& resources = *stage.resources;
	const auto  name      = ShaderName(program);
	const auto  n         = ++g_dispatches;

	const auto list = Config::TraceList();
	ListReport report;
	const uint8_t* data = nullptr;
	uint64_t       size = 0;
	bool           list_ok = false;
	if (list.enabled) {
		if (list.buffer >= program.info.buffers.size() || list.stride == 0 ||
		    list.next_offset + sizeof(uint32_t) > list.stride) {
			if (n == 1) {
				std::printf("ShaderTrace: --trace-list does not fit %s (it has %zu buffers)\n",
				            name.c_str(), program.info.buffers.size());
			}
		} else {
			const auto r = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[list.buffer]);
			size         = std::min(r.GetSize(), MaxListBytes);
			if (r.Base48() != 0 && size >= list.stride) {
				// The list was written by the GPU; bring guest memory up to date before reading it.
				context.GetBufferCache().ReadMemory(r.Base48(), size);
				data    = reinterpret_cast<const uint8_t*>(r.Base48());
				report  = CheckList(data, size, list.stride, list.next_offset);
				list_ok = true;
			}
		}
	}

	const bool cycle    = list_ok && report.cycles != 0;
	const bool detailed = n <= DetailedDispatches || (cycle && g_cycles_seen < DetailedCycles);
	if (detailed || n % SummaryEvery == 0) {
		std::printf("ShaderTrace: dispatch #%u %s submit=%" PRIu64 " groups=%ux%ux%u\n", n,
		            name.c_str(), submit_id, groups_x, groups_y, groups_z);
	}
	if (detailed) {
		std::lock_guard lock(g_mutex);
		PrintBindings(program, resources);
	}
	if (list_ok && (detailed || n % SummaryEvery == 0)) {
		std::printf("ShaderTrace: list buffer[%u] dispatch #%u nodes=%" PRIu64 " linked=%" PRIu64
		            " cycles=%" PRIu64 "\n",
		            list.buffer, n, report.nodes, report.linked, report.cycles);
	}
	if (cycle && g_cycles_seen < DetailedCycles) {
		g_cycles_seen++;
		std::string nodes;
		for (size_t i = 0; i < report.first_cycle.size() && i < CycleNodesShown; i++) {
			nodes += (i == 0 ? "" : " -> ") + std::to_string(report.first_cycle[i]);
		}
		if (report.first_cycle.size() > CycleNodesShown) {
			nodes += " -> ...";
		}
		std::printf("ShaderTrace: CYCLE in list buffer[%u] at dispatch #%u: %" PRIu64
		            " cycles, first has %zu nodes: %s -> %u\n",
		            list.buffer, n, report.cycles, report.first_cycle.size(), nodes.c_str(),
		            report.first_cycle.front());
		std::printf("ShaderTrace: list dump saved to %s\n",
		            SaveDump(name, n, list.buffer, data, size).c_str());
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics::ShaderTrace
