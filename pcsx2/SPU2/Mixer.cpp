// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"
#include "SPU2/Debug.h"
#include "SPU2/defs.h"
#include "SPU2/spu2.h"
#include "SPU2/interpolate_table.h"

#include "common/Assertions.h"
#include "common/Timer.h"

#include <cstring>
#include <unistd.h> // Forja SX2: access() for the PS4 test markers

// LOOP/END sets the ENDX bit and sets NAX to LSA, and the voice is muted if LOOP is not set
// LOOP seems to only have any effect on the block with LOOP/END set, where it prevents muting the voice
// (the documented requirement that every block in a loop has the LOOP bit set is nonsense according to tests)
// LOOP/START sets LSA to NAX unless LSA was written manually since sound generation started
// (see LoopMode, the method by which this is achieved on the real SPU2 is unknown)
#define XAFLAG_LOOP_END (1ul << 0)
#define XAFLAG_LOOP (1ul << 1)
#define XAFLAG_LOOP_START (1ul << 2)

// Forja SX2: the SPU2 time split (spu2Mix() below, main.cpp's heartbeat). Global: what follows MULTI_ISA_UNSHARED_START is in a
// namespace of its own for each ISA it is compiled for.
extern u64 g_forja_spu2_split_voices, g_forja_spu2_split_mix, g_forja_spu2_split_output;
extern u64 g_forja_spu2_split_samples, g_forja_spu2_split_active, g_forja_spu2_split_irq;
extern u64 g_forja_spu2_split_fx_on, g_forja_spu2_split_fx_heard, g_forja_spu2_split_fx_area;
// Forja SX2: stopped voices replayed in batches (MixCoreVoices below; spu2.cpp and Dma.cpp call the catch-up).
extern void (*g_forja_spu2_catch_up)();
extern u32 g_forja_spu2_pending[2][24];
extern bool g_forja_spu2_any_pending;
extern u64 g_forja_spu2_deferred_samples, g_forja_spu2_catch_ups;

#if MULTI_ISA_COMPILE_ONCE
u64 g_forja_spu2_split_voices = 0, g_forja_spu2_split_mix = 0, g_forja_spu2_split_output = 0;
u64 g_forja_spu2_split_samples = 0, g_forja_spu2_split_active = 0, g_forja_spu2_split_irq = 0;
u64 g_forja_spu2_split_fx_on = 0, g_forja_spu2_split_fx_heard = 0, g_forja_spu2_split_fx_area = 0;
void (*g_forja_spu2_catch_up)() = nullptr;
u32 g_forja_spu2_pending[2][24] = {};
bool g_forja_spu2_any_pending = false;
u64 g_forja_spu2_deferred_samples = 0, g_forja_spu2_catch_ups = 0;

// A reset or a loaded savestate replaces the voices: their pending samples belong to the state before.
void ForjaSPU2ClearPending()
{
	std::memset(g_forja_spu2_pending, 0, sizeof(g_forja_spu2_pending));
	g_forja_spu2_any_pending = false;
}

// decoded pcm data, used to cache the decoded data so that it needn't be decoded
// multiple times.  Cache chunks are decoded when the mixer requests the blocks, and
// invalided when DMA transfers and memory writes are performed.
PcmCacheEntry pcm_cache_data[pcm_BlockCount];

int g_counter_cache_hits = 0;
int g_counter_cache_misses = 0;
int g_counter_cache_ignores = 0;
#endif

MULTI_ISA_UNSHARED_START

static const s32 tbl_XA_Factor[16][2] =
	{
		{0, 0},
		{60, 0},
		{115, -52},
		{98, -55},
		{122, -60}};

static void __forceinline XA_decode_block(s16* buffer, const s16* block, s32& prev1, s32& prev2)
{
	const s32 header = *block;
	const s32 shift = (header & 0xF) + 16;
	const int id = header >> 4 & 0xF;
	if (id > 4 && SPU2::MsgToConsole())
		SPU2::ConLog("* SPU2: Unknown ADPCM coefficients table id %d\n", id);
	const s32 pred1 = tbl_XA_Factor[id][0];
	const s32 pred2 = tbl_XA_Factor[id][1];

	const s8* blockbytes = (s8*)&block[1];
	const s8* blockend = &blockbytes[13];

	for (; blockbytes <= blockend; ++blockbytes)
	{
		s32 data = ((*blockbytes) << 28) & 0xF0000000;
		s32 pcm = (data >> shift) + (((pred1 * prev1) + (pred2 * prev2) + 32) >> 6);

		pcm = std::clamp<s32>(pcm, -0x8000, 0x7fff);
		*(buffer++) = pcm;

		data = ((*blockbytes) << 24) & 0xF0000000;
		s32 pcm2 = (data >> shift) + (((pred1 * pcm) + (pred2 * prev1) + 32) >> 6);

		pcm2 = std::clamp<s32>(pcm2, -0x8000, 0x7fff);
		*(buffer++) = pcm2;

		prev2 = pcm;
		prev1 = pcm2;
	}
}

static void __forceinline IncrementNextA(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	// Important!  Both cores signal IRQ when an address is read, regardless of
	// which core actually reads the address.

	for (int i = 0; i < 2; i++)
	{
		if (Cores[i].IRQEnable && (vc.NextA == Cores[i].IRQA))
		{
			//if( IsDevBuild )
			//	ConLog(" * SPU2 Core %d: IRQ Requested (IRQA (%05X) passed; voice %d).\n", i, Cores[i].IRQA, thiscore.Index * 24 + voiceidx);

			SetIrqCall(i);
		}
	}

	vc.NextA++;
	vc.NextA &= 0xFFFFF;
}

static __forceinline void GetNextDataBuffered(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	if (vc.SBuffer == nullptr)
	{
		const int cacheIdx = (vc.NextA & 0xFFFF8) / pcm_WordsPerBlock;
		PcmCacheEntry& cacheLine = pcm_cache_data[cacheIdx];
		vc.SBuffer = cacheLine.Sampledata;

		if (cacheLine.Validated && vc.Prev1 == cacheLine.Prev1 && vc.Prev2 == cacheLine.Prev2)
		{
			// Cached block!  Read from the cache directly.
			// Make sure to propagate the prev1/prev2 ADPCM:

			vc.Prev1 = vc.SBuffer[27];
			vc.Prev2 = vc.SBuffer[26];

			//ConLog( "* SPU2: Cache Hit! NextA=0x%x, cacheIdx=0x%x\n", vc.NextA, cacheIdx );

			if (IsDevBuild)
				g_counter_cache_hits++;
		}
		else
		{
			// Only flag the cache if it's a non-dynamic memory range.
			if (vc.NextA >= SPU2_DYN_MEMLINE)
			{
				cacheLine.Validated = true;
				cacheLine.Prev1 = vc.Prev1;
				cacheLine.Prev2 = vc.Prev2;
			}

			if (IsDevBuild)
			{
				if (vc.NextA < SPU2_DYN_MEMLINE)
					g_counter_cache_ignores++;
				else
					g_counter_cache_misses++;
			}


			s16* memptr = GetMemPtr(vc.NextA & 0xFFFF8);
			XA_decode_block(vc.SBuffer, memptr, vc.Prev1, vc.Prev2);
		}
	}

	// Get the sample index for NextA, we have to subtract 1 to ignore the loop header
	int sampleIdx = ((vc.NextA % pcm_WordsPerBlock) - 1) * 4;
	for (int i = 0; i < 4; i++)
	{
		vc.DecodeFifo[(vc.DecPosWrite + i) % 32] = vc.SBuffer[sampleIdx + i];
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
//                                                                                     //

static __forceinline s32 ApplyVolume(s32 data, s32 volume)
{
	return (volume * data) >> 15;
}

static __forceinline StereoOut32 ApplyVolume(const StereoOut32& data, const V_VolumeLR& volume)
{
	return StereoOut32(
		ApplyVolume(data.Left, volume.Left),
		ApplyVolume(data.Right, volume.Right));
}

static __forceinline StereoOut32 ApplyVolume(const StereoOut32& data, const V_VolumeSlideLR& volume)
{
	return StereoOut32(
		ApplyVolume(data.Left, volume.Left.Value),
		ApplyVolume(data.Right, volume.Right.Value));
}

static __forceinline void UpdateBlockHeader(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	for (int i = 0; i < 2; i++)
		if (Cores[i].IRQEnable && Cores[i].IRQA == (vc.NextA & 0xFFFF8))
			SetIrqCall(i);

	s16* memptr = GetMemPtr(vc.NextA & 0xFFFF8);
	vc.LoopFlags = *memptr >> 8; // grab loop flags from the upper byte.

	if ((vc.LoopFlags & XAFLAG_LOOP_START) && !vc.LoopMode)
	{
		vc.LoopStartA = vc.NextA & 0xFFFF8;
	}
}

static __forceinline void DecodeSamples(uint coreidx, uint voiceidx)
{
	V_Core& thiscore(Cores[coreidx]);
	V_Voice& vc(thiscore.Voices[voiceidx]);

	// Update the block header on every audio frame
	UpdateBlockHeader(thiscore, voiceidx);

	// When a voice is started at 0 pitch, NAX quickly advances to SSA + 5
	// So that would mean the decode buffer holds around 12 samples
	if (((int)(vc.DecPosWrite - vc.DecPosRead)) > 12) {
		// Sufficient data buffered
		return;
	}

	if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
	{
		GetNextDataBuffered(thiscore, voiceidx);
	}

	vc.DecPosWrite += 4;

	IncrementNextA(thiscore, voiceidx);
	if ((vc.NextA & 7) == 0)
	{
		if (vc.LoopFlags & XAFLAG_LOOP_END)
		{
			thiscore.Regs.ENDX |= (1 << voiceidx);
			vc.NextA = vc.LoopStartA;
			if (!(vc.LoopFlags & XAFLAG_LOOP))
			{
				vc.Stop();

				if (IsDevBuild)
				{
					if (SPU2::MsgVoiceOff())
						SPU2::ConLog("* SPU2: Voice Off by EndPoint: %d \n", voiceidx);
				}
			}
		}

		IncrementNextA(thiscore, voiceidx);
		vc.SBuffer = nullptr;
	}
}

static void __forceinline UpdatePitch(uint coreidx, uint voiceidx)
{
	V_Voice& vc(Cores[coreidx].Voices[voiceidx]);
	s32 pitch;

	// [Air] : re-ordered comparisons: Modulated is much more likely to be zero than voice,
	//   and so the way it was before it's have to check both voice and modulated values
	//   most of the time.  Now it'll just check Modulated and short-circuit past the voice
	//   check (not that it amounts to much, but eh every little bit helps).
	if ((vc.Modulated == 0) || (voiceidx == 0))
		pitch = vc.Pitch;
	else
		pitch = std::clamp((vc.Pitch * (32768 + Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0, 0x3fff);

	pitch = std::min(pitch, 0x3FFF);
	vc.SP += pitch;
}

static __forceinline void CalculateADSR(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	if (vc.ADSR.Phase == V_ADSR::PHASE_STOPPED)
	{
		vc.ADSR.Value = 0;
		return;
	}

	if (!vc.ADSR.Calculate(thiscore.Index | (voiceidx << 1)))
	{
		if (IsDevBuild)
		{
			if (SPU2::MsgVoiceOff())
				SPU2::ConLog("* SPU2: Voice Off by ADSR: %d \n", voiceidx);
		}
		vc.Stop();
	}

	pxAssume(vc.ADSR.Value >= 0); // ADSR should never be negative...
}

static __forceinline void ConsumeSamples(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	int consumed = vc.SP >> 12;
	vc.SP &= 0xfff;
	vc.DecPosRead += consumed;
}

static __forceinline s32 GetVoiceValues(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	int phase = (vc.SP & 0x0ff0) >> 4;
	s32 out = 0;
	out += (interpTable[phase][0] * vc.DecodeFifo[(vc.DecPosRead + 0) % 32]) >> 15;
	out += (interpTable[phase][1] * vc.DecodeFifo[(vc.DecPosRead + 1) % 32]) >> 15;
	out += (interpTable[phase][2] * vc.DecodeFifo[(vc.DecPosRead + 2) % 32]) >> 15;
	out += (interpTable[phase][3] * vc.DecodeFifo[(vc.DecPosRead + 3) % 32]) >> 15;

	return out;
}

// This is Dr. Hell's noise algorithm as implemented in pcsxr
// Supposedly this is 100% accurate
static __forceinline void UpdateNoise(V_Core& thiscore)
{
	static const uint8_t noise_add[64] = {
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1};

	static const uint16_t noise_freq_add[5] = {
		0, 84, 140, 180, 210};


	u32 level = 0x8000 >> (thiscore.NoiseClk >> 2);
	level <<= 16;

	thiscore.NoiseCnt += 0x10000;

	thiscore.NoiseCnt += noise_freq_add[thiscore.NoiseClk & 3];
	if ((thiscore.NoiseCnt & 0xffff) >= noise_freq_add[4])
	{
		thiscore.NoiseCnt += 0x10000;
		thiscore.NoiseCnt -= noise_freq_add[thiscore.NoiseClk & 3];
	}

	if (thiscore.NoiseCnt >= level)
	{
		while (thiscore.NoiseCnt >= level)
			thiscore.NoiseCnt -= level;

		thiscore.NoiseOut = (thiscore.NoiseOut << 1) | noise_add[(thiscore.NoiseOut >> 10) & 63];
	}
}

static __forceinline s32 GetNoiseValues(V_Core& thiscore)
{
	return (s16)thiscore.NoiseOut;
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
//                                                                                     //

// writes a signed value to the SPU2 ram
// Performs no cache invalidation -- use only for dynamic memory ranges
// of the SPU2 (between 0x0000 and SPU2_DYN_MEMLINE)
static __forceinline void spu2M_WriteFast(u32 addr, s16 value)
{
	// Fixes some of the oldest hangs in pcsx2's history! :p
	for (int i = 0; i < 2; i++)
	{
		if (Cores[i].IRQEnable && Cores[i].IRQA == addr)
		{
			//printf("Core %d special write IRQ Called (IRQ passed). IRQA = %x\n",i,addr);
			SetIrqCall(i);
		}
	}
// throw an assertion if the memory range is invalid:
#ifndef DEBUG_FAST
	pxAssume(addr < SPU2_DYN_MEMLINE);
#endif
	*GetMemPtr(addr) = value;
}


static __forceinline StereoOut32 MixVoice(uint coreidx, uint voiceidx)
{
	V_Core& thiscore(Cores[coreidx]);
	V_Voice& vc(thiscore.Voices[voiceidx]);

	// Most games don't use much volume slide effects.  So only call the UpdateVolume
	// methods when needed by checking the flag outside the method here...
	// (Note: Ys 6 : Ark of Nephistm uses these effects)

	vc.Volume.Update();

	DecodeSamples(coreidx, voiceidx);

	StereoOut32 voiceOut(0, 0);
	s32 Value = 0;

	if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
	{
		if (vc.Noise)
			Value = GetNoiseValues(thiscore);
		else
			Value = GetVoiceValues(thiscore, voiceidx);

		// Update and Apply ADSR  (applies to normal and noise sources)

		CalculateADSR(thiscore, voiceidx);
		Value = ApplyVolume(Value, vc.ADSR.Value);
		vc.OutX = Value;

		if (IsDevBuild)
			DebugCores[coreidx].Voices[voiceidx].displayPeak = std::max(DebugCores[coreidx].Voices[voiceidx].displayPeak, (s32)vc.OutX);

		voiceOut = ApplyVolume(StereoOut32(Value, Value), vc.Volume);
	}

	// SPU2 Note: The spu2 continues to process voices for eternity, always, so we
	// have to run through all the motions of updating the voice regardless of it's
	// audible status.  Otherwise IRQs might not trigger and emulation might fail.

	UpdatePitch(coreidx, voiceidx);

	ConsumeSamples(thiscore, voiceidx);

	// Write-back of raw voice data (post ADSR applied)
	if (voiceidx == 1)
		spu2M_WriteFast(((0 == coreidx) ? 0x400 : 0xc00) + OutPos, Value);
	else if (voiceidx == 3)
		spu2M_WriteFast(((0 == coreidx) ? 0x600 : 0xe00) + OutPos, Value);

	return voiceOut;
}

// Forja SX2: stopped voices in batches. The SPU2 keeps every voice reading its memory even when it is silent (MixVoice), for IRQs
// and for games that read a voice's address (NAX) or its end flags (ENDX); MMXCM's audio uses none of its 48 voices and they took
// ~95 ms of the EE thread a second. Skipping them (compilation 20, vm-spu2-voces-rapidas) hung the game on loading a save: it
// waited for something of theirs that never moved. Now a stopped voice is not skipped but deferred: what MixVoice does to it
// (the volume slide, the block header and address, the pitch and the samples it consumes, nothing audible) is replayed in a
// batch, with the same functions in the same order, before anything could tell the difference: any SPU2 register read or write
// (SPU2read/SPU2write, after their TimeUpdate), a DMA writing SPU2 memory, a savestate, or 256 samples (5 ms) at most. A voice
// is deferred only while neither core has an IRQ armed (a voice reading IRQA raises the IRQ of both), when it is not pitch
// modulated (that reads the previous voice's output each sample), when it is not voice 1 or 3 (they write SPU2 memory) and when
// it is not reading the output areas below 0x1000 (written every sample). /data/ForjaSX2/vm-spu2-voces-exactas turns it off.
static void ForjaReplayStoppedVoice(uint coreidx, uint voiceidx, u32 samples)
{
	V_Core& thiscore(Cores[coreidx]);
	V_Voice& vc(thiscore.Voices[voiceidx]);
	for (u32 i = 0; i < samples; i++)
	{
		vc.Volume.Update();
		DecodeSamples(coreidx, voiceidx);
		UpdatePitch(coreidx, voiceidx);
		ConsumeSamples(thiscore, voiceidx);
	}
}

static void ForjaCatchUpVoices()
{
	if (!g_forja_spu2_any_pending)
		return;
	g_forja_spu2_any_pending = false;
	g_forja_spu2_catch_ups++;
	for (uint c = 0; c < 2; c++)
	{
		for (uint v = 0; v < V_Core::NumVoices; v++)
		{
			if (const u32 n = g_forja_spu2_pending[c][v])
			{
				g_forja_spu2_pending[c][v] = 0;
				ForjaReplayStoppedVoice(c, v, n);
			}
		}
	}
}

static __forceinline void MixCoreVoices(VoiceMixSet& dest, const uint coreidx)
{
	V_Core& thiscore(Cores[coreidx]);
	static const bool s_defer_voices = access("/data/ForjaSX2/vm-spu2-voces-exactas", F_OK) != 0;
	const bool defer = s_defer_voices && !Cores[0].IRQEnable && !Cores[1].IRQEnable;
	if (defer)
		g_forja_spu2_catch_up = &ForjaCatchUpVoices;
	else if (g_forja_spu2_any_pending)
		ForjaCatchUpVoices();

	for (uint voiceidx = 0; voiceidx < V_Core::NumVoices; ++voiceidx)
	{
		V_Voice& vc(thiscore.Voices[voiceidx]);
		u32& pending = g_forja_spu2_pending[coreidx][voiceidx];
		if (defer && voiceidx != 1 && voiceidx != 3 && vc.ADSR.Phase == V_ADSR::PHASE_STOPPED && !vc.Modulated && vc.NextA >= 0x1000)
		{
			g_forja_spu2_any_pending = true;
			g_forja_spu2_deferred_samples++;
			if (++pending >= 256)
			{
				const u32 n = pending;
				pending = 0;
				ForjaReplayStoppedVoice(coreidx, voiceidx, n);
			}
			continue; // a stopped voice adds nothing to the mix
		}
		if (pending)
		{
			const u32 n = pending;
			pending = 0;
			ForjaReplayStoppedVoice(coreidx, voiceidx, n);
		}
		StereoOut32 VVal(MixVoice(coreidx, voiceidx));

		// Note: Results from MixVoice are ranged at 16 bits.

		dest.Dry.Left += VVal.Left & thiscore.VoiceGates[voiceidx].DryL;
		dest.Dry.Right += VVal.Right & thiscore.VoiceGates[voiceidx].DryR;
		dest.Wet.Left += VVal.Left & thiscore.VoiceGates[voiceidx].WetL;
		dest.Wet.Right += VVal.Right & thiscore.VoiceGates[voiceidx].WetR;
	}
}

static __forceinline StereoOut32 MixCore(const uint coreidx, const VoiceMixSet& inVoices, const StereoOut32& Input, const StereoOut32& Ext)
{
	V_Core& thiscore(Cores[coreidx]);

	thiscore.MasterVol.Update();
	UpdateNoise(thiscore);

	// Saturate final result to standard 16 bit range.
	const VoiceMixSet Voices(clamp_mix(inVoices.Dry), clamp_mix(inVoices.Wet));

	// Write Mixed results To Output Area
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1000 : 0x1800) + OutPos, Voices.Dry.Left);
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1200 : 0x1A00) + OutPos, Voices.Dry.Right);
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1400 : 0x1C00) + OutPos, Voices.Wet.Left);
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1600 : 0x1E00) + OutPos, Voices.Wet.Right);

	// Write mixed results to logfile (if enabled)

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(thiscore.Index, CoreSrc_DryVoiceMix, Voices.Dry);
	WaveDump::WriteCore(thiscore.Index, CoreSrc_WetVoiceMix, Voices.Wet);
#endif

	// Mix in the Input data

	StereoOut32 TD(
		Input.Left & thiscore.DryGate.InpL,
		Input.Right & thiscore.DryGate.InpR);

	// Mix in the Voice data
	TD.Left += Voices.Dry.Left & thiscore.DryGate.SndL;
	TD.Right += Voices.Dry.Right & thiscore.DryGate.SndR;

	// Mix in the External (nothing/core0) data
	TD.Left += Ext.Left & thiscore.DryGate.ExtL;
	TD.Right += Ext.Right & thiscore.DryGate.ExtR;

	// ----------------------------------------------------------------------------
	//    Reverberation Effects Processing
	// ----------------------------------------------------------------------------
	// SPU2 has an FxEnable bit which seems to disable all reverb processing *and*
	// output, but does *not* disable the advancing buffers.  IRQs are not triggered
	// and reverb is rendered silent.
	//
	// Technically we should advance the buffers even when fx are disabled.  However
	// there are two things that make this very unlikely to matter:
	//
	//  1. Any SPU2 app wanting to avoid noise or pops needs to clear the reverb buffers
	//     when adjusting settings anyway; so the read/write positions in the reverb
	//     buffer after FxEnabled is set back to 1 doesn't really matter.
	//
	//  2. Writes to ESA (and possibly EEA) reset the buffer pointers to 0.
	//
	// On the other hand, updating the buffer is cheap and easy, so might as well. ;)

	StereoOut32 TW;

	// Mix Input, Voice, and External data:

	TW.Left = Input.Left & thiscore.WetGate.InpL;
	TW.Right = Input.Right & thiscore.WetGate.InpR;

	TW.Left += Voices.Wet.Left & thiscore.WetGate.SndL;
	TW.Right += Voices.Wet.Right & thiscore.WetGate.SndR;
	TW.Left += Ext.Left & thiscore.WetGate.ExtL;
	TW.Right += Ext.Right & thiscore.WetGate.ExtR;

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(thiscore.Index, CoreSrc_PreReverb, TW);
#endif

	StereoOut32 RV = thiscore.DoReverb(TW);

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(thiscore.Index, CoreSrc_PostReverb, RV);
#endif

	// Mix Dry + Wet
	// (master volume is applied later to the result of both outputs added together).
	return TD + ApplyVolume(RV, thiscore.FxVol);
}

// Forja SX2: where the SPU2's time goes (main.cpp's heartbeat reports it). One sample in 16 is timed in three parts: the 48 voices,
// the cores' mix with the reverb, and the output (the chunk to the audio stream, with its time stretching).
// (The counters themselves are defined once, at the top of the file, outside the per-ISA namespace.)
static u32 s_forja_spu2_split_counter = 0;

void spu2Mix()
{
	const bool forja_timed = (++s_forja_spu2_split_counter & 15) == 0;
	const Common::Timer::Value forja_t0 = forja_timed ? Common::Timer::GetCurrentValue() : 0;
	// Note: Playmode 4 is SPDIF, which overrides other inputs.
	StereoOut32 InputData[2] =
		{
			// SPDIF is on Core 0:
			// Fixme:
			// 1. We do not have an AC3 decoder for the bitstream.
			// 2. Games usually provide a normal ADMA stream as well and want to see it getting read!
			/*(PlayMode&4) ? StereoOut32::Empty : */ ApplyVolume(Cores[0].ReadInput(), Cores[0].InpVol),

			// CDDA is on Core 1:
			(PlayMode & 8) ? StereoOut32::Empty : ApplyVolume(Cores[1].ReadInput(), Cores[1].InpVol)};

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(0, CoreSrc_Input, InputData[0]);
	WaveDump::WriteCore(1, CoreSrc_Input, InputData[1]);
#endif

	// Todo: Replace me with memzero initializer!
	VoiceMixSet VoiceData[2] = {{StereoOut32(), StereoOut32()}, {StereoOut32(), StereoOut32()}}; // mixed voice data for each core.

	MixCoreVoices(VoiceData[0], 0);
	MixCoreVoices(VoiceData[1], 1);
	const Common::Timer::Value forja_t1 = forja_timed ? Common::Timer::GetCurrentValue() : 0;

	StereoOut32 Ext(MixCore(0, VoiceData[0], InputData[0], StereoOut32::Empty));

	if ((PlayMode & 4) || (Cores[0].Mute != 0))
		Ext = StereoOut32::Empty;
	else
	{
		Ext = ApplyVolume(clamp_mix(Ext), Cores[0].MasterVol);
	}

	// Commit Core 0 output to ram before mixing Core 1:
	spu2M_WriteFast(0x800 + OutPos, Ext.Left);
	spu2M_WriteFast(0xA00 + OutPos, Ext.Right);

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(0, CoreSrc_External, Ext);
#endif

	Ext = ApplyVolume(Ext, Cores[1].ExtVol);
	StereoOut32 Out(MixCore(1, VoiceData[1], InputData[1], Ext));

	if (PlayMode & 8)
	{
		// Experimental CDDA support
		// The CDDA overrides all other mixer output.  It's a direct feed!

		Out = Cores[1].ReadInput_HiFi();
		//WaveLog::WriteCore( 1, "CDDA-32", OutL, OutR );
	}
	else
	{
		Out = ApplyVolume(clamp_mix(Out), Cores[1].MasterVol);
	}

#ifdef PCSX2_DEVBUILD
	// Log final output to wavefile.
	WaveDump::WriteCore(1, CoreSrc_External, Out);
#endif

	const Common::Timer::Value forja_t2 = forja_timed ? Common::Timer::GetCurrentValue() : 0;
	spu2Output(Out);
	if (forja_timed)
	{
		const Common::Timer::Value forja_t3 = Common::Timer::GetCurrentValue();
		g_forja_spu2_split_voices += forja_t1 - forja_t0;
		g_forja_spu2_split_mix += forja_t2 - forja_t1;
		g_forja_spu2_split_output += forja_t3 - forja_t2;
		g_forja_spu2_split_samples++;
		for (uint c = 0; c < 2; c++)
		{
			for (uint v = 0; v < V_Core::NumVoices; v++)
				g_forja_spu2_split_active += Cores[c].Voices[v].ADSR.Phase > V_ADSR::PHASE_STOPPED ? 1 : 0;
			g_forja_spu2_split_irq += Cores[c].IRQEnable ? 1 : 0;
			// The reverb runs whenever its work area is set (V_Core::DoReverb), even with effects off: is it in use?
			g_forja_spu2_split_fx_on += Cores[c].FxEnable ? 1 : 0;
			g_forja_spu2_split_fx_heard += (Cores[c].FxVol.Left != 0 || Cores[c].FxVol.Right != 0) ? 1 : 0;
			g_forja_spu2_split_fx_area += Cores[c].EffectsStartA < Cores[c].EffectsEndA ? 1 : 0;
		}
	}

	// Update AutoDMA output positioning
	OutPos++;
	if (OutPos >= 0x200)
		OutPos = 0;

	if constexpr (IsDevBuild)
	{
		// used to throttle the output rate of cache stat reports
		static int p_cachestat_counter = 0;

		p_cachestat_counter++;
		if (p_cachestat_counter > (48000 * 10))
		{
			p_cachestat_counter = 0;
			if (SPU2::MsgCache())
			{
				SPU2::ConLog(" * SPU2 > CacheStats > Hits: %d  Misses: %d  Ignores: %d\n",
					g_counter_cache_hits,
					g_counter_cache_misses,
					g_counter_cache_ignores);
			}

			g_counter_cache_hits =
				g_counter_cache_misses =
					g_counter_cache_ignores = 0;
		}
	}
}

MULTI_ISA_UNSHARED_END
