// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Cumulative counters avoid resetting a live EE producer.
#include "EEEventProfilePS4.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstring>
#include "pcsx2/vtlb.h"

extern void ProbeLog(const char*, ...);

namespace PS4EEEventProfile
{
	SamplingState s_sampling;
	std::atomic<bool> s_compact{false};
	namespace
	{
		struct Counter
		{
			std::atomic<std::uint64_t> nanoseconds{0};
			std::atomic<std::uint64_t> samples{0};
			std::uint64_t last_nanoseconds = 0; // GS reader only.
			std::uint64_t last_samples = 0;
		};
		Counter s_counters[static_cast<unsigned>(Phase::Count)];
		// (AI-assisted) EE-owned bounded table. Time belongs to a whole interval
		// between event boundaries, not exclusively to either guest PC.
		struct Gap
		{
			u32 start = 0, end = 0;
			u64 ns = 0, count = 0, max_ns = 0;
		};
		std::array<Gap, 4096> s_gaps{};
		u64 s_dropped_gaps = 0, s_dropped_ns = 0;
		constexpr const char* NAMES[] = {"exceptions", "IOP", "timers", "DMA", "VU", "schedule", "between_events"};
	} // namespace

	void RecordGap(u32 start_pc, u32 end_pc, u64 nanoseconds)
	{
		const u32 hash = ((start_pc >> 2) * 2654435761U ^ (end_pc >> 2)) & 4095;
		for (u32 i = 0; i < 16; ++i)
		{
			auto& gap = s_gaps[(hash + i) & 4095];
			if (!gap.count || (gap.start == start_pc && gap.end == end_pc))
			{
				gap.start = start_pc;
				gap.end = end_pc;
				gap.ns += nanoseconds;
				++gap.count;
				gap.max_ns = std::max(gap.max_ns, nanoseconds);
				return;
			}
		}
		++s_dropped_gaps;
		s_dropped_ns += nanoseconds;
	}

	void LogGaps(double seconds)
	{
		if (!s_sampling.enabled || seconds <= 0.0 || !s_sampling.gap_observer)
			return; // compilation 66: no table without its observer (the video meter's split)
		std::array<Gap, 8> top{};
		u64 total_ns = s_dropped_ns;
		for (const auto& gap : s_gaps)
		{
			total_ns += gap.ns;
			if (gap.ns <= top.back().ns)
				continue;
			top.back() = gap;
			std::sort(top.begin(), top.end(), [](const Gap& a, const Gap& b) { return a.ns > b.ns; });
		}
		ProbeLog("EE gap profile: interval=%.3f sample_ratio=1/%u dropped=%llu wall_time_includes_host_poll_and_preemption=yes\n",
			seconds, SAMPLE_WEIGHT, static_cast<unsigned long long>(s_dropped_gaps));
		for (const auto& gap : top)
		{
			if (!gap.count)
				break;
			ProbeLog("EE gap: start=%08x end=%08x samples=%llu sampled_ms=%.3f share_pct=%.2f max_us=%.3f\n",
				gap.start, gap.end, static_cast<unsigned long long>(gap.count), gap.ns / 1e6,
				total_ns ? 100.0 * gap.ns / total_ns : 0.0, gap.max_ns / 1e3);
			// Read only directly mapped RAM/ROM, never MMIO, and stay in-page.
			const u32 first = gap.start & ~15U;
			const auto mapping = vtlb_private::vtlbdata.vmap[first >> vtlb_private::VTLB_PAGE_BITS];
			if (!mapping.isHandler(first))
			{
				u32 words[8]{};
				const u32 count = std::min(8U, (4096 - (first & 4095)) / 4);
				std::memcpy(words, reinterpret_cast<const void*>(mapping.assumePtr(first)), count * 4);
				ProbeLog("EE gap context: start=%08x words=%u data=%08x %08x %08x %08x %08x %08x %08x %08x\n",
					first, count, words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
			}
		}
		s_gaps = {};
		s_dropped_gaps = s_dropped_ns = 0;
	}

	void Record(Phase phase, std::uint64_t nanoseconds)
	{
		auto& counter = s_counters[static_cast<unsigned>(phase)];
		counter.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
		counter.samples.fetch_add(1, std::memory_order_release);
	}

	void LogInterval(double seconds)
	{
		// Forja SX2 (compilation 66): the video meter's line, the EE thread's second split as the game's code (between events,
		// with the memory handlers it calls: IPU registers, the decoder started by them) and each phase of the events.
		if (s_compact.load(std::memory_order_relaxed))
		{
			double ms[static_cast<unsigned>(Phase::Count)] = {};
			bool any = false;
			for (unsigned i = 0; i < static_cast<unsigned>(Phase::Count); ++i)
			{
				auto& counter = s_counters[i];
				const auto samples = counter.samples.load(std::memory_order_acquire);
				const auto ns = counter.nanoseconds.load(std::memory_order_relaxed);
				const auto delta_ns = ns - counter.last_nanoseconds;
				any |= samples != counter.last_samples;
				counter.last_samples = samples;
				counter.last_nanoseconds = ns;
				ms[i] = seconds > 0.0 ? static_cast<double>(delta_ns) * SAMPLE_WEIGHT / (seconds * 1000000.0) : 0.0;
			}
			if (any && seconds > 0.0)
				ProbeLog("Video meter: EE thread a second: game code %.0f ms, IOP %.0f, DMA and IPU %.0f, timers %.0f, VU %.0f, "
						 "interrupts %.0f, scheduling %.0f (one event in %u)\n",
					ms[static_cast<unsigned>(Phase::BetweenEvents)], ms[static_cast<unsigned>(Phase::IOP)],
					ms[static_cast<unsigned>(Phase::DMA)], ms[static_cast<unsigned>(Phase::Timers)], ms[static_cast<unsigned>(Phase::VU)],
					ms[static_cast<unsigned>(Phase::Exceptions)], ms[static_cast<unsigned>(Phase::Schedule)], SAMPLE_WEIGHT);
			return;
		}
		for (unsigned i = 0; i < static_cast<unsigned>(Phase::Count); ++i)
		{
			auto& counter = s_counters[i];
			const auto samples = counter.samples.load(std::memory_order_acquire);
			const auto ns = counter.nanoseconds.load(std::memory_order_relaxed);
			const auto delta_samples = samples - counter.last_samples;
			const auto delta_ns = ns - counter.last_nanoseconds;
			if (seconds > 0.0 && delta_samples == 0)
				continue; // A duration can publish just before its sample count.
			counter.last_samples = samples;
			counter.last_nanoseconds = ns;
			if (seconds <= 0.0 || delta_samples == 0)
				continue;
			// Statistical wall time, including preemption and sampled clock overhead.
			// Counter pairs may straddle an update; attribution needs proper PS4 testing.
			ProbeLog("EE event profile: phase=%s interval_s=%.2f samples=%llu sample_ratio=1/%u "
					 "sampled_wall_ms=%.3f estimated_wall_ms_per_second=%.2f\n",
				NAMES[i], seconds, static_cast<unsigned long long>(delta_samples), SAMPLE_WEIGHT,
				static_cast<double>(delta_ns) / 1000000.0,
				static_cast<double>(delta_ns) * SAMPLE_WEIGHT / (seconds * 1000000.0));
		}
	}
} // namespace PS4EEEventProfile
