// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) FreeBSD 9 pthread_getcpuclockid returns the caller-relative clock
// for every handle. Read CLOCK_THREAD_CPUTIME_ID on its owner, then publish it.
#include "ThreadCPUMetricsPS4.h"
#include "common/Timer.h"

#include <atomic>
#include <ctime>

namespace PS4ThreadCPUMetrics
{
	namespace
	{
		struct alignas(64) SampleState
		{
			std::atomic<u64> cpu_us{0};
			Common::Timer::Value next_sample = 0; // Written only by this role's owner.
		};
		SampleState s_samples[static_cast<unsigned>(Role::Count)];
	} // namespace

	void Sample(Role role)
	{
		auto& state = s_samples[static_cast<unsigned>(role)];
		const auto now = Common::Timer::GetCurrentValue();
		if (now < state.next_sample)
			return;
		state.next_sample = now + Common::Timer::ConvertMillisecondsToValue(100.0);
		timespec ts{};
		// This is an absolute cumulative CPU time, not elapsed wall time. Console
		// comparison of separate EE/VU1 samples still needs proper testing.
		const u64 time = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ?
		                     static_cast<u64>(ts.tv_sec) * 1000000ULL + static_cast<u64>(ts.tv_nsec) / 1000ULL :
		                     0;
		state.cpu_us.store(time, std::memory_order_relaxed);
	}

	u64 Read(Role role)
	{
		return s_samples[static_cast<unsigned>(role)].cpu_us.load(std::memory_order_relaxed);
	}
} // namespace PS4ThreadCPUMetrics
