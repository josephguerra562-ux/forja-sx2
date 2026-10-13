// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Sample IPU worker duration without changing emulated timing.
#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Timer.h"

namespace PS4IPUProfile
{
	constexpr u32 SAMPLE_WEIGHT = 128;
	struct SamplingState
	{
		bool enabled = false; // Set by the launcher before starting the EE thread.
		u32 random = 0x49505526; // Modified only by the EE thread.
	};
	extern SamplingState s_sampling;

	inline bool ShouldSample()
	{
		if (!s_sampling.enabled)
			return false;
		// Use high PRNG bits so regular FIFO command patterns don't always select
		// the same phase. One in 128 invocations on average gets timed.
		s_sampling.random = s_sampling.random * 1664525U + 1013904223U;
		return (s_sampling.random >> 25) == 0;
	}

	void Record(u32 command, u64 nanoseconds);
	void LogInterval(double seconds);

	// (AI-assisted) Inclusive register/FIFO time complements worker-only samples.
	// Nested worker time must not be added to these access totals.
	enum class Access : u32 { Read32, Read64, Write32, Write64, FifoRead, FifoWrite, Count };
	constexpr u32 ACCESS_SAMPLE_WEIGHT = 1024;
	struct AccessSamplingState
	{
		bool enabled = false;
		u32 random = 0x41434352;
	};
	extern AccessSamplingState s_access_sampling;
	void RecordAccess(Access kind, u64 ns);
	class AccessScope
	{
	public:
		explicit AccessScope(Access kind) : m_kind(kind)
		{
			if (!s_access_sampling.enabled)
				return;
			s_access_sampling.random = s_access_sampling.random * 1664525U + 1013904223U;
			m_sampled = (s_access_sampling.random >> 22) == 0;
			if (m_sampled)
				m_start = Common::Timer::GetCurrentValue();
		}
		~AccessScope()
		{
			if (m_sampled)
			{
				const auto end = Common::Timer::GetCurrentValue();
				RecordAccess(m_kind, static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(end - m_start)));
			}
		}
	private:
		Access m_kind;
		bool m_sampled = false;
		Common::Timer::Value m_start = 0;
	};
} // namespace PS4IPUProfile
