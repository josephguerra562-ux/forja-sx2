// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/spu2.h"
#include "SPU2/defs.h"
#include "SPU2/Debug.h"
#include "SPU2/Dma.h"
#include "Host/AudioStream.h"
#include "Host.h"
#include "GS/GSCapture.h"
#include "MTGS.h"
#include "R3000A.h"
#include "VMManager.h"

#include "common/Error.h"
#include "common/Timer.h"

#include "ps4/core/ThreadAffinityPS4.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <emmintrin.h>
#include <mutex>
#include <thread>
#include <unistd.h>

extern void ProbeLog(const char* format, ...);

const StereoOut32 StereoOut32::Empty(0, 0);

namespace SPU2
{
	static void CreateOutputStream();
	static void UpdateSampleRate();
	static float GetNominalRate();
	static void InternalReset(bool psxmode);
} // namespace SPU2

u64 lClocks = 0;

static bool s_audio_capture_active = false;
static bool s_psxmode = false;
static bool s_output_muted = false;

// Forja SX2: stopped SPU2 voices are replayed in batches (Mixer.cpp, MixCoreVoices); they are brought up to date before
// anything reads or writes the SPU2's registers, and dropped on a reset or a loaded savestate.
extern void (*g_forja_spu2_catch_up)();
extern void ForjaSPU2ClearPending();

// Forja SX2 (compilation 32): the SPU2 mix on a thread of its own, on the audio core (nucleos.txt AUDIO=).
// The mix took 8 to 14 % of the EE thread in every game (2026-10-16: the voices, the cores' mix and reverb, the output), and
// the EE thread is what limits God of War II's movies and Tales of the Abyss. The game cannot see a sample's mix until it next
// touches the SPU2: it reads or writes a register (SPU2read/SPU2write, after their TimeUpdate), a DMA writes or reads SPU2
// memory, a voice is keyed on or off, a savestate is taken. So the EE thread reads the sample's input (the ADMA stream: its
// requests and interrupts are timing the game sees), queues the rest, and goes on; this thread mixes the queue. Before any of
// those, ForjaSPU2Sync() mixes whatever is still queued right there on the EE thread: the EE thread never waits for this thread
// to wake up (on the PS4, waking a thread cost more than the work: see ForjaAudioThread above), at most for the one sample it is
// in the middle of. While an IRQ address is armed (a voice, the reverb or the output reaching it raises the IOP's interrupt at
// that very sample) and in CDDA mode, every sample is mixed on the EE thread, as before. /data/ForjaSX2/vm-spu2-mismo-hilo
// keeps every sample on the EE thread.
namespace
{
	struct ForjaSPU2Sample
	{
		StereoOut32 in0, in1;
		u32 cycles;
		u16 outpos;
	};
	constexpr u32 FORJA_SPU2_RING = 4096; // 85 ms of samples
	std::array<ForjaSPU2Sample, FORJA_SPU2_RING> s_forja_spu2_ring;
	std::atomic<u32> s_forja_spu2_head{0}; // the next sample to queue: written by the EE thread
	std::atomic<u32> s_forja_spu2_tail{0}; // the next sample to mix: written while holding s_forja_spu2_mixing
	std::atomic<bool> s_forja_spu2_mixing{false}; // whoever mixes queued samples holds it
	std::atomic<bool> s_forja_spu2_ee_wants{false}; // the EE thread is waiting for it: the thread stops after its sample
	int s_forja_spu2_threaded = -1; // read from the marker at the first sample
	bool s_forja_spu2_thread_started = false;
	// For the heartbeat line (EE thread, but s_forja_spu2_thread_mixed).
	u64 s_forja_spu2_queued = 0, s_forja_spu2_direct = 0, s_forja_spu2_direct_irq = 0, s_forja_spu2_stolen = 0;
	u64 s_forja_spu2_syncs = 0, s_forja_spu2_sync_ticks = 0;
	std::atomic<u64> s_forja_spu2_thread_mixed{0};
	u32 s_forja_spu2_log_counter = 0;
	Common::Timer::Value s_forja_spu2_log_start = 0;

	void ForjaSPU2Thread()
	{
		PS4ThreadAffinity::PinCurrentThread("Audio", 5);
		// Between bursts of samples it keeps looking for ~50 us, then naps 0.2 ms at a time, and 2 ms at a time once nothing
		// came for 0.1 s (a pause, the library): being late costs nothing but the EE thread mixing those samples itself.
		const Common::Timer::Value spin_ticks = Common::Timer::ConvertNanosecondsToValue(50000.0);
		const Common::Timer::Value long_idle_ticks = Common::Timer::ConvertMillisecondsToValue(100.0);
		Common::Timer::Value idle_since = 0;
		u32 idle_checks = 0;
		for (;;)
		{
			const u32 head = s_forja_spu2_head.load(std::memory_order_acquire);
			if (s_forja_spu2_tail.load(std::memory_order_relaxed) == head || s_forja_spu2_ee_wants.load(std::memory_order_relaxed))
			{
				if (idle_checks++ == 0)
					idle_since = Common::Timer::GetCurrentValue();
				else if ((idle_checks & 63) == 0)
				{
					const Common::Timer::Value idle = Common::Timer::GetCurrentValue() - idle_since;
					if (idle > spin_ticks)
					{
						// This thread lives on from game to game: each game's profile can put it on another core.
						PS4ThreadAffinity::RepinIfPlanChanged("Audio", 5);
						usleep(idle > long_idle_ticks ? 2000 : 200);
						continue;
					}
				}
				_mm_pause();
				continue;
			}
			idle_checks = 0;
			if (s_forja_spu2_mixing.exchange(true, std::memory_order_acquire))
			{
				_mm_pause();
				continue;
			}
			// Compilation 33: head and tail again, now holding the lock. The EE thread can mix the whole queue between the
			// look above and the lock (ForjaSPU2Sync); with the old head this thread then mixed up to 64 stale ring entries and
			// left tail past head, and the EE thread's next sync mixed ~4 billion "samples" (God of War II froze within a
			// minute of fighting, 2026-10-16: the EE thread at 100 % in the mix, no vsync for 8 s).
			const u32 head_now = s_forja_spu2_head.load(std::memory_order_acquire);
			u32 tail = s_forja_spu2_tail.load(std::memory_order_relaxed);
			u32 mixed = 0;
			while (tail != head_now && mixed < 64 && !s_forja_spu2_ee_wants.load(std::memory_order_relaxed))
			{
				const ForjaSPU2Sample& sample = s_forja_spu2_ring[tail % FORJA_SPU2_RING];
				ForjaSPU2MixBack(sample.in0, sample.in1, sample.outpos, sample.cycles);
				tail++;
				mixed++;
			}
			s_forja_spu2_tail.store(tail, std::memory_order_release);
			s_forja_spu2_mixing.store(false, std::memory_order_release);
			s_forja_spu2_thread_mixed.fetch_add(mixed, std::memory_order_relaxed);
		}
	}

	// Every ~5 s of the game's audio: where its samples were mixed and what keeping up cost the EE thread.
	void ForjaSPU2LogMaybe()
	{
		if (++s_forja_spu2_log_counter < 48000 * 5)
			return;
		s_forja_spu2_log_counter = 0;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		const double seconds = s_forja_spu2_log_start ? Common::Timer::ConvertValueToSeconds(now - s_forja_spu2_log_start) : 0.0;
		s_forja_spu2_log_start = now;
		const u64 thread_mixed = s_forja_spu2_thread_mixed.exchange(0, std::memory_order_relaxed);
		const double total = static_cast<double>(s_forja_spu2_queued + s_forja_spu2_direct);
		if (seconds > 0.0 && total > 0.0)
			ProbeLog("Audio SPU2 thread: on=%d mixed_by_thread_pct=%.1f by_EE_at_sync_pct=%.1f by_EE_direct_pct=%.1f "
					 "(irq_armed_pct=%.1f) syncs_per_second=%.0f EE_sync_ms_per_second=%.1f\n",
				s_forja_spu2_threaded, thread_mixed * 100.0 / total, s_forja_spu2_stolen * 100.0 / total,
				s_forja_spu2_direct * 100.0 / total, s_forja_spu2_direct_irq * 100.0 / total, s_forja_spu2_syncs / seconds,
				Common::Timer::ConvertValueToMilliseconds(s_forja_spu2_sync_ticks) / seconds);
		s_forja_spu2_queued = s_forja_spu2_direct = s_forja_spu2_direct_irq = s_forja_spu2_stolen = 0;
		s_forja_spu2_syncs = s_forja_spu2_sync_ticks = 0;
	}
} // namespace

void ForjaSPU2Sync()
{
	const u32 head = s_forja_spu2_head.load(std::memory_order_relaxed);
	if (s_forja_spu2_tail.load(std::memory_order_acquire) == head)
		return;
	const Common::Timer::Value start = Common::Timer::GetCurrentValue();
	s_forja_spu2_ee_wants.store(true, std::memory_order_relaxed);
	while (s_forja_spu2_mixing.exchange(true, std::memory_order_acquire))
		_mm_pause();
	u32 tail = s_forja_spu2_tail.load(std::memory_order_relaxed);
	// Never more than the ring: a tail past head (the compilation 32 bug above) would be ~4 billion samples. Drop the
	// queue instead, once, and say so.
	if (head - tail > FORJA_SPU2_RING)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			ProbeLog("Audio SPU2 thread: queue out of order (head %u tail %u), dropped\n", head, tail);
		}
		tail = head;
	}
	s_forja_spu2_stolen += head - tail;
	while (tail != head)
	{
		const ForjaSPU2Sample& sample = s_forja_spu2_ring[tail % FORJA_SPU2_RING];
		ForjaSPU2MixBack(sample.in0, sample.in1, sample.outpos, sample.cycles);
		tail++;
	}
	s_forja_spu2_tail.store(tail, std::memory_order_release);
	s_forja_spu2_mixing.store(false, std::memory_order_release);
	s_forja_spu2_ee_wants.store(false, std::memory_order_relaxed);
	s_forja_spu2_syncs++;
	s_forja_spu2_sync_ticks += Common::Timer::GetCurrentValue() - start;
}

void ForjaSPU2CatchUp()
{
	ForjaSPU2Sync();
	if (g_forja_spu2_catch_up)
		g_forja_spu2_catch_up();
}

void ForjaSPU2Submit(StereoOut32 in0, StereoOut32 in1, u16 outpos, u32 cycles)
{
	if (s_forja_spu2_threaded < 0)
	{
		s_forja_spu2_threaded = access("/data/ForjaSX2/vm-spu2-mismo-hilo", F_OK) != 0 ? 1 : 0;
		ProbeLog("Audio SPU2 thread: %s (/data/ForjaSX2/vm-spu2-mismo-hilo keeps the mix on the EE thread)\n",
			s_forja_spu2_threaded ? "the mix runs on the audio core" : "off, the mix stays on the EE thread");
	}
	const bool irq_armed = Cores[0].IRQEnable || Cores[1].IRQEnable;
	if (!s_forja_spu2_threaded || irq_armed || (PlayMode & 8))
	{
		ForjaSPU2Sync(); // the queue first: samples are mixed in order
		ForjaSPU2MixBack(in0, in1, outpos, cycles);
		s_forja_spu2_direct++;
		s_forja_spu2_direct_irq += irq_armed ? 1 : 0;
		ForjaSPU2LogMaybe();
		return;
	}
	if (!s_forja_spu2_thread_started)
	{
		s_forja_spu2_thread_started = true;
		std::thread(ForjaSPU2Thread).detach();
	}
	const u32 head = s_forja_spu2_head.load(std::memory_order_relaxed);
	if (head - s_forja_spu2_tail.load(std::memory_order_acquire) >= FORJA_SPU2_RING)
		ForjaSPU2Sync(); // the thread is 85 ms behind: the EE thread mixes the queue
	s_forja_spu2_ring[head % FORJA_SPU2_RING] = {in0, in1, cycles, outpos};
	s_forja_spu2_head.store(head + 1, std::memory_order_release);
	s_forja_spu2_queued++;
	ForjaSPU2LogMaybe();
}

static std::unique_ptr<AudioStream> s_output_stream;
// Forja SX2: the audio stream is also written by ForjaAudioThread() below; everything that uses it holds this.
static std::recursive_mutex s_stream_mutex;

// Forja SX2: how often the EE thread waited for the audio write thread (main.cpp's heartbeat reports it).
u64 g_forja_audio_wait_ticks = 0, g_forja_audio_waits = 0, g_forja_audio_chunks = 0;

namespace
{
	// Forja SX2: the stream's chunk writing, with its time stretching (SoundTouch), on a thread of its own. It ran on the EE
	// thread, in spu2Output(), and took ~60 ms of it a second in MMXCM's and Tales of the Abyss's movies (2026-10-15), when the EE
	// thread was at 103-107 %. The EE thread now copies each chunk into a ring and goes on; the thread writes them into the stream
	// in order, on the helper cores. /data/ForjaSX2/vm-audio-sync-stretch keeps the old way.
	constexpr u32 FORJA_AUDIO_SLOTS = 64; // 64 chunks of 64 frames: 85 ms
	std::array<std::array<float, AudioStream::CHUNK_SIZE * 2>, FORJA_AUDIO_SLOTS> s_forja_audio_ring;
	std::atomic<u32> s_forja_audio_head{0};
	std::atomic<u32> s_forja_audio_tail{0};
	bool s_forja_audio_thread_started = false;
	int s_forja_audio_async = -1; // read from the marker at the first chunk

	void ForjaAudioThread()
	{
		// Compilation 23: on the audio core (nucleos.txt AUDIO=), not the helpers: there the ring filled up and the EE thread
		// waited for it (the output's share of the EE thread went from ~60 to ~77 ms a second in God of War II).
		PS4ThreadAffinity::PinCurrentThread("Audio", 5);
		// Compilation 24: it looks at the ring every millisecond by itself. Waking it for each chunk (a mutex and a condition
		// signal, ~600 a second) cost the EE thread more than the chunk writing had (~75 ms a second in Tales' movies).
		for (;;)
		{
			if (s_forja_audio_head.load(std::memory_order_acquire) == s_forja_audio_tail.load(std::memory_order_relaxed))
			{
				// This thread lives on from game to game: each game's profile can put it on another core.
				PS4ThreadAffinity::RepinIfPlanChanged("Audio", 5);
				usleep(1000);
				continue;
			}
			u32 tail = s_forja_audio_tail.load(std::memory_order_relaxed);
			while (tail != s_forja_audio_head.load(std::memory_order_acquire))
			{
				{
					std::lock_guard stream_lock(s_stream_mutex);
					if (s_output_stream)
						s_output_stream->WriteChunk(s_forja_audio_ring[tail % FORJA_AUDIO_SLOTS].data());
				}
				tail++;
				s_forja_audio_tail.store(tail, std::memory_order_release);
			}
		}
	}

	void ForjaAudioWrite(const float* chunk)
	{
		if (s_forja_audio_async < 0)
			s_forja_audio_async = access("/data/ForjaSX2/vm-audio-sync-stretch", F_OK) != 0 ? 1 : 0;
		if (!s_forja_audio_async)
		{
			std::lock_guard stream_lock(s_stream_mutex);
			s_output_stream->WriteChunk(chunk);
			return;
		}
		if (!s_forja_audio_thread_started)
		{
			s_forja_audio_thread_started = true;
			std::thread(ForjaAudioThread).detach();
		}
		const u32 head = s_forja_audio_head.load(std::memory_order_relaxed);
		// A full ring (the thread 85 ms behind) waits for a slot: dropping or reordering audio would be heard.
		if (head - s_forja_audio_tail.load(std::memory_order_acquire) >= FORJA_AUDIO_SLOTS)
		{
			const Common::Timer::Value wait_start = Common::Timer::GetCurrentValue();
			while (head - s_forja_audio_tail.load(std::memory_order_acquire) >= FORJA_AUDIO_SLOTS)
				std::this_thread::yield();
			g_forja_audio_wait_ticks += Common::Timer::GetCurrentValue() - wait_start;
			g_forja_audio_waits++;
		}
		g_forja_audio_chunks++;
		std::memcpy(s_forja_audio_ring[head % FORJA_AUDIO_SLOTS].data(), chunk, sizeof(s_forja_audio_ring[0]));
		s_forja_audio_head.store(head + 1, std::memory_order_release);
	}
} // namespace

static std::array<float, AudioStream::CHUNK_SIZE * 2> s_current_chunk;
static u32 s_current_chunk_pos;
static u32 s_standard_volume = 0;
static u32 s_fast_forward_volume = 0;

float DCFilterIn[2], DCFilterOut[2];

u32 SPU2::GetConsoleSampleRate()
{
	return s_psxmode ? PSX_SAMPLE_RATE : SAMPLE_RATE;
}

// --------------------------------------------------------------------------------------
//  DMA 4/7 Callbacks from Core Emulator
// --------------------------------------------------------------------------------------


void SPU2readDMA4Mem(u16* pMem, u32 size) // size now in 16bit units
{
	TimeUpdate(psxRegs.cycle);

	SPU2::FileLog("[%10d] SPU2 readDMA4Mem size %x\n", Cycles, size << 1);
	Cores[0].DoDMAread(pMem, size);
}

void SPU2writeDMA4Mem(u16* pMem, u32 size) // size now in 16bit units
{
	TimeUpdate(psxRegs.cycle);

	SPU2::FileLog("[%10d] SPU2 writeDMA4Mem size %x at address %x\n", Cycles, size << 1, Cores[0].TSA);

	Cores[0].DoDMAwrite(pMem, size);
}

void SPU2interruptDMA4()
{
	SPU2::FileLog("[%10d] SPU2 interruptDMA4\n", Cycles);
	if (Cores[0].DmaMode)
		Cores[0].Regs.STATX |= 0x80;
	Cores[0].Regs.STATX &= ~0x400;
	Cores[0].TSA = Cores[0].ActiveTSA;
}

void SPU2interruptDMA7()
{
	SPU2::FileLog("[%10d] SPU2 interruptDMA7\n", Cycles);
	if (Cores[1].DmaMode)
		Cores[1].Regs.STATX |= 0x80;
	Cores[1].Regs.STATX &= ~0x400;
	Cores[1].TSA = Cores[1].ActiveTSA;
}

void SPU2readDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	SPU2::FileLog("[%10d] SPU2 readDMA7Mem size %x\n", Cycles, size << 1);
	Cores[1].DoDMAread(pMem, size);
}

void SPU2writeDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	SPU2::FileLog("[%10d] SPU2 writeDMA7Mem size %x at address %x\n", Cycles, size << 1, Cores[1].TSA);

	Cores[1].DoDMAwrite(pMem, size);
}

void SPU2::CreateOutputStream()
{
	ForjaSPU2Sync(); // Forja SX2: the SPU2 mixing thread writes chunks to the stream (before the lock: it may need it)
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	// Initialize volume and mute settings on new session.
	if (!s_output_stream)
	{
		s_standard_volume = EmuConfig.SPU2.StandardVolume;
		s_fast_forward_volume = EmuConfig.SPU2.FastForwardVolume;
		s_output_muted = EmuConfig.SPU2.OutputMuted;
	}
	// Else persist volume through stream recreates.
	else if (!s_output_muted)
		SPU2::SaveOutputVolume();

	const u32 sample_rate = GetConsoleSampleRate();
	s_output_stream.reset();

	Error error;
	s_output_stream = AudioStream::CreateStream(EmuConfig.SPU2.Backend, sample_rate, EmuConfig.SPU2.StreamParameters,
		EmuConfig.SPU2.DriverName.c_str(), EmuConfig.SPU2.DeviceName.c_str(), EmuConfig.SPU2.IsTimeStretchEnabled(), &error);
	if (!s_output_stream)
	{
		Host::ReportErrorAsync("Error",
			fmt::format("Failed to create or configure audio stream, falling back to null output. The error was:\n{}",
				error.GetDescription()));

		s_output_stream = AudioStream::CreateNullStream(sample_rate, EmuConfig.SPU2.StreamParameters.buffer_ms);
	}

	SPU2::UpdateOutputVolume();
	s_output_stream->SetNominalRate(GetNominalRate());
	s_output_stream->SetPaused(VMManager::GetState() == VMState::Paused);
}

void SPU2::UpdateSampleRate()
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	if (s_output_stream && s_output_stream->GetSampleRate() == GetConsoleSampleRate())
		return;

	CreateOutputStream();

	// Can't be capturing when the sample rate changes.
	if (IsAudioCaptureActive())
	{
		MTGS::RunOnGSThread(&GSEndCapture);
		MTGS::WaitGS(false, false, false);
	}
}

u32 SPU2::GetOutputVolume()
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	return s_output_stream ? s_output_stream->GetOutputVolume() : 0;
}

void SPU2::SetOutputVolume(u32 volume)
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	if (VMManager::GetTargetSpeed() == 1.0f)
		s_standard_volume = volume;
	else
		s_fast_forward_volume = volume;

	if (s_output_stream)
		SPU2::UpdateOutputVolume();
}

float SPU2::GetNominalRate()
{
	// Adjust nominal rate when syncing to host.
	return VMManager::IsTargetSpeedAdjustedToHost() ? VMManager::GetTargetSpeed() : 1.0f;
}

bool SPU2::SetOutputMuted(const bool muted)
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	// User setting takes precedence. Unmute not guaranteed by design.
	if (!s_output_stream || (!muted && EmuConfig.SPU2.OutputMuted))
		return false;

	if (muted == s_output_muted)
		return true;

	if (muted)
		SPU2::SaveOutputVolume();

	s_output_muted = muted;
	SPU2::UpdateOutputVolume();
	return true;
}

bool SPU2::IsOutputMuted()
{
	return s_output_muted;
}

void SPU2::UpdateOutputVolume()
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	s_output_stream->SetOutputVolume(s_output_muted ?
										 0 : (VMManager::GetTargetSpeed() == 1.0f ?
										 	s_standard_volume : s_fast_forward_volume));
}

void SPU2::SaveOutputVolume()
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	if (!s_output_muted)
	{
		if (VMManager::GetTargetSpeed() == 1.0f)
			s_standard_volume = GetOutputVolume();
		else
			s_fast_forward_volume = GetOutputVolume();
	}
}

void SPU2::SetOutputPaused(bool paused)
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	s_output_stream->SetPaused(paused);
}

void SPU2::SetAudioCaptureActive(bool active)
{
	s_audio_capture_active = active;
}

bool SPU2::IsAudioCaptureActive()
{
	return s_audio_capture_active;
}

void SPU2::InternalReset(bool psxmode)
{
	ForjaSPU2Sync(); // Forja SX2: the queued samples belong to the SPU2 being reset
	spu2Mix = MULTI_ISA_SELECT(spu2Mix);
	ForjaSPU2MixBack = MULTI_ISA_SELECT(ForjaSPU2MixBack);
	ReverbDownsample = MULTI_ISA_SELECT(ReverbDownsample);
	ReverbUpsample = MULTI_ISA_SELECT(ReverbUpsample);

	s_current_chunk_pos = 0;
	s_psxmode = psxmode;
	ForjaSPU2ClearPending();
	if (!s_psxmode)
	{
		memset(spu2regs, 0, 0x010000);
		memset(_spu2mem, 0, 0x200000);
		memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
		memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.

		memset(DCFilterIn, 0, sizeof(DCFilterIn));
		memset(DCFilterOut, 0, sizeof(DCFilterOut));

		Spdif.Info = 0; // Reset IRQ Status if it got set in a previously run game

		Cores[0].Init(0);
		Cores[1].Init(1);
	}
}

void SPU2::Reset(bool psxmode)
{
	InternalReset(psxmode);
	UpdateSampleRate();
}

void SPU2::OnTargetSpeedChanged()
{
	ForjaSPU2Sync(); // Forja SX2: s_current_chunk_pos below is the SPU2 mixing thread's too
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	if (!s_output_stream)
		return;

	if (!s_output_stream->IsStretchEnabled())
	{
		s_output_stream->EmptyBuffer();
		s_current_chunk_pos = 0;
	}

	s_output_stream->SetNominalRate(GetNominalRate());

	// Flipped save as speed has already changed.
	if (!s_output_muted)
	{
		if (VMManager::GetTargetSpeed() == 1.0f)
		{
			s_fast_forward_volume = GetOutputVolume();
			s_output_stream->SetOutputVolume(s_standard_volume);
		}
		else
		{
			s_standard_volume = GetOutputVolume();
			s_output_stream->SetOutputVolume(s_fast_forward_volume);
		}
	}
}

bool SPU2::Open()
{
#ifdef PCSX2_DEVBUILD
	if (SPU2::AccessLog())
		SPU2::OpenFileLog();
#endif

#ifdef PCSX2_DEVBUILD
	DMALogOpen();

	FileLog("[%10d] SPU2 Open\n", Cycles);
#endif

	lClocks = psxRegs.cycle;

	InternalReset(false);

	CreateOutputStream();

#ifdef PCSX2_DEVBUILD
	WaveDump::Open();
#endif

	return true;
}

void SPU2::Close()
{
	ForjaSPU2Sync(); // Forja SX2: the SPU2 mixing thread's last samples go to the stream before it closes
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	FileLog("[%10d] SPU2 Close\n", Cycles);

	s_output_stream.reset();

#ifdef PCSX2_DEVBUILD
	WaveDump::Close();
	DMALogClose();

	DoFullDump();
	CloseFileLog();
#endif
}

bool SPU2::IsRunningPSXMode()
{
	return s_psxmode;
}

void SPU2::CheckForConfigChanges(const Pcsx2Config& old_config)
{
	std::lock_guard forja_stream_lock(s_stream_mutex); // Forja SX2: ForjaAudioThread() writes the stream too
	const Pcsx2Config::SPU2Options& opts = EmuConfig.SPU2;
	const Pcsx2Config::SPU2Options& old_opts = old_config.SPU2;

	// No need to reinit for volume change.
	if (opts.OutputMuted != old_opts.OutputMuted)
		SPU2::SetOutputMuted(opts.OutputMuted);

	bool volume_settings_changed = false;
	if (opts.StandardVolume != old_opts.StandardVolume)
	{
		s_standard_volume = opts.StandardVolume;
		volume_settings_changed = true;
	}

	if (opts.FastForwardVolume != old_opts.FastForwardVolume)
	{
		s_fast_forward_volume = opts.FastForwardVolume;
		volume_settings_changed = true;
	}

	if (volume_settings_changed)
		SPU2::UpdateOutputVolume();

	// Things which require re-initialzing the output.
	if (opts.Backend != old_opts.Backend ||
		opts.StreamParameters != old_opts.StreamParameters ||
		opts.DriverName != old_opts.DriverName ||
		opts.DeviceName != old_opts.DeviceName)
	{
		CreateOutputStream();
	}
	else if (opts.IsTimeStretchEnabled() != old_opts.IsTimeStretchEnabled())
	{
		s_output_stream->SetStretchEnabled(opts.IsTimeStretchEnabled());
	}

#ifdef PCSX2_DEVBUILD
	// AccessLog controls file output.
	if (opts.AccessLog != old_opts.AccessLog)
	{
		if (AccessLog())
			OpenFileLog();
		else
			CloseFileLog();
	}
#endif
}

void SPU2async()
{
	TimeUpdate(psxRegs.cycle);
}

u16 SPU2read(u32 rmem)
{
	u16 ret = 0xDEAD;
	u32 core = 0;
	const u32 mem = rmem & 0xFFFF;
	u32 omem = mem;

	if (mem & 0x400)
	{
		omem ^= 0x400;
		core = 1;
	}

	if (omem == 0x1f9001AC)
	{
		Cores[core].ActiveTSA = Cores[core].TSA;
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
			{
				SetIrqCall(i);
			}
		}
		ret = Cores[core].DmaRead();
	}
	else
	{
		TimeUpdate(psxRegs.cycle);
		ForjaSPU2CatchUp();

		if (rmem >> 16 == 0x1f80)
		{
			ret = Cores[0].ReadRegPS1(rmem);
		}
		else if (mem >= 0x800)
		{
			ret = spu2Ru16(mem);
			if (SPU2::MsgToConsole())
				SPU2::ConLog("* SPU2: Read from reg>=0x800: %x value %x\n", mem, ret);
		}
		else
		{
			ret = *(regtable[(mem >> 1)]);
#ifdef PCSX2_DEVBUILD
			//FileLog("[%10d] SPU2 read mem %x (core %d, register %x): %x\n",Cycles, mem, core, (omem & 0x7ff), ret);
			SPU2::WriteRegLog("read", rmem, ret);
#endif
		}
	}

	return ret;
}

void SPU2write(u32 rmem, u16 value)
{
	// Note: Reverb/Effects are very sensitive to having precise update timings.
	// If the SPU2 isn't in in sync with the IOP, samples can end up playing at rather
	// incorrect pitches and loop lengths.

	TimeUpdate(psxRegs.cycle);
	ForjaSPU2CatchUp();

	if (rmem >> 16 == 0x1f80)
		Cores[0].WriteRegPS1(rmem, value);
	else
	{
#ifdef PCSX2_DEVBUILD
		SPU2::WriteRegLog("write", rmem, value);
#endif
		SPU2_FastWrite(rmem, value);
	}
}

s32 SPU2freeze(FreezeAction mode, freezeData* data)
{
	pxAssume(data != nullptr);
	if (!data)
	{
		printf("SPU2 savestate null pointer!\n");
		return -1;
	}

	if (mode == FreezeAction::Size)
	{
		data->size = SPU2Savestate::SizeIt();
		return 0;
	}

	pxAssume(mode == FreezeAction::Load || mode == FreezeAction::Save);

	if (data->data == nullptr)
	{
		printf("SPU2 savestate null pointer!\n");
		return -1;
	}

	auto& spud = (SPU2Savestate::DataBlock&)*(data->data);

	switch (mode)
	{
		case FreezeAction::Load:
			ForjaSPU2Sync();
			ForjaSPU2ClearPending();
			return SPU2Savestate::ThawIt(spud);
		case FreezeAction::Save:
			ForjaSPU2CatchUp();
			return SPU2Savestate::FreezeIt(spud);

			jNO_DEFAULT;
	}

	// technically unreachable, but kills a warning:
	return 0;
}

static void DCFilter(float *input)
{
	// A simple DC blocking high-pass filter
	// Implementation from http://peabody.sapp.org/class/dmp2/lab/dcblock/
	float output[2];
	output[0] = (input[0] - DCFilterIn[0] + ((0.995f * DCFilterOut[0])));
	output[1] = (input[1] - DCFilterIn[1] + ((0.995f * DCFilterOut[1])));

	DCFilterIn[0] = input[0];
	DCFilterIn[1] = input[1];
	DCFilterOut[0] = output[0];
	DCFilterOut[1] = output[1];

	input[0] = output[0];
	input[1] = output[1];
}

__forceinline void spu2Output(StereoOut32 out)
{
	float conv[2];

	conv[0] = static_cast<float>(clamp_mix(out.Left)) / INT16_MAX;
	conv[1] = static_cast<float>(clamp_mix(out.Right)) / INT16_MAX;

	/* Some games pause voices with the volume left on leaving us with
     * significant DC offset, so we filter it out (e.x. SSX 3)*/
	DCFilter(conv);

	s_current_chunk[s_current_chunk_pos++] = conv[0];
	s_current_chunk[s_current_chunk_pos++] = conv[1];
	if (s_current_chunk_pos == s_current_chunk.size())
	{
		s_current_chunk_pos = 0;

		ForjaAudioWrite(s_current_chunk.data()); // Forja SX2: on the audio write thread

		if (SPU2::IsAudioCaptureActive()) [[unlikely]]
			GSCapture::DeliverAudioPacket(s_current_chunk.data());
	}
}
