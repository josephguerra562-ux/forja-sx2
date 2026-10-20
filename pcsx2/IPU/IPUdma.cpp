// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/IPU_MultiISA.h"

IPUDMAStatus IPU1Status;

void ipuDmaReset()
{
	IPU1Status.InProgress	= false;
	IPU1Status.DMAFinished	= true;
}

bool SaveStateBase::ipuDmaFreeze()
{
	if (!FreezeTag("IPUdma"))
		return false;

	Freeze(IPU1Status);
	return IsOkay();
}

static __fi int IPU1chain() {

	int totalqwc = 0;

	int qwc = ipu1ch.qwc;
	u32 *pMem;

	pMem = (u32*)dmaGetAddr(ipu1ch.madr, false);

	if (pMem == NULL)
	{
		Console.Error("ipu1dma NULL!");
		return totalqwc;
	}

	//Write our data to the fifo
	qwc = ipu_fifo.in.write(pMem, qwc);
	ipu1ch.madr += qwc << 4;
	ipu1ch.qwc -= qwc;
	totalqwc += qwc;

	//Update TADR etc
	hwDmacSrcTadrInc(ipu1ch);

	if (!ipu1ch.qwc)
		IPU1Status.InProgress = false;

	return totalqwc;
}

#if defined(PCSX2_PS4_VM_BOOT)
u64 g_forja_ipu_events[3] = {};
u64 g_forja_ipu_inline_fills[2] = {};

// Forja SX2 (compilation 66): each 8 QW the decoder reads cost two EE events, IPU1dma's (the fill it asked for) and the
// decoder's resume after it. A colour conversion (CSC) reads three fills a macroblock: in Tales of the Abyss's movies ~150,000
// of the ~520,000 events a second (2026-10-20), each one an exit from the recompiled code, a turn of the IOP and the timers.
// Here the fill IPU1dma would make at its next event is made at once: only the current tag's data, with the DMA running and
// the decoder waiting for it, and the DMA left as IPU1dma leaves it after a fill (waiting for the next request), or with its
// end scheduled at IPU1dma's delay when this was the chain's last data. A tag's end and the next tag stay IPU1dma's.
static int ForjaIPUFeed()
{
	if (!g_ps4_ipu_feed || !ipuRegs.ctrl.BUSY || !IPUCoreStatus.WaitingOnIPUTo || !IPUCoreStatus.DataRequested || g_BP.IFC >= 8)
		return 0;
	if (!ipu1ch.chcr.STR || ipu1ch.chcr.MOD == 2 || !IPU1Status.InProgress || ipu1ch.qwc == 0 || !dmacRegs.ctrl.DMAE ||
		(psHu8(DMAC_ENABLER + 2) & 1))
		return 0;
	const int moved = IPU1chain();
	if (moved <= 0)
		return 0;
	g_forja_ipu_inline_fills[1]++;
	if (IPU1Status.DMAFinished && !IPU1Status.InProgress)
	{
		// The chain's last data: its end and IRQ as IPU1dma would schedule them (the decoder's request may have one already).
		IPU_INT_TO(std::max(4, moved) * BIAS);
	}
	else
	{
		// The request this fill answered is done; the DMA waits for the next one, as after IPU1dma's own fill.
		cpuRegs.interrupt &= ~(1u << DMAC_TO_IPU);
		cpuRegs.eCycle[4] = 0x9999;
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);
	}
	IPUCoreStatus.WaitingOnIPUTo = false;
	return moved;
}

// Compilation 66: IPU1dma's resume of a decoder that waited for input, run now and fed while that is all it waits for, up to
// FORJA_IPU_PUMP_RUNS turns. It stops when the decoder finishes, yields with its own timing (a macroblock's 64 cycles) or
// waits for its output FIFO (the fromIPU DMA's event, and IPU0dma's burst, take it from there).
static constexpr u32 FORJA_IPU_PUMP_RUNS = 32;
static void ForjaIPUPump(int qwc)
{
	u32 moved = static_cast<u32>(std::max(qwc, 1));
	for (u32 runs = 0;; runs++)
	{
		if (runs == FORJA_IPU_PUMP_RUNS)
		{
			// Fed and not run yet: it goes on at the usual delay for what was moved.
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(moved * BIAS);
			return;
		}
		IPUCoreStatus.WaitingOnIPUTo = false;
		IPUProcessInterrupt();
		if (!ipuRegs.ctrl.BUSY || (cpuRegs.interrupt & (1 << IPU_PROCESS)) || !IPUCoreStatus.WaitingOnIPUTo)
			return;
		const int more = ForjaIPUFeed();
		if (more <= 0)
			return; // the next tag or the DMA's end: its request scheduled IPU1dma, as usual
		moved += more;
	}
}
#endif

void IPU1dma()
{
	if(!ipu1ch.chcr.STR || ipu1ch.chcr.MOD == 2)
	{
		//We MUST stop the IPU from trying to fill the FIFO with more data if the DMA has been suspended
		//if we don't, we risk causing the data to go out of sync with the fifo and we end up losing some!
		//This is true for Dragons Quest 8 and probably others which suspend the DMA.
		DevCon.Warning("IPU1 running when IPU1 DMA disabled! CHCR %x QWC %x", ipu1ch.chcr._u32, ipu1ch.qwc);
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);
		return;
	}

	if (IPUCoreStatus.DataRequested == false)
	{
		// IPU isn't expecting any data, so put it in to wait mode.
		cpuRegs.eCycle[4] = 0x9999;
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);

		// Shouldn't Happen.
		if (IPUCoreStatus.WaitingOnIPUTo)
		{
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(4 * BIAS);
		}
		return;
	}

	int tagcycles = 0;
	int totalqwc = 0;

	IPU_LOG("IPU1 DMA Called QWC %x Finished %d In Progress %d tadr %x", ipu1ch.qwc, IPU1Status.DMAFinished, IPU1Status.InProgress, ipu1ch.tadr);
	if (!IPU1Status.InProgress)
	{
		if (IPU1Status.DMAFinished)
			DevCon.Warning("IPU1 DMA Somehow reading tag when finished??");

		tDMA_TAG* ptag = dmaGetAddr(ipu1ch.tadr, false);  //Set memory pointer to TADR

		if (!ipu1ch.transfer("IPU1", ptag))
		{
			return;
		}
		ipu1ch.madr = ptag[1]._u32;

		tagcycles += 1; // Add 1 cycles from the QW read for the tag

		if (ipu1ch.chcr.TTE) DevCon.Warning("TTE?");

		IPU1Status.DMAFinished = hwDmacSrcChain(ipu1ch, ptag->ID);

		IPU_LOG("dmaIPU1 dmaChain %8.8x_%8.8x size=%d, addr=%lx, fifosize=%x",
			ptag[1]._u32, ptag[0]._u32, ipu1ch.qwc, ipu1ch.madr, 8 - g_BP.IFC);

		if (ipu1ch.chcr.TIE && ptag->IRQ) //Tag Interrupt is set, so schedule the end/interrupt
			IPU1Status.DMAFinished = true;

		if (ipu1ch.qwc)
			IPU1Status.InProgress = true;
	}

	if (IPU1Status.InProgress)
		totalqwc += IPU1chain();

	// Nothing has been processed except maybe a tag, or the DMA is ending
	if(totalqwc == 0 || (IPU1Status.DMAFinished && !IPU1Status.InProgress))
	{
		totalqwc = std::max(4, totalqwc) + tagcycles;
		IPU_INT_TO(totalqwc * BIAS);
	}
	else
	{
			cpuRegs.eCycle[4] = 0x9999;
			CPU_SET_DMASTALL(DMAC_TO_IPU, true);
	}

	// Resume even for a short DMA tail; waiting for four QWs can strand the
	// decoder after a completed 1-3 QW transfer. (AI-assisted)
	if (IPUCoreStatus.WaitingOnIPUTo && g_BP.IFC >= 1)
	{
#if defined(PCSX2_PS4_VM_BOOT)
		// Forja SX2 (compilation 66): the decoder runs now, fed from this tag while that is all it waits for (ForjaIPUPump).
		if (g_ps4_ipu_feed && ipuRegs.ctrl.BUSY && !(cpuRegs.interrupt & (1 << IPU_PROCESS)))
			ForjaIPUPump(totalqwc);
		else
#endif
		{
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(totalqwc * BIAS);
		}
	}

	IPU_LOG("Completed Call IPU1 DMA QWC Remaining %x Finished %d In Progress %d tadr %x", ipu1ch.qwc, IPU1Status.DMAFinished, IPU1Status.InProgress, ipu1ch.tadr);
}

void IPU0dma()
{
	if(!ipuRegs.ctrl.OFC)
	{
		// This shouldn't happen.
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUProcessInterrupt();
		}
		CPU_SET_DMASTALL(DMAC_FROM_IPU, true);
		return;
	}

	int readsize;
	tDMA_TAG* pMem;

	if ((!(ipu0ch.chcr.STR) || (cpuRegs.interrupt & (1 << DMAC_FROM_IPU))) || (ipu0ch.qwc == 0))
	{
		DevCon.Warning("How??");
		// This shouldn't happen.
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPU_INT_PROCESS(ipuRegs.ctrl.OFC * BIAS);
		}
		return;
	}

	pxAssert(!(ipu0ch.chcr.TTE));

	IPU_LOG("dmaIPU0 chcr = %lx, madr = %lx, qwc  = %lx",
	        ipu0ch.chcr._u32, ipu0ch.madr, ipu0ch.qwc);

	pxAssert(ipu0ch.chcr.MOD == NORMAL_MODE);

	pMem = dmaGetAddr(ipu0ch.madr, true);

	readsize = std::min(ipu0ch.qwc, (u32)ipuRegs.ctrl.OFC);
	ipu_fifo.out.read(pMem, readsize);

	ipu0ch.madr += readsize << 4;
	ipu0ch.qwc -= readsize;

#if defined(PCSX2_PS4_VM_BOOT)
	// (AI-assisted) Every 8 QW output FIFO fill cost one EE event (decoder resume, then this DMA),
	// about 800k events/s in God of War's FMVs on PS4. Drain up to PS4_IPU0_BURST_FILLS more fills
	// in this call while the decoder only waits for FIFO room. The DMA and decoder resume below are
	// still scheduled with the cycles of every QW moved, so the IPU's throughput is unchanged; data
	// lands up to ~32 * BIAS cycles per fill earlier. Needs proper testing on more FMV games.
	int forja_fed = 0; // compilation 66: input fills moved by the burst, their cycles added to the delays below
	if (g_ps4_ipu0_burst && dmacRegs.ctrl.STS != STS_fromIPU)
	{
		// (AI-assisted) Doubled from 8 to 16 in v0.86, raised to 32 in v0.87 to cut FMV DMA events further;
		// cycles of every QW remain accounted for, so throughput is unchanged.
		// Needs proper testing on more FMV games.
		// Forja SX2 (compilation 66): when the decoder waits for its input instead, the toIPU DMA feeds it here too
		// (ForjaIPUFeed). A CSC reads three input fills a macroblock between its eight output fills, so the burst ended at each
		// macroblock; now it goes on for 64 fills, out or in.
		constexpr u32 PS4_IPU0_BURST_FILLS = 64;
		bool drained_inline = false;
		for (u32 fills = 0; fills < PS4_IPU0_BURST_FILLS; fills++)
		{
			if (ipu0ch.qwc == 0 || ipuRegs.ctrl.OFC != 0 || !ipuRegs.ctrl.BUSY || (cpuRegs.interrupt & (1 << IPU_PROCESS)))
				break;

			if (IPUCoreStatus.WaitingOnIPUFrom)
				IPUCoreStatus.WaitingOnIPUFrom = false;
			else
			{
				const int fed = ForjaIPUFeed(); // compilation 66: nothing to do unless it waits for input it can have now
				if (fed <= 0)
					break;
				forja_fed += fed;
			}
			IPUProcessInterrupt();
			if (ipuRegs.ctrl.OFC == 0)
				continue; // Needs input (fed next turn), yielded with its own interrupt, or finished.

			tDMA_TAG* const next = dmaGetAddr(ipu0ch.madr, true);
			if (!next)
				break; // Leave the fill to the normal DMA event the decoder's FIFO write scheduled.

			const u32 chunk = std::min(ipu0ch.qwc, (u32)ipuRegs.ctrl.OFC);
			ipu_fifo.out.read(next, chunk);
			ipu0ch.madr += chunk << 4;
			ipu0ch.qwc -= chunk;
			readsize += chunk;
			drained_inline = true;
			g_forja_ipu_inline_fills[0]++;
		}
		// The decoder's FIFO writes requested a DMA event for data already moved here. Without it the
		// state matches a single large transfer; any remaining OFC keeps its pending event.
		if (drained_inline && ipuRegs.ctrl.OFC == 0)
			cpuRegs.interrupt &= ~(1u << DMAC_FROM_IPU);
	}
#endif

	if (dmacRegs.ctrl.STS == STS_fromIPU)   // STS == fromIPU
	{
		//DevCon.Warning("fromIPU Stall Control");
		dmacRegs.stadr.ADDR = ipu0ch.madr;
	}

#if defined(PCSX2_PS4_VM_BOOT)
	readsize += forja_fed; // compilation 66: the input moved by the burst takes its time as well
#endif

	if (!ipu0ch.qwc)
		IPU_INT_FROM(readsize * BIAS);

	CPU_SET_DMASTALL(DMAC_FROM_IPU, true);

	if (ipuRegs.ctrl.BUSY && IPUCoreStatus.WaitingOnIPUFrom)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPU_INT_PROCESS(readsize * BIAS);
	}
}

__fi void dmaIPU0() // fromIPU
{
	//if (dmacRegs.ctrl.STS == STS_fromIPU) DevCon.Warning("DMA Stall enabled on IPU0");

	if (dmacRegs.ctrl.STS == STS_fromIPU)   // STS == fromIPU - Initial settings
		dmacRegs.stadr.ADDR = ipu0ch.madr;

	CPU_SET_DMASTALL(DMAC_FROM_IPU, false);
	// Note: This should probably be a very small value, however anything lower than this will break Mana Khemia
	// This is because the game sends bad DMA information, starts an IDEC, then sets it to the correct values
	// but because our IPU is too quick, it messes up the sync between the DMA and IPU.
	// So this will do until (if) we sort the timing out of IPU, shouldn't cause any problems for games for now.
	//IPU_INT_FROM( 160 );
	// Update 22/12/2021 - Doesn't seem to need this now after fixing some FIFO/DMA behaviour
	IPU0dma();

	// Explanation of this:
	// The DMA logic on a NORMAL transfer is generally a "transfer first, ask questions later" so when it's sent
	// QWC == 0 (which we change to 0x10000) it transfers, causing an underflow, then asks if it's reached 0
	// since IPU_FROM is beholden to the OUT FIFO, if there's nothing to transfer, it will stay at 0 and won't underflow
	// so the DMA will end.
	if (ipu0ch.qwc == 0x10000)
	{
		ipu0ch.qwc = 0;
		ipu0ch.chcr.STR = false;
		hwDmacIrq(DMAC_FROM_IPU);
		DMA_LOG("IPU0 DMA End");
	}
}

__fi void dmaIPU1() // toIPU
{
	IPU_LOG("IPU1DMAStart QWC %x, MADR %x, CHCR %x, TADR %x", ipu1ch.qwc, ipu1ch.madr, ipu1ch.chcr._u32, ipu1ch.tadr);
	CPU_SET_DMASTALL(DMAC_TO_IPU, false);

	if (ipu1ch.chcr.MOD == CHAIN_MODE)  //Chain Mode
	{
		IPU_LOG("Setting up IPU1 Chain mode");
		if(ipu1ch.qwc == 0)
		{
			IPU1Status.InProgress = false;
			IPU1Status.DMAFinished = false;
		}
		else // Attempting to continue a previous chain
		{
			IPU_LOG("Resuming DMA TAG %x", (ipu1ch.chcr.TAG >> 12));
			IPU1Status.InProgress = true;
			if ((ipu1ch.chcr.tag().ID == TAG_REFE) || (ipu1ch.chcr.tag().ID == TAG_END) || (ipu1ch.chcr.tag().IRQ && ipu1ch.chcr.TIE))
			{
				IPU1Status.DMAFinished = true;
			}
			else
			{
				IPU1Status.DMAFinished = false;
			}
		}
	}
	else // Normal Mode
	{
			IPU_LOG("Setting up IPU1 Normal mode");
			IPU1Status.InProgress = true;
			IPU1Status.DMAFinished = true;
	}

	IPU1dma();
}

void ipuCMDProcess()
{
#if defined(PCSX2_PS4_VM_BOOT)
	g_forja_ipu_events[0]++; // compilation 66
#endif
	IPUProcessInterrupt();
}

void ipu0Interrupt()
{
	IPU_LOG("ipu0Interrupt: %llx", cpuRegs.cycle);
#if defined(PCSX2_PS4_VM_BOOT)
	g_forja_ipu_events[2]++; // compilation 66
#endif

	if(ipu0ch.qwc > 0)
	{
		IPU0dma();
		return;
	}

	ipu0ch.chcr.STR = false;
	hwDmacIrq(DMAC_FROM_IPU);
	CPU_SET_DMASTALL(DMAC_FROM_IPU, false);
	DMA_LOG("IPU0 DMA End");
}

__fi void ipu1Interrupt()
{
	IPU_LOG("ipu1Interrupt %llx:", cpuRegs.cycle);
#if defined(PCSX2_PS4_VM_BOOT)
	g_forja_ipu_events[1]++; // compilation 66
#endif

	if(!IPU1Status.DMAFinished || IPU1Status.InProgress)  //Sanity Check
	{
		IPU1dma();
		return;
	}

	DMA_LOG("IPU1 DMA End");
	ipu1ch.chcr.STR = false;
	hwDmacIrq(DMAC_TO_IPU);
	CPU_SET_DMASTALL(DMAC_TO_IPU, false);
}
