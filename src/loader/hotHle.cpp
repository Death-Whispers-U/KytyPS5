#include "loader/hotHle.h"

#include "common/common.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::HotHle {

namespace {

constexpr uint32_t MAX_ENTRIES = 4096;

struct Slot {
	uint64_t vaddr;
	uint64_t original;
	bool     resolved;
};

struct Patch {
	uint64_t    func;
	std::string name;
};

std::mutex                                         g_mutex;
std::unordered_map<std::string, std::vector<Slot>> g_slots;  // by NID
std::unordered_map<std::string, Patch>             g_active; // by NID
std::once_flag                                     g_start_once;

const std::filesystem::path& Directory() {
	static const std::filesystem::path dir = [] {
		const char* env = std::getenv("KYTY_HOT_HLE");
		if (env == nullptr || *env == '\0') {
			return std::filesystem::path();
		}
		std::error_code ec;
		auto            abs = std::filesystem::absolute(std::filesystem::path(env), ec);
		return ec ? std::filesystem::path(env) : abs;
	}();
	return dir;
}

std::string NidOf(const std::string& symbol_name) {
	return symbol_name.substr(0, symbol_name.find('['));
}

template <typename... Args>
void Print(const char* format, Args... args) {
	std::printf(format, args...);
	std::fflush(stdout);
}

// Callers hold g_mutex. Import slots are 8-byte aligned (checked by the caller of
// WriteImportSlot), so this store is atomic: a guest thread calling through the slot sees either
// the old target or the new one, never a torn pointer.
bool WriteSlot(uint64_t vaddr, uint64_t value) {
	auto*      ptr     = reinterpret_cast<uint64_t*>(vaddr);
	const bool changed = __atomic_load_n(ptr, __ATOMIC_SEQ_CST) != value;
	__atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
	return changed;
}

[[maybe_unused]] void PatchLog(const char* text) {
	Print("HotHLE patch: %s\n", text != nullptr ? text : "");
}

[[maybe_unused]] void* PatchOriginal(const char* nid) {
	if (nid == nullptr) {
		return nullptr;
	}
	std::lock_guard lock(g_mutex);
	const auto      it = g_slots.find(nid);
	if (it == g_slots.end() || it->second.empty()) {
		return nullptr;
	}
	// Prefer the emulator's implementation over an unresolved-import stub.
	for (const auto& slot: it->second) {
		if (slot.resolved) {
			return reinterpret_cast<void*>(slot.original);
		}
	}
	return reinterpret_cast<void*>(it->second.front().original);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

void Apply(const std::unordered_map<std::string, Patch>& next, uint32_t generation) {
	std::lock_guard lock(g_mutex);

	for (const auto& [nid, patch]: g_active) {
		if (next.contains(nid)) {
			continue;
		}
		const auto it = g_slots.find(nid);
		size_t     n  = 0;
		if (it != g_slots.end()) {
			for (const auto& slot: it->second) {
				WriteSlot(slot.vaddr, slot.original);
				n++;
			}
		}
		Print("HotHLE: restored %s %s (%zu slots)\n", nid.c_str(), patch.name.c_str(), n);
	}

	for (const auto& [nid, patch]: next) {
		const auto it = g_slots.find(nid);
		if (it == g_slots.end() || it->second.empty()) {
			Print("HotHLE: %s %s is not imported yet; it will be used when it is\n", nid.c_str(),
			      patch.name.c_str());
			continue;
		}
		for (const auto& slot: it->second) {
			WriteSlot(slot.vaddr, patch.func);
		}
		Print("HotHLE: patched %s %s (%zu slots)\n", nid.c_str(), patch.name.c_str(),
		      it->second.size());
	}

	g_active = next;
	Print("HotHLE: kyty_hot.dll #%u active with %zu functions\n", generation, next.size());
}

bool Load(const std::filesystem::path& dll, uint32_t generation) {
	std::error_code ec;
	const auto      loaded_dir = Directory() / ".loaded";
	std::filesystem::create_directories(loaded_dir, ec);

	// Load a copy so the build can overwrite kyty_hot.dll while this one stays mapped.
	// Copies are never unloaded: a guest thread may still be executing old code.
	const auto copy = loaded_dir / ("kyty_hot_" + std::to_string(GetCurrentProcessId()) + "_" +
	                                std::to_string(generation) + ".dll");
	std::filesystem::copy_file(dll, copy, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		Print("HotHLE: cannot copy %s: %s\n", dll.string().c_str(), ec.message().c_str());
		return false;
	}

	HMODULE module = LoadLibraryW(copy.wstring().c_str());
	if (module == nullptr) {
		Print("HotHLE: cannot load %s (error %lu)\n", copy.string().c_str(), GetLastError());
		return false;
	}

	auto abi_func = reinterpret_cast<AbiVersionFunc>(GetProcAddress(module, "kyty_hot_abi_version"));
	auto entries_func =
	    reinterpret_cast<EntriesFunc>(GetProcAddress(module, "kyty_hot_entries"));
	if (abi_func == nullptr || entries_func == nullptr) {
		Print("HotHLE: %s does not export kyty_hot_abi_version and kyty_hot_entries\n",
		      copy.string().c_str());
		return true; // the file is complete but unusable; wait for the next change
	}
	if (const auto abi = abi_func(); abi != ABI_VERSION) {
		Print("HotHLE: patch uses ABI version %u, the emulator expects %u; rebuild it with the "
		      "matching kyty_hot.h\n",
		      abi, ABI_VERSION);
		return true;
	}

	static const Host host {ABI_VERSION, PatchLog, PatchOriginal};
	uint32_t          count   = 0;
	const Entry*      entries = entries_func(&host, &count);
	if (count > MAX_ENTRIES) {
		Print("HotHLE: patch reports %u functions, more than the limit of %u\n", count, MAX_ENTRIES);
		return true;
	}

	std::unordered_map<std::string, Patch> next;
	for (uint32_t i = 0; i < count && entries != nullptr; i++) {
		const auto& e = entries[i];
		if (e.nid == nullptr || std::strlen(e.nid) != 11 || e.func == nullptr) {
			Print("HotHLE: entry %u is invalid and was skipped\n", i);
			continue;
		}
		next[e.nid] = Patch {reinterpret_cast<uint64_t>(e.func), e.name != nullptr ? e.name : ""};
	}

	Apply(next, generation);
	return true;
}

void Watch(std::filesystem::file_time_type seen, uint32_t generation) {
	const auto dll = Directory() / "kyty_hot.dll";

	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		try {
			std::error_code ec;
			const auto      time = std::filesystem::last_write_time(dll, ec);
			if (ec || time == seen) {
				continue;
			}
			// Let the writer finish: wait until the timestamp holds still for one interval.
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			if (std::filesystem::last_write_time(dll, ec) != time || ec) {
				continue;
			}
			// A failed copy or load (file still locked) is retried on the next pass.
			if (Load(dll, generation + 1)) {
				seen = time;
				generation++;
			}
		} catch (const std::exception& e) {
			Print("HotHLE: watcher error: %s\n", e.what());
		}
	}
}

void Start() {
	try {
		const auto dll = Directory() / "kyty_hot.dll";
		Print("HotHLE: watching %s\n", dll.string().c_str());

		// A patch that exists at launch is applied before the game makes its first call, so
		// functions called once during boot can be patched too.
		std::error_code                 ec;
		std::filesystem::file_time_type seen       = std::filesystem::last_write_time(dll, ec);
		uint32_t                        generation = 0;
		if (ec) {
			seen = {};
		} else if (Load(dll, generation + 1)) {
			generation++;
		} else {
			seen = {};
		}
		std::thread(Watch, seen, generation).detach();
	} catch (const std::exception& e) {
		Print("HotHLE: cannot start: %s\n", e.what());
	}
}

#else

void Start() {
	Print("HotHLE: not supported on this platform\n");
}

#endif

} // namespace

bool IsEnabled() {
	return !Directory().empty();
}

bool WriteImportSlot(const std::string& symbol_name, uint64_t slot_vaddr, uint64_t value,
                     bool resolved) {
	std::call_once(g_start_once, Start);

	const auto      nid = NidOf(symbol_name);
	std::lock_guard lock(g_mutex);

	auto&      slots  = g_slots[nid];
	const auto active = g_active.find(nid);
	Slot*      slot   = nullptr;
	for (auto& s: slots) {
		if (s.vaddr == slot_vaddr) {
			slot = &s;
			break;
		}
	}
	if (slot == nullptr) {
		slots.push_back(Slot {slot_vaddr, value, resolved});
		if (active != g_active.end()) {
			Print("HotHLE: patched %s %s (%zu slots)\n", nid.c_str(), active->second.name.c_str(),
			      slots.size());
		}
	} else if (active == g_active.end() || value != active->second.func) {
		slot->original = value;
		slot->resolved = resolved;
	}

	return WriteSlot(slot_vaddr, active != g_active.end() ? active->second.func : value);
}

void OnProgramUnloading(uint64_t base_vaddr, uint64_t size) {
	if (!IsEnabled()) {
		return;
	}
	const auto      inside = [&](uint64_t a) { return a >= base_vaddr && a < base_vaddr + size; };
	std::lock_guard lock(g_mutex);
	for (auto& [nid, slots]: g_slots) {
		// The program's own import slots are about to be freed.
		std::erase_if(slots, [&](const Slot& s) { return inside(s.vaddr); });
		// The linker re-relocates slots whose current target lies in the unloaded program. A
		// patched slot holds the patch instead, so put the original back for the linker to see;
		// its re-relocation goes through WriteImportSlot and re-applies the patch.
		if (g_active.contains(nid)) {
			for (const auto& s: slots) {
				if (inside(s.original)) {
					WriteSlot(s.vaddr, s.original);
				}
			}
		}
	}
}

} // namespace Loader::HotHle
