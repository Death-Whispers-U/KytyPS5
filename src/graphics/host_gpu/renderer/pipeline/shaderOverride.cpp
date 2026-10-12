#include "graphics/host_gpu/renderer/pipeline/shaderOverride.h"

#include <spirv-tools/libspirv.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#include <xxhash.h>

namespace Libs::Graphics::ShaderOverride {

namespace {

const std::filesystem::path& Directory() {
	static const std::filesystem::path dir = [] {
		const char* env = std::getenv("KYTY_SHADER_OVERRIDE");
		if (env == nullptr || *env == '\0') {
			return std::filesystem::path();
		}
		std::error_code ec;
		auto            abs = std::filesystem::absolute(std::filesystem::path(env), ec);
		return ec ? std::filesystem::path(env) : abs;
	}();
	return dir;
}

// Overrides are validated against the same environment the translator targets.
constexpr spv_target_env TARGET_ENV = SPV_ENV_VULKAN_1_3;

bool ReadFile(const std::filesystem::path& path, std::string* out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	// Write next to the target, then rename: a reader never sees a half-written file.
	const auto    tmp = std::filesystem::path(path).concat(".tmp");
	std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
	if (!out) {
		return false;
	}
	out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	out.close();
	std::filesystem::rename(tmp, path, ec);
	return !ec;
}

// The assembler stamps the newest SPIR-V version its environment allows (1.6 for Vulkan 1.3),
// and from 1.4 on every global variable must be listed in the entry point's interface, which
// the translator's 1.3 modules do not do. Assemble at the version the dump's header records.
spv_target_env AssembleEnvFor(const std::string& text) {
	const auto at = text.find("; Version: 1.");
	if (at != std::string::npos && at + 13 < text.size()) {
		switch (text[at + 13]) {
			case '0': return SPV_ENV_UNIVERSAL_1_0;
			case '1': return SPV_ENV_UNIVERSAL_1_1;
			case '2': return SPV_ENV_UNIVERSAL_1_2;
			case '3': return SPV_ENV_UNIVERSAL_1_3;
			case '4': return SPV_ENV_UNIVERSAL_1_4;
			case '5': return SPV_ENV_UNIVERSAL_1_5;
			case '6': return SPV_ENV_UNIVERSAL_1_6;
			default: break;
		}
	}
	return SPV_ENV_UNIVERSAL_1_3; // the translator's output version
}

int64_t StampOf(const std::filesystem::path& path) {
	std::error_code ec;
	const auto      time = std::filesystem::last_write_time(path, ec);
	if (ec) {
		return 0;
	}
	const auto size = std::filesystem::file_size(path, ec);
	// Include the size so two saves within the timestamp resolution still differ.
	return static_cast<int64_t>(time.time_since_epoch().count()) ^
	       (ec ? 0 : static_cast<int64_t>(size) << 1u);
}

} // namespace

bool IsEnabled() {
	return !Directory().empty();
}

std::string VariantName(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> spirv) {
	return fmt::format("{}_{:016x}_{:016x}", stage_name, shader_hash,
	                   XXH3_64bits(spirv.data(), spirv.size_bytes()));
}

void Dump(const std::string& name, std::span<const uint32_t> spirv) {
	// Reported every time this process compiles the variant, so tools can list the shaders in
	// use even when an earlier run already wrote the files.
	std::printf("ShaderOverride: translated %s\n", name.c_str());
	std::fflush(stdout);

	const auto      dump = Directory() / "dump";
	const auto      bin  = dump / (name + ".spv");
	const auto      text_path = dump / (name + ".spvasm");
	std::error_code ec;
	// Each file is checked on its own, so one lost to an interrupted run is written again.
	if (!std::filesystem::exists(bin, ec) && !WriteFile(bin, spirv.data(), spirv.size_bytes())) {
		std::printf("ShaderOverride: cannot write %s\n", bin.string().c_str());
	}
	if (!std::filesystem::exists(text_path, ec)) {
		spvtools::SpirvTools tools(TARGET_ENV);
		std::string          text;
		if (!tools.Disassemble(spirv.data(), spirv.size(), &text,
		                       static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
		                           static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
		                           static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT))) {
			std::printf("ShaderOverride: cannot disassemble %s\n", name.c_str());
		} else if (!WriteFile(text_path, text.data(), text.size())) {
			std::printf("ShaderOverride: cannot write %s\n", text_path.string().c_str());
		}
	}
	std::fflush(stdout);
}

namespace {

// What a pipeline depends on, collected with a word walk. Ids are not compared: an edited and
// re-assembled module may number them differently.
struct Interface {
	std::set<uint32_t>                                capabilities;
	std::multiset<std::tuple<uint32_t, uint32_t, uint32_t>> decorations; // storage class, kind, value
};

Interface ReadInterface(std::span<const uint32_t> spirv) {
	constexpr uint32_t OP_CAPABILITY = 17;
	constexpr uint32_t OP_VARIABLE   = 59;
	constexpr uint32_t OP_DECORATE   = 71;
	constexpr uint32_t DEC_BUILTIN   = 11;
	constexpr uint32_t DEC_LOCATION  = 30;
	constexpr uint32_t DEC_BINDING   = 33;
	constexpr uint32_t DEC_SET       = 34;

	Interface                                        result;
	std::map<uint32_t, uint32_t>                     storage; // variable id -> storage class
	std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> decorated; // id, kind, value
	for (size_t i = 5; i < spirv.size();) {
		const uint32_t count = spirv[i] >> 16u;
		const uint32_t op    = spirv[i] & 0xffffu;
		if (count == 0 || i + count > spirv.size()) {
			break;
		}
		if (op == OP_CAPABILITY && count >= 2) {
			result.capabilities.insert(spirv[i + 1]);
		} else if (op == OP_VARIABLE && count >= 4) {
			storage[spirv[i + 2]] = spirv[i + 3];
		} else if (op == OP_DECORATE && count >= 4 &&
		           (spirv[i + 2] == DEC_BUILTIN || spirv[i + 2] == DEC_LOCATION ||
		            spirv[i + 2] == DEC_BINDING || spirv[i + 2] == DEC_SET)) {
			decorated.emplace_back(spirv[i + 1], spirv[i + 2], spirv[i + 3]);
		}
		i += count;
	}
	for (const auto& [id, kind, value]: decorated) {
		const auto it = storage.find(id);
		result.decorations.emplace(it != storage.end() ? it->second : 0xffffffffu, kind, value);
	}
	return result;
}

} // namespace

std::string InterfaceMismatch(std::span<const uint32_t> translated,
                              std::span<const uint32_t> override_spirv) {
	const auto want = ReadInterface(translated);
	const auto have = ReadInterface(override_spirv);
	for (const auto cap: have.capabilities) {
		if (!want.capabilities.contains(cap)) {
			return fmt::format("adds capability {}, which the translated shader does not declare", cap);
		}
	}
	if (want.decorations != have.decorations) {
		auto describe = [](const auto& set) {
			static constexpr auto kind_name = [](uint32_t k) {
				switch (k) {
					case 11: return "BuiltIn";
					case 30: return "Location";
					case 33: return "Binding";
					default: return "DescriptorSet";
				}
			};
			static constexpr auto storage_name = [](uint32_t sc) -> std::string {
				switch (sc) {
					case 0: return "UniformConstant";
					case 1: return "Input";
					case 2: return "Uniform";
					case 3: return "Output";
					case 9: return "PushConstant";
					case 12: return "StorageBuffer";
					default: return fmt::format("storage class {}", sc);
				}
			};
			std::string out;
			for (const auto& [sc, kind, value]: set) {
				out += fmt::format("{}{} {}={}", out.empty() ? "" : ", ", storage_name(sc), kind_name(kind),
				                   value);
			}
			return out.empty() ? std::string("none") : out;
		};
		std::multiset<std::tuple<uint32_t, uint32_t, uint32_t>> missing;
		std::multiset<std::tuple<uint32_t, uint32_t, uint32_t>> extra;
		std::ranges::set_difference(want.decorations, have.decorations, std::inserter(missing, missing.end()));
		std::ranges::set_difference(have.decorations, want.decorations, std::inserter(extra, extra.end()));
		return fmt::format("changes the shader interface: missing [{}], added [{}]", describe(missing),
		                   describe(extra));
	}
	return {};
}

int64_t OverrideStamp(const std::string& name) {
	const auto dir = Directory() / "override";
	if (const auto stamp = StampOf(dir / (name + ".spvasm")); stamp != 0) {
		return stamp;
	}
	return StampOf(dir / (name + ".spv"));
}

std::string Validate(std::span<const uint32_t> spirv) {
	spvtools::SpirvTools tools(TARGET_ENV);
	std::string          messages;
	tools.SetMessageConsumer(
	    [&messages](spv_message_level_t, const char*, const spv_position_t& position,
	                const char* message) {
		    if (!messages.empty()) {
			    messages += "; ";
		    }
		    messages += fmt::format("word {}: {}", position.index, message);
	    });
	return tools.Validate(spirv.data(), spirv.size()) ? std::string()
	                                                  : (messages.empty() ? "invalid" : messages);
}

LoadResult Load(const std::string& name, std::span<const uint32_t> translated) {
	LoadResult result;
	const auto dir = Directory() / "override";
	const auto asm_path = dir / (name + ".spvasm");
	const auto bin_path = dir / (name + ".spv");
	std::string data;

	std::error_code ec;
	if (std::filesystem::exists(asm_path, ec)) {
		result.present = true;
		if (!ReadFile(asm_path, &data)) {
			result.readable = false;
			result.error    = "cannot read " + asm_path.filename().string();
			return result;
		}
		spvtools::SpirvTools tools(AssembleEnvFor(data));
		std::string          messages;
		tools.SetMessageConsumer(
		    [&messages](spv_message_level_t, const char*, const spv_position_t& position,
		                const char* message) {
			    messages += fmt::format("line {}: {}", position.line + 1, message);
		    });
		if (!tools.Assemble(data, &result.spirv,
		                    SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS)) {
			result.error = "assembly failed: " + messages;
			return result;
		}
	} else if (std::filesystem::exists(bin_path, ec)) {
		result.present = true;
		if (!ReadFile(bin_path, &data)) {
			result.readable = false;
			result.error    = "cannot read " + bin_path.filename().string();
			return result;
		}
		if (data.empty() || data.size() % 4 != 0) {
			result.error = bin_path.filename().string() + " is not a SPIR-V binary";
			return result;
		}
		result.spirv.resize(data.size() / 4);
		std::memcpy(result.spirv.data(), data.data(), data.size());
	} else {
		return result;
	}

	result.error = Validate(result.spirv);
	if (result.error.empty()) {
		result.error = InterfaceMismatch(translated, result.spirv);
	}
	result.ok = result.error.empty();
	return result;
}

} // namespace Libs::Graphics::ShaderOverride
