// SPDX-License-Identifier: GPL-3.0+
#pragma once

namespace PS4Audio
{
	// (AI-assisted) Read atomic output counters from the VM heartbeat thread.
	void LogStatus();
	// Forja 1.06: the output's underruns since its stream opened, and the frames the limiter turned down (SessionStats.h).
	void Totals(unsigned& underruns, unsigned long long& limited);
} // namespace PS4Audio
