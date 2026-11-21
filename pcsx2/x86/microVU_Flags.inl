// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Sets FDIV Flags at the proper time
__fi void mVUdivSet(mV)
{
	if (mVUinfo.doDivFlag)
	{
		if (!sFLAG.doFlag)
			xMOV(getFlagReg(sFLAG.write), getFlagReg(sFLAG.lastWrite));
		xAND(getFlagReg(sFLAG.write), 0xfff3ffff);
		xOR(getFlagReg(sFLAG.write), ptr32[&mVU.divFlag]);
	}
}

// Optimizes out unneeded status flag updates
// This can safely be done when there is an FSSET opcode
__fi void mVUstatusFlagOp(mV)
{
	int curPC = iPC;
	int i = mVUcount;
	bool runLoop = true;

	if (sFLAG.doFlag)
	{
		sFLAG.doNonSticky = true;
	}
	else
	{
		for (; i > 0; i--)
		{
			incPC2(-2);
			if (sFLAG.doNonSticky)
			{
				runLoop = false;
				break;
			}
			else if (sFLAG.doFlag)
			{
				sFLAG.doNonSticky = true;
				break;
			}
		}
	}
	if (runLoop)
	{
		for (; i > 0; i--)
		{
			incPC2(-2);

			if (sFLAG.doNonSticky)
				break;

			sFLAG.doFlag = false;
		}
	}
	iPC = curPC;
	DevCon.WriteLn(Color_Green, "microVU%d: FSSET Optimization", getIndex);
}

int findFlagInst(int* fFlag, int cycles)
{
	int j = 0, jValue = -1;
	for (int i = 0; i < 4; i++)
	{
		if ((fFlag[i] <= cycles) && (fFlag[i] > jValue))
		{
			j = i;
			jValue = fFlag[i];
		}
	}
	return j;
}

// Setup Last 4 instances of Status/Mac/Clip flags (needed for accurate block linking)
int sortFlag(int* fFlag, int* bFlag, int cycles)
{
	int lFlag = -5;
	int x = 0;
	for (int i = 0; i < 4; i++)
	{
		bFlag[i] = findFlagInst(fFlag, cycles);
		if (lFlag != bFlag[i])
			x++;
		lFlag = bFlag[i];
		cycles++;
	}
	return x; // Returns the number of Valid Flag Instances
}

void sortFullFlag(int* fFlag, int* bFlag)
{
	int m = std::max(std::max(fFlag[0], fFlag[1]), std::max(fFlag[2], fFlag[3]));
	for (int i = 0; i < 4; i++)
	{
		int t = 3 - (m - fFlag[i]);
		bFlag[i] = (t < 0) ? 0 : t + 1;
	}
}

#define sFlagCond (sFLAG.doFlag || mVUlow.isFSSET || mVUinfo.doDivFlag)
#define sHackCond (mVUsFlagHack && !sFLAG.doNonSticky)

// Note: Flag handling is 'very' complex, it requires full knowledge of how microVU recs work, so don't touch!
// 159 in the app, "Banderas justas" ([Forja] VU1BanderasJustas): `forja_mac_skip` (mVUsetFlagInfo, only for the block's own end) is
// how many of the 4 MAC instances the next blocks hand on can't be read. The next block's slot s holds the latest FMAC issued 4 - s
// cycles before this block's end, and a read at the next block's instruction p (p cycles or more) sees slot min(p, 3) or a later one:
// with the first read at p, only the FMACs of the last 3 - p instructions and the one before them are needed. The MAC flag isn't
// sticky, so the rest don't matter. God of War II (the 157's map): its hottest blocks computed 4 and their next block read 1.
__fi void mVUsetFlags(mV, microFlagCycles& mFC, int forja_mac_skip = 0)
{
	int endPC = iPC;
	u32 aCount = 0; // Amount of instructions needed to get valid mac flag instances for block linking
	//bool writeProtect = false;
	bool forja_mac_done = false; // 159 in the app

	// Ensure last ~4+ instructions update mac/status flags (if next block's first 4 instructions will read them)
	for (int i = mVUcount; i > 0; i--, aCount++)
	{
		if (sFLAG.doFlag)
		{

			if (__Mac)
			{
				if (!forja_mac_done)
					mFLAG.doFlag = true;
				else if (!mFLAG.doFlag)
					g_forja_vu1_mac_left_out.fetch_add(1, std::memory_order_relaxed);
				if (static_cast<int>(aCount) >= 3 - forja_mac_skip)
					forja_mac_done = true; // 159 in the app: with no skip, at aCount 3, where the loop stops anyway
				//writeProtect = true;
			}

			if (__Status)
			{
				sFLAG.doNonSticky = true;
				//writeProtect = true;
			}

			if (aCount >= 3)
			{
				break;
			}
		}
		incPC2(-2);
	}

	// Status/Mac Flags Setup Code
	int xS = 0, xM = 0, xC = 0;

	for (int i = 0; i < 4; i++)
	{
		mFC.xStatus[i] = i;
		mFC.xMac   [i] = i;
		mFC.xClip  [i] = i;
	}

	if (!(mVUpBlock->pState.needExactMatch & 1))
	{
		xS = (mVUpBlock->pState.flagInfo >> 2) & 3;
		mFC.xStatus[0] = -1;
		mFC.xStatus[1] = -1;
		mFC.xStatus[2] = -1;
		mFC.xStatus[3] = -1;
		mFC.xStatus[(xS - 1) & 3] = 0;
	}

	if (!(mVUpBlock->pState.needExactMatch & 2))
	{
		mFC.xMac[0] = -1;
		mFC.xMac[1] = -1;
		mFC.xMac[2] = -1;
		mFC.xMac[3] = -1;
	}

	if (!(mVUpBlock->pState.needExactMatch & 4))
	{
		xC = (mVUpBlock->pState.flagInfo >> 6) & 3;
		mFC.xClip[0] = -1;
		mFC.xClip[1] = -1;
		mFC.xClip[2] = -1;
		mFC.xClip[3] = -1;
		mFC.xClip[(xC - 1) & 3] = 0;
	}

	mFC.cycles = 0;
	u32 xCount = mVUcount; // Backup count
	iPC = mVUstartPC;
	for (mVUcount = 0; mVUcount < xCount; mVUcount++)
	{
		if (mVUlow.isFSSET && !noFlagOpts)
		{
			if (__Status) // Don't Optimize out on the last ~4+ instructions
			{
				if ((xCount - mVUcount) > aCount)
					mVUstatusFlagOp(mVU);
			}
			else
				mVUstatusFlagOp(mVU);
		}
		mFC.cycles += mVUstall;

		sFLAG.read = doSFlagInsts ? findFlagInst(mFC.xStatus, mFC.cycles) : 0;
		mFLAG.read = doMFlagInsts ? findFlagInst(mFC.xMac,    mFC.cycles) : 0;
		cFLAG.read = doCFlagInsts ? findFlagInst(mFC.xClip,   mFC.cycles) : 0;

		sFLAG.write = doSFlagInsts ? xS : 0;
		mFLAG.write = doMFlagInsts ? xM : 0;
		cFLAG.write = doCFlagInsts ? xC : 0;

		sFLAG.lastWrite = doSFlagInsts ? (xS - 1) & 3 : 0;
		mFLAG.lastWrite = doMFlagInsts ? (xM - 1) & 3 : 0;
		cFLAG.lastWrite = doCFlagInsts ? (xC - 1) & 3 : 0;

		if (sHackCond)
		{
			sFLAG.doFlag = false;
		}

		if (sFLAG.doFlag)
		{
			if (noFlagOpts)
			{
				sFLAG.doNonSticky = true;
				mFLAG.doFlag = true;
			}
		}

		if (sFlagCond)
		{
			mFC.xStatus[xS] = mFC.cycles + 4;
			xS = (xS + 1) & 3;
		}

		if (mFLAG.doFlag)
		{
			mFC.xMac[xM] = mFC.cycles + 4;
			xM = (xM + 1) & 3;
		}

		if (cFLAG.doFlag)
		{
			mFC.xClip[xC] = mFC.cycles + 4;
			xC = (xC + 1) & 3;
		}

		mFC.cycles++;
		incPC2(2);
	}

	mVUregs.flagInfo |= ((__Status) ? 0 : (xS << 2));
	mVUregs.flagInfo |= /*((__Mac||1) ? 0 :*/ (xM << 4)/*)*/; //TODO: Optimise this? Might help with number of blocks.
	mVUregs.flagInfo |= ((__Clip)   ? 0 : (xC << 6));
	iPC = endPC;
}

#define getFlagReg2(x) ((bStatus[0] == x) ? getFlagReg(x) : gprT1)
#define getFlagReg3(x) ((gFlag == x) ? gprT1 : getFlagReg(x))
#define getFlagReg4(x) ((gFlag == x) ? gprT1 : gprT2)
#define shuffleMac     ((bMac[3] << 6) | (bMac[2] << 4) | (bMac[1] << 2) | bMac[0])
#define shuffleClip    ((bClip[3] << 6) | (bClip[2] << 4) | (bClip[1] << 2) | bClip[0])

// Recompiles Code for Proper Flags on Block Linkings
__fi void mVUsetupFlags(mV, microFlagCycles& mFC)
{

	if (mVUregs.flagInfo & 1)
	{
		if (mVUregs.needExactMatch)
			DevCon.Error("mVU ERROR!!!");
	}

	const bool pf = false; // Print Flag Info
	if (pf)
		DevCon.WriteLn("mVU%d - [#%d][sPC=%04x][bPC=%04x][mVUBranch=%d][branch=%d]",
			mVU.index, mVU.prog.cur->idx, mVUstartPC / 2 * 8, xPC, mVUbranch, mVUlow.branch);

	if (doSFlagInsts && __Status)
	{
		if (pf)
			DevCon.WriteLn("mVU%d - Status Flag", mVU.index);
		int bStatus[4];
		int sortRegs = sortFlag(mFC.xStatus, bStatus, mFC.cycles);
		// DevCon::Status("sortRegs = %d", params sortRegs);
		// Note: Emitter will optimize out mov(reg1, reg1) cases...
		if (sortRegs == 1)
		{
			xMOV(gprF0, getFlagReg(bStatus[0]));
			xMOV(gprF1, getFlagReg(bStatus[1]));
			xMOV(gprF2, getFlagReg(bStatus[2]));
			xMOV(gprF3, getFlagReg(bStatus[3]));
		}
		else if (sortRegs == 2)
		{
			xMOV(gprT1, getFlagReg (bStatus[3]));
			xMOV(gprF0, getFlagReg (bStatus[0]));
			xMOV(gprF1, getFlagReg2(bStatus[1]));
			xMOV(gprF2, getFlagReg2(bStatus[2]));
			xMOV(gprF3, gprT1);
		}
		else if (sortRegs == 3)
		{
			int gFlag = (bStatus[0] == bStatus[1]) ? bStatus[2] : bStatus[1];
			xMOV(gprT1, getFlagReg (gFlag));
			xMOV(gprT2, getFlagReg (bStatus[3]));
			xMOV(gprF0, getFlagReg (bStatus[0]));
			xMOV(gprF1, getFlagReg3(bStatus[1]));
			xMOV(gprF2, getFlagReg4(bStatus[2]));
			xMOV(gprF3, gprT2);
		}
		else
		{
			const xRegister32& temp3 = mVU.regAlloc->allocGPR();
			xMOV(gprT1, getFlagReg(bStatus[0]));
			xMOV(gprT2, getFlagReg(bStatus[1]));
			xMOV(temp3, getFlagReg(bStatus[2]));
			xMOV(gprF3, getFlagReg(bStatus[3]));
			xMOV(gprF0, gprT1);
			xMOV(gprF1, gprT2);
			xMOV(gprF2, temp3);
			mVU.regAlloc->clearNeeded(temp3);
		}
	}

	if (doMFlagInsts && __Mac)
	{
		if (pf)
			DevCon.WriteLn("mVU%d - Mac Flag", mVU.index);
		int bMac[4];
		sortFlag(mFC.xMac, bMac, mFC.cycles);
		xMOVAPS(xmmT1, ptr128[mVU.macFlag]);
		xSHUF.PS(xmmT1, xmmT1, shuffleMac);
		xMOVAPS(ptr128[mVU.macFlag], xmmT1);
	}

	if (doCFlagInsts && __Clip)
	{
		if (pf)
			DevCon.WriteLn("mVU%d - Clip Flag", mVU.index);
		int bClip[4];
		sortFlag(mFC.xClip, bClip, mFC.cycles);
		xMOVAPS(xmmT2, ptr128[mVU.clipFlag]);
		xSHUF.PS(xmmT2, xmmT2, shuffleClip);
		xMOVAPS(ptr128[mVU.clipFlag], xmmT2);
	}
}

// 153 in the app, "Banderas muertas del VU1" ([Forja] VU1BanderasMuertas; set at the game's start by ps4/vm/main.cpp, defined in
// pcsx2/MTVU.cpp): the flags (1 status, 2 MAC: needExactMatch's bits) that no code of this game reads across a JR/JALR. Without it
// a JR/JALR asks all three flags exact, and every block whose next block might be past one computes the status and MAC flags of its
// last FMACs at every run. God of War II (the 152's VU1 map, 2026-11-20, simular_vu1_banderas.py): in 386 programs nothing reads or
// sets the status flag, and its 53 MAC readers read their own program's FMACs; its hottest loop (transforming vertices, leaving by
// JALR) spent 28 % of its x86 on those flags. The clip flag stays exact. Only for VU1.
#include <atomic>
extern std::atomic<int> g_forja_vu1_dead_flags;
extern std::atomic<bool> g_forja_vu1_keep_regs; // 157 in the app (pcsx2/MTVU.cpp)
extern std::atomic<u64> g_forja_vu1_kept_blocks, g_forja_vu1_kept_links;
extern std::atomic<u64> g_forja_vu1_flag_risk_progs; // 160 in the app (pcsx2/MTVU.cpp)
extern std::atomic<int> g_forja_vu1_dead_flags_learned; // 161 in the app (pcsx2/MTVU.cpp)

// 160 in the app, "Banderas muertas seguras". The 153's map had no reader of a flag across a JR/JALR, but it hadn't seen every program:
// right after loading a saved state at the Colossus a program read the MAC flag at 0x08f8 and 0x0b98 just after a jump, got a stale
// one, and the VU1 and the GS ended waiting on each other (the freeze of the 153, 155, 158 and 159; none from the 126 to the 152 in 29
// loads). So each program's microcode is scanned once, when its first JR/JALR is compiled: the places only a jump reaches (after the
// delay slot of a B, BAL, JR or JALR: return points and jump tables' targets; after an E-bit; a direct branch's target), and a flag
// reader (FSxxx, FMxxx) in the 4 instructions from one of them. That program keeps that flag exact at its jumps. In the 156, 157 and
// 159's maps (some 1,600 programs) none has one, so the hot loops keep the 153's saving.
static int ForjaFlagReadersAfterJumps(microVU& mVU, const microProgram& prog)
{
	const u32* code = reinterpret_cast<const u32*>(mVU.regs().Micro);
	const int n = static_cast<int>(mVU.microMemSize / 8);
	std::vector<u8> entry(n, 0);
	for (int i = 0; i < n; i++)
	{
		const u32 lo = code[2 * i], up = code[2 * i + 1];
		if ((up & (1u << 30)) && i + 2 < n)
			entry[i + 2] = 1; // after an E-bit: the next routine
		if (up & (1u << 31))
			continue; // I-bit: the lower word is a number
		const u32 op = lo >> 25;
		const bool jump = op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25; // B, BAL, JR, JALR
		const bool cond = op == 0x28 || op == 0x29 || (op >= 0x2c && op <= 0x2f); // IBEQ, IBNE, IBLTZ..IBGEZ
		if (jump && i + 2 < n)
			entry[i + 2] = 1; // after the delay slot: reached only by a jump
		if (op == 0x20 || op == 0x21 || cond)
		{
			s32 imm = static_cast<s32>(lo & 0x7ff);
			if (imm & 0x400)
				imm -= 0x800;
			entry[((i * 8 + 8 + imm * 8) & static_cast<int>(mVU.microMemSize - 8)) / 8] = 1; // as branchAddr
		}
	}
	int risk = 0, at[3] = {-1, -1, -1};
	for (int i = 0; i < n && risk != 3; i++)
	{
		const u32 lo = code[2 * i], up = code[2 * i + 1];
		if (up & (1u << 31))
			continue;
		const u32 op = lo >> 25;
		const int bit = (op == 0x18 || op == 0x1a || op == 0x1b) ? 2 : (op >= 0x14 && op <= 0x17) ? 1 : 0; // FMxxx : FSxxx
		if (!bit || (risk & bit))
			continue;
		for (int k = 0; k < 4 && i - k >= 0; k++)
		{
			if (entry[i - k])
			{
				risk |= bit;
				at[bit] = i * 8;
				break;
			}
		}
	}
	if (risk)
	{
		g_forja_vu1_flag_risk_progs.fetch_add(1, std::memory_order_relaxed);
		ProbeLog("VU1 dead flags (160 in the app): program %d keeps%s%s at its jumps (a reader right after a jump target)\n", prog.idx,
				 (risk & 1) ? " the status flag" : "", (risk & 2) ? " the MAC flag" : "");
		if (risk & 2)
			ProbeLog("  MAC reader at %04x\n", at[2]);
		if (risk & 1)
			ProbeLog("  status reader at %04x\n", at[1]);
	}
	return risk;
}

// The flags (1 status, 2 MAC, 4 clip) a JR/JALR asks exact: all three, but for the VU1 the game's dead flags (153) the program can spare.
static __fi int ForjaFlagsKeptAtJump(microVU& mVU)
{
	if (!mVU.index)
		return 7;
	const int dead = g_forja_vu1_dead_flags.load(std::memory_order_relaxed) &
	                 ~g_forja_vu1_dead_flags_learned.load(std::memory_order_relaxed); // 161 in the app
	if (!dead)
		return 7;
	microProgram* prog = mVU.prog.cur;
	if (!prog)
		return 7;
	if (!prog->forjaFlagScan)
	{
		prog->forjaFlagRisk = static_cast<u8>(ForjaFlagReadersAfterJumps(mVU, *prog));
		prog->forjaFlagScan = 1;
	}
	return 7 & ~(dead & ~(prog->forjaFlagRisk | prog->forjaFlagLearned));
}

// 159 in the app, "Banderas justas": the earliest position where a next block reads the MAC flag (mVUsetFlagInfo's flag pass).
static __fi void ForjaMacReadAt(microVU& mVU, int pos)
{
	if (pos < mVU.forjaMacFirstRead)
		mVU.forjaMacFirstRead = pos;
}

// 162 in the app, "MAC justa en los saltos" ([Forja] VU1MacEnSaltos, set at the game's start by ps4/vm/main.cpp, defined in
// pcsx2/MTVU.cpp). The 161 kept the MAC flag exact at every JR/JALR (God of War II: the Colossus freeze came from programs 0x08e0
// and 0x0b90 reading the MAC flag the previous program left), so each block before one computed its last 4 MAC instances: the VU1
// ~10 % dearer and the game's frame from ~43 to ~37. After an unknown jump only two kinds of reader can follow: one in the same
// program right after the jump's target (the 160's scan flags those programs: their 4 instances stay), or the next program's, which
// comes after this program's end, at least two instructions on (the E-bit instruction and its delay slot). So the next blocks read
// no earlier than two instructions after the target (a block that ends with the jump: from the instance of its last 2 instructions'
// FMACs and the one before), or three after the delay slot (a jump met in a direct branch's flag pass). Only with "Banderas justas"
// (159), which turns those positions into fewer instances.
extern std::atomic<bool> g_forja_vu1_mac_jump;
static __fi bool ForjaMacOnlyNextProgram(microVU& mVU)
{
	if (!mVU.index || !g_forja_vu1_mac_jump.load(std::memory_order_relaxed))
		return false;
	const microProgram* prog = mVU.prog.cur;
	return prog && prog->forjaFlagScan && !((prog->forjaFlagRisk | prog->forjaFlagLearned) & 2);
}

#define shortBranch() \
	{ \
		if ((branch == 3) || (branch == 4)) /*Branches*/ \
		{ \
			_mVUflagPass(mVU, aBranchAddr, sCount + found, found, v, pos + 1); \
			if (branch == 3) /*Non-conditional Branch*/ \
				break; \
			branch = 0; \
		} \
		else if (branch == 5) /*JR/JARL*/ \
		{ \
			if (sCount + found < 4) \
			{ \
				mVUregs.needExactMatch |= ForjaFlagsKeptAtJump(mVU); /* 153 in the app */ \
				if (track && (ForjaFlagsKeptAtJump(mVU) & 2)) \
					ForjaMacReadAt(mVU, pos + (ForjaMacOnlyNextProgram(mVU) ? 3 : 1)); /* 159: unknown code may read it after the jump; 162: the next program's, 3 on */ \
			} \
			break; \
		} \
		else /*E-Bit End*/ \
			break; \
	}

// Scan through instructions and check if flags are read (FSxxx, FMxxx, FCxxx opcodes)
// 159 in the app, "Banderas justas": `pos` is the position of startPC's first instruction after the block whose end is being set up.
// While mVU.forjaMacTracking is on, each MAC read's position is kept (ForjaMacReadAt), a block is scanned again when a path reaches it
// earlier than before, and the scan doesn't stop when all three flags are already asked for (a later read can't lower the position,
// but an earlier path's could).
void _mVUflagPass(mV, u32 startPC, u32 sCount, u32 found, std::vector<u32>& v, int pos = 0)
{
	const bool track = mVU.forjaMacTracking;
	if (track)
	{
		const u32 key = (startPC & 0xffff) | (static_cast<u32>(pos) << 16);
		for (u32 i = 0; i < v.size(); i++)
		{
			if ((v[i] & 0xffff) == (startPC & 0xffff) && static_cast<int>(v[i] >> 16) <= pos)
				return; // already scanned from this position or an earlier one
		}
		v.push_back(key);
	}
	else
	{
		for (u32 i = 0; i < v.size(); i++)
		{
			if (v[i] == startPC)
				return; // Prevent infinite recursion
		}
		v.push_back(startPC);
	}

	int oldPC = iPC;
	int oldBranch = mVUbranch;
	int aBranchAddr = 0;
	iPC = startPC / 4;
	mVUbranch = 0;
	for (int branch = 0; sCount < 4; sCount += found)
	{
		mVUregs.needExactMatch &= 7;
		incPC(1);
		mVUopU(mVU, 3);
		found |= (mVUregs.needExactMatch & 8) >> 3;
		mVUregs.needExactMatch &= 7;
		if (curI & _Ebit_)
		{
			branch = 1;
		}
		if (curI & _Tbit_)
		{
			branch = 6;
		}
		if ((curI & _Dbit_) && doDBitHandling)
		{
			branch = 6;
		}
		if (!(curI & _Ibit_))
		{
			const int mac_before = mVUregs.needExactMatch & 2; // 159 in the app: tell this instruction's MAC read apart
			if (track)
				mVUregs.needExactMatch &= ~2;
			incPC(-1);
			mVUopL(mVU, 3);
			incPC(1);
			if (track)
			{
				if (mVUregs.needExactMatch & 2)
					ForjaMacReadAt(mVU, pos);
				mVUregs.needExactMatch |= mac_before;
			}
		}

		if (branch >= 2)
		{
			shortBranch();
		}
		else if (branch == 1)
		{
			branch = 2;
		}
		if (mVUbranch)
		{
			branch = ((mVUbranch > 8) ? (5) : ((mVUbranch < 3) ? 3 : 4));
			incPC(-1);
			aBranchAddr = branchAddr(mVU);
			incPC(1);
			mVUbranch = 0;
		}
		incPC(1);
		pos++; // 159 in the app
		if (!track && (mVUregs.needExactMatch & 7) == 7)
			break;
	}
	iPC = oldPC;
	mVUbranch = oldBranch;
	mVUregs.needExactMatch &= 7;
	setCode();
}

void mVUflagPass(mV, u32 startPC, u32 sCount = 0, u32 found = 0)
{
	std::vector<u32> v;
	_mVUflagPass(mVU, startPC, sCount, found, v, 0);
}

// Checks if the first ~4 instructions of a block will read flags
// 159 in the app, "Banderas justas": also how many of this block's last MAC instances no next block can read (mVU.forjaMacSkip, used by
// mVUsetFlags). Only from flag passes that saw every read: not after a JR/JALR to an unknown place, nor when the MAC flag was already
// asked for before the passes (the block's own type, an E-bit or M-bit), nor when it was asked for without a read the pass saw.
void mVUsetFlagInfo(mV)
{
	mVU.forjaMacSkip = 0;
	// (Not for a branch in another one's delay slot, whose next instructions don't follow in order.)
	const bool forja_track = ForjaMacJust(mVU) && !noFlagOpts && !(mVUregs.needExactMatch & 2) && !mVUlow.evilBranch && !mVUlow.badBranch;
	bool forja_unknown = false;
	mVU.forjaMacTracking = forja_track;
	mVU.forjaMacFirstRead = 99;
	if (noFlagOpts)
	{
		mVU.forjaMacTracking = false;
		mVUregs.needExactMatch = 0x7;
		mVUregs.flagInfo = 0x0;
		return;
	}
	if (mVUbranch <= 2) // B/BAL
	{
		incPC(-1);
		mVUflagPass(mVU, branchAddr(mVU));
		incPC(1);

		mVUregs.needExactMatch &= 0x7;
	}
	else if (mVUbranch <= 8) // Conditional Branch
	{
		incPC(-1); // Branch Taken
		mVUflagPass(mVU, branchAddr(mVU));
		int backupFlagInfo = mVUregs.needExactMatch;
		mVUregs.needExactMatch = 0;

		incPC(4); // Branch Not Taken
		mVUflagPass(mVU, xPC);
		incPC(-3);

		mVUregs.needExactMatch |= backupFlagInfo;
		mVUregs.needExactMatch &= 0x7;
	}
	else // JR/JALR
	{
		if (!doConstProp || !mVUlow.constJump.isValid)
		{
			mVUregs.needExactMatch |= ForjaFlagsKeptAtJump(mVU); // 153 in the app: 0x7 unless the game's dead flags
			if (forja_track && (mVUregs.needExactMatch & 2) && ForjaMacOnlyNextProgram(mVU))
				ForjaMacReadAt(mVU, 2); // 162 in the app: the next program's read, 2 instructions after the target at least
			else
				forja_unknown = true; // 159 in the app
		}
		else
		{
			mVUflagPass(mVU, (mVUlow.constJump.regValue * 8) & (mVU.microMemSize - 8));
		}
		mVUregs.needExactMatch &= 0x7;
	}
	mVU.forjaMacTracking = false;
	if (forja_track && !forja_unknown && (mVUregs.needExactMatch & 2) && mVU.forjaMacFirstRead < 99)
		mVU.forjaMacSkip = std::min(mVU.forjaMacFirstRead, 3);
}
