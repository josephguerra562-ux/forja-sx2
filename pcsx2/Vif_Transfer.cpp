// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Vif_Dma.h"
#include "Gif_Unit.h" // 167 in the app: gifUnit.checkPaths
#include "OrbisCopy.h" // 168 in the app
#include "Vif_Dynarec.h"
#include "MTVU.h" // vk-285-84: VU_Thread kick batching
#include "OrbisEEDiag.h" // vk-285-100

// 167 in the app, "VIF en tramos" (Vif_Dma.h says what it is).
std::atomic<int> g_forja_vif_tramos_mode{0};
std::atomic<bool> g_forja_vif_tramos_check_only{false}; // vm-vif-tramos-comprobar: it checks and never goes on by itself
std::atomic<u64> g_forja_vif_tramos_runs{0}, g_forja_vif_tramos_unpacks{0}, g_forja_vif_tramos_words{0};
std::atomic<u64> g_forja_vif_tramos_checked{0}, g_forja_vif_tramos_wrong{0};
// The first wrong one: the prediction and what the normal path sent (data pointer the same, addr, size, cmd, usn, start_aligned, num,
// cycle, mode, mask). Written once by the EE thread before g_forja_vif_tramos_wrong becomes 1 (release); main.cpp's meter prints it.
u32 g_forja_vif_tramos_first_wrong[2][10] = {};
extern std::atomic<int> g_orbis_vif1_exec_early; // Vif_Unpack.cpp
#if defined(PCSX2_PS4)
// 193 in the app, "VIF en su hilo", step 2 (informes/VIF_HILO_PROPIO_PLAN_2026-11-22.md): each VIF1 piece (one vifTransfer) measured.
// "Hard" codes are the ones another thread couldn't run by itself: DIRECT/DIRECTHL (the GIF's PATH2, ordered with PATH3 and PATH1),
// FLUSH/FLUSHA/MSCALF (wait for the GIF), MARK, MSKPATH3, and any code with the interrupt bit. [0] pieces without one, [1] with;
// codes, the EE's time (TSC ticks), and in the hard pieces the codes before the first hard one (another thread could take those).
u64 g_forja_vifm_pieces[2] = {}, g_forja_vifm_codes[2] = {}, g_forja_vifm_ticks[2] = {}, g_forja_vifm_hardcodes = 0, g_forja_vifm_prefix = 0;
u64 g_forja_vifm_hard_by[8] = {}; // 0 DIRECT, 1 DIRECTHL, 2 FLUSH, 3 FLUSHA, 4 MSCALF, 5 MARK, 6 MSKPATH3, 7 interrupt bit
static u32 s_forja_vifm_codes = 0, s_forja_vifm_first_hard = 0;
static bool s_forja_vifm_hard = false;
#endif

namespace
{
	// Mode 1: the UNPACKs the walk predicted for the run the normal path is about to read, in order. Only good inside one
	// vifTransferLoop (cleared at its start: a stall or the piece's end leaves the rest unread).
	struct ForjaTramoPrediction
	{
		ForjaTramoUnpack up;
		u32 cycle, mode, mask;
		const u32* data;
	};
	ForjaTramoPrediction s_tramo_pred[256];
	u32 s_tramo_pred_n = 0, s_tramo_pred_i = 0;
} // namespace

// Walks a run of VIF1 codes from data (pSize words left in the DMA piece). live: applies the EE's side of each code (as its handler
// would) and sends the run to the VU thread; else (mode 1) only predicts its UNPACKs. Returns the words the run takes (0: the first
// code goes the normal way).
static u32 ForjaVif1Tramo(const u32* data, u32 pSize, bool live)
{
	// 168 in the app: room for the run is reserved before the walk (Vif1TramoBegin), so not too much: 4096 words (the 167 took up to 16384;
	// GoW's runs are ~90 words).
	constexpr u32 kMaxWords = 4096;
	const u32 limit = std::min<u32>(pSize, kMaxWords);
	ForjaTramoState s;
	s.cycle = *reinterpret_cast<const u32*>(&vif1Regs.cycle) & 0xffffu;
	s.mode = vif1Regs.mode;
	s.mask = vif1Regs.mask;
	s.tops = vif1Regs.tops;
	s.base = vif1Regs.base;
	const ForjaTramoState s0 = s;
	// A queued VU1 program starts at the UNPACK that takes unpackcalls past exec_after (vifUnpackSetup) or at a NOP (the loop): what
	// came before it must reach the ring first. Only if it really would start (vifExecQueue's own tests; nothing in a run changes them).
	bool queued = vif1.queued_program && !(VU0.VI[REG_VPU_STAT].UL & 0x100) && !(vif1.queued_gif_wait && gifUnit.checkPaths(1, 1, 0));
	const int exec_after = g_orbis_vif1_exec_early.load(std::memory_order_relaxed) ? 0 : 3;
	int calls = vif1.unpackcalls;
	u32 k = 0, unpacks = 0, last_code = 0;
	// 168 in the app: the run goes into the ring as the walk goes (each UNPACK's code and data copied as it's passed). The 167 walked
	// first, reading only the code words, ~240 bytes apart and not in the caches: a wait for memory per UNPACK (ForjaVif1Tramo 7.8 % of
	// the EE in the Colossus), and then copied it all again. Copying as it goes streams the data as VifUnpack did, and the next code is
	// the word right after what was just copied: already in the cache.
	u32* out = nullptr;
	u32 copied = 0;
	if (!live)
		s_tramo_pred_n = s_tramo_pred_i = 0;
	while (k < limit)
	{
		const u32 code = data[k];
		const u32 cmd = (code >> 24) & 0x7f;
		if (!live && cmd >= 0x60 && s_tramo_pred_n >= sizeof(s_tramo_pred) / sizeof(s_tramo_pred[0]))
			break;
		ForjaTramoUnpack up;
		// Only an UNPACK or a NOP can stop the run after this call, and neither changes s.
		const u32 n = ForjaTramoCode(data, k, pSize - k, s, &up, live ? &vif1.MaskRow : nullptr, live ? &vif1.MaskCol : nullptr);
		if (n == 0 || k + n > limit)
			break;
		if (cmd == 0x00)
		{
			if (pSize - k > 1 && ((data[k + 1] >> 24) & 0x7f) == 0x6 && (data[k + 1] & 0x1))
				break; // MSKPATH3 next: the loop stalls at this NOP
			if (queued)
			{
				if (k)
					break; // the program starts at this NOP: the run so far first
				queued = false;
				if (live)
					vifExecQueue(1);
			}
		}
		else if (up.size)
		{
			if (queued && calls + 1 > exec_after)
			{
				if (k)
					break; // the program starts before this UNPACK's data: the run so far first
				queued = false;
				if (live)
					vifExecQueue(1);
			}
			calls++;
			unpacks++;
			if (live)
			{
				// vifUnpackSetup, then nVifUnpack's whole transfer
				vif1.usn = static_cast<u8>(up.usn);
				vif1.tag.addr = up.addr;
				vif1.tag.cmd = up.cmd;
				vif1.tag.size = 0;
				vif1.cl = 0;
				vif1.start_aligned = static_cast<u8>(up.start_aligned);
			}
			else
				s_tramo_pred[s_tramo_pred_n++] = {up, s.cycle, s.mode, s.mask, data + k + 1};
		}
		else if (live)
		{
			switch (cmd)
			{
				case 0x01: *reinterpret_cast<u32*>(&vif1Regs.cycle) = s.cycle; break;
				case 0x02:
					vif1Regs.stat.DBF = false;
					vif1Regs.ofst = code & 0x3ff;
					vif1Regs.tops = vif1Regs.base;
					break;
				case 0x03: vif1Regs.base = code & 0x3ff; break;
				case 0x04: vif1Regs.itops = code & 0x3ff; break;
				case 0x05: vif1Regs.mode = s.mode; break;
				case 0x20:
					vif1Regs.mask = s.mask;
					vif1.tag.size = 0;
					break;
				case 0x30:
				case 0x31:
					vif1.tag.addr = 4;
					vif1.tag.size = 0;
					break;
				default: break;
			}
		}
		// 168 in the app: the ring's room once this run has its first code (a queued program this code starts went to the ring first,
		// above); nothing else writes to the ring until Vif1TramoEnd.
		if (live && !out)
			out = vu1Thread.Vif1TramoBegin(limit);
		last_code = code;
		k += n;
		if (live && up.size)
		{
			OrbisCopy(out + copied, data + copied, (k - copied) * 4);
			copied = k;
		}
	}
	if (k == 0 || !live)
		return k;
	vif1Regs.code = last_code;
	if (unpacks)
		vif1Regs.num = 0;
	vif1.unpackcalls = calls;
	if (k > copied)
		OrbisCopy(out + copied, data + copied, (k - copied) * 4);
	vu1Thread.Vif1TramoEnd(k, s0, pSize, unpacks, data[k]); // 168 in the app
	g_forja_vif_tramos_runs.fetch_add(1, std::memory_order_relaxed);
	g_forja_vif_tramos_unpacks.fetch_add(unpacks, std::memory_order_relaxed);
	g_forja_vif_tramos_words.fetch_add(k, std::memory_order_relaxed);
	return k;
}

void ForjaVif1TramoCheck(const vifStruct& vif, const VIFregisters& regs, const void* data)
{
	if (s_tramo_pred_i >= s_tramo_pred_n)
		return; // an UNPACK no run covered (split between pieces, after a stop): nothing to compare
	const ForjaTramoPrediction& p = s_tramo_pred[s_tramo_pred_i++];
	const u32 cycle = *reinterpret_cast<const u32*>(&regs.cycle) & 0xffffu;
	const bool same = p.data == data && p.up.addr == vif.tag.addr && p.up.size == vif.tag.size && p.up.cmd == vif.tag.cmd &&
					  p.up.cmd == static_cast<u32>(vif.cmd) && p.up.usn == vif.usn && p.up.start_aligned == vif.start_aligned &&
					  vif.cl == 0 && vif.pass == 1 && p.up.num == regs.num && p.cycle == cycle && p.mode == regs.mode &&
					  p.mask == regs.mask;
	if (same)
	{
		const u64 ok = g_forja_vif_tramos_checked.fetch_add(1, std::memory_order_relaxed) + 1;
		if (ok >= 100000 && !g_forja_vif_tramos_check_only.load(std::memory_order_relaxed) &&
			g_forja_vif_tramos_wrong.load(std::memory_order_relaxed) == 0)
			g_forja_vif_tramos_mode.store(2, std::memory_order_relaxed);
		return;
	}
	if (g_forja_vif_tramos_wrong.load(std::memory_order_relaxed) == 0)
	{
		const u32 a[10] = {1, p.up.addr, p.up.size, p.up.cmd, p.up.usn, p.up.start_aligned, p.up.num, p.cycle, p.mode, p.mask};
		const u32 b[10] = {p.data == data ? 1u : 0u, vif.tag.addr, vif.tag.size, vif.tag.cmd, vif.usn, vif.start_aligned, regs.num,
						   cycle, regs.mode, regs.mask};
		memcpy(g_forja_vif_tramos_first_wrong[0], a, sizeof(a));
		memcpy(g_forja_vif_tramos_first_wrong[1], b, sizeof(b));
	}
	g_forja_vif_tramos_wrong.fetch_add(1, std::memory_order_release);
	g_forja_vif_tramos_mode.store(0, std::memory_order_relaxed);
	s_tramo_pred_n = s_tramo_pred_i = 0;
}

//------------------------------------------------------------------
// VifCode Transfer Interpreter (Vif0/Vif1)
//------------------------------------------------------------------

// Interprets packet
_vifT void vifTransferLoop(u32* &data) {
	vifStruct& vifX = GetVifX;

	u32& pSize = vifX.vifpacketsize;

	int ret = 0;

	vifXRegs.stat.VPS |= VPS_TRANSFERRING;
	vifXRegs.stat.ER1  = false;
	if (idx)
		s_tramo_pred_n = s_tramo_pred_i = 0; // 167 in the app: predictions don't outlive the loop
	//VIF_LOG("Starting VIF%d loop, pSize = %x, stalled = %x", idx, pSize, vifX.vifstalled.enabled );
	while (pSize > 0 && !vifX.vifstalled.enabled) {

		// 167 in the app, "VIF en tramos": a run of codes only the VU1 needs goes to the VU thread in one piece (mode 2), or (mode 1) the
		// walk predicts its UNPACKs for the normal path to compare.
		if (idx && !vifX.cmd && !vifX.pass && !vifX.irq && THREAD_VU1)
		{
			const int tramos = g_forja_vif_tramos_mode.load(std::memory_order_relaxed);
			if (tramos == 2)
			{
				const u32 n = ForjaVif1Tramo(data, pSize, true);
				if (n)
				{
					data += n;
					pSize -= n;
					continue;
				}
			}
			else if (tramos == 1 && s_tramo_pred_i == s_tramo_pred_n)
				ForjaVif1Tramo(data, pSize, false);
		}

		if(!vifX.cmd) { // Get new VifCode

			if(!vifXRegs.err.MII)
			{
				if(vifX.irq && !CHECK_VIF1STALLHACK)
					break;

				vifX.irq      |= data[0] >> 31;
			}

			vifXRegs.code = data[0];
			vifX.cmd	  = data[0] >> 24;
#if defined(PCSX2_PS4)
			if (idx) // 193 in the app: the piece's codes and its first hard one
			{
				const u32 c = vifX.cmd & 0x7f;
				const int hb = (vifX.cmd & 0x80) ? 7 : c == 0x50 ? 0 : c == 0x51 ? 1 : c == 0x11 ? 2 : c == 0x13 ? 3 : c == 0x15 ? 4 : c == 0x07 ? 5 :
								 c == 0x06 ? 6 : -1;
				if (hb >= 0)
				{
					if (!s_forja_vifm_hard)
						s_forja_vifm_first_hard = s_forja_vifm_codes;
					s_forja_vifm_hard = true;
					g_forja_vifm_hardcodes++;
					g_forja_vifm_hard_by[hb]++;
				}
				s_forja_vifm_codes++;
			}
#endif
			if (idx)
				ORBIS_EEDIAG(OrbisEEDiag::vif1_codes[vifX.cmd & 0x7f]++; OrbisEEDiag::vif1_irq_codes += vifX.cmd >> 7); // vk-285-100; 150


			VIF_LOG("New VifCMD %x tagsize %x irq %d", vifX.cmd, vifX.tag.size, vifX.irq);
			if (IsDevBuild && TraceLogging.EE.VIFcode.IsActive()) {
				// Pass 2 means "log it"
				vifCmdHandler[idx][vifX.cmd & 0x7f](2, data);
			}
		}

		// PS5 port (vk-285-96): a NOP (code 0, no IRQ bit) without the handler table's indirect call:
		// vifCode_Nop's pass 1, step for step. Shadow of the Colossus's VIF1 lists have enough of them
		// that the handler was ~1.7% of the EE thread at its heaviest view (vk-285-95's profile).
		if (vifX.cmd == 0 && vifX.pass == 0)
		{
			if (vifX.queued_program)
				vifExecQueue(idx);
			if (pSize > 1 && ((data[1] >> 24) & 0x7f) == 0x6 && (data[1] & 0x1)) // is mskpath3 next
			{
				vifX.vifstalled.enabled = VifStallEnable(vifXch);
				vifX.vifstalled.value = VIF_TIMING_BREAK;
			}
			data  += 1;
			pSize -= 1;
			continue;
		}

		// PS5 port (vk-285-102): the simple register codes too (pass 1 of vifCode_STMod, _STCycl, _Base, _Offset
		// and _ITop, step for step), without the handler table's indirect call. Shadow of the Colossus's heavy
		// view, per second: STMOD 1.54M, STCYCL 0.43M, BASE and OFFSET 0.18M each (of 11.6M VIF1 codes,
		// vk-285-101's [vifcodes]). IRQ-bit forms (cmd | 0x80) still take the table. flags/vifslow turns
		// these off, live. Needs proper testing.
		if (vifX.pass == 0 && vifX.cmd <= 0x05 && g_orbis_vif_fast.load(std::memory_order_relaxed))
		{
			bool done = true;
			switch (vifX.cmd)
			{
				case 0x05: // STMOD
					vifXRegs.mode = vifXRegs.code & 0x3;
					break;
				case 0x01: // STCYCL (whole-word store, as vk-285-99's vifCode_STCycl)
					*reinterpret_cast<u32*>(&vifXRegs.cycle) = vifXRegs.code & 0xffffu;
					break;
				case 0x03: // BASE (VIF1 only)
					if (!idx)
						done = false;
					else
						vif1Regs.base = vif1Regs.code & 0x3ff;
					break;
				case 0x02: // OFFSET (VIF1 only)
					if (!idx)
						done = false;
					else
					{
						vif1Regs.stat.DBF = false;
						vif1Regs.ofst = vif1Regs.code & 0x3ff;
						vif1Regs.tops = vif1Regs.base;
					}
					break;
				case 0x04: // ITOP
					vifXRegs.itops = vifXRegs.code & 0x3ff;
					break;
				default:
					done = false;
					break;
			}
			if (done)
			{
				vifX.cmd = 0;
				data += 1;
				pSize -= 1;
				continue;
			}
		}

		ret = vifCmdHandler[idx][vifX.cmd & 0x7f](vifX.pass, data);
		data   += ret;
		pSize  -= ret;
		if (vifX.vifstalled.enabled)
		{
			int current_STR = idx ? vif1ch.chcr.STR : vif0ch.chcr.STR;
			if (!current_STR)
				DevCon.Warning("Warning! VIF%d stalled during FIFO transfer!", idx);
		}
	}
}

_vifT static __fi bool vifTransfer(u32 *data, int size, bool TTE) {
	vifStruct& vifX = GetVifX;

	// PS5 port (vk-285-84): with EmuCore/Speedhacks/OrbisMTVUBatch on, the MTVU packets this transfer
	// writes (unpacks, MSCAL/MSCNT, STROW...) wake the VU thread once, at the end, instead of each doing
	// a locked add on the semaphore state (MTVU.h, m_defer_kicks). Needs proper testing.
	struct OrbisKickBatch
	{
		bool active = false, was = false;
		~OrbisKickBatch() { if (active) vu1Thread.EndKickBatch(was); }
	} orbis_batch;
	if (idx && THREAD_VU1)
	{
		orbis_batch.was = vu1Thread.BeginKickBatch();
		orbis_batch.active = true;
	}

	// irqoffset necessary to add up the right qws, or else will spin (spiderman)
	int transferred = vifX.irqoffset.enabled ? vifX.irqoffset.value : 0;

	vifX.vifpacketsize = size;
#if defined(PCSX2_PS4)
	u64 forja_vifm_t0 = 0;
	if (idx) // 193 in the app
	{
		forja_vifm_t0 = __builtin_ia32_rdtsc();
		s_forja_vifm_codes = 0;
		s_forja_vifm_hard = false;
	}
#endif
	vifTransferLoop<idx>(data);
#if defined(PCSX2_PS4)
	if (idx)
	{
		const int h = s_forja_vifm_hard ? 1 : 0;
		g_forja_vifm_pieces[h]++;
		g_forja_vifm_codes[h] += s_forja_vifm_codes;
		g_forja_vifm_ticks[h] += __builtin_ia32_rdtsc() - forja_vifm_t0;
		if (h)
			g_forja_vifm_prefix += s_forja_vifm_first_hard;
	}
#endif

	transferred += size - vifX.vifpacketsize;

	//Make this a minimum of 1 cycle so if it's the end of the packet it doesnt just fall through.
	//Metal Saga can do this, just to be safe :)
	if (!idx) g_vif0Cycles += std::max(1, (int)((transferred * BIAS) >> 2));
	else	  g_vif1Cycles += std::max(1, (int)((transferred * BIAS) >> 2));

	vifX.irqoffset.value = transferred % 4; // cannot lose the offset

	if (vifX.irq && vifX.cmd == 0) {
		VIF_LOG("Vif%d IRQ Triggering", idx);
		//Always needs to be set to return to the correct offset if there is data left.
		vifX.vifstalled.enabled = VifStallEnable(vifXch);
		vifX.vifstalled.value = VIF_IRQ_STALL;
	}

	if (!TTE) // *WARNING* - Tags CAN have interrupts! so lets just ignore the dma modifying stuffs (GT4)
	{
		transferred  = transferred >> 2;
		transferred = std::min((int)vifXch.qwc, transferred);
		vifXch.madr +=(transferred << 4);
		vifXch.qwc  -= transferred;

		hwDmacSrcTadrInc(vifXch);

		vifX.irqoffset.enabled = false;

		if(!vifXch.qwc)
			vifX.inprogress &= ~0x1;
		else if (vifX.irqoffset.value != 0)
			vifX.irqoffset.enabled = true;
	}
	else
	{
		if(vifX.irqoffset.value != 0){
			vifX.irqoffset.enabled = true;
		}else
			vifX.irqoffset.enabled = false;
	}

	vifExecQueue(idx);

	return !vifX.vifstalled.enabled;
}

// When TTE is set to 1, MADR and QWC are not updated as part of the transfer.
bool VIF0transfer(u32 *data, int size, bool TTE) {
	return vifTransfer<0>(data, size, TTE);
}
bool VIF1transfer(u32 *data, int size, bool TTE) {
	return vifTransfer<1>(data, size, TTE);
}
