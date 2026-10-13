// SPDX-License-Identifier: GPL-3.0+

#include "ThreadAffinityPS4.h"

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>

// OpenOrbis's public header declares these without parameters. Use the ABI
// verified by DolphinPS4 until the SDK declaration is corrected.
extern "C" void* scePthreadSelf();
extern "C" int scePthreadSetaffinity(void* thread, unsigned long long mask);

namespace PS4ThreadAffinity
{
	static bool IsEnabled()
	{
		static std::atomic<int> enabled{-1};
		int value = enabled.load(std::memory_order_acquire);
		if (value >= 0)
			return value != 0;

		value = access("/data/PS4SX2/vm-no-thread-affinity", F_OK) == 0 ? 0 : 1;
		enabled.store(value, std::memory_order_release);
		return value != 0;
	}

	void PinCurrentThread(const char* role, int core)
	{
		if (!IsEnabled())
		{
			std::printf("PS4 thread affinity: %s left to scheduler (disabled by marker)\n", role);
			std::fflush(stdout);
			return;
		}

		const unsigned long long mask = 1ULL << core;
		const int result = scePthreadSetaffinity(scePthreadSelf(), mask);
		// (AI-assisted) Core placement needs proper testing across base/Pro consoles.
		std::printf("PS4 thread affinity: %s core=%d mask=0x%llx sce_result=0x%x\n", role, core, mask, result);
		std::fflush(stdout);
	}
} // namespace PS4ThreadAffinity
