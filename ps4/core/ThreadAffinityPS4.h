// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace PS4ThreadAffinity
{
	// Pins the calling emulation worker to one of the six game CPU cores.
	// Create /data/ForjaSX2/vm-no-thread-affinity to keep the system scheduler.
	void PinCurrentThread(const char* role, int core);
	// Forja SX2: helper threads (disc reads and decompression, log and cache writers, telemetry) go to
	// cores 0-1, which are idle while a game runs (the launcher thread only waits), instead of
	// floating onto the EE/VU1/GS/Audio cores. nucleos.txt can move them with AYUDANTES=<core>.
	void PinCurrentThreadToHelpers(const char* role);
	// Forja SX2: the running game's cores, from its profile ([Forja] Nucleos, nucleos.txt's syntax on one line, e.g.
	// "EE=1 VU1=2 GS=3 AUDIO=5 AYUDANTES=4"); empty or "global" = nucleos.txt (or the built-in cores). Each thread takes
	// it the next time it pins itself: PinCurrentThread, or RepinIfPlanChanged from a long-lived thread's loop.
	void SetGamePlan(const char* plan);
	// One atomic load when the plan has not changed since this thread last pinned itself.
	void RepinIfPlanChanged(const char* role, int core);
} // namespace PS4ThreadAffinity
