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
	bool IsHardForThread(const u32* data, u32 pSize);
	bool On();
	bool Pending();
	void FullDrain();
	void EventHook();
	void ApplyPending();
	// 196 in the app: on a reset or a state load, the queue as far as it goes and then dropped (the state is replaced).
	void Discard();
	// The VIF thread keeps its own DBF (VIF1_STAT's double-buffer flag, which MSCAL/MSCNT/OFFSET change): vif1Regs.stat stays the EE's, so
	// the two never write that word at once. Handed over at the EE's sync points.
	bool DbfGet();
	void DbfSet(bool v);
} // namespace ForjaVifT
