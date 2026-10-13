#include "graphics/host_gpu/renderer/pipeline/shaderDiskCache.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

// [u64 checksum][u32 kind][u32 key size][u32 value size][key][value]. The checksum covers
// everything after itself.
struct RecordHeader {
	uint64_t checksum   = 0;
	uint32_t kind       = 0;
	uint32_t key_size   = 0;
	uint32_t value_size = 0;
};
constexpr size_t RecordHeaderSize = sizeof(uint64_t) + 3 * sizeof(uint32_t);

void AppendRecord(std::vector<uint8_t>& out, uint32_t kind, std::span<const uint8_t> key,
                  std::span<const uint8_t> value) {
	const auto start = out.size();
	out.resize(start + RecordHeaderSize);
	const uint32_t fields[3] = {kind, static_cast<uint32_t>(key.size()),
	                            static_cast<uint32_t>(value.size())};
	std::memcpy(out.data() + start + sizeof(uint64_t), fields, sizeof(fields));
	out.insert(out.end(), key.begin(), key.end());
	out.insert(out.end(), value.begin(), value.end());
	const auto checksum = XXH3_64bits(out.data() + start + sizeof(uint64_t),
	                                  out.size() - start - sizeof(uint64_t));
	std::memcpy(out.data() + start, &checksum, sizeof(checksum));
}

bool WriteFile(const std::filesystem::path& path, std::span<const uint8_t> bytes,
               std::ios::openmode mode) {
	std::ofstream file(path, std::ios::binary | mode);
	file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	file.flush();
	return static_cast<bool>(file);
}

} // namespace

ShaderDiskCache::~ShaderDiskCache() {
	Flush();
}

std::string ShaderDiskCache::MapKey(Kind kind, std::span<const uint8_t> key) {
	std::string result(1, static_cast<char>(kind));
	result.append(reinterpret_cast<const char*>(key.data()), key.size());
	return result;
}

bool ShaderDiskCache::Open(const std::filesystem::path& path, std::string_view signature) {
	m_path = path;
	std::string header(signature);
	header += '\n';

	std::vector<uint8_t> bytes;
	{
		std::ifstream file(path, std::ios::binary);
		if (file) {
			bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}
	}
	const bool same_build =
	    bytes.size() >= header.size() && std::memcmp(bytes.data(), header.data(), header.size()) == 0;
	if (!same_build) {
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		m_open = WriteFile(path, std::span(reinterpret_cast<const uint8_t*>(header.data()), header.size()),
		                   std::ios::trunc);
		return m_open;
	}

	size_t offset = header.size();
	while (bytes.size() - offset >= RecordHeaderSize) {
		RecordHeader record;
		std::memcpy(&record.checksum, bytes.data() + offset, sizeof(uint64_t));
		std::memcpy(&record.kind, bytes.data() + offset + sizeof(uint64_t), 3 * sizeof(uint32_t));
		const auto body = static_cast<uint64_t>(record.key_size) + record.value_size;
		if (body > bytes.size() - offset - RecordHeaderSize ||
		    XXH3_64bits(bytes.data() + offset + sizeof(uint64_t),
		                RecordHeaderSize - sizeof(uint64_t) + body) != record.checksum) {
			break;
		}
		const auto* key   = bytes.data() + offset + RecordHeaderSize;
		const auto* value = key + record.key_size;
		// A key written again (after Forget) replaces its older record.
		auto map_key = MapKey(static_cast<Kind>(record.kind), std::span(key, record.key_size));
		m_known.insert(map_key);
		m_records.insert_or_assign(std::move(map_key),
		                           std::vector<uint8_t>(value, value + record.value_size));
		offset += RecordHeaderSize + body;
	}
	m_loaded = m_records.size();
	if (offset != bytes.size()) {
		// Drop the damaged tail so new records follow the last good one.
		std::error_code error;
		std::filesystem::resize_file(path, offset, error);
		if (error) {
			return false;
		}
	}
	m_open = true;
	return true;
}

std::optional<std::vector<uint8_t>> ShaderDiskCache::Take(Kind kind, std::span<const uint8_t> key) {
	if (!m_open) {
		return std::nullopt;
	}
	const auto found = m_records.find(MapKey(kind, key));
	if (found == m_records.end()) {
		return std::nullopt;
	}
	auto value = std::move(found->second);
	m_records.erase(found);
	return value;
}

void ShaderDiskCache::Forget(Kind kind, std::span<const uint8_t> key) {
	m_known.erase(MapKey(kind, key));
}

void ShaderDiskCache::Add(Kind kind, std::span<const uint8_t> key, std::span<const uint8_t> value) {
	if (!m_open || !m_known.insert(MapKey(kind, key)).second) {
		return;
	}
	std::lock_guard lock(m_pending_mutex);
	AppendRecord(m_pending, static_cast<uint32_t>(kind), key, value);
}

bool ShaderDiskCache::HasPending() const {
	std::lock_guard lock(m_pending_mutex);
	return !m_pending.empty();
}

void ShaderDiskCache::WritePending() {
	std::vector<uint8_t> records;
	{
		std::lock_guard lock(m_pending_mutex);
		records.swap(m_pending);
	}
	if (records.empty()) {
		return;
	}
	std::lock_guard lock(m_file_mutex);
	if (!WriteFile(m_path, records, std::ios::app)) {
		// A partial append fails its checksum and is dropped on the next open.
		m_open = false;
	}
}

void ShaderDiskCache::FlushAsync() {
	if (!m_open || m_writing.load() || !HasPending()) {
		return;
	}
	if (m_writer.joinable()) {
		m_writer.join(); // finished: m_writing is false
	}
	m_writing = true;
	m_writer  = std::thread([this] {
		WritePending();
		m_writing = false;
	});
}

void ShaderDiskCache::Flush() {
	if (m_writer.joinable()) {
		m_writer.join();
	}
	if (m_open) {
		WritePending();
	}
}

} // namespace Libs::Graphics
