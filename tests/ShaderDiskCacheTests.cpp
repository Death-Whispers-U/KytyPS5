// Tests for the persistent shader cache: the byte encodings of translated shader metadata and
// the record file that stores them between runs.

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDiskCache.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/ResourcePlanEncoding.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler::IR;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

void InitializeSubsystems() {
	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	ShaderInit();
}

// Captured b71ef7c579835a0d (see CheckRuntimeBufferRecords in the compute tests): reads an
// input count, builds a structured descriptor from it, and clears each record's last DWORD.
// Its resource plan has a real IR graph, specialization reads and an SRT walk.
const std::vector<uint32_t> RuntimeBufferRecordsCode = {
    0xbfa00003u, 0xd7460000u, 0x04010c0eu, 0x8805ff0du, 0x00040000u, 0xbe84030cu,
    0xbe860381u, 0xbe8703ffu, 0x00016204u, 0xb06a0100u, 0xe0300000u, 0x80010400u,
    0x92049281u, 0x7e0a026au, 0x996a9480u, 0xb0062000u, 0xb00a0080u, 0x380212f9u,
    0x8686066au, 0x7e040208u, 0x7e0602ffu, 0x00016204u, 0xbf8c3f70u, 0x7d8808ffu,
    0x00000200u, 0x7d8808f9u, 0x06868404u, 0x7d8808f9u, 0x06868606u, 0x020d80f9u,
    0x8686060au, 0xd5010005u, 0x00120affu, 0x00000200u, 0xd5010006u, 0x001a0d05u,
    0x7e0a0d06u, 0x7e0c0506u, 0xd76d0004u, 0x041a08c1u, 0x7e0a5705u, 0x81ea0680u,
    0x7e080504u, 0x100a0affu, 0x4f7ffffeu, 0x7e0a0f05u, 0x7ed60505u, 0x936a6b6au,
    0x9aea6b6au, 0x816a6a6bu, 0x9a87046au, 0x936a0607u, 0x4e08086au, 0x4e0a0806u,
    0x7d8608f9u, 0x06868406u, 0x7d860a06u, 0x87ea046au, 0x020902f9u, 0x86860680u,
    0xd5286a04u, 0x00120807u, 0xbeea047eu, 0x7da40080u, 0xbf880003u, 0x7e0a0280u,
    0xe0700000u, 0x80000500u, 0xbefe046au, 0x7e0a0280u, 0x7e000502u, 0x7e020501u,
    0x7e040504u, 0x7e060503u, 0xe0702010u, 0x80000500u, 0xbf810000u,
};

constexpr uint64_t CountAddress = 4;

struct Shader {
	std::array<uint32_t, 16>      user_data {};
	ShaderComputeInputInfo        compute_info;
	ShaderRecompiler::CompileOptions options;
};

void SetUpShader(Shader& shader) {
	shader.user_data[0]  = 16;
	shader.user_data[1]  = 4u << 16u;
	shader.user_data[2]  = 1;
	shader.user_data[3]  = 0x5204;
	shader.user_data[8]  = 20;
	shader.user_data[12] = CountAddress;
	auto& info = shader.compute_info;
	info.threads_num[0]             = 64;
	info.threads_num[1]             = 1;
	info.threads_num[2]             = 1;
	info.lds_size_dwords            = 0;
	info.wave_size                  = 64;
	info.float_mode                 = 0xc0;
	info.group_id[0]                = true;
	info.thread_ids_num             = 1;
	info.workgroup_register         = 14;
	info.dispatch_thread_dimensions = true;
	info.dispatch_threads_num[0]    = 2;
	info.dispatch_threads_num[1]    = 1;
	info.dispatch_threads_num[2]    = 1;
	shader.options.stage              = ShaderType::Compute;
	shader.options.wave_size          = 64;
	shader.options.dump_ir            = false;
	shader.options.user_data          = shader.user_data;
	shader.options.input_info.compute = &shader.compute_info;
}

bool ReadCount(void*, uint64_t address, std::span<uint32_t> words) {
	if (address != CountAddress || words.size() != 1) {
		return false;
	}
	words[0] = 72;
	return true;
}

SrtRuntime MakeRuntime(const Shader& shader) {
	return {.user_data = shader.user_data, .read_specialization_memory = ReadCount};
}

void TestResourcePlanRoundTrip() {
	Shader shader;
	SetUpShader(shader);
	auto translated = ShaderRecompiler::TranslateProgram(RuntimeBufferRecordsCode, shader.options);
	const auto plan = ExtractResourcePlan(translated.program);
	Check(!plan.value_storage.empty(), "the captured shader's plan has an IR graph to encode");

	std::vector<uint8_t> bytes;
	Check(EncodeResourcePlan(plan, bytes), "a translated plan encodes");

	ResourcePlan decoded;
	Check(DecodeResourcePlan(bytes, decoded), "the encoded plan decodes");

	std::vector<uint8_t> again;
	Check(EncodeResourcePlan(decoded, again) && again == bytes,
	      "re-encoding the decoded plan gives the same bytes");
	Check(decoded.value_storage.size() == plan.value_storage.size(),
	      "the decoded plan has the same number of IR instructions");
	Check(decoded.info == plan.info, "the decoded plan keeps its shader info");
	Check(decoded.memory_info == plan.memory_info, "the decoded plan keeps its memory info");
	Check((decoded.srt_code != nullptr) == (plan.srt_code != nullptr),
	      "the SRT walker is rebuilt on decode exactly when the original had one");

	const auto runtime = MakeRuntime(shader);
	ResourceSnapshot       original_snapshot;
	ResourceSnapshot       decoded_snapshot;
	ResourceSpecialization original_specialization;
	ResourceSpecialization decoded_specialization;
	const bool original_ok =
	    MaterializeResources(plan, runtime, original_snapshot, original_specialization);
	const bool decoded_ok =
	    MaterializeResources(decoded, runtime, decoded_snapshot, decoded_specialization);
	Check(original_ok && decoded_ok, "both plans materialize against the same guest state");
	Check(decoded_specialization == original_specialization,
	      "the decoded plan selects the same specialization");
	Check(decoded_snapshot.buffers == original_snapshot.buffers &&
	          decoded_snapshot.images == original_snapshot.images &&
	          decoded_snapshot.samplers == original_snapshot.samplers &&
	          decoded_snapshot.flattened_srt == original_snapshot.flattened_srt &&
	          decoded_snapshot.specialization_reads == original_snapshot.specialization_reads &&
	          decoded_snapshot.uniform_fill == original_snapshot.uniform_fill,
	      "the decoded plan produces the same descriptors");
	Check(decoded_snapshot.buffers.size() == 3 && decoded_snapshot.buffers[2].dwords[2] == 2,
	      "the decoded plan still computes the runtime record count");

	for (size_t size = 0; size < bytes.size(); size++) {
		ResourcePlan partial;
		if (DecodeResourcePlan(std::span(bytes).first(size), partial)) {
			Check(false, "a truncated plan is rejected");
			break;
		}
	}
	auto corrupt = bytes;
	corrupt.push_back(0);
	ResourcePlan trailing;
	Check(!DecodeResourcePlan(corrupt, trailing), "a plan with trailing bytes is rejected");
}

void TestCompiledInfoRoundTrip() {
	Shader shader;
	SetUpShader(shader);
	auto translated = ShaderRecompiler::TranslateProgram(RuntimeBufferRecordsCode, shader.options);
	const auto plan = ExtractResourcePlan(translated.program);
	ResourceSnapshot       snapshot;
	ResourceSpecialization specialization;
	Check(MaterializeResources(plan, MakeRuntime(shader), snapshot, specialization),
	      "the shader materializes for compilation");
	auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), shader.options,
	                                                 specialization, 3);
	const auto info = std::move(compiled.program).TakeCompiledInfo();

	std::vector<uint8_t> bytes;
	EncodeCompiledShaderInfo(info, bytes);
	CompiledShaderInfo decoded;
	Check(DecodeCompiledShaderInfo(bytes, decoded), "compiled shader info decodes");
	Check(decoded.stage == info.stage && decoded.shader_hash == info.shader_hash &&
	          decoded.wave_size == info.wave_size && decoded.user_data_base == info.user_data_base &&
	          decoded.user_data_count == info.user_data_count &&
	          decoded.scratch_dwords == info.scratch_dwords &&
	          decoded.param_export_mask == info.param_export_mask &&
	          decoded.has_address_writes == info.has_address_writes,
	      "compiled shader info keeps its scalar fields");
	Check(decoded.info == info.info, "compiled shader info keeps its resources");
	Check(decoded.bindings == info.bindings, "compiled shader info keeps its binding layout");
	bytes.pop_back();
	Check(!DecodeCompiledShaderInfo(bytes, decoded), "truncated compiled shader info is rejected");

	std::vector<uint8_t> first;
	std::vector<uint8_t> second;
	EncodeSpecialization(specialization, first);
	auto changed = specialization;
	Check(!changed.buffers.empty(), "the shader has a buffer specialization");
	if (!changed.buffers.empty()) {
		changed.buffers[0].packed_stride++;
	}
	EncodeSpecialization(changed, second);
	Check(first != second, "different specializations encode to different keys");
}

// s_load_dwordx4 s[4:7], s[0:1], 0 / buffer_store_dword v0, v0, s[4:7] idxen / s_endpgm.
// The descriptor comes through a user-data pointer, which gives the plan a native SRT walker.
void TestSrtWalkerRoundTrip() {
	const std::vector<uint32_t> code = {
	    (0x3du << 26u) | (0x02u << 18u) | (4u << 6u) | 0u, (0x7du << 25u),
	    (0x38u << 26u) | (0x1cu << 18u) | (1u << 13u),     (128u << 24u) | (1u << 16u),
	    0x80000000u | (0x7fu << 23u) | (0x01u << 16u),
	};
	alignas(16) static const std::array<uint32_t, 4> table = {0x1000u, 0x0u, 0x40u, 0x24924u};
	const auto address = reinterpret_cast<uint64_t>(table.data());
	std::array<uint32_t, 16> user_data {};
	user_data[0] = static_cast<uint32_t>(address);
	user_data[1] = static_cast<uint32_t>(address >> 32u);
	ShaderComputeInputInfo compute_info;
	ShaderRecompiler::CompileOptions options;
	options.stage              = ShaderType::Compute;
	options.wave_size          = 64;
	options.dump_ir            = false;
	options.user_data          = user_data;
	options.input_info.compute = &compute_info;
	auto translated = ShaderRecompiler::TranslateProgram(code, options);
	const auto plan = ExtractResourcePlan(translated.program);
	Check(plan.srt_code != nullptr, "a user-data pointer load gives the plan a native SRT walker");

	std::vector<uint8_t> bytes;
	ResourcePlan         decoded;
	Check(EncodeResourcePlan(plan, bytes) && DecodeResourcePlan(bytes, decoded),
	      "a plan with an SRT walker round-trips");
	Check(decoded.srt_code != nullptr, "the decoded plan rebuilds its native SRT walker");

	const SrtRuntime       runtime {.user_data = user_data};
	ResourceSnapshot       original_snapshot;
	ResourceSnapshot       decoded_snapshot;
	ResourceSpecialization original_specialization;
	ResourceSpecialization decoded_specialization;
	Check(MaterializeResources(plan, runtime, original_snapshot, original_specialization) &&
	          MaterializeResources(decoded, runtime, decoded_snapshot, decoded_specialization),
	      "both plans walk the guest table");
	Check(!decoded_snapshot.buffers.empty() && decoded_snapshot.buffers == original_snapshot.buffers &&
	          decoded_specialization == original_specialization,
	      "the rebuilt walker reads the same descriptor");
}

std::vector<uint8_t> Bytes(std::string_view text) {
	return {text.begin(), text.end()};
}

void TestDiskCacheFile() {
	const auto dir  = std::filesystem::temp_directory_path() / "kyty_shader_disk_cache_test";
	std::filesystem::remove_all(dir);
	const auto path = dir / "TEST00000.bin";
	using Kind      = ShaderDiskCache::Kind;

	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-1"), "a new cache file opens");
		Check(!cache.Take(Kind::Plan, Bytes("a")).has_value(), "an empty cache has no records");
		cache.Add(Kind::Plan, Bytes("a"), Bytes("plan a"));
		cache.Add(Kind::Permutation, Bytes("a"), Bytes("variant a"));
		cache.Add(Kind::Plan, Bytes("a"), Bytes("plan a again"));  // already queued: ignored
		cache.Flush();
		cache.Add(Kind::Plan, Bytes("b"), Bytes("plan b"));
	}  // destruction writes what is still queued

	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-1"), "the cache reopens");
		Check(cache.LoadedRecords() == 3, "every record written is loaded back once");
		Check(cache.Take(Kind::Plan, Bytes("a")) == Bytes("plan a"), "a plan record reads back");
		Check(cache.Take(Kind::Permutation, Bytes("a")) == Bytes("variant a"),
		      "records of different kinds with the same key are distinct");
		Check(cache.Take(Kind::Plan, Bytes("b")) == Bytes("plan b"),
		      "a record queued at shutdown was written");
		Check(!cache.Take(Kind::Plan, Bytes("a")).has_value(), "a taken record is removed");
		cache.Add(Kind::Plan, Bytes("a"), Bytes("plan a"));  // already in the file: ignored
		cache.Flush();
	}
	const auto good_size = std::filesystem::file_size(path);

	{
		std::ofstream tail(path, std::ios::binary | std::ios::app);
		tail.write("\x01\x00\x00\x00\xff", 5);  // a record cut off by a crash
	}
	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-1"), "a cache with a damaged tail opens");
		Check(cache.LoadedRecords() == 3, "records before a damaged tail survive");
		cache.Add(Kind::Plan, Bytes("c"), Bytes("plan c"));
		cache.Flush();
	}
	Check(std::filesystem::file_size(path) > good_size, "new records follow the good records");
	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-1"), "the repaired cache reopens");
		Check(cache.LoadedRecords() == 4, "a record appended after repair is readable");
	}

	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		file.seekp(static_cast<std::streamoff>(good_size) - 1);
		file.put('X');  // flip a payload byte of the last good record
	}
	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-1"), "a cache with a corrupt record opens");
		Check(cache.LoadedRecords() == 2, "a record failing its checksum ends the readable part");
	}

	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-2"), "a cache from another emulator build opens");
		Check(cache.LoadedRecords() == 0, "records from another emulator build are discarded");
		cache.Add(Kind::Plan, Bytes("d"), Bytes("plan d"));
	}
	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-2"), "the rewritten cache reopens");
		Check(cache.LoadedRecords() == 1 && cache.Take(Kind::Plan, Bytes("d")) == Bytes("plan d"),
		      "the file now belongs to the new build");
	}

	{
		// A record that no longer decodes is forgotten, so its replacement is written and wins.
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-2"), "the cache reopens for replacement");
		Check(cache.Take(Kind::Plan, Bytes("d")).has_value(), "the stale record is read");
		cache.Forget(Kind::Plan, Bytes("d"));
		cache.Add(Kind::Plan, Bytes("d"), Bytes("plan d v2"));
	}
	{
		ShaderDiskCache cache;
		Check(cache.Open(path, "sig-2"), "the cache with a replaced record reopens");
		Check(cache.LoadedRecords() == 1 && cache.Take(Kind::Plan, Bytes("d")) == Bytes("plan d v2"),
		      "the newest record for a key wins");
	}
	std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	InitializeSubsystems();
	TestResourcePlanRoundTrip();
	TestCompiledInfoRoundTrip();
	TestSrtWalkerRoundTrip();
	TestDiskCacheFile();
	if (g_failures != 0) {
		std::printf("shader_disk_cache_tests: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("shader_disk_cache_tests: ok\n");
	return EXIT_SUCCESS;
}
