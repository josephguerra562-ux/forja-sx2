// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"

#include "SIO/Sio0.h"
#include "Sif.h"
#include "DebugTools/Breakpoints.h"
#include "R5900OpcodeTables.h"
#include "IopCounters.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopDma.h"
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

using namespace R3000A;

R3000Acpu *psxCpu;

// used for constant propagation
u32 g_psxConstRegs[32];
u32 g_psxHasConstReg, g_psxFlushedConstReg;

// Used to signal to the EE when important actions that need IOP-attention have
// happened (hsyncs, vsyncs, IOP exceptions, etc).  IOP runs code whenever this
// is true, even if it's already running ahead a bit.
bool iopEventAction = false;

#if defined(PCSX2_PS4_VM_BOOT)
// Forja SX2 (compilation 67): in a movie the EE stops for ~430,000 events a second (Tales of the Abyss, 2026-10-20), and at every
// one it ran the IOP to catch up and its event test, while the IOP sat in its idle loop at almost every look (41 of 45 heartbeats
// at 0xbe9c; Crash Nitro Kart's 34 of 43 at 0xb99c): the video meter put "IOP" at ~300 ms of the EE thread's second. Waiting
// there with nothing due, the IOP would only move its clock on, so it can wait: until its next event (its counters, its DMA,
// the CD; iopNextEventCycle, 384 cycles at most), an interrupt the EE raises (iopTestIntc, iopEventAction) or an event the EE
// schedules (PSX_INT, which then counts from the EE's time and wakes it). Its idle loop is one the recompiler fast-forwards
// (a branch to itself and nops, compiled with the wait loop speedhack), checked against memory each time. PS2 mode only, and
// while a movie plays (main.cpp, ForjaMovieSettings; vm-no-iop-sleep turns it off).
bool g_forja_iop_sleep = false;
u32 g_forja_iop_sleep_lag = 0;
u64 g_forja_iop_sleeps = 0, g_forja_iop_runs = 0;

namespace
{
	struct ForjaIopIdleLoop
	{
		u32 pc = 0;
		u32 words = 0;
		u32 code[4] = {};
	};
	ForjaIopIdleLoop s_forja_iop_idle[4];
	u32 s_forja_iop_idle_next = 0;

	bool ForjaIopIdleAt(u32 pc)
	{
		for (const ForjaIopIdleLoop& loop : s_forja_iop_idle)
		{
			if (loop.words == 0 || loop.pc != pc)
				continue;
			for (u32 i = 0; i < loop.words; i++)
				if (iopMemRead32(pc + i * 4) != loop.code[i])
					return false;
			return true;
		}
		return false;
	}
} // namespace

// iR3000A.cpp, when it compiles a block it fast-forwards (from its start to past its delay slot).
void ForjaNoteIopIdleLoop(u32 pc, u32 end)
{
	const u32 words = (end - pc) / 4;
	if (end <= pc || words > 4)
		return;
	ForjaIopIdleLoop* slot = nullptr;
	for (ForjaIopIdleLoop& loop : s_forja_iop_idle)
		if (loop.words != 0 && loop.pc == pc)
			slot = &loop;
	if (!slot)
	{
		slot = &s_forja_iop_idle[s_forja_iop_idle_next];
		s_forja_iop_idle_next = (s_forja_iop_idle_next + 1) % std::size(s_forja_iop_idle);
	}
	slot->pc = pc;
	slot->words = words;
	for (u32 i = 0; i < words; i++)
		slot->code[i] = iopMemRead32(pc + i * 4);
}

// recResetIOP (also a state load): no loop known, and no IOP behind.
void ForjaForgetIopIdleLoops()
{
	for (ForjaIopIdleLoop& loop : s_forja_iop_idle)
		loop = ForjaIopIdleLoop{};
	g_forja_iop_sleep_lag = 0;
}

bool ForjaIopMaySleep(s32 ee_behind)
{
	if (!g_forja_iop_sleep || ee_behind <= 0 || ee_behind > 3072 || PSXCLK != 36864000 || (psxHu32(HW_ICFG) & (1 << 3)))
		return false;
	// An interrupt waiting for it: the event test would take it.
	if (psxHu32(HW_ICTRL) != 0 && (psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0)
		return false;
	// Its next event within the time it would catch up.
	const u64 behind = (static_cast<u64>(ee_behind) + 7) / 8;
	if (psxRegs.cycle + behind + 8 >= psxRegs.iopNextEventCycle)
		return false;
	return ForjaIopIdleAt(psxRegs.pc);
}
#endif

static constexpr uint iopWaitCycles = 384; // Keep inline with EE wait cycle max.

bool iopEventTestIsActive = false;

alignas(16) psxRegisters psxRegs;

void psxReset()
{
	std::memset(&psxRegs, 0, sizeof(psxRegs));

	psxRegs.pc = 0xbfc00000; // Start in bootstrap
	psxRegs.CP0.n.Status = 0x00400000; // BEV = 1
	psxRegs.CP0.n.PRid   = 0x0000001f; // PRevID = Revision ID, same as the IOP R3000A

	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = -1;
	psxRegs.iopCycleEECarry = 0;
	psxRegs.iopNextEventCycle = psxRegs.cycle + 4;

	psxHwReset();
	PSXCLK = 36864000;
	ioman::reset();
	psxBiosReset();
}

void psxShutdown() {
	//psxCpu->Shutdown();
}

void psxException(u32 code, u32 bd)
{
//	PSXCPU_LOG("psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	//Console.WriteLn("!! psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	// Set the Cause
	psxRegs.CP0.n.Cause &= ~0x7f;
	psxRegs.CP0.n.Cause |= code;

	// Set the EPC & PC
	if (bd)
	{
		PSXCPU_LOG("bd set");
		psxRegs.CP0.n.Cause|= 0x80000000;
		psxRegs.CP0.n.EPC = (psxRegs.pc - 4);
	}
	else
		psxRegs.CP0.n.EPC = (psxRegs.pc);

	if (psxRegs.CP0.n.Status & 0x400000)
		psxRegs.pc = 0xbfc00180;
	else
		psxRegs.pc = 0x80000080;

	// Set the Status
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status &~0x3f) |
						  ((psxRegs.CP0.n.Status & 0xf) << 2);

	/*if ((((PSXMu32(psxRegs.CP0.n.EPC) >> 24) & 0xfe) == 0x4a)) {
		// "hokuto no ken" / "Crash Bandicot 2" ... fix
		PSXMu32(psxRegs.CP0.n.EPC)&= ~0x02000000;
	}*/

	/*if (psxRegs.CP0.n.Cause == 0x400 && (!(psxHu32(0x1450) & 0x8))) {
		hwIntcIrq(INTC_SBUS);
	}*/
}

__fi void psxSetNextBranch( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(psxRegs.iopNextEventCycle - startCycle) > delta )
		psxRegs.iopNextEventCycle = startCycle + delta;
}

__fi void psxSetNextBranchDelta( s32 delta )
{
	psxSetNextBranch( psxRegs.cycle, delta );
}

__fi int psxTestCycle( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(psxRegs.cycle - startCycle) >= delta;
}

__fi int psxRemainingCycles(IopEventId n)
{
	if (psxRegs.interrupt & (1 << n))
		return ((psxRegs.cycle - psxRegs.sCycle[n]) + psxRegs.eCycle[n]);
	else
		return 0;
}

__fi void PSX_INT( IopEventId n, s32 ecycle )
{
#if defined(PCSX2_PS4_VM_BOOT)
	// Forja SX2 (compilation 67): scheduled by the EE while the IOP sleeps behind it: from the EE's time, as with the IOP in step,
	// and the IOP catches up at the EE's next event, soon.
	if (g_forja_iop_sleep_lag != 0)
	{
		ecycle += static_cast<s32>(g_forja_iop_sleep_lag);
		iopEventAction = true;
		cpuSetNextEventDelta(16);
	}
#endif
	// 19 is CDVD read int, it's supposed to be high.
	//if (ecycle > 8192 && n != 19)
	//	DevCon.Warning( "IOP cycles high: %d, n %d", ecycle, n );

	psxRegs.interrupt |= 1 << n;

	psxRegs.sCycle[n] = psxRegs.cycle;
	psxRegs.eCycle[n] = ecycle;

	psxSetNextBranchDelta(ecycle);
	const float mutiplier = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	const s32 iopDelta = (psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier;

	if (psxRegs.iopCycleEE < iopDelta)
	{
		// The EE called this int, so inform it to branch as needed:
		
		cpuSetNextEventDelta(iopDelta - psxRegs.iopCycleEE);
	}
}

static __fi void IopTestEvent( IopEventId n, void (*callback)() )
{
	if( !(psxRegs.interrupt & (1 << n)) ) return;

	if( psxTestCycle( psxRegs.sCycle[n], psxRegs.eCycle[n] ) )
	{
		psxRegs.interrupt &= ~(1 << n);
		callback();
	}
	else
		psxSetNextBranch( psxRegs.sCycle[n], psxRegs.eCycle[n] );
}

static __fi void Sio0TestEvent(IopEventId n)
{
	if (!(psxRegs.interrupt & (1 << n)))
	{
		return;
	}

	if (psxTestCycle(psxRegs.sCycle[n], psxRegs.eCycle[n]))
	{
		psxRegs.interrupt &= ~(1 << n);
		g_Sio0.Interrupt(Sio0Interrupt::TEST_EVENT);
	}
	else
	{
		psxSetNextBranch(psxRegs.sCycle[n], psxRegs.eCycle[n]);
	}
}

static __fi void _psxTestInterrupts()
{
	IopTestEvent(IopEvt_SIF0,		sif0Interrupt);	// SIF0
	IopTestEvent(IopEvt_SIF1,		sif1Interrupt);	// SIF1
	IopTestEvent(IopEvt_SIF2,		sif2Interrupt);	// SIF2
	Sio0TestEvent(IopEvt_SIO);
	IopTestEvent(IopEvt_CdvdSectorReady, cdvdSectorReady);
	IopTestEvent(IopEvt_CdvdRead,	cdvdReadInterrupt);

	// Profile-guided Optimization (sorta)
	// The following ints are rarely called.  Encasing them in a conditional
	// as follows helps speed up most games.

	if( psxRegs.interrupt & ((1 << IopEvt_Cdvd) | (1 << IopEvt_Dma11) | (1 << IopEvt_Dma12)
		| (1 << IopEvt_Cdrom) | (1 << IopEvt_CdromRead) | (1 << IopEvt_DEV9) | (1 << IopEvt_USB)))
	{
		IopTestEvent(IopEvt_Cdvd,		cdvdActionInterrupt);
		IopTestEvent(IopEvt_Dma11,		psxDMA11Interrupt);	// SIO2
		IopTestEvent(IopEvt_Dma12,		psxDMA12Interrupt);	// SIO2
		IopTestEvent(IopEvt_Cdrom,		cdrInterrupt);
		IopTestEvent(IopEvt_CdromRead,	cdrReadInterrupt);
		IopTestEvent(IopEvt_DEV9,		dev9Interrupt);
		IopTestEvent(IopEvt_USB,		usbInterrupt);
	}
}

__ri void iopEventTest()
{
	psxRegs.iopNextEventCycle = psxRegs.cycle + iopWaitCycles;

	if (psxTestCycle(psxNextStartCounter, psxNextDeltaCounter))
	{
		psxRcntUpdate();
		iopEventAction = true;
	}
	else
	{
		// start the next branch at the next counter event by default
		// the interrupt code below will assign nearer branches if needed.
		if (psxNextDeltaCounter < static_cast<s32>(psxRegs.iopNextEventCycle - psxNextStartCounter))
			psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;
	}

	if (psxRegs.interrupt)
	{
		iopEventTestIsActive = true;
		_psxTestInterrupts();
		iopEventTestIsActive = false;
	}

	if ((psxHu32(HW_ICTRL) != 0) && ((psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0))
	{
		if ((psxRegs.CP0.n.Status & 0xFE01) >= 0x401)
		{
			PSXCPU_LOG("Interrupt: %x  %x", psxHu32(HW_ISTAT), psxHu32(HW_IMASK));
			psxException(0, 0);
			iopEventAction = true;
		}
	}
}

void iopTestIntc()
{
	if( psxHu32(HW_ICTRL) == 0 ) return;
	if( (psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) == 0 ) return;

	if( !eeEventTestIsActive )
	{
		// An iop exception has occurred while the EE is running code.
		// Inform the EE to branch so the IOP can handle it promptly:

		cpuSetNextEventDelta( 16 );
		iopEventAction = true;
		//Console.Error( "** IOP Needs an EE EventText, kthx **  %d", iopCycleEE );

		// Note: No need to set the iop's branch delta here, since the EE
		// will run an IOP branch test regardless.
	}
	else if ( !iopEventTestIsActive )
		psxSetNextBranchDelta( 2 );
}

inline bool psxIsBranchOrJump(u32 addr)
{
	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int psxIsBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (psxIsBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr + 4))
		bpFlags += 2;

	return bpFlags;
}

int psxIsMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (psxIsBranchOrJump(addr))
		addr += 4;

	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
