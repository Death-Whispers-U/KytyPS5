#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_HOT_HLE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_HOT_HLE_H_

#include <cstdint>
#include <string>

// Development aid: replaces imported HLE functions while a game runs.
//
// Set KYTY_HOT_HLE to a directory. The emulator then watches <dir>/kyty_hot.dll and, each time
// it changes, loads a copy and points the game's import slots for every function the DLL
// exports at the new code. Functions dropped from the DLL get their original target back.
//
// Only GOT/PLT import slots (GLOB_DAT, JUMP_SLOT) are redirected. Addresses the game obtained
// another way (dlsym, a pointer it copied earlier) keep their old target.

namespace Loader::HotHle {

// ABI shared with the patch DLL. Bump when either struct changes.
constexpr uint32_t ABI_VERSION = 1;

struct Host {
	uint32_t version;
	void (*log)(const char* text);
	// The target an import slot had before any patch: preferably the emulator's own HLE
	// function, otherwise the unresolved-import stub. nullptr if the game has no slot for this
	// NID yet, so patches must look it up lazily, from inside the patched function.
	void* (*original)(const char* nid);
};

struct Entry {
	const char* nid;  // 11-character NID
	const char* name; // for log lines only
	void*       func; // must use the guest (SysV) calling convention
};

// The patch DLL exports both. The version is checked before the entries are read.
using AbiVersionFunc = uint32_t (*)();
using EntriesFunc    = const Entry* (*)(const Host* host, uint32_t* count);

[[nodiscard]] bool IsEnabled();

// Writes an import slot during relocation, under the same lock the patch watcher uses, so a
// relocation and a patch swap can never overwrite each other. Records the slot and its original
// target, and writes the active patch instead if one exists for this symbol.
// Returns true if the slot's contents changed.
bool WriteImportSlot(const std::string& symbol_name, uint64_t slot_vaddr, uint64_t value,
                     bool resolved);

// Call before a program is unloaded and its importers are re-relocated. Forgets the program's own
// slots, and un-patches slots whose original target lies inside it so the re-relocation sees and
// re-records them.
void OnProgramUnloading(uint64_t base_vaddr, uint64_t size);

} // namespace Loader::HotHle

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_HOT_HLE_H_ */
