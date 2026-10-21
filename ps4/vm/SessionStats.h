// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja 1.06: the session's summary, for whoever reads a report (Joseph, or a player who sends their logs):
// logs/resumen.txt, a page that says how a game ran and what held it back, rewritten every minute and at the end of the
// game, and archived with the session's other logs (SessionLogs.h). The 5 s lines of PCSX2-vm-boot.log stay as they are;
// this adds them up: the speed over the session, what held the game back, the hitches by their cause, the sound's cuts, the
// memory left, what frame generation and Saltar cuadros did, and the movies.
#pragma once

#include <cstdint>
#include <string>

namespace ForjaSession
{
	// What held a 5 s window of the game back (HostVM.cpp's "Limit=" in the Performance line, and the HUD's brackets).
	enum Limit : int
	{
		LimitNone = 0, // at full speed
		LimitEE,
		LimitVU1,
		LimitGS,
		LimitGPU,
		LimitWait, // slow, and none of them full: waits (the disc, the IOP, a load)
		LimitCount,
	};
	const char* LimitName(int limit);

	// The limit of a window: under 97 % of speed, the busiest of the EE, VU1 and GS threads and the GPU at 65 % or more. Dragon
	// Ball's fights held back by the GS had it at 65-88 % (it also waits for the GPU and the presents).
	int ClassifyLimit(double speed, double ee_pct, double vu1_pct, double gs_pct, double gpu_pct);

	// GS thread, every 5 s (HostVM.cpp): one window of the game.
	void AddWindow(double speed, double fps, int limit, bool movie, bool fg_generating);
	// GS thread (GSRendererPS4.cpp, ForjaSessionHitch): a hitch of gap_ms between two pictures, and what happened in it.
	void AddHitch(double gap_ms, unsigned ee_blocks_compiled, unsigned ee_rec_resets, unsigned pipelines_created, unsigned disc_kb);
	// CPU thread, every heartbeat (main.cpp): the regular flexible memory free, how far the heap reaches, and the sound's
	// output counters (underruns since its stream opened, frames the limiter turned down).
	void AddHeartbeat(uint64_t flexible_free, uint64_t heap_high, uint32_t audio_underruns, uint64_t audio_limited);
	// CPU thread, the 5 s Saltar cuadros report (main.cpp): frames left out, and the guard's pauses.
	void AddSkip(unsigned skipped, unsigned guard_pauses);

	// CPU thread: a new game, everything starts over.
	void Begin();
	// CPU thread: writes logs/resumen.txt with the identity and settings lines the caller gives, and the figures so far.
	void Write(const std::string& header, const std::string& settings, bool final);
	// The summary's text, for the log and the PC test.
	std::string Text(const std::string& header, const std::string& settings, bool final);
} // namespace ForjaSession
