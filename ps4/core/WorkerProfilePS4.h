// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Sample worker execution separately from synchronization waits.
#pragma once

#if defined(PCSX2_PS4_VM_BOOT)
#include "common/Timer.h"
#include <atomic>
#include <cstdint>

extern void ProbeLog(const char*, ...);

namespace PS4WorkerProfile
{
	enum Kind { VUExecute, VUUnpack, VUIdle, GSWaitVU, GSTransfer, Count };
	inline std::atomic<bool> enabled{false};
	struct Counter
	{
		// Exactly one producer per kind (VU or GS), including nested scopes.
		std::uint32_t random = 0x7256a913;
		std::uint64_t calls = 0, samples = 0, elapsed = 0;
		std::atomic<std::uint64_t> published_calls{0}, published_samples{0}, published_elapsed{0};
		std::uint64_t last_calls = 0, last_samples = 0, last_elapsed = 0; // GS reader only.
	};
	inline Counter counters[Count];
	class Scope
	{
		Counter* m_counter = nullptr;
		Common::Timer::Value m_start = 0;
	public:
		explicit Scope(Kind kind)
		{
			if (!enabled.load(std::memory_order_relaxed))
				return;
			auto& c = counters[kind];
			++c.calls;
			c.random = c.random * 1664525U + 1013904223U;
			if ((c.random >> 22) != 0) // 1/1024; no clock/syscall on unsampled work.
				return;
			m_counter = &c;
			m_start = Common::Timer::GetCurrentValue();
		}
		~Scope()
		{
			if (!m_counter)
				return;
			auto& c = *m_counter;
			c.elapsed += Common::Timer::GetCurrentValue() - m_start;
			++c.samples;
			c.published_elapsed.store(c.elapsed, std::memory_order_relaxed);
			c.published_calls.store(c.calls, std::memory_order_relaxed);
			c.published_samples.store(c.samples, std::memory_order_release);
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};
	inline void LogInterval(double seconds)
	{
		if (!enabled.load(std::memory_order_relaxed))
			return;
		constexpr const char* names[] = {"vu_execute", "vu_unpack", "vu_wait_work", "gs_wait_vu", "gs_gif_transfer"};
		for (unsigned i = 0; i < Count; ++i)
		{
			auto& c = counters[i];
			const auto samples = c.published_samples.load(std::memory_order_acquire);
			const auto elapsed = c.published_elapsed.load(std::memory_order_relaxed);
			const auto calls = c.published_calls.load(std::memory_order_relaxed);
			// Independent atomic totals can straddle a sample: these are approximate
			// wall-time estimates, never CPU percentages. Needs proper console testing.
			if (seconds > 0.0 && samples > c.last_samples)
				ProbeLog("Worker profile: scope=%s interval_s=%.3f calls_approx=%llu samples=%llu sample_ratio=1/1024 sampled_wall_ms=%.3f estimated_wall_ms_per_second=%.2f includes_wait_and_preemption=yes\n",
					names[i], seconds, static_cast<unsigned long long>(calls - c.last_calls),
					static_cast<unsigned long long>(samples - c.last_samples),
					Common::Timer::ConvertValueToMilliseconds(elapsed - c.last_elapsed),
					Common::Timer::ConvertValueToMilliseconds(elapsed - c.last_elapsed) * 1024.0 / seconds);
			c.last_calls = calls;
			c.last_samples = samples;
			c.last_elapsed = elapsed;
		}
	}
}
#define PS4_WORKER_SCOPE(kind) PS4WorkerProfile::Scope ps4_worker_scope_(PS4WorkerProfile::kind)
#else
#define PS4_WORKER_SCOPE(kind) do {} while (0)
#endif
