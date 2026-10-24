// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja 1.06: the session's summary, for whoever reads a report (Joseph, or a player who sends their logs):
// logs/resumen.txt, a page that says how a game ran and what held it back, rewritten every minute and at the end of the
// game, and archived with the session's other logs (SessionLogs.h). The 5 s lines of PCSX2-vm-boot.log stay as they are;
// this adds them up: the speed over the session, what held the game back, the hitches by their cause, the sound's cuts, the
// memory left, what frame generation and Saltar cuadros did, and the movies. Compilation 80: and the compatibility it measures,
// with the game told apart from its menus, movies and loads, the controllers, when it reached the game, and its hangs.
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

	// Compilation 80: what a 5 s window was, so the summary tells the game apart from its menus, movies and loads (the 75's numbers
	// mixed a title screen's "100 %" with the game's).
	enum Scene : int
	{
		SceneGame = 0, // the game itself: a fight, a race, a level
		SceneSimple,   // a simple screen: a menu, a map, a still (fewer than 100 draws a frame)
		SceneMovie,    // a movie (the video mode's detection)
		SceneLoad,     // a load or a still picture: no new frame of the game in the window
		SceneCount,
	};
	const char* SceneName(int scene);
	// The scene of a window from the movie flag, the PS2's vsyncs and frames a second, and the GS's draws a second. In the five
	// games of 2026-10-23 the menus drew 1-71 things a frame, the games 150 (Guitar Hero's songs) to 2,200, and the odd window
	// between them 82-93.
	int ClassifyScene(bool movie, double ps2_hz, double fps, double draws_per_s);

	// GS thread, every 5 s (HostVM.cpp): one window of the game.
	struct Window
	{
		double speed = 0.0, fps = 0.0;
		int limit = LimitNone;
		bool movie = false, fg_generating = false;
		// Compilation 77: the time the limiting part took per PS2 refresh, against a refresh's time at the game's own rate (20 ms
		// at 50 Hz): how much cheaper it would have to be.
		double limit_ms = 0.0, budget_ms = 0.0;
		int scene = SceneGame; // compilation 80
		int pads = 1;          // compilation 80: controllers connected
		bool two_at_limit = false; // compilation 80: a second part within 10 points of the busiest, both at 80 % or more
	};
	void AddWindow(const Window& window);
	// Compilation 80: the hang watch's (main.cpp) 10 s without a frame while the game runs.
	void AddHang();
	// GS thread (GSRendererPS4.cpp, ForjaSessionHitch): a hitch of gap_ms between two pictures, and what happened in it.
	void AddHitch(double gap_ms, unsigned ee_blocks_compiled, unsigned ee_rec_resets, unsigned pipelines_created, unsigned disc_kb);
	// CPU thread, every heartbeat (main.cpp): the regular flexible memory free, how far the heap reaches, and the sound's
	// output counters (underruns since its stream opened, frames the limiter turned down). Compilation 79: and whether a movie
	// plays, so the sound's cuts are told apart (in movies, in the game).
	void AddHeartbeat(uint64_t flexible_free, uint64_t heap_high, uint32_t audio_underruns, uint64_t audio_limited, bool movie = false);
	// CPU thread, the 5 s Saltar cuadros report (main.cpp): frames left out, and the guard's pauses.
	void AddSkip(unsigned skipped, unsigned guard_pauses);

	// CPU thread: a new game, everything starts over.
	void Begin();
	// CPU thread: writes logs/resumen.txt with the identity and settings lines the caller gives, and the figures so far.
	void Write(const std::string& header, const std::string& settings, bool final);
	// The summary's text, for the log and the PC test.
	std::string Text(const std::string& header, const std::string& settings, bool final);
	// Compilation 80: the compatibility the speed says, in a few words ("velocidad completa (mediana 100 %, ...)"), for the report.
	std::string Verdict();
} // namespace ForjaSession
