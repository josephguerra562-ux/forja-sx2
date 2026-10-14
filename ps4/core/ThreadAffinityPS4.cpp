// SPDX-License-Identifier: GPL-3.0+

#include "ThreadAffinityPS4.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
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

		value = access("/data/ForjaSX2/vm-no-thread-affinity", F_OK) == 0 ? 0 : 1;
		enabled.store(value, std::memory_order_release);
		return value != 0;
	}

	// Forja SX2: optional /data/ForjaSX2/nucleos.txt picks the core of each worker without a rebuild,
	// one "ROLE=core" per line (EE, VU1, GS, Audio; '#' starts a comment). The Jaguar CPU is two clusters
	// of four cores (0-3 and 4-7) with one L2 each, so this lets testers compare keeping EE, VU1 and GS
	// on one cluster against the default split. Cores 0-5 are the game's; 6 needs the SFO 7-CPU flag.
	struct Override
	{
		char role[12];
		int core;
	};
	static Override s_overrides[8];
	static int s_override_count = 0;

	static void LoadOverrides()
	{
		std::FILE* file = std::fopen("/data/ForjaSX2/nucleos.txt", "r");
		if (!file)
			return;
		char line[128];
		while (s_override_count < 8 && std::fgets(line, sizeof(line), file))
		{
			char role[12] = {};
			int core = -1;
			if (line[0] == '#' || std::sscanf(line, " %11[A-Za-z0-9] = %d", role, &core) != 2)
				continue;
			if (core < 0 || core > 6)
			{
				std::printf("PS4 thread affinity: nucleos.txt ignores %s=%d (cores 0-6 only)\n", role, core);
				continue;
			}
			for (char* c = role; *c; ++c)
				*c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
			std::strcpy(s_overrides[s_override_count].role, role);
			s_overrides[s_override_count].core = core;
			++s_override_count;
		}
		std::fclose(file);
	}

	static int ResolveCore(const char* role, int core)
	{
		static std::once_flag loaded;
		std::call_once(loaded, LoadOverrides);
		for (int i = 0; i < s_override_count; ++i)
		{
			const char* a = s_overrides[i].role;
			const char* b = role;
			while (*a && *b && *a == std::toupper(static_cast<unsigned char>(*b)))
				++a, ++b;
			if (*a == 0 && *b == 0)
				return s_overrides[i].core;
		}
		return core;
	}

	void PinCurrentThread(const char* role, int core)
	{
		if (!IsEnabled())
		{
			std::printf("PS4 thread affinity: %s left to scheduler (disabled by marker)\n", role);
			std::fflush(stdout);
			return;
		}

		const int requested = core;
		core = ResolveCore(role, core);
		const unsigned long long mask = 1ULL << core;
		const int result = scePthreadSetaffinity(scePthreadSelf(), mask);
		// (AI-assisted) Core placement needs proper testing across base/Pro consoles.
		std::printf("PS4 thread affinity: %s core=%d%s mask=0x%llx sce_result=0x%x\n", role, core,
			core != requested ? " (nucleos.txt)" : "", mask, result);
		std::fflush(stdout);
	}

	void PinCurrentThreadToHelpers(const char* role)
	{
		if (!IsEnabled())
			return;
		// Default: cores 0 and 1. AYUDANTES=<core> in nucleos.txt narrows them to one core.
		const int single = ResolveCore("AYUDANTES", -1);
		const unsigned long long mask = single >= 0 ? (1ULL << single) : 0x3ULL;
		const int result = scePthreadSetaffinity(scePthreadSelf(), mask);
		std::printf("PS4 thread affinity: helper %s mask=0x%llx sce_result=0x%x\n", role, mask, result);
		std::fflush(stdout);
	}
} // namespace PS4ThreadAffinity
