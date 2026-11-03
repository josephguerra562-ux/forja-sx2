// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (compilation 110): what the GS thread is doing now, read 200 times a second by the meter in ps4/vm/HostVM.cpp ("GS thread split").
// Drawing was ~8 of the 21 ms of God of War II's GS per refresh (2026-11-02) and the other 13 had no name. The GS (MTGS) thread sets it at the start
// of each part and gives back the one it was in at the end, so nested parts (a readback inside a draw) count where they are. A read can be one part
// late; over 1000 samples that doesn't show. Plain stores: no lock, no clock.
#pragma once

#include <atomic>

namespace ForjaGsPhase
{
	enum Phase : int
	{
		Other = 0, // the ring's own bookkeeping, and anything without a name yet
		WaitWork, // nothing queued by the EE
		WaitVU, // waiting for the VU1 thread's packet (XGKICK)
		Transfer, // reading GIF packets: registers, vertices, image transfers into the GS memory
		Draw, // GSRendererHW::Draw: texture cache, uploads, the Vulkan commands
		Readback, // the GS memory read back from the GPU's copy
		VSync, // the end of a frame: presenting it
		Count
	};
	// A cache line of its own: the GS thread writes it hundreds of thousands of times a second (a packet each), and a neighbour written by the EE
	// thread (g_orbis_ee_waiting, say) on the same line would have the two threads taking it from each other.
	struct alignas(64) Now
	{
		std::atomic<int> phase{Other};
	};
	inline Now now;
	class Scope
	{
		int m_previous;

	public:
		explicit Scope(Phase phase)
			: m_previous(now.phase.load(std::memory_order_relaxed))
		{
			now.phase.store(phase, std::memory_order_relaxed);
		}
		~Scope() { now.phase.store(m_previous, std::memory_order_relaxed); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};
} // namespace ForjaGsPhase
