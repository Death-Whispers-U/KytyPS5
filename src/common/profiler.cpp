#include "common/profiler.h"

#include "common/common.h"
#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <string>
#include <tracy/Tracy.hpp>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Profiler {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static void DescribeThread(HANDLE thread, const char* name) {
	// Looked up at run time: SetThreadDescription needs Windows 10 1607, and a static import
	// would stop the emulator from starting on older systems just to name threads.
	using SetThreadDescriptionFunc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const auto set_description = reinterpret_cast<SetThreadDescriptionFunc>(
	    reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
	if (set_description == nullptr) {
		return;
	}
	const int size = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
	if (size > 0) {
		std::wstring wide(static_cast<size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), size);
		set_description(thread, wide.c_str());
	}
}
#endif

void SetOsThreadName(uint64_t os_thread_id, const char* name) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (name == nullptr || os_thread_id == 0) {
		return;
	}
	HANDLE thread = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE,
	                           static_cast<DWORD>(os_thread_id));
	if (thread != nullptr) {
		DescribeThread(thread, name);
		CloseHandle(thread);
	}
#else
	(void)os_thread_id;
	(void)name;
#endif
}

void SetThreadName(const char* name) {
	if (name == nullptr) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Name the OS thread too, so debuggers, Process Explorer and per-thread CPU sampling can
	// tell threads apart without Tracy.
	DescribeThread(GetCurrentThread(), name);
#endif
	if (tracy::ProfilerAvailable()) {
		tracy::SetThreadName(name);
	}
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
