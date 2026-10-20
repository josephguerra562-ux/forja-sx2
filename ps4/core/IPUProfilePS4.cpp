// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) EE publishes sampled durations; GS periodically consumes them.
#include "IPUProfilePS4.h"

#include <atomic>

extern void ProbeLog(const char*, ...);

namespace PS4IPUProfile
{
	SamplingState s_sampling;
	AccessSamplingState s_access_sampling;
	namespace
	{
		struct Counter
		{
			std::atomic<u64> nanoseconds{0};
			std::atomic<u64> samples{0};
			u64 last_nanoseconds = 0; // GS reader only.
			u64 last_samples = 0;
		};
		Counter s_commands[16];
		Counter s_access[static_cast<unsigned>(Access::Count)];
		constexpr const char* NAMES[16] = {
			"BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ", "SETVQ", "CSC", "PACK", "SETTH",
			"unknown10", "unknown11", "unknown12", "unknown13", "unknown14", "unknown15"};
	} // namespace

	void Record(u32 command, u64 nanoseconds)
	{
		auto& counter = s_commands[command & 15];
		counter.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
		counter.samples.fetch_add(1, std::memory_order_release);
	}

	void RecordAccess(Access kind, u64 ns)
	{
		auto& counter = s_access[static_cast<unsigned>(kind)];
		counter.nanoseconds.fetch_add(ns, std::memory_order_relaxed);
		counter.samples.fetch_add(1, std::memory_order_release);
	}

	void LogInterval(double seconds)
	{
		// Forja SX2 (compilation 65): the totals, for the video meter (main.cpp turns the sampling on while a movie plays).
		// The worker is the decoding itself (IDEC, BDEC, VDEC, CSC...); the accesses are the game's reads and writes of the
		// IPU's registers and FIFOs, inclusive: a write that starts a command counts the worker's time too.
		double worker_ms_per_second = 0.0, access_ms_per_second = 0.0, csc_ms_per_second = 0.0;
		constexpr const char* access_names[] = {"read32", "read64", "write32", "write64", "fifo_read", "fifo_write"};
		for (unsigned i = 0; i < static_cast<unsigned>(Access::Count); ++i)
		{
			auto& counter = s_access[i];
			const u64 total_samples = counter.samples.load(std::memory_order_acquire);
			const u64 total_ns = counter.nanoseconds.load(std::memory_order_relaxed);
			const u64 samples = total_samples - counter.last_samples;
			if (seconds > 0.0 && !samples)
				continue;
			const u64 ns = total_ns - counter.last_nanoseconds;
			counter.last_samples = total_samples;
			counter.last_nanoseconds = total_ns;
			if (seconds > 0.0 && samples)
			{
				const double ms_per_second = ns * static_cast<double>(ACCESS_SAMPLE_WEIGHT) / (seconds * 1e6);
				access_ms_per_second += ms_per_second;
				ProbeLog("IPU access profile: kind=%s samples=%llu sampled_ms=%.3f estimated_wall_ms_per_second=%.2f inclusive=yes sample_ratio=1/%u\n",
					access_names[i], static_cast<unsigned long long>(samples), ns / 1e6, ms_per_second, ACCESS_SAMPLE_WEIGHT);
			}
		}
		for (unsigned i = 0; i < 16; i++)
		{
			// Adjacent updates can straddle the interval boundary. These statistical
			// estimates aren't exact totals; console profiling needs proper testing.
			auto& counter = s_commands[i];
			const u64 total_samples = counter.samples.load(std::memory_order_acquire);
			const u64 total_ns = counter.nanoseconds.load(std::memory_order_relaxed);
			if (seconds <= 0.0)
			{
				// Establish the baseline without attributing boot work to gameplay.
				counter.last_samples = total_samples;
				counter.last_nanoseconds = total_ns;
				continue;
			}
			const u64 samples = total_samples - counter.last_samples;
			if (samples == 0)
				continue;
			const u64 ns = total_ns - counter.last_nanoseconds;
			counter.last_samples = total_samples;
			counter.last_nanoseconds = total_ns;
			const double ms_per_second = static_cast<double>(ns) * SAMPLE_WEIGHT / (seconds * 1000000.0);
			worker_ms_per_second += ms_per_second;
			if (i == 7) // CSC
				csc_ms_per_second = ms_per_second;
			ProbeLog("IPU profile: command=%s interval_s=%.2f samples=%llu sample_ratio=1/%u "
					 "sampled_wall_ms=%.3f estimated_wall_ms_per_second=%.2f mean_us=%.3f\n",
				NAMES[i], seconds, static_cast<unsigned long long>(samples), SAMPLE_WEIGHT,
				static_cast<double>(ns) / 1000000.0, ms_per_second,
				static_cast<double>(ns) / (static_cast<double>(samples) * 1000.0));
		}
		if (seconds > 0.0 && (worker_ms_per_second > 0.0 || access_ms_per_second > 0.0))
			ProbeLog("Video meter: IPU decoding %.0f ms a second (%.1f %% of the EE thread; colour conversion %.0f ms), IPU "
					 "registers and FIFOs %.0f ms a second including it\n",
				worker_ms_per_second, worker_ms_per_second / 10.0, csc_ms_per_second, access_ms_per_second);
	}
} // namespace PS4IPUProfile
