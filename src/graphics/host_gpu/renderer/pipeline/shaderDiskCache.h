#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDISKCACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDISKCACHE_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

// Translated shaders kept between runs, as opaque key/value records in one file per title.
//
// The file starts with a signature line naming the emulator build. A file written by another
// build is discarded on open, so record contents never need versioning. Records are only ever
// appended, each with a checksum: a record cut short by a crash ends the readable part, and the
// file is truncated back to the last good record before anything new is appended.
//
// Take() and Add() belong to one thread (the GPU thread). Writing happens on a background
// thread started by FlushAsync(), or synchronously in Flush() and the destructor.
class ShaderDiskCache {
public:
	enum class Kind : uint32_t {
		Plan        = 1, // resource plan of a translated program
		Permutation = 2, // compiled info + SPIR-V of one specialization of that program
	};

	ShaderDiskCache() = default;
	~ShaderDiskCache();
	ShaderDiskCache(const ShaderDiskCache&)            = delete;
	ShaderDiskCache& operator=(const ShaderDiskCache&) = delete;

	// Loads the records at `path` written under `signature`. Returns false when the file cannot
	// be read or created; the cache then stays closed and every call below does nothing.
	bool Open(const std::filesystem::path& path, std::string_view signature);

	[[nodiscard]] bool   IsOpen() const { return m_open; }
	[[nodiscard]] size_t LoadedRecords() const { return m_loaded; }

	// Removes and returns a loaded record. The key stays known, so it is never written twice.
	std::optional<std::vector<uint8_t>> Take(Kind kind, std::span<const uint8_t> key);
	// Lets a taken record that turned out unusable be written again; the newest record wins.
	void Forget(Kind kind, std::span<const uint8_t> key);
	// Queues a record for writing, unless the key is already in the file or queued.
	void Add(Kind kind, std::span<const uint8_t> key, std::span<const uint8_t> value);

	[[nodiscard]] bool HasPending() const;
	void               FlushAsync();
	void               Flush();

private:
	static std::string MapKey(Kind kind, std::span<const uint8_t> key);
	void               WritePending();

	std::filesystem::path                                  m_path;
	std::atomic_bool                                       m_open {false}; // cleared by a failed write
	size_t                                                 m_loaded = 0;
	std::unordered_map<std::string, std::vector<uint8_t>> m_records;
	std::unordered_set<std::string>                        m_known;

	mutable std::mutex   m_pending_mutex;
	std::vector<uint8_t> m_pending;
	std::mutex           m_file_mutex;
	std::thread          m_writer;
	std::atomic_bool     m_writing {false};
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDISKCACHE_H_ */
