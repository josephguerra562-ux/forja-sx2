// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "OrbisCopy.h" // vk-285-98
#include "OrbisEEDiag.h" // vk-285-100
#include "VMManager.h"
#include "Vif_Dynarec.h"
#include "OrbisEEProf.h"
#include "ps4/core/WorkerProfilePS4.h"

#include <thread>
#if defined(PCSX2_PS4_VM_BOOT)
#include "ps4/core/ThreadCPUMetricsPS4.h"
#include "ps4/core/ThreadAffinityPS4.h"
#endif
#if defined(PCSX2_PS4)
// ps4/vm/PlatformVM.cpp: a "STAGE:" line with the PS4's free flexible memory.
extern "C" void orbis_stage(const char* stage) __attribute__((weak));
#endif

VU_Thread vu1Thread;
unsigned long long g_orbis_vu_idle_ticks, g_orbis_ee_waitvu_ticks, g_orbis_ee_vuring_ticks; // eerec-281
void OrbisCpuSample(int slot); // eerec-285 (GSRenderer.cpp)
void OrbisVUProfStart(); // vk-285-29 (orbis_eeprof.cpp)
std::atomic<int> g_orbis_vu_waiting{0}; // vk-285-29: the VU1 profiler skips the ring waits
// vk-285-74: VU1 cycles and program runs the MTVU thread executed (the [load] line prints them per
// second, so VU busy ms over VU1 cycles says how fast the recompiled code runs), and the vudump
// request the GS thread raises when /data/PCSX2/flags/vudump appears (served here, between
// programs, where the recompiler's lists can't change under it; x86/microVU.cpp).
std::atomic<u64> g_orbis_vu1_cycles{0}, g_orbis_vu1_runs{0};
// 152 in the app, "Ahorro del VU1" ([Forja] VU1Ahorro, set at the game's start by ps4/vm/main.cpp): the microVU runs of the PS5 port
// (microVU_Execute.inl) and, in ExecuteRingBuffer, a microcode upload with the bytes already there leaves the translated programs alone. The
// uploads and the ones kept (VU thread), for the heartbeat.
std::atomic<bool> g_forja_vu1_ahorro{false};
// 153 in the app, "Banderas muertas del VU1" ([Forja] VU1BanderasMuertas): microVU_Flags.inl reads it when it compiles VU1 code.
std::atomic<int> g_forja_vu1_dead_flags{0};
// 157 in the app, "Registros entre bloques" ([Forja] VU1RegistrosEntreBloques): microVU_Branch.inl reads it when it compiles VU1 code;
// the blocks compiled for a handed-on map and the branches that handed registers on (both counted at compile time).
std::atomic<bool> g_forja_vu1_keep_regs{false};
std::atomic<u64> g_forja_vu1_kept_blocks{0}, g_forja_vu1_kept_links{0};
// 158 in the app, "Tres operandos" ([Forja] VU1TresOperandos, microVU_Upper.inl) and "Memoria del VU1 en un registro"
// ([Forja] VU1MemoriaEnRegistro, microVU_Misc.inl): read when VU1 code is compiled; the copies left out, and the VU memory addresses
// that loaded the base into a GPR or reused it (all counted at compile time).
std::atomic<bool> g_forja_vu1_three_op{false}, g_forja_vu1_mem_reg{false};
// 159 in the app, "Banderas justas" ([Forja] VU1BanderasJustas, microVU_Flags.inl and microVU_Analyze.inl): read when VU1 code is
// compiled; the MAC flag calculations left out (counted at compile time).
std::atomic<bool> g_forja_vu1_mac_just{false};
std::atomic<u64> g_forja_vu1_mac_left_out{0};
// 160 in the app, "Banderas muertas seguras" (microVU_Flags.inl, microVU_Misc.inl): the VU1 programs that keep a flag exact at their
// jumps because their microcode reads it right after one, and the readers found at compile time that the scan hadn't seen.
std::atomic<u64> g_forja_vu1_flag_risk_progs{0}, g_forja_vu1_flag_missed{0};
// 161 in the app: the dead flags a program's start showed read (the previous program's flag): kept at every jump from then on.
std::atomic<int> g_forja_vu1_dead_flags_learned{0};
// 161 in the app, "Ciclos en una sola cuenta" ([Forja] VU1CuentaCiclos, microVU_Compile.inl): read when VU1 code is compiled.
std::atomic<bool> g_forja_vu1_one_count{false};
// 162 in the app, "MAC justa en los saltos" ([Forja] VU1MacEnSaltos, microVU_Flags.inl): read when VU1 code is compiled.
std::atomic<bool> g_forja_vu1_mac_jump{false};
// 163 in the app, "MAC al final" ([Forja] VU1MacAlFinal, microVU_Upper.inl, microVU_Flags.inl, microVU_Branch.inl): read when VU1
// code is compiled.
std::atomic<bool> g_forja_vu1_mac_end{false};
std::atomic<u64> g_forja_vu1_three_op_copies{0}, g_forja_vu1_mem_reg_loads{0}, g_forja_vu1_mem_reg_reuses{0};
u64 g_forja_mpg_total = 0, g_forja_mpg_same = 0;

#if defined(PCSX2_PS4_VM_BOOT)
extern void ProbeLog(const char* format, ...);
// Forja SX2 (compilation 35): the VU1's programs by the time they take, for a game's heaviest scenes. In God of War II's
// biggest fights the VU1 is the limit (2026-10-16: ~66 % of its thread in the game's own microcode), and a game's effects are
// VU1 programs of their own: this tells which start addresses (MSCAL) cost the most, the first step towards leaving out a
// costly effect per game, as Sony's emulator does for God of War. Every ~5 s the boot log gets "VU1 programs:" with the 8
// costliest, their share of the VU1's run time, their runs and their VU cycles a second. Only this thread writes the table.
namespace
{
	struct ForjaVU1Program
	{
		u64 ticks = 0, cycles = 0;
		u32 runs = 0;
	};
	ForjaVU1Program s_forja_vu1_programs[2048]; // by start address / 8
	u64 s_forja_vu1_programs_since = 0;
} // namespace

// Forja SX2 (compilation 44): the VU1 filter, an effects lab for any game (L3+R3 > Extras, main.cpp). A program the player
// leaves out (by its start address / 8, one bit each) doesn't run: what it would have drawn isn't drawn, and its VU1 time is
// saved. The last ~5 s's eight costliest programs (start / 8 << 16 | share in permille) are what the menu offers.
std::atomic<u32> g_forja_vu1_skip[64];
std::atomic<u32> g_forja_vu1_top[8];
std::atomic<u32> g_forja_vu1_top_count{0};
std::atomic<u32> g_forja_vu1_skipped_runs{0};

namespace
{

	void ForjaVU1ProgramsLog(u64 now)
	{
		if (s_forja_vu1_programs_since == 0)
		{
			s_forja_vu1_programs_since = now;
			return;
		}
		if (now - s_forja_vu1_programs_since < 5ull * 1600000000ull) // the PS4's TSC runs at ~1.6 GHz
			return;
		const double seconds = static_cast<double>(now - s_forja_vu1_programs_since) / 1600000000.0;
		s_forja_vu1_programs_since = now;
		u64 total = 0;
		for (const ForjaVU1Program& program : s_forja_vu1_programs)
			total += program.ticks;
		if (total == 0)
			return;
		// The 8 costliest, highest first (insertion into a short sorted list).
		unsigned top[8] = {};
		unsigned found = 0;
		for (unsigned i = 0; i < 2048; i++)
		{
			const u64 ticks = s_forja_vu1_programs[i].ticks;
			if (ticks == 0 || (found == 8 && ticks <= s_forja_vu1_programs[top[7]].ticks))
				continue;
			unsigned at = found < 8 ? found : 7;
			while (at > 0 && s_forja_vu1_programs[top[at - 1]].ticks < ticks)
			{
				top[at] = top[at - 1];
				at--;
			}
			top[at] = i;
			if (found < 8)
				found++;
		}
		char line[640];
		int len = std::snprintf(line, sizeof(line), "VU1 programs: run_time_pct=%.1f top8(start=share/runs_per_s/kcycles_per_s):",
			static_cast<double>(total) * 100.0 / (seconds * 1600000000.0));
		for (unsigned k = 0; k < found && len > 0 && len < static_cast<int>(sizeof(line)); k++)
		{
			const ForjaVU1Program& program = s_forja_vu1_programs[top[k]];
			len += std::snprintf(line + len, sizeof(line) - len, " %04x=%.1f%%/%.0f/%.0f", top[k] * 8,
				static_cast<double>(program.ticks) * 100.0 / static_cast<double>(total), program.runs / seconds,
				static_cast<double>(program.cycles) / seconds / 1000.0);
		}
		ProbeLog("%s\n", line);
		for (unsigned k = 0; k < found; k++)
		{
			const u32 permille = static_cast<u32>(s_forja_vu1_programs[top[k]].ticks * 1000 / total);
			g_forja_vu1_top[k].store((top[k] << 16) | std::min(permille, 1000u), std::memory_order_relaxed);
		}
		g_forja_vu1_top_count.store(found, std::memory_order_release);
		if (const u32 skipped = g_forja_vu1_skipped_runs.exchange(0, std::memory_order_relaxed))
			ProbeLog("VU1 filter: %.0f runs a second left out\n", skipped / seconds);
		for (ForjaVU1Program& program : s_forja_vu1_programs)
			program = ForjaVU1Program();
	}
} // namespace
#endif
std::atomic<int> g_orbis_vu1_dump_request{0};
void OrbisVU1Dump();
// vk-285-75: the emulated VU1 speed in percent, for Instant VU1 off (ExecuteVU): the busy time the EE
// sees is the recent programs' average cycles divided by this. Set by main-boot.cpp from the settings
// key EmuCore/Speedhacks/OrbisVU1Speed (gs.ini or a game's settings file; 25..800, default 100).
// Needs proper testing.
std::atomic<u32> g_orbis_vu1_speed{100};
// vk-285-77: how long VU1 programs run. Per second (GSRenderer.cpp prints "[vuruns]" and resets them):
// runs by length in VU1 cycles (<1k, <4k, <16k, <64k, <256k, <1M, <2.9M, and the rest, which is the
// 3M-cycle budget of one Execute: a program that didn't reach its E-bit), the longest run, and the start
// PC (bytes; 0xffff = continued, MSCNT) and VIF1 TOP of the last run of 1M cycles or more.
std::atomic<u32> g_orbis_vu1_run_hist[8];
std::atomic<u32> g_orbis_vu1_run_max{0}, g_orbis_vu1_long_pc{0}, g_orbis_vu1_long_top{0}, g_orbis_vu1_long_tpc{0};

// vk-285-80: the last 24 ring commands the MTVU thread ran (what reached VU1 memory before a program),
// and Shadow of the Colossus's lighting program (MSCAL 0xb0) checked at its start: it reads NLOOP at TOP
// (after copying qword 0 to 0x1a6) and counts down by 2, so an odd value can never end. Counts per second
// (GSRenderer.cpp prints them on the [vuruns] line) and the first 10 odd starts of a launch, each with the
// history. Needs proper testing.
std::atomic<u32> g_orbis_vu1_b0_runs{0}, g_orbis_vu1_b0_odd{0};

// vk-285-84: the EE thread's share of handing work to the VU thread (vk-285-83's SotC profile: about half
// of the EE thread at ~26,000 VU1 programs a frame). Set by main-boot.cpp from the settings keys
// EmuCore/Speedhacks/OrbisMTVUSpin and OrbisMTVUBatch (gs.ini or a game's settings file, live).
// - spin: the VU thread spins up to ~50 us for new work before it sleeps, so the EE's kick is rarely a
//   semaphore post (a system call; ~7% of the EE thread in that profile). Default on.
// - batch: inside a VIF1 transfer the kicks wait for its end (VU_Thread::BeginKickBatch). Default off.
// Needs proper testing.
std::atomic<int> g_orbis_mtvu_spin{1};
std::atomic<int> g_orbis_mtvu_batch{0};
// vk-285-90: the ring's length in KB (EmuCore/Speedhacks/OrbisMTVURingKB, live, taken at the next wrap):
// 16384 (the whole 16 MB buffer, as before) by default. A shorter lap keeps the lines the EE thread writes
// in the caches (a 16 MB lap comes back to lines long gone to memory). 256..16384. Needs proper testing.
std::atomic<u32> g_orbis_mtvu_ring_kb{16384};
extern std::atomic<bool> g_forja_vif_adelanto; // Vif1_Dma.cpp (170 in the app)
// 175 in the app, "VIF sin copia" ([Forja] VifSinCopia, set at the game's start by ps4/vm/main.cpp; the EE side in Vif_Unpack.cpp).
// What the EE spends on the VIF is mostly moving the data: each UNPACK's bytes read from the game's RAM and written into this ring
// (the 167-170 showed the bookkeeping isn't the cost). The VU thread can read them from the game's RAM itself, if the game hasn't
// written them again by then. A check (MTVU_VIF_UNPACK_CHECK) sends the copy and the address: the VU thread compares the copy with the
// RAM right when it would read it, and unpacks the copy.
// 176 in the app: the 175 found, at its 119th check, an 80-byte packet the game had already written again (per-frame data), and
// turned itself off. Now it learns per 4 KB page of the game's RAM: a page's packets go as checks until the page has 256 the same,
// then as their address (1 in 32 still a check); a page that ever gives a different one is per-frame data for good (always the copy).
// Mode: 0 off, 1 learns only (checks, never the address: vm-vif-sincopia-comprobar), 2 learns and uses. If pages that were already
// used give 8 different ones (a check sampled in mode 2 that failed), mode 1 for the rest of the session.
std::atomic<int> g_forja_vif_nocopy_mode{0};
std::atomic<bool> g_forja_vif_nocopy_check_only{false};
std::atomic<u64> g_forja_vif_nocopy_checked{0}, g_forja_vif_nocopy_wrong{0}, g_forja_vif_nocopy_ptr{0}, g_forja_vif_nocopy_late{0};
u32 g_forja_vif_nocopy_first_wrong[2] = {}; // the unpack's bytes, the first byte that differed
std::atomic<u16> g_forja_vif_page_good[Ps2MemSize::MainRam >> 12] = {};
std::atomic<u8> g_forja_vif_page_dynamic[Ps2MemSize::MainRam >> 12] = {};

static void ForjaVifNoCopyChecked(const u8* copy, const u8* live, u32 bytes, bool sampled)
{
	const uptr base = reinterpret_cast<uptr>(eeMem->Main);
	const u32 p0 = static_cast<u32>((reinterpret_cast<uptr>(live) - base) >> 12);
	const u32 p1 = static_cast<u32>((reinterpret_cast<uptr>(live) + bytes + 3 - base) >> 12);
	if (std::memcmp(copy, live, bytes) == 0)
	{
		g_forja_vif_nocopy_checked.store(g_forja_vif_nocopy_checked.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		for (u32 p = p0; p <= p1 && p < (Ps2MemSize::MainRam >> 12); p++)
		{
			const u16 g = g_forja_vif_page_good[p].load(std::memory_order_relaxed);
			if (g != 0xFFFF)
				g_forja_vif_page_good[p].store(static_cast<u16>(g + 1), std::memory_order_relaxed);
		}
		return;
	}
	if (g_forja_vif_nocopy_wrong.load(std::memory_order_relaxed) == 0)
	{
		u32 at = 0;
		while (at < bytes && copy[at] == live[at])
			at++;
		g_forja_vif_nocopy_first_wrong[0] = bytes;
		g_forja_vif_nocopy_first_wrong[1] = at;
	}
	for (u32 p = p0; p <= p1 && p < (Ps2MemSize::MainRam >> 12); p++)
		g_forja_vif_page_dynamic[p].store(1, std::memory_order_relaxed);
	g_forja_vif_nocopy_wrong.fetch_add(1, std::memory_order_release);
	if (sampled && g_forja_vif_nocopy_late.fetch_add(1, std::memory_order_relaxed) + 1 >= 8)
		g_forja_vif_nocopy_mode.store(1, std::memory_order_relaxed); // used pages going wrong: learn only from here
}
// 169 in the app: nothing ever set it (no EmuCore/Speedhacks/OrbisMTVURingKB is read): the lap was always the whole 16 MB, 8 times the
// 2 MB L2 the EE, VU1 and GS cores share (nucleos.txt: 1, 2, 3), so every line the EE writes into the ring and the VU thread reads back
// goes out to memory and returns (~120-250 MB/s in God of War II). ps4/vm/main.cpp now sets it from [Forja] VU1ColaKB.
extern std::atomic<u64> g_orbis_ee_evtests; // R5900.cpp
extern std::atomic<u64> g_orbis_ee_vif1ints; // Vif1_Dma.cpp
// Cumulative counts: event tests, VIF1 DMA interrupts, MTVU packets through the GIF fast path and through
// Execute, VIF1 unpacks handed to the VU thread, VU-thread kicks (locked adds on its semaphore), and
// (vk-285-93) ring wraps and the wraps that waited for the VU thread to leave the lap's first packet.
// vk-285-99: the VIF1 DMA tags read per ID (Vif1_Dma.cpp), for the [eestat] line.
extern u64 g_orbis_vif1_tag_ids[8];
void OrbisEETagStats(u64 out[8])
{
	for (int i = 0; i < 8; i++)
		out[i] = g_orbis_vif1_tag_ids[i];
}
void OrbisEEStats(u64 out[8])
{
	out[0] = g_orbis_ee_evtests.load(std::memory_order_relaxed);
	out[1] = g_orbis_ee_vif1ints.load(std::memory_order_relaxed);
	out[2] = gifUnit.orbis_mtvu_fast;
	out[3] = gifUnit.orbis_mtvu_slow;
	out[4] = vu1Thread.OrbisUnpacks(); // on the EE thread's line of VU_Thread
	out[5] = vu1Thread.OrbisKicks();
	out[6] = vu1Thread.OrbisWraps();
	out[7] = vu1Thread.OrbisLapWaits();
}
static s32 OrbisRingLimitWords(s32 buffer_words)
{
	u32 kb = g_orbis_mtvu_ring_kb.load(std::memory_order_relaxed);
	kb = std::clamp<u32>(kb, 256u, 16384u);
	return std::min<s32>(buffer_words, static_cast<s32>(kb * 256u)); // KB -> u32s
}
namespace
{
	struct OrbisMtvuHist
	{
		u8 type;
		u32 a, b, c, d;
	};
	OrbisMtvuHist s_orbis_hist[24];
	u32 s_orbis_hist_pos = 0;
	__fi void OrbisHist(u8 type, u32 a, u32 b, u32 c, u32 d)
	{
		s_orbis_hist[s_orbis_hist_pos % 24] = {type, a, b, c, d};
		s_orbis_hist_pos++;
	}
	// The history, oldest first, on the current line (the caller ends it).
	void OrbisPrintHist()
	{
		for (u32 i = 0; i < 24; i++)
		{
			const OrbisMtvuHist& h = s_orbis_hist[(s_orbis_hist_pos + i) % 24];
			switch (h.type)
			{
				case 1: printf(" [EXEC pc %04x top %03x at %u]", h.a, h.b, h.c); break;
				case 2: printf(" [UNPACK to %03x cmd %02x num %u size %u]", h.a, h.b, h.c, h.d); break;
				case 3: printf(" [DATA to %03x %u B]", h.a, h.b); break;
				case 4: printf(" [MICRO to %04x %u B]", h.a, h.b); break;
				case 5: printf(" [REGS]"); break;
				case 6: printf(" [WRAP at %u, lap %u]", h.a, h.b); break;
				default: break;
			}
		}
	}
} // namespace

#define MTVU_ALWAYS_KICK 0
#define MTVU_SYNC_MODE 0

// Rounds up a size in bytes for size in u32's
static __fi u32 size_u32(u32 x) { return (x + 3) >> 2; }

enum MTVU_EVENT
{
	MTVU_VU_EXECUTE,     // Execute VU program
	MTVU_VU_WRITE_MICRO, // Write to VU micro-mem
	MTVU_VU_WRITE_DATA,  // Write to VU data-mem
	MTVU_VU_WRITE_VIREGS,// Write to VU registers
	MTVU_VU_WRITE_VFREGS,// Write to VU registers
	MTVU_VIF_WRITE_COL,  // Write to Vif col reg
	MTVU_VIF_WRITE_ROW,  // Write to Vif row reg
	MTVU_VIF_UNPACK,     // Execute Vif Unpack
	MTVU_NULL_PACKET,    // Go back to beginning of buffer
	MTVU_RESET,
	MTVU_VIF1_TRAMO,     // 167 in the app, "VIF en tramos": a run of VIF1 codes (Vif_Dma.h)
	MTVU_VIF_UNPACK_CHECK, // 175 in the app, "VIF sin copia": an unpack's copy and the address it came from (compared)
	MTVU_VIF_UNPACK_PTR    // 175 in the app: an unpack read from the game's RAM
};

// Calls the vif unpack functions from the MTVU thread
static void MTVU_Unpack(void* data, VIFregisters& vifRegs)
{
	PS4_WORKER_SCOPE(VUUnpack);
	u16 wl = vifRegs.cycle.wl > 0 ? vifRegs.cycle.wl : 256;
	bool isFill = vifRegs.cycle.cl < wl;
	if (newVifDynaRec)
		dVifUnpack<1>((u8*)data, isFill);
	else
		_nVifUnpack(1, (u8*)data, vifRegs.mode, isFill);
}

// 167 in the app, "VIF en tramos": the VU thread reads a run with the same steps the EE took (ForjaTramoCode), from the registers the run
// started with; each UNPACK gets the state the normal path would have copied for it (VifUnpack) and unpacks as MTVU_VIF_UNPACK does.
// STROW/STCOL go to its own MaskRow/MaskCol, in their place among the UNPACKs, as WriteRow/WriteCol did.
static void ForjaVif1TramoRun(const u32* d, u32 words, ForjaTramoState s, u32 packet, vifStruct& vif, VIFregisters& vifRegs)
{
	for (u32 k = 0; k < words;)
	{
		ForjaTramoUnpack up;
		const u32 n = ForjaTramoCode(d, k, packet - k, s, &up, &vif.MaskRow, &vif.MaskCol);
		if (n == 0)
			break; // can't be: the EE took the same steps
		if (up.size)
		{
			vif.tag.addr = up.addr;
			vif.tag.size = up.size;
			vif.tag.cmd = up.cmd;
			vif.cmd = static_cast<int>(up.cmd);
			vif.pass = 1;
			vif.cl = 0;
			vif.usn = static_cast<u8>(up.usn);
			vif.start_aligned = static_cast<u8>(up.start_aligned);
			*reinterpret_cast<u32*>(&vifRegs.cycle) = s.cycle;
			vifRegs.mode = s.mode;
			vifRegs.mask = s.mask;
			vifRegs.num = up.num;
			MTVU_Unpack(const_cast<u32*>(&d[k + 1]), vifRegs);
		}
		k += n;
	}
	*reinterpret_cast<u32*>(&vifRegs.cycle) = s.cycle;
	vifRegs.mode = s.mode;
	vifRegs.mask = s.mask;
}

// Called on Saving/Loading states...
bool SaveStateBase::mtvuFreeze()
{
	if (!FreezeTag("MTVU"))
		return false;

	pxAssert(vu1Thread.IsDone());
	if (!IsSaving())
	{
		vu1Thread.Reset();
		vu1Thread.WriteCol(vif1);
		vu1Thread.WriteRow(vif1);
		vu1Thread.WriteMicroMem(0, VU1.Micro, 0x4000);
		vu1Thread.WriteDataMem(0, VU1.Mem, 0x4000);
		vu1Thread.WriteVIRegs(&VU1.VI[0]);
		vu1Thread.WriteVFRegs(&VU1.VF[0]);
	}
	for (size_t i = 0; i < 4; ++i)
	{
		unsigned int v = vu1Thread.vuCycles[i].load();
		Freeze(v);
	}

	u32 gsInterrupts = vu1Thread.mtvuInterrupts.load();
	Freeze(gsInterrupts);
	vu1Thread.mtvuInterrupts.store(gsInterrupts);
	u64 gsSignal = vu1Thread.gsSignal.load();
	Freeze(gsSignal);
	vu1Thread.gsSignal.store(gsSignal);
	u64 gsLabel = vu1Thread.gsLabel.load();
	Freeze(gsLabel);
	vu1Thread.gsLabel.store(gsLabel);

	Freeze(vu1Thread.vuCycleIdx);
	return IsOkay();
}

VU_Thread::VU_Thread()
{
	Reset();
}

VU_Thread::~VU_Thread()
{
	Close();
}

void VU_Thread::Open()
{
	if (IsOpen())
		return;

	Reset();
	semaEvent.Reset();
	m_shutdown_flag.store(false, std::memory_order_release);
	m_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	m_thread.Start([this]() { ExecuteRingBuffer(); });
#if defined(PCSX2_PS4)
	if (orbis_stage)
		orbis_stage("VU1 thread started");
#endif
}

void VU_Thread::Close()
{
	if (!IsOpen())
		return;

	m_shutdown_flag.store(true, std::memory_order_release);
	semaEvent.NotifyOfWork();
	m_thread.Join();
}

void VU_Thread::Reset()
{
	vuCycleIdx = 0;
	m_ato_write_pos = 0;
	m_write_pos = 0;
	m_ato_read_pos = 0;
	m_read_pos = 0;
	m_cached_read_pos = 0; // vk-285-84
	m_defer_kicks = false;
	m_kick_pending = false;
	m_orbis_batch = g_orbis_mtvu_batch.load(std::memory_order_relaxed) != 0; // vk-285-90
	m_orbis_ring_limit = OrbisRingLimitWords(buffer_size);
	std::memset(&vif, 0, sizeof(vif));
	std::memset(&vifRegs, 0, sizeof(vifRegs));
	for (size_t i = 0; i < 4; ++i)
		vu1Thread.vuCycles[i] = 0;
	vu1Thread.mtvuInterrupts = 0;
}

void VU_Thread::ExecuteRingBuffer()
{
#if defined(PCSX2_PS4_VM_BOOT)
	u32 cpu_sample_commands = 0;
#endif
	Threading::SetNameOfCurrentThread("MTVU");
#if defined(PCSX2_PS4_VM_BOOT)
	PS4ThreadAffinity::PinCurrentThread("VU1", 3);
#endif

	for (;;)
	{
		{
			PS4_WORKER_SCOPE(VUIdle);
			const unsigned long long t0 = __builtin_ia32_rdtsc(); // eerec-281
			g_orbis_vu_waiting.store(1, std::memory_order_relaxed); // vk-285-29
#if defined(PCSX2_PS4_VM_BOOT)
			PS4ThreadCPUMetrics::Sample(PS4ThreadCPUMetrics::Role::VU1);
			// Forja SX2: a game's profile can move the VU1 (the L3+R3 menu's Núcleos row).
			PS4ThreadAffinity::RepinIfPlanChanged("VU1", 3);
#endif
			if (g_orbis_mtvu_spin.load(std::memory_order_relaxed)) // vk-285-84
				semaEvent.WaitForWorkWithSpin();
			else
				semaEvent.WaitForWork();
			g_orbis_vu_waiting.store(0, std::memory_order_relaxed);
			g_orbis_vu_idle_ticks += __builtin_ia32_rdtsc() - t0;
			OrbisCpuSample(2); // eerec-285
			OrbisVUProfStart(); // vk-285-29
		}
		if (m_shutdown_flag.load(std::memory_order_acquire))
			break;

		while (m_ato_read_pos.load(std::memory_order_relaxed) != GetWritePos())
		{
			u32 tag = Read();
#if defined(PCSX2_PS4_VM_BOOT)
			// (AI-assisted) Avoid a clock check for every VU command in a busy ring.
			if ((++cpu_sample_commands & 255) == 0)
			{
				PS4ThreadCPUMetrics::Sample(PS4ThreadCPUMetrics::Role::VU1);
				// Forja SX2: in heavy scenes the ring never empties, so the check above the wait is not reached for minutes:
				// with Plan B picked in play the VU1 stayed on core 3, where the GS had just moved (2026-10-16, Tales).
				PS4ThreadAffinity::RepinIfPlanChanged("VU1", 3);
			}
#endif
			switch (tag)
			{
				case MTVU_VU_EXECUTE:
				{
					VU1.cycle = 0;
					s32 addr = Read();
					vifRegs.top = Read();
					vifRegs.itop = Read();
					vuFBRST = Read();
					if (addr != -1)
						VU1.VI[REG_TPC].UL = addr & 0x7FF;
					// vk-285-80: the lighting program's count, checked before it runs.
					if (addr != -1 && (addr & 0x7ff) == (0xb0 >> 3))
					{
						const u32* mem = reinterpret_cast<const u32*>(VU1.Mem);
						const u32 top = vifRegs.top & 0x3ff;
						// vk-285-81: the program reads its count with ILWR.x vi03, 0(vi00): qword 0, whatever TOP is
						// (vk-285-80 misread it as TOP). A count of 0 makes the loop go round 32768 times (~1M cycles),
						// its stores sweeping all of VU1 memory.
						const u32 nloop = mem[0] & 0x7fff;
						g_orbis_vu1_b0_runs.store(g_orbis_vu1_b0_runs.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
						if ((nloop & 1) || nloop == 0)
						{
							g_orbis_vu1_b0_odd.store(g_orbis_vu1_b0_odd.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
							static u32 s_logged = 0;
							if (s_logged < 10)
							{
								s_logged++;
								printf("[vuodd] #%u MSCAL 00b0 top %03x itop %03x nloop %u (odd or 0) | q[0] %08x %08x %08x %08x | q[top] %08x %08x %08x %08x | "
									   "q[1a6] %08x %08x %08x %08x | last ring commands (oldest first):",
									s_logged, top, vifRegs.itop & 0x3ff, nloop, mem[0], mem[1], mem[2], mem[3], mem[top * 4], mem[top * 4 + 1],
									mem[top * 4 + 2], mem[top * 4 + 3], mem[0x1a6 * 4], mem[0x1a6 * 4 + 1], mem[0x1a6 * 4 + 2], mem[0x1a6 * 4 + 3]);
								OrbisPrintHist();
								printf("\n");
								fflush(stdout);
							}
						}
					}
					OrbisHist(1, addr == -1 ? 0xffffu : static_cast<u32>(addr & 0x7ff) * 8, vifRegs.top & 0x3ff, static_cast<u32>(m_read_pos - 5), 0);
					// vk-285-82: the state each run starts from, for the long-run log after it.
					const u32 orbis_start_tpc = (VU1.VI[REG_TPC].UL & 0x7ff) * 8;
					const u16 orbis_start_vi3 = VU1.VI[3].US[0], orbis_start_vi4 = VU1.VI[4].US[0];
					const u32 orbis_start_q0 = reinterpret_cast<const u32*>(VU1.Mem)[0];
					CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
#if defined(PCSX2_PS4_VM_BOOT)
					const u64 forja_t0 = __builtin_ia32_rdtsc();
#endif
#if defined(PCSX2_PS4_VM_BOOT)
					// Forja SX2 (compilation 44): the VU1 filter: a program the player left out doesn't run.
					const u32 forja_program = (orbis_start_tpc / 8) & 2047;
					if ((g_forja_vu1_skip[forja_program >> 5].load(std::memory_order_relaxed) >> (forja_program & 31)) & 1)
						g_forja_vu1_skipped_runs.fetch_add(1, std::memory_order_relaxed);
					else
#endif
					{
						PS4_WORKER_SCOPE(VUExecute);
						CpuVU1->Execute(vu1RunCycles);
					}
#if defined(PCSX2_PS4_VM_BOOT)
					{
						const u64 forja_t1 = __builtin_ia32_rdtsc();
						ForjaVU1Program& program = s_forja_vu1_programs[(orbis_start_tpc / 8) & 2047];
						program.ticks += forja_t1 - forja_t0;
						program.cycles += VU1.cycle;
						program.runs++;
						ForjaVU1ProgramsLog(forja_t1);
					}
#endif
					gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU();
					semaXGkick.Post(); // Tell MTGS a path1 packet is complete
					vuCycles[vuCycleIdx].store(VU1.cycle, std::memory_order_release);
					vuCycleIdx = (vuCycleIdx + 1) & 3;
					// vk-285-74: only this thread writes them, so no locked adds.
					g_orbis_vu1_cycles.store(g_orbis_vu1_cycles.load(std::memory_order_relaxed) + VU1.cycle, std::memory_order_relaxed);
					g_orbis_vu1_runs.store(g_orbis_vu1_runs.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
					{
						// vk-285-77: the run's length (the GS thread reads and resets these once a second).
						const u64 c = VU1.cycle;
						const int b = c < 1024 ? 0 : c < 4096 ? 1 : c < 16384 ? 2 : c < 65536 ? 3 : c < 262144 ? 4 :
							c < 1048576 ? 5 : c < 2900000 ? 6 : 7;
						g_orbis_vu1_run_hist[b].store(g_orbis_vu1_run_hist[b].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
						const u32 c32 = static_cast<u32>(std::min<u64>(c, 0xffffffffu));
						if (c32 > g_orbis_vu1_run_max.load(std::memory_order_relaxed))
							g_orbis_vu1_run_max.store(c32, std::memory_order_relaxed);
						if (b >= 6)
						{
							// vk-285-82: every run of 1M cycles or more, the first 40 of a launch: where and with what it
							// started (a continuation starts at its TPC with the VI registers the last run left).
							static u32 s_long_logged = 0;
							if (s_long_logged < 40)
							{
								s_long_logged++;
								printf("[vulong] #%u %s %04x -> stop %04x, %llu cycles | at start: vi03 %04x vi04 %04x q0.x %08x top %03x | at stop: vi03 %04x vi04 %04x\n",
									s_long_logged, addr == -1 ? "MSCNT from" : "MSCAL", orbis_start_tpc, VU1.VI[REG_TPC].UL * 8,
									static_cast<unsigned long long>(c), orbis_start_vi3, orbis_start_vi4, orbis_start_q0, vifRegs.top & 0x3ff,
									VU1.VI[3].US[0], VU1.VI[4].US[0]);
								fflush(stdout);
							}
							g_orbis_vu1_long_pc.store(addr == -1 ? 0xffffu : static_cast<u32>(addr & 0x7ff) * 8, std::memory_order_relaxed);
							g_orbis_vu1_long_top.store(vifRegs.top, std::memory_order_relaxed);
							g_orbis_vu1_long_tpc.store(VU1.VI[REG_TPC].UL * 8, std::memory_order_relaxed);
						}
						if (b == 7)
						{
							// vk-285-78: a run that used the whole budget: its VI registers and the qwords the
							// program reads its count from (0, TOP, 0x1a6: Shadow of the Colossus's lighting program
							// copies qword 0 to 0x1a6 and reads NLOOP at TOP), the first 12 of a launch, then one
							// every ~5 s. Needs proper testing.
							static u32 s_logged = 0;
							static u64 s_last = 0;
							const u64 now = __builtin_ia32_rdtsc();
							if (s_logged < 12 || now - s_last > 5ull * 1600000000ull)
							{
								s_logged++;
								s_last = now;
								const u32* mem = reinterpret_cast<const u32*>(VU1.Mem);
								const auto q = [mem](u32 qw, char* out, size_t n) {
									const u32 i = (qw & 0x3ff) * 4;
									snprintf(out, n, "%08x %08x %08x %08x", mem[i], mem[i + 1], mem[i + 2], mem[i + 3]);
								};
								char q0[40], qt[40], qb[40];
								q(0, q0, sizeof(q0));
								q(vifRegs.top, qt, sizeof(qt));
								q(0x1a6, qb, sizeof(qb));
								printf("[vurunaway] start %04x stop %04x top %03x itop %03x | vi1-15:", addr == -1 ? 0xffffu :
									static_cast<u32>(addr & 0x7ff) * 8, VU1.VI[REG_TPC].UL * 8, vifRegs.top, vifRegs.itop);
								for (int i = 1; i < 16; i++)
									printf(" %04x", VU1.VI[i].US[0]);
								printf(" | q[0] %s | q[top] %s | q[1a6] %s\n", q0, qt, qb);
								fflush(stdout);
							}
						}
					}
					if (g_orbis_vu1_dump_request.load(std::memory_order_relaxed)) [[unlikely]]
					{
						g_orbis_vu1_dump_request.store(0, std::memory_order_relaxed);
						OrbisVU1Dump();
					}
					break;
				}
				case MTVU_VU_WRITE_MICRO:
				{
					u32 vu_micro_addr = Read();
					u32 size = Read();
					OrbisHist(4, vu_micro_addr, size, 0, 0); // vk-285-80
					// 152 in the app ("Ahorro del VU1"): God of War II uploads microcode ~3,400 times a second (vm-eediag, 2026-11-20). Each
					// Clear empties the quick table and the pipeline state, and the next runs search the programs again (bcmp, mVUcompileJIT).
					// Bytes equal to what is there change nothing: the translated programs stay right, as if no upload had come.
					g_forja_mpg_total++;
					if (g_forja_vu1_ahorro.load(std::memory_order_relaxed) && size &&
						std::memcmp(&VU1.Micro[vu_micro_addr], &buffer[m_read_pos], size) == 0)
					{
						m_read_pos += size_u32(size);
						g_forja_mpg_same++;
						break;
					}
					CpuVU1->Clear(vu_micro_addr, size);
					Read(&VU1.Micro[vu_micro_addr], size);
					break;
				}
				case MTVU_VU_WRITE_DATA:
				{
					u32 vu_data_addr = Read();
					u32 size = Read();
					OrbisHist(3, vu_data_addr >> 4, size, 0, 0); // vk-285-80
					Read(&VU1.Mem[vu_data_addr], size);
					break;
				}
				case MTVU_VU_WRITE_VIREGS:
					OrbisHist(5, 0, 0, 0, 0); // vk-285-80
					Read(&VU1.VI, size_u32(32));
					break;
				case MTVU_VU_WRITE_VFREGS:
					OrbisHist(5, 1, 0, 0, 0); // vk-285-80
					Read(&VU1.VF, size_u32(4*32));
					break;
				case MTVU_VIF_WRITE_COL:
					Read(&vif.MaskCol, sizeof(vif.MaskCol));
					break;
				case MTVU_VIF_WRITE_ROW:
					Read(&vif.MaskRow, sizeof(vif.MaskRow));
					break;
				case MTVU_VIF_UNPACK:
				{
					u32 vif_copy_size = static_cast<u32>((uptr)&vif.StructEnd - (uptr)&vif.tag);
					Read(&vif.tag, vif_copy_size);
					ReadRegs(&vifRegs);
					u32 size = Read();
					OrbisHist(2, (vif.tag.addr >> 4) & 0x3ff, vif.tag.cmd & 0xff, vifRegs.num, size); // vk-285-80
					MTVU_Unpack(&buffer[m_read_pos], vifRegs);
					m_read_pos += size_u32(size);
					break;
				}
				case MTVU_VIF1_TRAMO: // 167 in the app, "VIF en tramos"
				{
					const u32 words = Read();
					ForjaTramoState s;
					s.cycle = Read();
					s.mode = Read();
					s.mask = Read();
					s.tops = Read();
					s.base = Read();
					const u32 packet = Read();
					ForjaVif1TramoRun(&buffer[m_read_pos], words, s, packet, vif, vifRegs);
					m_read_pos += words + 1;
					break;
				}
				case MTVU_VIF_UNPACK_PTR: // 175 in the app, "VIF sin copia": straight from the game's RAM
				{
					u32 vif_copy_size = static_cast<u32>((uptr)&vif.StructEnd - (uptr)&vif.tag);
					Read(&vif.tag, vif_copy_size);
					ReadRegs(&vifRegs);
					Read(); // the size (the unpack knows its own)
					const u32 lo = Read();
					const u32 hi = Read();
					MTVU_Unpack(reinterpret_cast<void*>(static_cast<uptr>(lo) | (static_cast<uptr>(hi) << 32)), vifRegs);
					g_forja_vif_nocopy_ptr.store(g_forja_vif_nocopy_ptr.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
					break;
				}
				case MTVU_VIF_UNPACK_CHECK: // 175 in the app: the copy against the game's RAM now, then the copy unpacked
				{
					u32 vif_copy_size = static_cast<u32>((uptr)&vif.StructEnd - (uptr)&vif.tag);
					Read(&vif.tag, vif_copy_size);
					ReadRegs(&vifRegs);
					const u32 size = Read();
					const u32* copy = &buffer[m_read_pos];
					m_read_pos += size_u32(size);
					const u32 lo = Read();
					const u32 hi = Read();
					const u8* live = reinterpret_cast<const u8*>(static_cast<uptr>(lo) | (static_cast<uptr>(hi) << 32));
					const bool sampled = Read() != 0; // 176 in the app
					// The unpack's own bytes: VifUnpack is given 4 more (the word after them, which an unpack may read).
					ForjaVifNoCopyChecked(reinterpret_cast<const u8*>(copy), live, size >= 4 ? size - 4 : size, sampled);
					MTVU_Unpack(const_cast<u32*>(copy), vifRegs);
					break;
				}
				case MTVU_NULL_PACKET:
					m_orbis_rlaps++; // vk-285-93
					OrbisHist(6, static_cast<u32>(m_read_pos - 1), m_orbis_rlaps, 0, 0);
					m_read_pos = 0;
					break;
				default:
					// vk-285-93: was jNO_DEFAULT (a bad command jumped through the switch's table into the
					// weeds; vk-285-92's crashes at 6x). Now a report, then a stop.
					OrbisBadCommand(tag);
			}

			CommitReadPos();
		}
	}

	semaEvent.Kill();
}


// Should only be called by ReserveSpace()
__ri void VU_Thread::WaitOnSize(s32 size)
{
	// PS5 port (vk-285-9): every ReserveSpace comes through here, so test for room first; the [load]
	// timer (two rdtsc) and the profiler's wait scope (two locked ops) cost ~2% of the EE thread at a
	// fight's peak when they ran for every packet (vk-285-8 profile). Same conditions as the loop.
	// vk-285-84: first against the last read position this thread loaded (see m_cached_read_pos in
	// MTVU.h: an old value only under-states the room), then the shared one.
	{
		const s32 readPos = m_cached_read_pos;
		if (readPos <= m_write_pos || readPos > m_write_pos + size + _4kb)
			return;
	}
	{
		const s32 readPos = GetReadPos();
		m_cached_read_pos = readPos;
		if (readPos <= m_write_pos || readPos > m_write_pos + size + _4kb)
			return;
	}
	struct OrbisRingTimer { unsigned long long t0 = __builtin_ia32_rdtsc(); ~OrbisRingTimer() { g_orbis_ee_vuring_ticks += __builtin_ia32_rdtsc() - t0; } } orbis_ring_timer; // eerec-281
	OrbisEEWaitScope orbis_wait; // vk-285-8
	for (;;)
	{
		s32 readPos = GetReadPos();
		m_cached_read_pos = readPos; // vk-285-84
		if (readPos <= m_write_pos)
			break; // MTVU is reading in back of write_pos
		// FIXME greg: there is a bug somewhere in the queue pointer
		// management. It creates a deadlock/corruption in SotC intro (before
		// the first menu). I added a 4KB safety net which seem to avoid to
		// trigger the bug.
		// Note: a wait lock instead of a yield also helps to avoid the bug.
		if (readPos > m_write_pos + size + _4kb)
			break; // Enough free front space
		{          // Let MTVU run to free up buffer space
			KickStart();
			// Locking might trigger a full flush of the ring buffer. Yield
			// will be more aggressive, and only flush the minimal size.
			// Performance will be smoother but it will consume extra CPU cycle
			// on the EE thread (not an issue on 4 cores).
			std::this_thread::yield();
		}
	}
}

// PS5 port (vk-285-93): the wrap's missing wait. Ring positions don't say which lap they're in, and the
// checks above take a read position at or behind the write position as "the VU thread is behind, in this
// lap". That's wrong for one value: a read position of 0 when the write position goes back to 0. The VU
// thread then stands at the start of the lap just written (on its first packet, whose end it hasn't stored
// yet), with the whole lap unread, and both sides see an empty ring: WaitOnSize finds room at 0 and the new
// lap's first packets go over the old lap's; the VU thread, done with its packet, reads on at the new lap's
// data. It happens when the ring runs full (the EE a lap ahead, waiting in WaitOnSize), which is SotC's
// heavy view at 6x (vuring 100..300 ms a second): when the VU thread wraps, the EE is only the 4K-word
// safety margin from its own wrap, and a VU1 program at the lap's start (it may wait for the GS thread in its
// XGKICK) can outlast the EE's writing of those 16 KB. Both of vk-285-92's crashes read their bad command at position 5,
// right after a 5-word MTVU_VU_EXECUTE at 0. (Upstream's "FIXME greg" above is likely this bug; its 4 KB
// net only narrows the window.)
// So before the write position goes back to 0, wait for the read position to be past this lap's first
// packet and not past the NULL packet at null_pos (still in this lap: a lap cut shorter by OrbisMTVURingKB
// can leave the VU thread in the one before, beyond null_pos). The VU thread can't be at 0 again until it
// follows that NULL packet, which it can't see before the new write position is stored.
void VU_Thread::OrbisWaitLapStart(s32 null_pos)
{
	s32 readPos = GetReadPos();
	if (readPos != 0 && readPos <= null_pos) [[likely]]
		return;
	m_orbis_lap_waits++;
	struct OrbisRingTimer { unsigned long long t0 = __builtin_ia32_rdtsc(); ~OrbisRingTimer() { g_orbis_ee_vuring_ticks += __builtin_ia32_rdtsc() - t0; } } orbis_ring_timer;
	OrbisEEWaitScope orbis_wait;
	do
	{
		KickStart();
		std::this_thread::yield();
		readPos = GetReadPos();
	} while (readPos == 0 || readPos > null_pos);
	m_cached_read_pos = readPos;
}

// PS5 port (vk-285-93): a ring command the VU thread doesn't know. A report on boot.log (the command, where,
// both positions, the laps each side went round, the ring's words around it, and the last 24 commands run),
// then a stop: carrying on would leave the GS thread waiting for XGKICKs of programs never run.
void VU_Thread::OrbisBadCommand(u32 tag)
{
	const s32 pos = m_read_pos - 1;
	printf("[mtvubad] unknown ring command %08x at %d | read pos %d (stored %d), write pos stored %d (EE's own %d), limit %d words | "
		   "VU thread laps %u, EE wraps %llu, EE lap-start waits %llu\n",
		tag, pos, m_read_pos, m_ato_read_pos.load(std::memory_order_relaxed), m_ato_write_pos.load(std::memory_order_relaxed),
		m_write_pos, m_orbis_ring_limit, m_orbis_rlaps, static_cast<unsigned long long>(m_orbis_wraps),
		static_cast<unsigned long long>(m_orbis_lap_waits));
	const s32 from = std::max<s32>(0, pos - 24), to = std::min<s32>(buffer_size, pos + 40);
	printf("[mtvubad] ring words %d..%d:", from, to - 1);
	for (s32 i = from; i < to; i++)
		printf(i == pos ? " <%08x>" : " %08x", buffer[i]);
	printf("\n[mtvubad] last ring commands run (oldest first):");
	OrbisPrintHist();
	printf("\n");
	fflush(stdout);
	__builtin_trap();
}

// Makes sure theres enough room in the ring buffer
// to write a continuous 'size * sizeof(u32)' bytes
void VU_Thread::ReserveSpace(s32 size)
{
	pxAssert(m_write_pos < buffer_size);
	pxAssert(size < buffer_size);
	pxAssert(size > 0);

	if (m_write_pos + size > (m_orbis_ring_limit - 1)) // vk-285-90: was buffer_size
	{
		WaitOnSize(1); // Size of MTVU_NULL_PACKET
		Write(MTVU_NULL_PACKET);
		// vk-285-93: not before the VU thread has left this lap's first packet (see OrbisWaitLapStart).
		OrbisWaitLapStart(m_write_pos - 1);
		// Reset local write pointer/position
		m_write_pos = 0;
		CommitWritePos();
		m_orbis_wraps++;
		// vk-285-84: a fresh read position for the new lap (the cached one is only ever older).
		m_cached_read_pos = GetReadPos();
		// vk-285-90: the next lap's length. The VU thread may still be reading the last lap past a new,
		// shorter limit: it wraps at the NULL packet above, and WaitOnSize sees it as ahead (room enough).
		m_orbis_ring_limit = OrbisRingLimitWords(buffer_size);
	}

	WaitOnSize(size);

	// PS5 port (vk-285-99): the ring's lines 1 KB past this packet's end, asked for with write intent. Each
	// was last touched a 16 MB lap ago and is long out of the caches, so the packets' stores (~0.9 GB/s of
	// them in Shadow of the Colossus's heavy view) each waited on a read from memory in the store queue, and
	// every load that had to wait for an older store (see VifUnpack) waited on those. Needs proper testing.
	if (g_orbis_pfw.load(std::memory_order_relaxed)) // vk-285-100: flags/nopfw turns these off, live
	{
		constexpr s32 ahead = 256; // u32s: the lines this packet's span will be 1 KB on
		const s32 to = std::min<s32>(m_write_pos + size + ahead, m_orbis_ring_limit);
		for (s32 at = (m_write_pos + ahead) & ~15; at < to; at += 16)
			__builtin_prefetch(&buffer[at], 1, 3);
	}
}

// Use this when reading read_pos from ee thread
__fi s32 VU_Thread::GetReadPos()
{
	return m_ato_read_pos.load(std::memory_order_acquire);
}

// Use this when reading write_pos from vu thread
__fi s32 VU_Thread::GetWritePos()
{
	return m_ato_write_pos.load(std::memory_order_acquire);
}

// Gets the effective write pointer after
__fi u32* VU_Thread::GetWritePtr()
{
	pxAssert(m_write_pos < buffer_size);
	return &buffer[m_write_pos];
}

__fi void VU_Thread::CommitWritePos()
{
	m_ato_write_pos.store(m_write_pos, std::memory_order_release);

	if (MTVU_ALWAYS_KICK)
		KickStart();
	if (MTVU_SYNC_MODE)
		WaitVU();
}

__fi void VU_Thread::CommitReadPos()
{
	m_ato_read_pos.store(m_read_pos, std::memory_order_release);
}

__fi u32 VU_Thread::Read()
{
	u32 ret = buffer[m_read_pos];
	m_read_pos++;
	return ret;
}

__fi void VU_Thread::Read(void* dest, u32 size)
{
	memcpy(dest, &buffer[m_read_pos], size);
	m_read_pos += size_u32(size);
}

__fi void VU_Thread::ReadRegs(VIFregisters* dest)
{
	VIFregistersMTVU* src = (VIFregistersMTVU*)&buffer[m_read_pos];
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->itop = src->itop;
	dest->top = src->top;
	m_read_pos += size_u32(sizeof(VIFregistersMTVU));
}

__fi void VU_Thread::Write(u32 val)
{
	GetWritePtr()[0] = val;
	m_write_pos += 1;
}

__fi void VU_Thread::Write(const void* src, u32 size)
{
	memcpy(GetWritePtr(), src, size);
	m_write_pos += size_u32(size);
}

__fi void VU_Thread::WriteRegs(VIFregisters* src)
{
	VIFregistersMTVU* dest = (VIFregistersMTVU*)GetWritePtr();
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->top = src->top;
	dest->itop = src->itop;
	m_write_pos += size_u32(sizeof(VIFregistersMTVU));
}

// Returns Average number of vu Cycles from last 4 runs
// Used for vu cycle stealing hack
u32 VU_Thread::Get_vuCycles()
{
	return (vuCycles[0].load(std::memory_order_acquire) +
			vuCycles[1].load(std::memory_order_acquire) +
			vuCycles[2].load(std::memory_order_acquire) +
			vuCycles[3].load(std::memory_order_acquire)) >>
		   2;
}

void VU_Thread::Get_MTVUChanges()
{
	// Note: Atomic communication is with Gif_Unit.cpp Gif_HandlerAD_MTVU
	u32 interrupts = mtvuInterrupts.load(std::memory_order_relaxed);
	if (!interrupts)
		return;
	// vk-285-84: the E-bit flag now stays set between programs (mVUEBit sets it only when it's clear,
	// and it's cleared below only when there's a VU1 status bit for it to clear), so the line stays
	// shared instead of bouncing between the threads twice per program. The common case ends here.
	if (interrupts == InterruptFlagVUEBit && !(VU0.VI[REG_VPU_STAT].UL & 0xFF00))
		return;

	if (interrupts & InterruptFlagSignal)
	{
		std::atomic_thread_fence(std::memory_order_acquire);
		const u64 signal = gsSignal.load(std::memory_order_relaxed);
		// If load of signal was moved after clearing the flag, the other thread could write a new value before we load without noticing the double signal
		// Prevent that with release semantics
		mtvuInterrupts.fetch_and(~InterruptFlagSignal, std::memory_order_release);
		ORBIS_EEDIAG(OrbisEEDiag::mtvu_signal++); // 150 in the app
		GUNIT_WARN("SIGNAL firing");
		const u32 signalMsk = (u32)(signal >> 32);
		const u32 signalData = (u32)signal;
		if (CSRreg.SIGNAL)
		{
			GUNIT_WARN("Queue SIGNAL");
			gifUnit.gsSIGNAL.queued = true;
			//DevCon.Warning("Firing pending signal");
			gifUnit.gsSIGNAL.data[0] = signalData;
			gifUnit.gsSIGNAL.data[1] = signalMsk;
		}
		else
		{
			CSRreg.SIGNAL = true;
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~signalMsk) | (signalData & signalMsk);

			if (!GSIMR.SIGMSK)
				gsIrq();
		}
	}
	if (interrupts & InterruptFlagFinish)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagFinish, std::memory_order_relaxed);
		ORBIS_EEDIAG(OrbisEEDiag::mtvu_finish++); // 150 in the app
		GUNIT_WARN("Finish firing");
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;

		if (!gifUnit.checkPaths(false, true, true, true))
			Gif_FinishIRQ();
	}
	if (interrupts & InterruptFlagLabel)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagLabel, std::memory_order_acquire);
		// If other thread updates gsLabel for a second interrupt, that's okay.  Worst case we think there's a label interrupt but gsLabel is 0
		// We do not want the exchange of gsLabel to move ahead of clearing the flag, or the other thread could add more work before we clear the flag, resulting in an update with the flag unset
		// acquire semantics should supply that guarantee
		const u64 label = gsLabel.exchange(0, std::memory_order_relaxed);
		ORBIS_EEDIAG(OrbisEEDiag::mtvu_label++); // 150 in the app
		GUNIT_WARN("LABEL firing");
		const u32 labelMsk = (u32)(label >> 32);
		const u32 labelData = (u32)label;
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~labelMsk) | (labelData & labelMsk);
	}
	if (interrupts & InterruptFlagVUEBit)
	{
		// vk-285-84: only clears VU1 status bits with Instant VU1 on (as before); the flag is left set
		// while there are none to clear (see above). With Instant VU1 under MTVU only the T-bit below
		// sets them.
		if (INSTANT_VU1 && (VU0.VI[REG_VPU_STAT].UL & 0xFF00))
		{
			mtvuInterrupts.fetch_and(~InterruptFlagVUEBit, std::memory_order_relaxed);
			VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		}
		//DevCon.Warning("E-Bit registered %x", VU0.VI[REG_VPU_STAT].UL);
	}
	if (interrupts & InterruptFlagVUTBit)
	{
		// vk-285-84: an E-bit flag already seen above goes with it, so a stale one can't clear the
		// T-bit stop set here.
		mtvuInterrupts.fetch_and(~(InterruptFlagVUTBit | (interrupts & InterruptFlagVUEBit)), std::memory_order_relaxed);
		VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		VU0.VI[REG_VPU_STAT].UL |= 0x0400;
		ORBIS_EEDIAG(OrbisEEDiag::mtvu_tbit++); // 150 in the app
		//DevCon.Warning("T-Bit registered %x", VU0.VI[REG_VPU_STAT].UL);
		hwIntcIrq(7);
	}
}

void VU_Thread::KickStart()
{
	m_kick_pending = false; // vk-285-84
	m_orbis_kicks++; // vk-285-90
	semaEvent.NotifyOfWork();
}

// vk-285-84: see m_defer_kicks in MTVU.h. The VIF1 transfer (Vif_Transfer.cpp) brackets its packets with
// BeginKickBatch/EndKickBatch; WaitVU, WaitOnSize (KickStart), and MTGS's WaitGS, GenericStall and vsync
// queue waits flush a deferred kick before they wait. Needs proper testing.
__fi void VU_Thread::KickAfterWrite()
{
	if (m_defer_kicks)
		m_kick_pending = true;
	else
		KickStart();
}

void VU_Thread::FlushKick()
{
	if (m_kick_pending)
		KickStart();
}

bool VU_Thread::BeginKickBatch()
{
	const bool was = m_defer_kicks;
	m_defer_kicks = m_orbis_batch; // vk-285-90: refreshed per vsync (OrbisVsyncRefresh)
	return was;
}

void VU_Thread::OrbisVsyncRefresh()
{
	m_orbis_batch = g_orbis_mtvu_batch.load(std::memory_order_relaxed) != 0;
}

void VU_Thread::EndKickBatch(bool was_deferring)
{
	m_defer_kicks = was_deferring;
	if (!was_deferring)
		FlushKick();
}

bool VU_Thread::IsDone()
{
	return GetReadPos() == GetWritePos();
}

void VU_Thread::WaitVU()
{
	MTVU_LOG("MTVU - WaitVU!");
	FlushKick(); // vk-285-84: WaitForEmpty takes a spinning or sleeping thread as done
	const unsigned long long t0 = __builtin_ia32_rdtsc(); // eerec-281
	OrbisEEWaitScope orbis_wait; // vk-285-8
	semaEvent.WaitForEmpty();
	g_orbis_ee_waitvu_ticks += __builtin_ia32_rdtsc() - t0;
}

// Forja SX2 (compilation 135): the cycle skip in sixteenths of a VU1 program's cycles, -1 when the row's quarters rule (ExecuteVU).
std::atomic<int> g_forja_skip16{-1};

void VU_Thread::ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst)
{
	MTVU_LOG("MTVU - ExecuteVU!");
	Get_MTVUChanges(); // Clear any pending interrupts
	ReserveSpace(5);
	{
		// vk-285-90: written through a local pointer; m_write_pos (which the stores might alias, as far as the
		// compiler knows) was reloaded after every Write.
		const s32 wp = m_write_pos;
		u32* p = &buffer[wp];
		p[0] = MTVU_VU_EXECUTE;
		p[1] = vu_addr;
		p[2] = vif_top;
		p[3] = vif_itop;
		p[4] = fbrst;
		m_write_pos = wp + 5;
	}
	CommitWritePos();
	// vk-285-90: the kick (a locked add on the semaphore state, which also waits for this thread's pending
	// stores to reach the cache) before the MTGS packet, so the ring stores just written drain with it and
	// the MTGS ring's stores drain in the background. The data packets before it (unpacks, memory and
	// register writes) no longer kick on their own: this kick covers them.
	KickAfterWrite(); // vk-285-84
	gifUnit.TransferGSPacketData(GIF_TRANS_MTVU, NULL, 0);
	// vk-285-84: the recent programs' cycles (four atomics the VU thread writes per program, so a
	// cross-core miss) are only used by the EE cycle skip and by Instant VU1 off.
	// Forja SX2 (compilation 102): in quarters. 1 took Guitar Hero III to 100 %, with 18 % of its frames shown twice; 0 was at 90 %.
	// Compilation 135: in sixteenths while "Salto de ciclo: Al vuelo" has it (g_forja_skip16, ps4/vm/main.cpp, ForjaSkipInstantVsync, on this
	// same EE thread); -1 = the row's quarters.
	const int skip_fine = g_forja_skip16.load(std::memory_order_relaxed);
	const u32 cycle_skip16 = skip_fine >= 0 ? static_cast<u32>(skip_fine) :
	                                          (EmuConfig.Speedhacks.EECycleSkip * 4u + EmuConfig.Speedhacks.EECycleSkipQuarters) * 4u;
	const bool instant = INSTANT_VU1;
	u32 cycles = 4;
	if (cycle_skip16 || !instant)
	{
		cycles = std::max(Get_vuCycles(), 4u);
		const u32 skip_cycles = std::min(cycles, 3000u);
		cpuRegs.cycle += skip_cycles * cycle_skip16 / 16;
		VU0.cycle += skip_cycles * cycle_skip16 / 16;
	}
	Get_MTVUChanges();

	if (!instant)
	{
		// vk-285-75: OrbisVU1Speed scales the busy time (percent; 100 leaves it as counted).
		const u32 speed = g_orbis_vu1_speed.load(std::memory_order_relaxed);
		const u32 busy = (speed == 100 || speed == 0) ? cycles :
			std::max<u32>(4u, static_cast<u32>(static_cast<u64>(cycles) * 100u / speed));
		VU0.VI[REG_VPU_STAT].UL |= 0x100;
		CPU_INT(VU_MTVU_BUSY, busy);
	}
}

void VU_Thread::VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size, int nocopy)
{
	MTVU_LOG("MTVU - VifUnpack!");
	const u32 vif_copy_size = (u32)((uptr)&_vif.StructEnd - (uptr)&_vif.tag);
	const bool with_data = nocopy != 2; // 175 in the app (176: 3 is a sampled check, of a page already used)
	const s32 words = 1 + size_u32(vif_copy_size) + size_u32(sizeof(VIFregistersMTVU)) + 1 + (with_data ? size_u32(size) : 0) +
					  (nocopy == 2 ? 2 : nocopy ? 3 : 0);
	ReserveSpace(words);
	{
		// vk-285-90: the same layout as Write/WriteRegs, through a local pointer.
		const s32 wp = m_write_pos;
		u32* p = &buffer[wp];
		p[0] = nocopy == 2 ? MTVU_VIF_UNPACK_PTR : nocopy ? MTVU_VIF_UNPACK_CHECK : MTVU_VIF_UNPACK; // 175 in the app
		p += 1;
		// PS5 port (vk-285-99): the VIF state from tag to StructEnd field by field, each read with the width
		// vifUnpackSetup has just written it (volatile, so the compiler doesn't merge the reads). memcpy read
		// the 30 bytes as two 16-byte loads, and a load that spans several recent stores can't take its value
		// from them: it waited for all of them to reach the cache, behind every older store in the queue
		// (the ring's). The same bytes, in the same places, as the VU thread's memcpy expects them.
		static_assert(offsetof(vifStruct, cmd) - offsetof(vifStruct, tag) == 16 &&
					  offsetof(vifStruct, StructEnd) - offsetof(vifStruct, tag) == 30, "vifStruct layout");
		{
			const auto rd32 = [](const auto& field) { return *reinterpret_cast<const volatile u32*>(&field); };
			const auto rd16 = [](const auto& field) { return static_cast<u32>(*reinterpret_cast<const volatile u16*>(&field)); };
			const auto rd8 = [](const auto& field) { return static_cast<u32>(*reinterpret_cast<const volatile u8*>(&field)); };
			p[0] = rd32(_vif.tag.addr);
			p[1] = rd32(_vif.tag.size);
			p[2] = rd32(_vif.tag.cmd);
			p[3] = rd16(_vif.tag.wl) | (rd16(_vif.tag.cl) << 16);
			p[4] = rd32(_vif.cmd);
			p[5] = rd32(_vif.pass);
			p[6] = rd32(_vif.cl);
			p[7] = rd8(_vif.usn) | (rd8(_vif.start_aligned) << 8);
		}
		p += size_u32(vif_copy_size);
		VIFregistersMTVU* regs = reinterpret_cast<VIFregistersMTVU*>(p);
		regs->cycle = _vifRegs.cycle;
		regs->mode = _vifRegs.mode;
		regs->num = _vifRegs.num;
		regs->mask = _vifRegs.mask;
		regs->top = _vifRegs.top;
		regs->itop = _vifRegs.itop;
		p += size_u32(sizeof(VIFregistersMTVU));
		p[0] = size;
		p += 1;
		if (with_data)
		{
			OrbisCopy(p, data, size); // vk-285-98 (OrbisCopy.h)
			p += size_u32(size);
		}
		if (nocopy) // 175 in the app: where the data is in the game's RAM
		{
			const uptr at = reinterpret_cast<uptr>(data);
			p[0] = static_cast<u32>(at);
			p[1] = static_cast<u32>(static_cast<u64>(at) >> 32);
			if (nocopy != 2)
				p[2] = nocopy == 3 ? 1u : 0u; // 176 in the app: a sampled check (its page was being read without the copy)
		}
		m_write_pos = wp + words;
		// 170 in the app, "VIF1 por adelantado": the ring's next 512 bytes asked for writing now (the 16 MB ring's lines are long out of the
		// caches, and each write first has to bring its line in), as Gif_Path::CopyGSPacketData does for the GS packets (vk-285-124).
		if (g_forja_vif_adelanto.load(std::memory_order_relaxed) && m_write_pos + 128 < buffer_size)
		{
			const char* next = reinterpret_cast<const char*>(&buffer[m_write_pos]);
			for (int off = 0; off < 512; off += 64)
				__builtin_prefetch(next + off, 1, 3);
		}
	}
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
	m_orbis_unpacks++;
}

// 167 in the app, "VIF en tramos": the VIF registers at the run's start, the DMA piece's words left there (each UNPACK's start_aligned),
// then the codes and the word after them (VifUnpack copies 4 bytes past an UNPACK's data too: the same word for the run's last one).
// 168 in the app: the EE copies the codes in while it walks them (Vif_Transfer.cpp): Begin reserves room for the longest run and returns
// where its codes go (after the 8 header words); End writes the header and the word after the codes, then commits.
u32* VU_Thread::Vif1TramoBegin(u32 max_words)
{
	ReserveSpace(8 + static_cast<s32>(max_words) + 1);
	return &buffer[m_write_pos + 8];
}

void VU_Thread::Vif1TramoEnd(u32 words, const ForjaTramoState& s, u32 packet, u32 unpacks, u32 next_word)
{
	const s32 wp = m_write_pos;
	u32* p = &buffer[wp];
	p[0] = MTVU_VIF1_TRAMO;
	p[1] = words;
	p[2] = s.cycle;
	p[3] = s.mode;
	p[4] = s.mask;
	p[5] = s.tops;
	p[6] = s.base;
	p[7] = packet;
	p[8 + words] = next_word;
	m_write_pos = wp + 8 + static_cast<s32>(words) + 1;
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
	m_orbis_unpacks += unpacks;
}

void VU_Thread::WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size)
{
	MTVU_LOG("MTVU - WriteMicroMem!");
	ReserveSpace(3 + size_u32(size));
	Write(MTVU_VU_WRITE_MICRO);
	Write(vu_micro_addr);
	Write(size);
	Write(data, size);
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
}

void VU_Thread::WriteDataMem(u32 vu_data_addr, const void* data, u32 size)
{
	MTVU_LOG("MTVU - WriteDataMem!");
	ReserveSpace(3 + size_u32(size));
	Write(MTVU_VU_WRITE_DATA);
	Write(vu_data_addr);
	Write(size);
	Write(data, size);
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
}

void VU_Thread::WriteVIRegs(REG_VI* viRegs)
{
	MTVU_LOG("MTVU - WriteRegs!");
	ReserveSpace(1 + size_u32(32));
	Write(MTVU_VU_WRITE_VIREGS);
	Write(viRegs, size_u32(32));
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
}

void VU_Thread::WriteVFRegs(VECTOR* vfRegs)
{
	MTVU_LOG("MTVU - WriteRegs!");
	ReserveSpace(1 + size_u32(32*4));
	Write(MTVU_VU_WRITE_VFREGS);
	Write(vfRegs, size_u32(32*4));
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
}

void VU_Thread::WriteCol(vifStruct& _vif)
{
	MTVU_LOG("MTVU - WriteCol!");
	ReserveSpace(1 + size_u32(sizeof(_vif.MaskCol)));
	Write(MTVU_VIF_WRITE_COL);
	Write(&_vif.MaskCol, sizeof(_vif.MaskCol));
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
}

void VU_Thread::WriteRow(vifStruct& _vif)
{
	MTVU_LOG("MTVU - WriteRow!");
	ReserveSpace(1 + size_u32(sizeof(_vif.MaskRow)));
	Write(MTVU_VIF_WRITE_ROW);
	Write(&_vif.MaskRow, sizeof(_vif.MaskRow));
	CommitWritePos();
	OrbisDataKick(); // vk-285-90
}
