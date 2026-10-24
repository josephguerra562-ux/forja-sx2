// SPDX-License-Identifier: GPL-3.0+

#include "ThreadAffinityPS4.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

// OpenOrbis's public header declares these without parameters. Use the ABI
// verified by DolphinPS4 until the SDK declaration is corrected.
extern "C" void* scePthreadSelf();
extern "C" int scePthreadSetaffinity(void* thread, unsigned long long mask);
// Forja SX2: PlayStation thread priorities run from 256 (highest) to 767 (lowest); new threads get 700
// (as PS5SX2's profiler notes and console emulators report). Same ABI shape as scePthreadSetaffinity.
extern "C" int scePthreadSetprio(void* thread, int prio);

// Forja SX2: the sampling profiler from the PS4 runtime (ps4/runtime/ps4_sampler.cpp): 1,000 times a second it records where
// each registered thread is, and every 10 s writes the busiest code addresses to /data/ForjaSX2/samples.log (symbolized with
// the vm_boot.sym.gz each release carries, by herramientas/perfil_funciones.py). On with /data/ForjaSX2/vm-sampler.
extern "C" void ps4_sampler_register_thread(const char* name);
extern "C" void ps4_sampler_start();
extern "C" void ps4_sampler_set_cpu_mask(unsigned long long mask);

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
	// A game's profile can bring its own plan in the same syntax ([Forja] Nucleos, SetGamePlan), which then
	// replaces nucleos.txt for that game: roles it leaves out keep their built-in cores.
	struct Override
	{
		char role[12];
		int core;
	};
	static constexpr int MAX_OVERRIDES = 8;
	static Override s_file_overrides[MAX_OVERRIDES];
	static int s_file_override_count = 0;
	static Override s_game_overrides[MAX_OVERRIDES];
	static int s_game_override_count = 0;
	static bool s_game_plan = false;
	static std::mutex s_plan_mutex;
	// Bumped by every plan change; each thread remembers the one it last pinned itself with.
	static std::atomic<unsigned> s_plan_generation{1};
	static thread_local unsigned t_pinned_generation = 0;
	static thread_local bool t_priority_raised = false;
	static thread_local bool t_sampler_registered = false;

	// "ROLE=core" pairs separated by spaces, commas, semicolons or new lines; '#' comments out the rest of its line.
	static int ParseOverrides(const char* text, Override* out, int max, const char* source)
	{
		int count = 0;
		const char* p = text;
		while (*p && count < max)
		{
			if (*p == '#')
			{
				while (*p && *p != '\n')
					++p;
				continue;
			}
			if (!std::isalnum(static_cast<unsigned char>(*p)))
			{
				++p;
				continue;
			}
			char role[12] = {};
			int length = 0;
			while (std::isalnum(static_cast<unsigned char>(*p)))
			{
				if (length < 11)
					role[length++] = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
				++p;
			}
			while (*p == ' ' || *p == '\t')
				++p;
			if (*p != '=')
				continue;
			++p;
			char* end = nullptr;
			const long core = std::strtol(p, &end, 10);
			if (end == p)
				continue;
			p = end;
			if (core < 0 || core > 6)
			{
				std::printf("PS4 thread affinity: %s ignores %s=%ld (cores 0-6 only)\n", source, role, core);
				continue;
			}
			std::strcpy(out[count].role, role);
			out[count].core = static_cast<int>(core);
			++count;
		}
		return count;
	}

	static void LoadOverrides()
	{
		std::FILE* file = std::fopen("/data/ForjaSX2/nucleos.txt", "r");
		if (!file)
			return;
		char text[1024] = {};
		const size_t length = std::fread(text, 1, sizeof(text) - 1, file);
		std::fclose(file);
		text[length] = 0;
		s_file_override_count = ParseOverrides(text, s_file_overrides, MAX_OVERRIDES, "nucleos.txt");
	}

	static std::once_flag s_file_loaded;

	static int ResolveCore(const char* role, int core)
	{
		std::call_once(s_file_loaded, LoadOverrides);
		std::lock_guard lock(s_plan_mutex);
		const Override* overrides = s_game_plan ? s_game_overrides : s_file_overrides;
		const int count = s_game_plan ? s_game_override_count : s_file_override_count;
		for (int i = 0; i < count; ++i)
		{
			const char* a = overrides[i].role;
			const char* b = role;
			while (*a && *b && *a == std::toupper(static_cast<unsigned char>(*b)))
				++a, ++b;
			if (*a == 0 && *b == 0)
				return overrides[i].core;
		}
		return core;
	}

	static const char* PlanSource()
	{
		std::lock_guard lock(s_plan_mutex);
		return s_game_plan ? "game profile" : "nucleos.txt";
	}

	// Forja SX2: PRIORIDAD=1 in nucleos.txt raises the emulation workers (EE, VU1, GS, Audio) above the
	// default 700 and drops helper threads to the lowest priority, so nothing that lands on a worker's core
	// can delay it. Off by default until measured on the console (a busy high-priority thread can starve
	// anything else sharing its core).
	constexpr int WORKER_PRIORITY = 448;
	constexpr int HELPER_PRIORITY = 767;
	constexpr int DEFAULT_PRIORITY = 700;

	static bool PriorityEnabled()
	{
		return ResolveCore("PRIORIDAD", 0) == 1;
	}

	static void ApplyPriority(const char* role, int prio)
	{
		if (PriorityEnabled())
		{
			const int result = scePthreadSetprio(scePthreadSelf(), prio);
			t_priority_raised = true;
			std::printf("PS4 thread priority: %s prio=%d sce_result=0x%x\n", role, prio, result);
			std::fflush(stdout);
		}
		else if (t_priority_raised)
		{
			// A game whose plan has no PRIORIDAD=1 after one that had it: back to the default.
			const int result = scePthreadSetprio(scePthreadSelf(), DEFAULT_PRIORITY);
			t_priority_raised = false;
			std::printf("PS4 thread priority: %s back to prio=%d sce_result=0x%x\n", role, DEFAULT_PRIORITY, result);
			std::fflush(stdout);
		}
	}

	void PinCurrentThread(const char* role, int core)
	{
		t_pinned_generation = s_plan_generation.load(std::memory_order_acquire);
		if (!IsEnabled())
		{
			std::printf("PS4 thread affinity: %s left to scheduler (disabled by marker)\n", role);
			std::fflush(stdout);
			return;
		}
		ApplyPriority(role, WORKER_PRIORITY);

		const int requested = core;
		core = ResolveCore(role, core);
		const unsigned long long mask = 1ULL << core;
		const int result = scePthreadSetaffinity(scePthreadSelf(), mask);
		// (AI-assisted) Core placement needs proper testing across base/Pro consoles.
		std::printf("PS4 thread affinity: %s core=%d%s%s mask=0x%llx sce_result=0x%x\n", role, core,
			core != requested ? " " : "", core != requested ? PlanSource() : "", mask, result);
		std::fflush(stdout);
		// Forja SX2: the three emulation workers into the sampling profiler (it holds four threads), once per thread.
		static const bool s_sampler = access("/data/ForjaSX2/vm-sampler", F_OK) == 0;
		if (s_sampler && !t_sampler_registered &&
			(std::strcmp(role, "EE") == 0 || std::strcmp(role, "VU1") == 0 || std::strcmp(role, "GS") == 0))
		{
			t_sampler_registered = true;
			// Its own thread goes on the helpers' cores, never on the worker that happens to start it.
			const int helper = ResolveCore("AYUDANTES", -1);
			ps4_sampler_set_cpu_mask(helper >= 0 ? (1ULL << helper) : 0x3ULL);
			ps4_sampler_register_thread(role);
			ps4_sampler_start();
			std::printf("PS4 sampler: %s registered\n", role);
			std::fflush(stdout);
		}
	}

	void PinCurrentThreadToHelpers(const char* role)
	{
		if (!IsEnabled())
			return;
		ApplyPriority(role, HELPER_PRIORITY);
		// Default: cores 0 and 1. AYUDANTES=<core> in nucleos.txt narrows them to one core.
		const int single = ResolveCore("AYUDANTES", -1);
		const unsigned long long mask = single >= 0 ? (1ULL << single) : 0x3ULL;
		const int result = scePthreadSetaffinity(scePthreadSelf(), mask);
		std::printf("PS4 thread affinity: helper %s mask=0x%llx sce_result=0x%x\n", role, mask, result);
		std::fflush(stdout);
	}

	void PinCurrentThreadToSoftwareDraw(int index)
	{
		if (!IsEnabled())
			return;
		// Cores 0-5 are the game's. Plan B (EE=1 VU1=2 GS=3 AUDIO=5) leaves 0, 4 and the GS's 3; the built-in cores (EE=2 VU1=3
		// GS=4 AUDIO=5) leave 0, 1 and the GS's 4.
		const int ee = ResolveCore("EE", 2), vu1 = ResolveCore("VU1", 3), gs = ResolveCore("GS", 4), audio = ResolveCore("Audio", 5);
		int cores[6];
		int count = 0;
		for (int core = 0; core < 6; core++)
		{
			if (core != ee && core != vu1 && core != audio && core != gs)
				cores[count++] = core;
		}
		if (gs >= 0 && gs < 6 && gs != ee && gs != vu1 && gs != audio)
			cores[count++] = gs;
		if (count == 0)
			return;
		const int core = cores[index % count];
		const int result = scePthreadSetaffinity(scePthreadSelf(), 1ULL << core);
		std::printf("PS4 thread affinity: software draw %d core=%d sce_result=0x%x\n", index, core, result);
		std::fflush(stdout);
	}

	void SetGamePlan(const char* plan)
	{
		std::call_once(s_file_loaded, LoadOverrides);
		const bool use = plan && *plan && strcasecmp(plan, "global") != 0;
		Override parsed[MAX_OVERRIDES] = {};
		const int count = use ? ParseOverrides(plan, parsed, MAX_OVERRIDES, "[Forja] Nucleos") : 0;
		bool changed;
		{
			std::lock_guard lock(s_plan_mutex);
			changed = use != s_game_plan || count != s_game_override_count;
			for (int i = 0; !changed && i < count; ++i)
				changed = std::strcmp(parsed[i].role, s_game_overrides[i].role) != 0 || parsed[i].core != s_game_overrides[i].core;
			s_game_plan = use;
			s_game_override_count = count;
			std::memcpy(s_game_overrides, parsed, sizeof(parsed));
		}
		if (changed)
			s_plan_generation.fetch_add(1, std::memory_order_acq_rel);
		std::printf("PS4 thread affinity: game plan %s%s%s\n", use ? "\"" : "", use ? plan : "none (nucleos.txt)", use ? "\"" : "");
		std::fflush(stdout);
	}

	void RepinIfPlanChanged(const char* role, int core)
	{
		if (t_pinned_generation != s_plan_generation.load(std::memory_order_relaxed))
			PinCurrentThread(role, core);
	}
} // namespace PS4ThreadAffinity
