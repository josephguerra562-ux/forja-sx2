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
#include <string>
#include <mutex>
#include <cstring>
#include <deque>
#include <future>
#include <pthread.h>
#include <atomic>

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

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
	Console.Error("Host Error [%.*s]: %.*s", static_cast<int>(title.size()), title.data(),
		static_cast<int>(message.size()), message.data());
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
	ProbeLog("Performance: seconds=%.1f PS2_Hz=%.2f Speed_pct=%.2f Game_FPS=%.2f FPS_method=%s "
		"Frame_ms=%.2f Frame_max_ms=%.2f EE_pct=%.2f EE_ms=%.2f GS_pct=%.2f GS_ms=%.2f "
		"VU1_pct=%.2f VU1_ms=%.2f CPU_timing=%s CPU_clock=owner_sampled_100ms GPU_pct=%.2f GPU_ms=%.2f GPU_timing=%s\n",
		Common::Timer::ConvertValueToSeconds(now - first_update), PerformanceMetrics::GetFPS(),
		PerformanceMetrics::GetSpeed(), PerformanceMetrics::GetInternalFPS(), method,
		PerformanceMetrics::GetAverageFrameTime(), PerformanceMetrics::GetMaximumFrameTime(),
		ee_usage, PerformanceMetrics::GetCPUThreadAverageTime(), gs_usage, PerformanceMetrics::GetGSThreadAverageTime(),
		vu_usage, PerformanceMetrics::GetVUThreadAverageTime(),
		(ee_usage + gs_usage + vu_usage > 0.0) ? "samples" : "no_samples",
		PerformanceMetrics::GetGPUUsage(), gpu_ms, gpu_ms > 0.0f ? "samples" : "no_samples");
	PS4IPUProfile::LogInterval(profile_interval);
	PS4EEEventProfile::LogInterval(profile_interval);
	PS4GSProfile::LogInterval(profile_interval);
	PS4WorkerProfile::LogInterval(profile_interval);
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
