// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Owner-thread CPU clock samples for the standalone PS4 VM.
#pragma once

#include "common/Pcsx2Defs.h"

namespace PS4ThreadCPUMetrics
{
	enum class Role : unsigned
	{
		EE,
		GS,
		VU1,
		Count
	};

	// Call only from the thread named by role. Cross-thread readers use atomics.
	// Samples are throttled to one clock syscall per 100 ms, even with frequent calls.
	void Sample(Role role);
	u64 Read(Role role);
} // namespace PS4ThreadCPUMetrics
