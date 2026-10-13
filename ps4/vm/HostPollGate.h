// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Bound host clock queries without changing emulated event timing.
#pragma once

#include <cstdint>

class HostPollGate
{
public:
	constexpr HostPollGate(std::uint32_t max_calls = 64, std::uint32_t cycle_interval = 32768)
		: m_max_calls(max_calls)
		, m_cycle_interval(cycle_interval)
	{
	}

	void Reset()
	{
		// (AI-assisted) Reset per-run progress without discarding configured limits.
		m_last_cycle = 0;
		m_calls = 0;
		m_initialized = false;
	}

	std::uint32_t GetMaxCalls() const { return m_max_calls; }
	std::uint32_t GetCycleInterval() const { return m_cycle_interval; }

	bool ShouldCheckClock(std::uint32_t cycle)
	{
		// The call bound also permits input polling when guest cycles stop advancing.
		// Unsigned subtraction handles wrap; a reset backwards forces a fresh check.
		if (m_initialized && ++m_calls < m_max_calls &&
			static_cast<std::uint32_t>(cycle - m_last_cycle) < m_cycle_interval)
			return false;
		m_initialized = true;
		m_calls = 0;
		m_last_cycle = cycle;
		return true;
	}

private:
	std::uint32_t m_max_calls;
	std::uint32_t m_cycle_interval;
	std::uint32_t m_last_cycle = 0;
	std::uint32_t m_calls = 0;
	bool m_initialized = false;
};
