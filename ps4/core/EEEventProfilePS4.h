// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Sample event phases and the following interval on the EE owner.
#pragma once

#include "common/Timer.h"

#include <atomic>

namespace PS4EEEventProfile
{
	constexpr std::uint32_t SAMPLE_WEIGHT = 1024;
	enum class Phase : unsigned
	{
		Exceptions,
		IOP,
		Timers,
		DMA,
		VU,
		Schedule,
		BetweenEvents,
		Count
	};
	struct SamplingState
	{
		bool enabled = false;
		bool gaps_only = false;
		std::uint32_t gap_pc = 0;
		void (*gap_observer)(std::uint32_t, std::uint32_t, std::uint64_t) = nullptr;
		std::uint32_t random = 0x45455029;
		bool gap_pending = false;
		Common::Timer::Value gap_start = 0;
	};
	extern SamplingState s_sampling; // EE owner only; configure before executing.
	// Forja SX2 (compilation 66): LogInterval writes one line, the video meter's (main.cpp, ForjaVideoMeter, during movies).
	extern std::atomic<bool> s_compact;
	void Record(Phase phase, std::uint64_t nanoseconds);
	void LogInterval(double seconds); // GS consumer only.
	void RecordGap(std::uint32_t start_pc, std::uint32_t end_pc, std::uint64_t nanoseconds);
	void LogGaps(double seconds); // EE owner only, called by VMHostPoll.

	class EventScope
	{
	public:
		explicit EventScope(const std::uint32_t* guest_pc = nullptr) : m_guest_pc(guest_pc)
		{
			if (!s_sampling.enabled)
				return;
			s_sampling.random = s_sampling.random * 1664525U + 1013904223U;
			m_sampled = (s_sampling.random >> 22) == 0;
			if (!m_sampled && !s_sampling.gap_pending)
				return;
			m_previous = Common::Timer::GetCurrentValue();
			if (s_sampling.gap_pending)
			{
				Record(Phase::BetweenEvents, ToNanoseconds(m_previous - s_sampling.gap_start));
				if (s_sampling.gap_observer && m_guest_pc)
					s_sampling.gap_observer(s_sampling.gap_pc, *m_guest_pc, ToNanoseconds(m_previous - s_sampling.gap_start));
				s_sampling.gap_pending = false;
			}
		}

		~EventScope()
		{
			if (!m_sampled)
				return;
			if (s_sampling.gaps_only)
				m_previous = Common::Timer::GetCurrentValue();
			else
				Checkpoint(Phase::Schedule);
			s_sampling.gap_start = m_previous;
			s_sampling.gap_pc = m_guest_pc ? *m_guest_pc : 0;
			s_sampling.gap_pending = true;
		}

		void Checkpoint(Phase phase)
		{
			if (!m_sampled || s_sampling.gaps_only)
				return;
			const auto now = Common::Timer::GetCurrentValue();
			Record(phase, ToNanoseconds(now - m_previous));
			m_previous = now;
		}

	private:
		static std::uint64_t ToNanoseconds(Common::Timer::Value value)
		{
			return static_cast<std::uint64_t>(Common::Timer::ConvertValueToNanoseconds(value));
		}
		bool m_sampled = false;
		const std::uint32_t* m_guest_pc;
		Common::Timer::Value m_previous = 0;
	};
} // namespace PS4EEEventProfile
