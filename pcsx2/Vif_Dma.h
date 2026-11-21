// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Vif.h"
#include "Vif_Unpack.h"
#include <atomic> // 167 in the app

struct vifCode {
   u32 addr;
   u32 size;
   u32 cmd;
   u16 wl;
   u16 cl;
};

union tBITBLTBUF {
	u64 _u64;
	struct {
		u32 SBP : 14;
		u32 _pad14 : 2;
		u32 SBW : 6;
		u32 _pad22 : 2;
		u32 SPSM : 6;
		u32 _pad30 : 2;
		u32 DBP : 14;
		u32 _pad46 : 2;
		u32 DBW : 6;
		u32 _pad54 : 2;
		u32 DPSM : 6;
		u32 _pad62 : 2;
	};
};

union tTRXPOS {
	u64 _u64;
	struct {
		u32 SSAX : 11;
		u32 _PAD1 : 5;
		u32 SSAY : 11;
		u32 _PAD2 : 5;
		u32 DSAX : 11;
		u32 _PAD3 : 5;
		u32 DSAY : 11;
		u32 DIRY : 1;
		u32 DIRX : 1;
		u32 _PAD4 : 3;
	};
};

union tTRXREG
{
	u64 _u64;
	struct
	{
		u32 RRW : 12;
		u32 _pad12 : 20;
		u32 RRH : 12;
		u32 _pad44 : 20;
	};
};

struct tVIF_CTRL {
   bool enabled;
   u32 value;
};

// NOTE, if debugging vif stalls, use sega classics, spyro, gt4, and taito
struct vifStruct {
	alignas(16) u128 MaskRow;
	alignas(16) u128 MaskCol;

	struct { // These must be together for MTVU
		vifCode tag;
		int cmd;
		int pass;
		int cl;
		u8  usn;
		u8 start_aligned;
		u8  StructEnd; // Address of this is used to calculate end of struct
	};

	int irq;

	bool done;
	tVIF_CTRL vifstalled;
	bool stallontag;
	bool waitforvu;
	int unpackcalls;
	// GS registers used for calculating the size of the last local->host transfer initiated on the GS
	// Transfer size calculation should be restricted to GS emulation in the future
	union
	{
		struct
		{
			tBITBLTBUF BITBLTBUF;
			tTRXPOS    TRXPOS;
			tTRXREG    TRXREG;
		};
		u64 transfer_registers[3];
	};

	u32        GSLastDownloadSize;

	tVIF_CTRL  irqoffset; // 32bit offset where next vif code is
	u32 vifpacketsize;
	u8  inprogress;
	u8  dmamode;

	bool queued_program;
	u32 queued_pc;
	bool queued_gif_wait;
};

alignas(16) extern vifStruct  vif0, vif1;

_vifT extern u32 vifRead32(u32 mem);
_vifT extern bool vifWrite32(u32 mem, u32 value);
extern void vif0Interrupt();
extern void vif0VUFinish();
extern void vif0Reset();

extern void vif1Interrupt();
extern void vif1VUFinish();
extern void vif1Reset();

typedef int FnType_VifCmdHandler(int pass, const u32 *data);
typedef FnType_VifCmdHandler* Fnptr_VifCmdHandler;

alignas(16) extern const Fnptr_VifCmdHandler vifCmdHandler[2][128];

__fi static int _limit(int a, int max)
{
	return ((a > max) ? max : a);
}

enum VifModes
{
	VIF_NORMAL_TO_MEM_MODE = 0,
	VIF_NORMAL_FROM_MEM_MODE = 1,
	VIF_CHAIN_MODE = 2
};

// Generic constants
static const unsigned int VIF0intc = 4;
static const unsigned int VIF1intc = 5;

extern u32 g_vif0Cycles;
extern u32 g_vif1Cycles;

// 167 in the app, "VIF en tramos" ([Forja] VifEnTramos; Vif_Transfer.cpp, MTVU.cpp, set at the game's start by ps4/vm/main.cpp). With
// the VU1 on its own thread the EE read every VIF1 code and sent every UNPACK to the VU thread's ring on its own (its state, room
// reserved and committed each time: God of War II ~450,000 a second, ~12 % of the EE in the Colossus). Now the EE walks a run of the
// codes only the VU1 needs (UNPACKs whole in the DMA piece, STCYCL, STMOD, STMASK, STROW, STCOL, OFFSET, BASE, ITOP, NOP), keeps its
// own registers as their handlers would, and sends the run once; the VU thread reads it with the same steps (ForjaTramoCode).
// Everything else (MSCAL*, MPG, DIRECT*, FLUSH*, MARK, MSKPATH3, an interrupt bit, an UNPACK or STROW/STCOL/STMASK split between two
// DMA pieces, a NOP before MSKPATH3, a queued VU1 program about to start) still goes the normal way, so the ring's order is the same.
// Mode: 0 off, 1 check (the EE predicts each run's UNPACKs and the normal path compares them with what it sends; the game runs the old
// way), 2 on. VifEnTramos = 1 starts in 1 and goes to 2 by itself after 100,000 UNPACKs predicted right; one wrong turns it off.
extern std::atomic<int> g_forja_vif_tramos_mode;

struct ForjaTramoState
{
	u32 cycle; // CL | WL << 8, as STCYCL writes it
	u32 mode, mask, tops, base;
};

struct ForjaTramoUnpack
{
	u32 addr, size, cmd, usn, start_aligned, num;
};

// An UNPACK's data words, as vifUnpackSetup computes them (0 for a code nVifT doesn't know, or one with nothing to take).
static __fi u32 ForjaUnpackWords(u32 code, u32 cycle)
{
	const u32 gsize = nVifT[(code >> 24) & 0x0f];
	u32 num = (code >> 16) & 0xff;
	if (num == 0)
		num = 256;
	const u32 cl = cycle & 0xff;
	const u32 wl = ((cycle >> 8) & 0xff) ? ((cycle >> 8) & 0xff) : 256;
	if (wl <= cl)
		return (num * gsize + 3) / 4;
	const u32 n = cl * (num / wl) + std::min(num % wl, cl);
	return (n * gsize + 3) >> 2;
}

// One VIF1 code of a run, at d[k], with avail words left in the DMA piece from it (vifpacketsize at that code). Returns its length in
// words, or 0 if it can't go in a run (the caller stops before it). An UNPACK fills *up (up->size != 0) and changes nothing in s;
// STROW/STCOL write row/col when given. The EE (predicting or walking) and the VU thread (reading) take exactly these steps.
static __fi u32 ForjaTramoCode(const u32* d, u32 k, u32 avail, ForjaTramoState& s, ForjaTramoUnpack* up, u128* row, u128* col)
{
	const u32 code = d[k];
	up->size = 0;
	if (code >> 31)
		return 0; // the interrupt bit
	const u32 cmd = (code >> 24) & 0x7f;
	if (cmd >= 0x60) // UNPACK
	{
		const u32 words = ForjaUnpackWords(code, s.cycle);
		if (words == 0 || words + 1 > avail)
			return 0;
		u32 addr = code;
		if ((addr >> 15) & 1)
			addr += s.tops;
		up->addr = (addr << 4) & 0x3ff0;
		up->size = words;
		up->cmd = cmd;
		up->usn = (code >> 14) & 1;
		up->start_aligned = 4 - ((avail - 1) & 0x3);
		up->num = ((code >> 16) & 0xff) ? ((code >> 16) & 0xff) : 256;
		return 1 + words;
	}
	switch (cmd)
	{
		case 0x00: // NOP
		case 0x04: // ITOP (its register is the EE's)
			return 1;
		case 0x01: // STCYCL
			s.cycle = code & 0xffff;
			return 1;
		case 0x02: // OFFSET (OFST and DBF are the EE's)
			s.tops = s.base;
			return 1;
		case 0x03: // BASE
			s.base = code & 0x3ff;
			return 1;
		case 0x05: // STMOD
			s.mode = code & 0x3;
			return 1;
		case 0x20: // STMASK
			if (avail < 2)
				return 0;
			s.mask = d[k + 1];
			return 2;
		case 0x30: // STROW
		case 0x31: // STCOL
		{
			if (avail < 5)
				return 0;
			u128* dst = cmd == 0x30 ? row : col;
			if (dst)
				memcpy(dst, &d[k + 1], 16);
			return 5;
		}
		default:
			return 0;
	}
}

// Mode 1: the normal path's UNPACK about to go to the VU thread, against the run's prediction (Vif_Transfer.cpp).
extern void ForjaVif1TramoCheck(const vifStruct& vif, const VIFregisters& regs, const void* data);

extern void vif0FLUSH();
extern void vif1FLUSH();

extern void vifExecQueue(int idx);
