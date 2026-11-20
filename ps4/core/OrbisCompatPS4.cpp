// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// (AI-assisted) Minimal defaults for optional instrumentation inherited from the PS5 port.

#include "pcsx2/OrbisEEHle.h"
#include "common/Pcsx2Defs.h"

#include <atomic>

bool OrbisFlag(const char*)
{
	return false;
}

void OrbisCpuSample(int)
{
}

void OrbisVUProfStart()
{
}

void OrbisEEVsyncClock()
{
}

// Preserve the upstream-safe behavior of PS5-only fast paths until the PS4
// build has a runtime configuration adapter for them.
std::atomic<int> g_orbis_hle_flush{0};
std::atomic<int> g_orbis_pfw{0};
std::atomic<int> g_orbis_gs_pfw{0};
std::atomic<int> g_orbis_hle_tid{0};
std::atomic<int> g_orbis_eret_fast{0};
std::atomic<int> g_orbis_vif1_fuse{0};
u64 g_orbis_vif1_fused = 0;

// 150 in the app: the PS5 port's EE counters (pcsx2/OrbisEEDiag.h), kept by the EE thread while /data/ForjaSX2/vm-eediag is there.
namespace OrbisEEDiag
{
	std::atomic<int> on{0};
	u64 syscalls[256] = {};
	u64 cpu_int[32] = {};
	u64 dmac_irq[16] = {};
	u64 intc_irq[16] = {};
	u64 exc_intc = 0, exc_dmac = 0;
	u64 ev_dma = 0;
	u64 iop_rapid = 0;
	u64 rcnt = 0;
	u64 vif1_codes[128] = {};
	u64 vif1_irq_codes = 0, mtvu_finish = 0, mtvu_signal = 0, mtvu_label = 0, mtvu_tbit = 0;
} // namespace OrbisEEDiag
OrbisTidCache g_orbis_tid{};
u64 g_orbis_hle_flushes = 0;
u64 g_orbis_tid_hits = 0;
u64 g_orbis_tid_checks = 0;
u64 g_orbis_tid_mismatches = 0;
u64 g_orbis_eret_skips = 0;
u64 g_orbis_eret_block[4] = {};
