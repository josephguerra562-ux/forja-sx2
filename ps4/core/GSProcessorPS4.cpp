// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Real CPUID feature queries for the single Jaguar ISA build; no fixed PS5 topology.
#include "pcsx2/GS/MultiISA.h"
#include "pcsx2/GS/GSXXH.h"
#include <cpuid.h>

namespace
{
	ProcessorFeatures DetectFeatures()
	{
		ProcessorFeatures features{};
		features.vectorISA = ProcessorFeatures::VectorISA::SSE4;
		unsigned int a, b, c, d;
		if (__get_cpuid(1, &a, &b, &c, &d) && (c & bit_OSXSAVE) && (c & bit_AVX))
		{
			unsigned int xcr_low, xcr_high;
			__asm__ volatile("xgetbv" : "=a"(xcr_low), "=d"(xcr_high) : "c"(0));
			if ((xcr_low & 6) == 6)
			{
				features.vectorISA = ProcessorFeatures::VectorISA::AVX;
				features.hasFMA = (c & bit_FMA) != 0;
			}
		}
		if (__get_cpuid_count(7, 0, &a, &b, &c, &d))
			features.hasBMI2 = (b & bit_BMI) && (b & bit_BMI2);
		return features;
	}
} // namespace

const ProcessorFeatures g_cpu = DetectFeatures();
u64 (&MultiISAFunctions::GSXXH3_64_Long)(const void*, size_t) = MULTI_ISA_SELECT(GSXXH3_64_Long);
u32 (&MultiISAFunctions::GSXXH3_64_Update)(void*, const void*, size_t) = MULTI_ISA_SELECT(GSXXH3_64_Update);
u64 (&MultiISAFunctions::GSXXH3_64_Digest)(void*) = MULTI_ISA_SELECT(GSXXH3_64_Digest);
