// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Platform services and adapters for the PS4 application.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "Host.h"
#include "Achievements.h"
#include "VMManager.h"
#include "common/Error.h"
#include "SaveState.h"
#include "Input/InputManager.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ImGuiOverlays.h"
#include "common/HTTPDownloader.h"
#include "common/HostSys.h"
#include "imgui.h"
#include <cstdarg>
#include <cstdio>
#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <ucontext.h>
#include <unistd.h>

namespace BuildVersion
{
	const char* GitRev = "FORJASX2-1.00 (base PS4SX2-EMU-1.00)";
}
namespace Host
{
	void OnAchievementsLoginRequested(Achievements::LoginRequestReason) {}
	void OnAchievementsRefreshed() {}
	void OnAchievementsHardcoreModeChanged(bool) {}
	void OnAchievementsLoginSuccess(const char*, u32, u32, u32) {}
	void OnSaveStateLoading(std::string_view) {}
	void OnSaveStateLoaded(std::string_view, bool) {}
	void OnSaveStateSaved(std::string_view) {}
	void SetMouseMode(bool, bool) {}
} // namespace Host
namespace Common
{
	bool InhibitScreensaver(bool) { return false; }
	bool PlaySoundAsync(const char*) { return false; }
} // namespace Common
namespace PageFaultHandler
{
	// (AI-assisted) Orbis delivers SIGSEGV/SIGBUS with an x86-64 ucontext; the
	// OpenOrbis signal header defines REG_RIP=16 and REG_ERR=19 for this layout.
	static std::recursive_mutex s_exception_handler_mutex;
	static bool s_in_exception_handler = false;
	static bool s_installed = false;
	static struct sigaction s_previous_sigsegv{};
	static struct sigaction s_previous_sigbus{};

	// (AI-assisted) The PS4 kernel places mcontext 0x30 bytes after the
	// FreeBSD-compatible SDK declaration. Reading uc_mcontext directly returns
	// another saved register instead of RIP/ERR and makes every Fastmem fault
	// look unhandled. These offsets were verified by the VM crash context.
	static u64 ReadOrbisMcontext(const void* context, const u32 field_offset)
	{
		u64 value;
		std::memcpy(&value, static_cast<const u8*>(context) + 0x40 + field_offset, sizeof(value));
		return value;
	}

	static void SignalHandler(int signal, siginfo_t* info, void* context)
	{
		HandlerResult result = HandlerResult::ExecuteNextHandler;
		if (info && context)
		{
			void* const exception_pc = reinterpret_cast<void*>(ReadOrbisMcontext(context, 0xa0));
			void* const exception_address = reinterpret_cast<void*>(ReadOrbisMcontext(context, 0x88));
			const bool is_write = (ReadOrbisMcontext(context, 0x98) & 2) != 0;

			s_exception_handler_mutex.lock();
			if (!s_in_exception_handler)
			{
				s_in_exception_handler = true;
				result = HandlePageFault(exception_pc, exception_address, is_write);
				s_in_exception_handler = false;
			}
			s_exception_handler_mutex.unlock();
		}

		if (result == HandlerResult::ContinueExecution)
			return;

		// Preserve any app-level crash reporter that was installed before Fastmem.
		static constexpr char message[] = "PCSX2 Fastmem: unhandled Orbis page fault\n";
		write(STDERR_FILENO, message, sizeof(message) - 1);
		const struct sigaction& previous = (signal == SIGBUS) ? s_previous_sigbus : s_previous_sigsegv;
		if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction &&
			previous.sa_sigaction != SignalHandler)
		{
			previous.sa_sigaction(signal, info, context);
			return;
		}
		if (previous.sa_handler && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN)
		{
			previous.sa_handler(signal);
			return;
		}
		struct sigaction default_action{};
		default_action.sa_handler = SIG_DFL;
		sigemptyset(&default_action.sa_mask);
		sigaction(signal, &default_action, nullptr);
		raise(signal);
		_exit(128 + signal);
	}

	bool Install(Error* error)
	{
		if (s_installed)
			return true;

		struct sigaction action{};
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_SIGINFO | SA_NODEFER;
		action.sa_sigaction = SignalHandler;
		if (sigaction(SIGSEGV, &action, &s_previous_sigsegv) != 0)
		{
			Error::SetErrno(error, "sigaction(SIGSEGV) failed: ", errno);
			return false;
		}
		if (sigaction(SIGBUS, &action, &s_previous_sigbus) != 0)
		{
			Error::SetErrno(error, "sigaction(SIGBUS) failed: ", errno);
			return false;
		}
		s_installed = true;
		return true;
	}

	// POSIX sigaction is process-wide, so helper threads inherit the handler.
	bool InstallSecondaryThread() { return true; }
} // namespace PageFaultHandler
namespace VMManager::Internal
{
	void ResetVMHotkeyState() {}
} // namespace VMManager::Internal

// (AI-assisted) Minimal adapters required by the x86 recompiler.  The full
// legacy diagnostics/overlay implementation is outside this application.
namespace ps4::debug
{
	void set_line(int, const char*, ...) {}
} // namespace ps4::debug
std::atomic<int> g_orbis_waitloop_ext{1};
u64 g_orbis_waitloop_ext_blocks = 0;
u64 g_orbis_waitloop_ext_runs = 0;
std::string OrbisLogPath(const char* name)
{
	return std::string("/data/ForjaSX2/") + (name ? name : "");
}

const HotkeyInfo g_common_hotkeys[] = {{nullptr, nullptr, nullptr, nullptr}};
const HotkeyInfo g_host_hotkeys[] = {{nullptr, nullptr, nullptr, nullptr}};
namespace InputManager
{
	std::optional<u32> ConvertHostKeyboardStringToCode(std::string_view) { return std::nullopt; }
	std::optional<std::string> ConvertHostKeyboardCodeToString(u32) { return std::nullopt; }
} // namespace InputManager
std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string) { return nullptr; }
namespace ImGuiManager
{
	float GetGlobalScale() { return 1.0f; }
	ImFont* GetFixedFont() { return ImGui::GetIO().Fonts->Fonts[0]; }
	float GetFontSizeStandard() { return 13.0f; }
} // namespace ImGuiManager
ImVec2 CalculatePerformanceOverlayTextPosition(OsdOverlayPos, float, const ImVec2&, float, float) { return ImVec2(0, 0); }

// Optional instrumentation inherited from the PS5 fork stays disabled.
std::atomic<int> g_orbis_vif_fast{0};
int g_orbis_swtex = 0, g_orbis_diag = 0, g_orbis_testpat = 0;
class GSTexture;
void OrbisDiagTexture(const char*, GSTexture*) {}
void OrbisHelperThreadAdd(pthread_t) {}
void OrbisHelperThreadRemove(pthread_t) {}
#if !defined(PCSX2_PS4_VM_BOOT_JIT)
void OrbisVU1Dump() {}
#endif
void OrbisCpuForget(int) {}
extern void ProbeLog(const char*, ...);
// ps4/runtime/ps4_runtime.cpp: the heap's diagnostics.
extern "C" const char* ps4rt_heap_source(void);
extern "C" size_t ps4rt_heap_size(void);
extern "C" size_t ps4rt_flexible_at_start(void);
extern "C" size_t ps4rt_heap_high_water(void);
extern "C" size_t ps4rt_overflow_heap_size(void);
extern "C" int ps4rt_heap_log_next(char* out, size_t size);
// Forja SX2: each stage line says how much regular flexible memory is free and how much it took since the previous
// one. Thread stacks, system modules, shared memory and the program's static buffers come out of it, and it stood
// at 3-4 MiB all through God of War II (2026-10-20). The heap lives in the system pool instead (heap_high_water_mb:
// how far its allocations have reached). The heap's own lines, written before this log exists, are copied in first.
extern "C" void orbis_stage(const char* message)
{
	static std::atomic<long long> s_last_kb{-1};
	char line[256];
	while (ps4rt_heap_log_next(line, sizeof(line)))
		ProbeLog("PS4 runtime: %s", line);
	const long long free_kb = static_cast<long long>(GetAvailablePhysicalMemory() / 1024);
	long long last_kb = s_last_kb.exchange(free_kb);
	if (last_kb < 0)
	{
		last_kb = static_cast<long long>(ps4rt_flexible_at_start() / 1024);
		ProbeLog("STAGE: app start (first malloc) flexible_free_kb=%lld heap=%s heap_mb=%zu\n", last_kb,
			ps4rt_heap_source(), ps4rt_heap_size() >> 20);
	}
	ProbeLog("STAGE: %s flexible_free_kb=%lld change_kb=%lld heap_high_water_mb=%zu overflow_heap_mb=%zu\n", message,
		free_kb, free_kb - last_kb, ps4rt_heap_high_water() >> 20, ps4rt_overflow_heap_size() >> 20);
}
// No JIT state exists in this interpreter-only application; reject savestates.
#if !defined(PCSX2_PS4_VM_BOOT_JIT)
bool SaveStateBase::vuJITFreeze() { return false; }
#endif
int OrbisDeferredPrintf(const char* format, ...)
{
	// (AI-assisted) Avoid the stdout FILE buffer: both v0.02 crashes were in
	// __fwritex -> memcpy with a null destination on the GS thread.
	char message[4096];
	va_list args;
	va_start(args, format);
	const int length = std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (length > 0)
		ProbeLog("%s", message);
	return length;
}
int OrbisDeferredFlush(FILE* file)
{
	return (file == stdout || file == stderr) ? 0 : std::fflush(file);
}
