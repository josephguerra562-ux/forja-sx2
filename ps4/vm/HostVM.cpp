// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#include "Host.h"
#include "GS/GS.h"
#include "VMManager.h"
#include "MTGS.h"
#include "Config.h"
#include "BuildVersion.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/WindowInfo.h"
#include "INISettingsInterface.h"
#include "PerformanceMetrics.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "common/Timer.h"
#include "ps4/core/IPUProfilePS4.h"
#include "ps4/core/EEEventProfilePS4.h"
#include "ps4/core/GSProfilePS4.h"
#include "ps4/core/WorkerProfilePS4.h"
#include "SessionStats.h"
#include "ps4/core/ThreadAffinityPS4.h"
#include "R5900.h"
#include "Memory.h"
#include "OrbisEEProf.h" // compilation 110: g_orbis_ee_waiting
#include "ForjaGsPhase.h"
#include <unistd.h>
#include <string>
extern double GetVerticalFrequency(); // pcsx2/GS.h (Counters.cpp): the PS2's refresh at the game's own rate (compilation 77)
#include <mutex>
#include <cstring>
#include <deque>
#include <future>
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <functional>
#include <iterator>

// (AI-assisted) CPU work is queued to the thread that owns VMManager and its memory.
static pthread_t s_cpu_thread;
static std::mutex s_queue_mutex;
static std::deque<std::function<void()>> s_cpu_queue;
static std::atomic<bool> s_cpu_queue_pending{false};
static u64 s_cpu_queue_drain_locks = 0; // CPU thread only.
u64 VMHostGetCPUQueueLockCount() { return s_cpu_queue_drain_locks; }
void VMHostSetCPUThread() { s_cpu_thread = pthread_self(); }
void VMHostDrainCPUQueue()
{
	// (AI-assisted) This runs at every EE event. Avoid locking an empty queue;
	// producers and the consumer update the hint while holding the same mutex.
	// Concurrent enqueue near the last pop still needs proper console testing.
	while (s_cpu_queue_pending.load(std::memory_order_acquire))
	{
		std::function<void()> task;
		{
			std::lock_guard lock(s_queue_mutex);
			++s_cpu_queue_drain_locks;
			if (s_cpu_queue.empty())
			{
				s_cpu_queue_pending.store(false, std::memory_order_release);
				break;
			}
			task = std::move(s_cpu_queue.front());
			s_cpu_queue.pop_front();
			s_cpu_queue_pending.store(!s_cpu_queue.empty(), std::memory_order_release);
		}
		task();
	}
}
void VMHostPoll();
extern void ProbeLog(const char*, ...);

// Compilation 106: where the game's code spends the EE's time, to find loops that only wait: 200 times a second a helper thread reads the PS2's
// program counter, which the recompiler writes at the end of each block, into buckets of 16 bytes, and every 1000 samples (~5 s) logs the busiest. A
// race is harmless: a sample off by one block. It runs for everyone so reports bring it too; the marker vm-sin-medidor-ee turns it off, and with the
// marker vm-ee-pc-codigo it also logs the game's instructions around the 3 busiest (never in a report: ReportLogLines keeps only "EE guest PC:").
// Compilation 110: a sample with the EE thread in a wait (the speed limiter, the GS's queue, the VU1; OrbisEEWaitScope) is counted apart. Its PC is wherever
// the EE stopped, often the PS2's own idle loop (00081fc0, EENULL_START): Budokai 3 and Tourist Trophy showed it at 25-29 % of the samples exactly
// when they had time to spare (reports F-5E51 and F-C01D, 2026-11-02). "samples" now counts the working ones only, so its shares are of the EE's work,
// and "EE guest PC: waiting=W of T" says how much was left out. The same thread reads what the GS thread is doing (ForjaGsPhase.h): "GS thread split".
static void* ForjaEePcMeterThread(void*)
{
	PS4ThreadAffinity::PinCurrentThreadToHelpers("ee pc");
	const bool with_code = access("/data/ForjaSX2/vm-ee-pc-codigo", F_OK) == 0;
	struct Bucket
	{
		u32 pc;
		u32 count;
	};
	static constexpr u32 kBuckets = 512;
	static constexpr u32 kWindow = 1000;
	static constexpr u32 kShown = 8;
	Bucket buckets[kBuckets] = {};
	u32 samples = 0, dropped = 0, waiting = 0, total = 0;
	u32 gs_phases[ForjaGsPhase::Count] = {};
	for (;;)
	{
		usleep(5000);
		if (VMManager::GetState() != VMState::Running)
			continue;
		const int gs_phase = ForjaGsPhase::now.phase.load(std::memory_order_relaxed);
		gs_phases[(gs_phase >= 0 && gs_phase < ForjaGsPhase::Count) ? gs_phase : ForjaGsPhase::Other]++;
		if (g_orbis_ee_waiting.load(std::memory_order_relaxed) > 0)
			waiting++;
		else
		{
			samples++;
			const u32 pc = cpuRegs.pc & ~15u;
			u32 slot = (pc >> 4) * 2654435761u >> 23; // 512 slots
			u32 tries = 0;
			while (buckets[slot].count != 0 && buckets[slot].pc != pc && tries < 8)
			{
				slot = (slot + 1) % kBuckets;
				tries++;
			}
			if (buckets[slot].count == 0 || buckets[slot].pc == pc)
			{
				buckets[slot].pc = pc;
				buckets[slot].count++;
			}
			else
			{
				dropped++;
			}
		}
		if (++total < kWindow)
			continue;
		if (samples > 0)
		{
			std::partial_sort(std::begin(buckets), std::begin(buckets) + kShown, std::end(buckets), [](const Bucket& a, const Bucket& b) { return a.count > b.count; });
			char line[256];
			int size = std::snprintf(line, sizeof(line), "EE guest PC: samples=%u dropped=%u top=", samples, dropped);
			for (u32 i = 0; i < kShown && buckets[i].count != 0 && size > 0 && size < static_cast<int>(sizeof(line)) - 20; i++)
				size += std::snprintf(line + size, sizeof(line) - size, "%s%08x:%u", i ? "," : "", buckets[i].pc, buckets[i].count);
			ProbeLog("%s\n", line);
			if (with_code && eeMem)
			{
				for (u32 i = 0; i < 3 && buckets[i].count != 0; i++)
				{
					const u32 address = buckets[i].pc & 0x01FFFFFFu;
					if (address + 32 > Ps2MemSize::MainRam)
						continue;
					const u32* code = reinterpret_cast<const u32*>(eeMem->Main + address);
					ProbeLog("EE guest code %08x: %08x %08x %08x %08x %08x %08x %08x %08x\n", buckets[i].pc, code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
				}
			}
		}
		ProbeLog("EE guest PC: waiting=%u of %u samples (the EE thread in a wait: speed limiter, GS queue, VU1)\n", waiting, total);
		ProbeLog("GS thread split: samples=%u wait_work=%u wait_vu=%u transfer=%u draw=%u readback=%u vsync=%u other=%u\n", total,
			gs_phases[ForjaGsPhase::WaitWork], gs_phases[ForjaGsPhase::WaitVU], gs_phases[ForjaGsPhase::Transfer], gs_phases[ForjaGsPhase::Draw],
			gs_phases[ForjaGsPhase::Readback], gs_phases[ForjaGsPhase::VSync], gs_phases[ForjaGsPhase::Other]);
		std::memset(buckets, 0, sizeof(buckets));
		std::memset(gs_phases, 0, sizeof(gs_phases));
		samples = dropped = waiting = total = 0;
	}
	return nullptr;
}

static void ForjaStartEePcMeter()
{
	static bool s_started = false;
	if (s_started)
		return;
	s_started = true;
	if (access("/data/ForjaSX2/vm-sin-medidor-ee", F_OK) == 0)
		return;
	pthread_t thread;
	if (pthread_create(&thread, nullptr, ForjaEePcMeterThread, nullptr) == 0)
		pthread_detach(thread);
}
extern void ForjaLogFramePacing(); // compilation 65: ps4/core/GSRendererPS4.cpp, the frame pacing meter
extern bool g_orbis_fg_movie;   // pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp: a movie playing (main.cpp sets it)
extern bool g_orbis_fg_engaged; // ...and frame generation generating now
extern bool g_orbis_fg_wanted;
// Compilation 80: the session's scenes and controllers (SessionStats.h): every draw of the GS (GSRendererHW.cpp), and the
// controllers connected (main.cpp, the second pad).
extern std::atomic<u64> g_forja_gs_draws_total;
extern std::atomic<int> g_forja_pads;
// Compilation 82: the EE thread's waits for the GS thread (pcsx2/MTGS.cpp, the PS5 port's counters, in TSC ticks): two
// refreshes already queued (vsyncq), its ring full (stall), a wait for it to empty (waitgs); and the GS thread's own wait for
// work. Only the EE thread writes the first three and only the GS thread the last: a meter's reads, no lock.
extern unsigned long long g_orbis_ee_vsyncq_ticks, g_orbis_ee_stall_ticks, g_orbis_ee_waitgs_ticks, g_orbis_gs_idle_ticks;
extern unsigned long long g_forja_gif_wait_ticks[3];
// Compilation 85: the GameDB's software draws (GSRendererHW.cpp), the GS thread's waits for their workers and each worker's
// drawing (pcsx2/GS/Renderers/SW/GSRasterizer.cpp, the PS5 port's counters), for the "Software draws" line.
extern unsigned long long g_forja_sw_draws, g_forja_sw_ticks, g_orbis_gs_swsync_ticks, g_orbis_sw_busy_ticks[16];
// Compilation 117: whether they use the generated code (the JIT) and how much of its block it filled
// (pcsx2/GS/Renderers/SW/GSDrawScanline.cpp).
extern std::atomic<bool> g_forja_sw_jit;
extern "C" int ForjaSwJitCodeKB();
// Forja 1.06: what held the last 5 s back (SessionStats.h, ForjaSession::Limit), for UIVM.cpp's HUD.
std::atomic<int> g_forja_limit{0};

namespace BuildVersion
{
	int GitTagHi = 0, GitTagMid = 0, GitTagLo = 1;
}

void Host::CommitBaseSettingChanges()
{
}

void Host::AddOSDMessage(std::string message, float)
{
	Console.WriteLn("OSD: %s", message.c_str());
}

void Host::AddIconOSDMessage(std::string key, const char*, std::string_view message, float)
{
	Console.WriteLn("OSD [%s]: %.*s", key.c_str(), static_cast<int>(message.size()), message.data());
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float)
{
	Console.WriteLn("OSD [%s]: %s", key.c_str(), message.c_str());
}

void Host::RemoveKeyedOSDMessage(std::string)
{
}

void Host::ClearOSDMessages()
{
}

void Host::ReportInfoAsync(std::string_view, std::string_view)
{
}

extern "C" void forja_guard_mark(const char* reason); // ps4/runtime/ps4_crashlog.cpp (compilation 131)

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
	Console.Error("Host Error [%.*s]: %.*s", static_cast<int>(title.size()), title.data(),
		static_cast<int>(message.size()), message.data());
	// Compilation 131: the EE jumped where there is no code and the game stays stopped (Need for Speed Most Wanted, 2026-11-13, the 130:
	// "Jump to unaligned address" 356 s in, after a storm of TLB misses). Nothing closes, so nothing else would tell the guardian.
	if (title.find("R5900 Exception") != std::string_view::npos)
		forja_guard_mark("ee exception");
}

bool Host::RequestResetSettings(bool, bool, bool, bool, bool)
{
	return false;
}

void Host::RequestResizeHostDisplay(s32, s32)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (pthread_equal(pthread_self(), s_cpu_thread))
	{
		function();
		return;
	}
	auto task = std::make_shared<std::packaged_task<void()>>(std::move(function));
	auto done = task->get_future();
	{
		std::lock_guard lock(s_queue_mutex);
		s_cpu_queue.emplace_back([task]() { (*task)(); });
		s_cpu_queue_pending.store(true, std::memory_order_release);
	}
	if (block)
		done.wait();
}
void Host::RunOnGSThread(std::function<void()> function)
{
	MTGS::RunOnGSThread(std::move(function));
}

void Host::RefreshGameListAsync(bool)
{
}

void Host::CancelGameListRefresh()
{
}

void Host::RequestVMShutdown(bool, bool, bool)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnVMStarting()
{
	Console.WriteLn("VM starting");
}

void Host::OnVMStarted()
{
	Console.WriteLn("VM started");
}

void Host::OnVMDestroyed()
{
	Console.WriteLn("VM destroyed");
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string&, const std::string&, const std::string&, u32, u32)
{
	Console.WriteLn("Game changed: %s", title.c_str());
}

std::string Host::TranslatePluralToString(const char*, const char* msg, const char*, int count)
{
	// Forja SX2 (compilation 33): no translations, but the message itself, with its count. An empty string made the patch
	// notice say "No cheats or patches ... are found / enabled" while one was on (God of War II, 2026-10-16).
	std::string text(msg ? msg : "");
	for (size_t pos = text.find("%n"); pos != std::string::npos; pos = text.find("%n", pos))
		text.replace(pos, 2, std::to_string(count));
	return text;
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return true;
}

void Host::OpenURL(const std::string_view)
{
}

bool Host::CopyTextToClipboard(const std::string_view)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return "";
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
	WindowInfo info{};
	info.type = WindowInfo::Type::Surfaceless;
	info.surface_width = 1920;
	info.surface_height = 1080;
	info.surface_refresh_rate = 60.0f;
	return info;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool)
{
}

void Host::SetDefaultUISettings(SettingsInterface&)
{
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return nullptr;
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	if (lhs < rhs)
		return -1;
	if (lhs > rhs)
		return 1;
	return 0;
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>&)
{
	// No persistent settings storage on PS4; use defaults.
}

void Host::OnPerformanceMetricsUpdated()
{
	// (AI-assisted) This callback runs on the GS thread, which owns these metrics.
	// Reading them from the CPU heartbeat would introduce unsynchronized access.
	static Common::Timer::Value first_update = Common::Timer::GetCurrentValue();
	static Common::Timer::Value last_log = 0;
	const auto now = Common::Timer::GetCurrentValue();
	// Forja SX2: keep the Vulkan pipeline cache on disk while playing (every 30 s, only when it changed),
	// so closing the app from the PS button does not throw away the compiled effects.
	static Common::Timer::Value last_cache_save = now;
	if (Common::Timer::ConvertValueToSeconds(now - last_cache_save) >= 30.0)
	{
		last_cache_save = now;
		if (g_vulkan_shader_cache)
			g_vulkan_shader_cache->FlushPipelineCacheInBackground();
	}
	ForjaStartEePcMeter();
	if (last_log != 0 && Common::Timer::ConvertValueToSeconds(now - last_log) < 5.0)
		return;
	const double profile_interval = last_log ? Common::Timer::ConvertValueToSeconds(now - last_log) : 0.0;
	last_log = now;
	const auto fps_method = PerformanceMetrics::GetInternalFPSMethod();
	const char* method = fps_method == PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister ? "GS-reg" :
		fps_method == PerformanceMetrics::InternalFPSMethod::DISPFBBlit ? "FB-blit" : "unavailable";
	const double ee_usage = PerformanceMetrics::GetCPUThreadUsage();
	const double gs_usage = PerformanceMetrics::GetGSThreadUsage();
	const double vu_usage = PerformanceMetrics::GetVUThreadUsage();
	const float gpu_ms = PerformanceMetrics::GetGPUAverageTime();
	// Forja 1.06: what held these 5 s back (ForjaSession::ClassifyLimit), and the window for the session's summary.
	const int limit = ForjaSession::ClassifyLimit(PerformanceMetrics::GetSpeed(), ee_usage, vu_usage, gs_usage,
		PerformanceMetrics::GetGPUUsage());
	g_forja_limit.store(limit, std::memory_order_relaxed);
	// Compilation 80: the GS's draws in these 5 s, for the scene.
	static u64 last_draws = 0;
	const u64 draws = g_forja_gs_draws_total.load(std::memory_order_relaxed);
	const double draws_per_s = profile_interval > 0.0 ? static_cast<double>(draws - last_draws) / profile_interval : 0.0;
	last_draws = draws;
	if (profile_interval > 0.0)
	{
		// Compilation 77: the limiting part's time per PS2 refresh against a refresh at the game's own rate (the summary's "cuánto
		// más barato").
		const double hz = GetVerticalFrequency();
		const double budget_ms = hz > 20.0 && hz < 100.0 ? 1000.0 / hz : 0.0;
		const double limit_ms = limit == ForjaSession::LimitEE ? PerformanceMetrics::GetCPUThreadAverageTime() :
		                        limit == ForjaSession::LimitVU1 ? PerformanceMetrics::GetVUThreadAverageTime() :
		                        limit == ForjaSession::LimitGS  ? PerformanceMetrics::GetGSThreadAverageTime() :
		                        limit == ForjaSession::LimitGPU ? static_cast<double>(gpu_ms) : 0.0;
		ForjaSession::Window window;
		window.speed = PerformanceMetrics::GetSpeed();
		window.fps = PerformanceMetrics::GetInternalFPS();
		window.limit = limit;
		window.movie = g_orbis_fg_movie;
		// Compilation 106: only while it's on. The flag stayed up after turning it off, and Guitar Hero III's summary said "generated 100 % of the time".
		window.fg_generating = g_orbis_fg_wanted && g_orbis_fg_engaged;
		window.limit_ms = limit_ms;
		window.budget_ms = budget_ms;
		// Compilation 80: the scene (game, menu, movie, load), the controllers, and whether a second part was at its limit too:
		// Guitar Hero's EE at 99 % and GS at 93 % (2026-10-23) said only "EE".
		window.scene = ForjaSession::ClassifyScene(g_orbis_fg_movie, PerformanceMetrics::GetFPS(), window.fps, draws_per_s);
		window.pads = g_forja_pads.load(std::memory_order_relaxed);
		double usages[] = {ee_usage, vu_usage, gs_usage, PerformanceMetrics::GetGPUUsage()};
		std::sort(std::begin(usages), std::end(usages), std::greater<double>());
		window.two_at_limit = window.speed < 97.0 && usages[1] >= 80.0 && usages[0] - usages[1] <= 10.0;
		ForjaSession::AddWindow(window);
	}
	ProbeLog("Performance: seconds=%.1f PS2_Hz=%.2f Speed_pct=%.2f Game_FPS=%.2f FPS_method=%s "
		"Frame_ms=%.2f Frame_max_ms=%.2f EE_pct=%.2f EE_ms=%.2f GS_pct=%.2f GS_ms=%.2f "
		"VU1_pct=%.2f VU1_ms=%.2f CPU_timing=%s CPU_clock=owner_sampled_100ms GPU_pct=%.2f GPU_ms=%.2f GPU_timing=%s Limit=%s\n",
		Common::Timer::ConvertValueToSeconds(now - first_update), PerformanceMetrics::GetFPS(),
		PerformanceMetrics::GetSpeed(), PerformanceMetrics::GetInternalFPS(), method,
		PerformanceMetrics::GetAverageFrameTime(), PerformanceMetrics::GetMaximumFrameTime(),
		ee_usage, PerformanceMetrics::GetCPUThreadAverageTime(), gs_usage, PerformanceMetrics::GetGSThreadAverageTime(),
		vu_usage, PerformanceMetrics::GetVUThreadAverageTime(),
		(ee_usage + gs_usage + vu_usage > 0.0) ? "samples" : "no_samples",
		PerformanceMetrics::GetGPUUsage(), gpu_ms, gpu_ms > 0.0f ? "samples" : "no_samples", ForjaSession::LimitName(limit));
	// Compilation 82: how long the EE waited for the GS each PS2 refresh. Guitar Hero III's songs (2026-10-24) had the EE and the GS
	// at ~50 ms a refresh in every window, rising and falling together (44.9/47.8, 52.0/50.5, 31.1/32.4): the EE may be
	// waiting for the GS, not working, and "Limit=EE" would then point at the wrong part.
	{
		static unsigned long long last[4] = {};
		const unsigned long long now_ticks[4] = {g_orbis_ee_vsyncq_ticks, g_orbis_ee_stall_ticks, g_orbis_ee_waitgs_ticks,
			g_orbis_gs_idle_ticks};
		double ms[4] = {};
		const double refreshes = profile_interval * PerformanceMetrics::GetFPS();
		for (int i = 0; i < 4; i++)
		{
			const unsigned long long delta = now_ticks[i] >= last[i] ? now_ticks[i] - last[i] : 0;
			last[i] = now_ticks[i];
			ms[i] = refreshes > 0.0 ? static_cast<double>(delta) / 1600000.0 / refreshes : 0.0; // the PS4's TSC runs at ~1.6 GHz
		}
		if (profile_interval > 0.0)
			ProbeLog("EE waits for the GS: per_refresh_ms total=%.2f vsync_queue=%.2f ring_full=%.2f wait_gs=%.2f of EE_ms=%.2f; "
					 "GS idle per_refresh_ms=%.2f of GS_ms=%.2f\n",
				ms[0] + ms[1] + ms[2], ms[0], ms[1], ms[2], PerformanceMetrics::GetCPUThreadAverageTime(), ms[3],
				PerformanceMetrics::GetGSThreadAverageTime());
		// Compilation 100: which GIF path's buffer the EE waits on.
		static unsigned long long last_path[3] = {};
		double path_ms[3] = {};
		for (int i = 0; i < 3; i++)
		{
			const unsigned long long now_path = g_forja_gif_wait_ticks[i];
			path_ms[i] = refreshes > 0.0 ? static_cast<double>(now_path - last_path[i]) / 1600000.0 / refreshes : 0.0;
			last_path[i] = now_path;
		}
		if (profile_interval > 0.0)
			ProbeLog("EE waits for GIF room: per_refresh_ms path1=%.2f path2=%.2f path3=%.2f\n", path_ms[0], path_ms[1], path_ms[2]);
	}
	// Compilation 85: the software draws in these 5 s, a second: how many, the GS thread's whole time in them, its waits for the
	// workers, and each of the 3 workers' drawing. Whole time much more than the workers' drawing: the cost is the setup around
	// each draw, not its pixels (Guitar Hero's crowd went from ~36 to ~28 ms a pass with 3 workers, not to ~12).
	{
		static unsigned long long last_draws = 0, last_ticks = 0, last_sync = 0, last_busy[3] = {};
		const unsigned long long draws_now = g_forja_sw_draws, ticks_now = g_forja_sw_ticks, sync_now = g_orbis_gs_swsync_ticks;
		const double to_ms_per_s = profile_interval > 0.0 ? 1.0 / 1600000.0 / profile_interval : 0.0; // TSC at ~1.6 GHz
		if (profile_interval > 0.0 && draws_now > last_draws)
		{
			double busy[3];
			for (int i = 0; i < 3; i++)
				busy[i] = static_cast<double>(g_orbis_sw_busy_ticks[i] - last_busy[i]) * to_ms_per_s;
			ProbeLog("Software draws: per_second=%.0f gs_thread_ms=%.1f waiting_workers_ms=%.1f workers_drawing_ms=%.1f,%.1f,%.1f "
					 "jit=%d jit_code_kb=%d\n",
				static_cast<double>(draws_now - last_draws) / profile_interval, static_cast<double>(ticks_now - last_ticks) * to_ms_per_s,
				static_cast<double>(sync_now - last_sync) * to_ms_per_s, busy[0], busy[1], busy[2],
				g_forja_sw_jit.load(std::memory_order_relaxed) ? 1 : 0, ForjaSwJitCodeKB());
		}
		last_draws = draws_now;
		last_ticks = ticks_now;
		last_sync = sync_now;
		for (int i = 0; i < 3; i++)
			last_busy[i] = g_orbis_sw_busy_ticks[i];
	}
	PS4IPUProfile::LogInterval(profile_interval);
	PS4EEEventProfile::LogInterval(profile_interval);
	PS4GSProfile::LogInterval(profile_interval);
	PS4WorkerProfile::LogInterval(profile_interval);
	ForjaLogFramePacing(); // compilation 65: ps4/core/GSRendererPS4.cpp
	ProbeLog("Render configuration: internal_scale=%.2f aspect_mode=%u\n",
		static_cast<double>(GSConfig.UpscaleMultiplier), static_cast<unsigned>(GSConfig.AspectRatio));
}

void Host::CheckForSettingsChanges(const Pcsx2Config&)
{
}

void Host::PumpMessagesOnCPUThread()
{
	::VMHostPoll();
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view, const std::string_view message, char* buffer, size_t capacity)
{
	if (capacity < message.size())
		return -1;
	std::memcpy(buffer, message.data(), message.size());
	return static_cast<s32>(message.size());
}
