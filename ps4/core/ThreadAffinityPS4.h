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
} // namespace PS4ThreadAffinity
