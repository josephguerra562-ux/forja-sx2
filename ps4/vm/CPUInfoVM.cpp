// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) CPUID features for the isolated Orbis boot target.
// Topology/affinity is deliberately unavailable; do not invent host processor IDs.
#include <cpuinfo.h>
#include <cpuid.h>
#include <cstring>
#include <mutex>
#include <unistd.h>

extern "C" {
struct cpuinfo_x86_isa cpuinfo_isa{};
}
static std::once_flag s_once;
static cpuinfo_package s_package{};
static uint32_t s_processors;

extern "C" bool cpuinfo_initialize()
{
	std::call_once(s_once, [] {
		std::strcpy(s_package.name, "Orbis x86-64");
		unsigned a, b, c, d;
		if (__get_cpuid_max(0x80000000, nullptr) >= 0x80000004)
		{
			for (unsigned i = 0; i < 3; i++)
			{
				__get_cpuid(0x80000002 + i, &a, &b, &c, &d);
				const unsigned words[] = {a, b, c, d};
				std::memcpy(s_package.name + i * 16, words, sizeof(words));
			}
		}
		if (__get_cpuid(1, &a, &b, &c, &d))
		{
			cpuinfo_isa.sse4_1 = (c & bit_SSE4_1) != 0;
			cpuinfo_isa.sse4_2 = (c & bit_SSE4_2) != 0;
			if ((c & bit_OSXSAVE) && (c & bit_AVX))
			{
				unsigned lo, hi;
				__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
				cpuinfo_isa.avx = (lo & 6) == 6;
				cpuinfo_isa.fma3 = cpuinfo_isa.avx && (c & bit_FMA);
			}
		}
		if (__get_cpuid_count(7, 0, &a, &b, &c, &d))
		{
			cpuinfo_isa.avx2 = cpuinfo_isa.avx && (b & bit_AVX2);
			cpuinfo_isa.bmi = (b & bit_BMI) != 0;
			cpuinfo_isa.bmi2 = (b & bit_BMI2) != 0;
		}
		const long online = sysconf(_SC_NPROCESSORS_ONLN);
		s_processors = online > 0 ? static_cast<uint32_t>(online) : 0;
		s_package.processor_count = s_processors;
	});
	return true;
}
extern "C" const cpuinfo_package* cpuinfo_get_package(uint32_t index)
{
	cpuinfo_initialize();
	return index == 0 ? &s_package : nullptr;
}
extern "C" uint32_t cpuinfo_get_processors_count()
{
	cpuinfo_initialize();
	return s_processors;
}
extern "C" uint32_t cpuinfo_get_clusters_count() { return 0; }
extern "C" const cpuinfo_cluster* cpuinfo_get_clusters() { return nullptr; }
extern "C" const cpuinfo_processor* cpuinfo_get_processor(uint32_t) { return nullptr; }
