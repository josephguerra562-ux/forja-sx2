// SPDX-License-Identifier: GPL-3.0+
// Forja SX2, 195 in the app: "VIF en su hilo" (pcsx2/Vif_Transfer.cpp; informes/VIF_HILO_PROPIO_PLAN_2026-11-22.md).
#pragma once
#include <atomic>
#include "common/Pcsx2Types.h"

namespace ForjaVifT
{
	extern std::atomic<int> g_mode;
	// True on the VIF thread. A function, not an "extern thread_local": read from another file, that makes clang call a TLS init
	// function (_ZTH...) the PS4's FSELF builder can't resolve (the 147's SPU2 mark and the 195's first build).
	bool OnThread();
	extern std::atomic<u64> g_skip_cycles;
	extern std::atomic<u32> g_mtvu_gif_pending;
	extern std::atomic<u32> g_stopped;
	extern std::atomic<u64> g_pieces_thread, g_pieces_inline, g_stops, g_drains, g_full_waits, g_stalls_left;
	// 196 in the app: why a piece stayed on the EE (0 the DMA not running, 1 an interrupt offset, 2 a stall, 3 the queue full), the
	// timing breaks the EE went through, the queues dropped on a reset or a state load, and the EE's waits of over ~2 s.
	extern std::atomic<u64> g_inline_why[4], g_breaks_through, g_discards, g_long_waits;
	// 197 in the app: pieces the thread left whole to the EE because a DIRECT/DIRECTHL was half done, and pieces that went to the thread
	// with an interrupt offset (a DMA tag's two words with TTE).
	extern std::atomic<u64> g_direct_left, g_offset_pieces;
	// 198 in the app: the EE's TSC ticks waiting for the thread, and how many waits, by site (above); the thread's sleeps.
	extern std::atomic<u64> g_wait_ticks[10], g_wait_n[10], g_sleeps;
	// 199 in the app: the thread's TSC ticks at work (a piece from its start to its end or its stop), and the EE's polls of the VU
	// thread's flags that found something (GS FINISH/SIGNAL/LABEL, VU1 end or T bit).
	extern std::atomic<u64> g_thread_busy_ticks, g_mtvu_polls;
	bool IsHardForThread(const u32* data, u32 pSize);
	bool On();
	bool Pending();
	// 198 in the app: where the EE waits from, for the meter: 0 VIF1 register read, 1 VIF1 register write, 2 GIF DMA start, 3 VIF1 DMA
	// start (to memory or MFIFO only), 4 VIF1 FIFO write, 5 state save/load or reset, 6 the EE writing the VU1's ring, 7 waiting for the
	// VU1, 8 vifTransfer (queue full, or a piece that stays on the EE), 9 other.
	void FullDrain(int site = 8);
	void EventHook();
	void ApplyPending();
	// 196 in the app: on a reset or a state load, the queue as far as it goes and then dropped (the state is replaced).
	void Discard();
	// The VIF thread keeps its own DBF (VIF1_STAT's double-buffer flag, which MSCAL/MSCNT/OFFSET change): vif1Regs.stat stays the EE's, so
	// the two never write that word at once. Handed over at the EE's sync points.
	bool DbfGet();
	void DbfSet(bool v);
} // namespace ForjaVifT
