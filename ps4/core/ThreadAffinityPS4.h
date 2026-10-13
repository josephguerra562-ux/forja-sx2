// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace PS4ThreadAffinity
{
	// Pins the calling emulation worker to one of the six game CPU cores.
	// Create /data/PS4SX2/vm-no-thread-affinity to keep the system scheduler.
	void PinCurrentThread(const char* role, int core);
} // namespace PS4ThreadAffinity
