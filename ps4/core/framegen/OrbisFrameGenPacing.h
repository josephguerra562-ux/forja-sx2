// PS5SX2 (vk-285-133, AI-assisted): frame generation's pacing, counted in the PS2's own vsyncs.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vk-285-131 measured the game's frame interval on the wall clock, between presents. At 60 Hz that fed itself: once a
// generated frame had been shown (a slow first second was enough), it took a refresh of its own, so the game's frames came
// every two refreshes, which looked like a 30 fps game that should get a generated frame, and so on. Ratchet & Clank ran at
// 30 fps and half speed, with its GS and VU threads waiting on the display (2026-10-08, on the console).
//
// Here the interval is the number of PS2 vsyncs between two new frames of the game, which the presents can't change, and a
// frame's presents never exceed the display refreshes the PS2 gives it:
//     refreshes = vsyncs x display Hz / PS2 Hz
// The game's frame is one present. Frames are generated only when the game's frames have two refreshes or more (the median of
// the last 8 intervals: a 60 fps game on a 120 Hz display, a 30 fps one at 60 Hz), and the generated frame is presented for
// about half of them (once at 2, twice at 4, three times at 6); the game's frame stays up until the next one. A 60 fps game at
// 60 Hz gets none. A frame never gets more presents than its own interval had refreshes, less the presents already made in
// it (the game's last frame, a repeat GSRenderer.cpp still presented). While frames are generated, the vsyncs that only
// repeat the game's last frame present nothing (GSRenderer.cpp): the generated frame used their refresh. After a load or a
// pause (more than 8 vsyncs without a new frame) and at the start, 8 frames come before any is generated.
//
// The safety net: when the game runs below 95% of its target speed for 3 seconds while frames are generated, none are for
// 10 s. If the game is back at full speed (97%+) in that pause, they were costing it: the next pause is twice as long (up to
// 160 s; back to 10 s after 5 minutes without one). If it is as slow without them, the game is slow by itself there: frames
// are generated again, and for a minute that speed (less 3 points) is the floor. Nothing is generated while the frame
// limiter isn't at normal speed (turbo, slow motion, the fast boot). Header-only, so a PC test runs it (tests/fgpacing).
//
// vk-285-135: and only while the game's frame rate is steady: 7 of the last 8 intervals the same length to start, 6 to go on.
// swordpdf on vk-285-134d, Ratchet & Clank where its frames take 1 or 2 vsyncs (45-58 fps): "it felt like actually worse
// than 50". One generated frame before each of the game's on frames of uneven length makes the motion speed up and slow
// down from refresh to refresh, on top of the in-between frames' artifacts and the frame of lag; an even rate (60 to 120,
// 30 to 60) is what it doubles well. Uneven stretches show the game's own frames.
//
// Forja SX2 (compilation 40): the fluid mode ([Forja] FrameGen = 2). A PAL game's frame is 1.2 refreshes of a 59.94 Hz TV, and
// the PS4 keeps its games at 59.94 (a 50 Hz output is refused: 0x80290016, unsupported output mode, 2026-10-18), so one frame
// in five stays on screen for two refreshes: a stutter ten times a second, even at full speed. God of War II at a 27 % EE
// rate also mixes frames of one and two vsyncs. Here the refreshes each interval had (vsyncs x display Hz / PS2 Hz, with its
// fraction) go to a budget, less the presents made in it; each whole refresh left is one present of the generated frame
// before the game's. 50 frames a second at 59.94 Hz get one generated frame in every five, where the TV would repeat one;
// frames of uneven length get as many as their refreshes leave, so every refresh shows a new picture. The game's frames all
// come one refresh later, the same for each (vk-285-135's complaint was the lag that changed from frame to frame). No steady
// rate is asked for; the warm-up, the limiter's check and the safety net are the same.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace orbis_fg
{
	struct Decision
	{
		bool engaged = false;  // frames are generated for the game now: the interpolator takes every new frame
		bool reset = false;    // the interpolator's last frame is no neighbour of this one (a gap, a pause): it starts over
		uint32_t presents = 0; // presents of the generated frame before the game's, for this frame (0: none this time)
	};

	// Where the pacing is, for the log.
	enum class State
	{
		Warming,    // the start, or a load or a pause: 8 frames first
		NoRoom,     // the game's frames have fewer than two refreshes each
		Unsteady,   // vk-285-135: the game's frames aren't of one length (fewer than 7 of the last 8, or 6 once generating)
		NotNominal, // the frame limiter isn't at normal speed
		Paused,     // the safety net's pause
		Generating,
		Held,       // Forja 1.06: held by main.cpp (a PAL game's movie on a 59.94 Hz TV): the game's own frames
		GaveUp,     // Forja 1.06: off for this game, the game ran faster without them kGiveUpPauses times (NewGame)
	};

	// The safety net's news, for the log.
	enum class Event
	{
		None,
		Paused,          // the game slowed while frames were generated: none for PauseSeconds()
		ResumedCostly,   // the game was at full speed without them: the next pause is longer
		ResumedSlowGame, // it was as slow without them: a lower floor for a minute
		GaveUp,          // Forja 1.06: it was faster without them for the kGiveUpPauses-th time: off for this game
	};

	class Pacing
	{
	public:
		static constexpr uint32_t kGapVsyncs = 8;      // a longer wait between two frames: a load, a pause, a still picture
		static constexpr uint32_t kWindow = 8;         // the intervals the median is taken over; as many frames after a gap
		static constexpr double kEngage = 1.97;        // two refreshes a frame, less rounding (an HD mode's 60 Hz on 119.88: 1.998)
		static constexpr double kDisengage = 1.6;      // ...kept down to here, each frame still capped by its own interval
		static constexpr double kSlowSpeed = 95.0;     // percent of the target speed
		static constexpr double kFullSpeed = 97.0;
		static constexpr int kSlowSeconds = 3;
		static constexpr double kFirstPause = 10.0;    // seconds
		static constexpr double kMaxPause = 160.0;
		static constexpr double kPauseForget = 300.0;  // a pause this long ago no longer lengthens the next one
		static constexpr double kSettle = 2.0;         // a pause's first seconds aren't the game's speed without them yet
		static constexpr double kFloorSeconds = 60.0;
		static constexpr double kCostly = 3.0;         // points of speed a pause has to win back to blame the generated frames
		static constexpr uint32_t kSteadyStart = 7;    // vk-285-135: intervals of the window's usual length, to start generating...
		static constexpr uint32_t kSteadyKeep = 6;     // ...and to go on
		static constexpr uint32_t kGiveUpPauses = 2;   // Forja 1.06: pauses after which the game ran faster without them (3 until 75)
		static constexpr double kMaxCredit = 4.0;      // Forja SX2 (compilation 40): the fluid mode's budget, in refreshes
		static constexpr uint32_t kMaxFluidPresents = 3;

		// Forja SX2 (compilation 40): the fluid mode, set before each frame. A change starts the budget and the interpolator over.
		void SetFluid(bool fluid)
		{
			if (fluid == m_fluid)
				return;
			m_fluid = fluid;
			m_credit = 0.0;
			m_reset = true;
		}
		bool Fluid() const { return m_fluid; }
		// Forja SX2 (compilation 66): a movie playing (main.cpp, ForjaMovieSettings): the safety net makes no pauses, and one
		// under way ends; the EE's core is a movie's limit, not the generated frames.
		void SetMovie(bool movie) { m_movie = movie; }
		// Forja 1.06: no generated frames while held (main.cpp, ForjaMovieSettings: a PAL game's movie on a 59.94 Hz TV). Its 25
		// pictures a second are 2.4 refreshes each, so the in-between frame went on screen once or twice by turns, and over a
		// movie's fast cuts it showed as jumps and warps (God of War II, 2026-10-21); NTSC movies (2.0 refreshes) stay smooth.
		// The interpolator starts over when the hold ends.
		void SetHeld(bool held)
		{
			if (held == m_held)
				return;
			m_held = held;
			m_credit = 0.0;
			m_reset = true;
		}
		// Forja 1.06: where they cost speed, off for the rest of the game. Dragon Ball Z Budokai Tenkaichi 4 (2026-10-21), whose
		// limit is the GS: 67-70 % while frames were generated against 75-84 % in the pauses, and a pause every 10-40 s that
		// the player saw as stutter; each pause ended "slow by itself", since without them it still wasn't at full speed. So
		// a pause counts when the game ran kCostly points faster without them, at any speed, and after kGiveUpPauses of them
		// they stay off (movies excepted: there the EE is the limit, and they cost it nothing). NewGame: a new game, or frame
		// generation turned off and on in the menu (main.cpp, g_orbis_fg_restart).
		// Compilation 76: two such pauses, not three. Each takes 10 s plus the minute of the lower floor after it, and on
		// 2026-10-23 the generated frames cost Need for Speed Most Wanted 13 points (80 % against 93 %) and Shadow of the Colossus
		// 5-10 through all three, while they kept Saltar cuadros waiting; a second pause that wins kCostly points back is proof.
		void NewGame()
		{
			m_costly_pauses = 0;
			m_gave_up = false;
			m_reset = true;
		}
		bool GaveUp() const { return m_gave_up; }

		// One new frame of the game: `vsyncs` since the last new frame (1 at 60 fps, 2 at 30), `already` the presents made since
		// the last one was given here (its own game frame and any repeat presented after it), the PS2's and the display's rates
		// in Hz, `now` in seconds (any steady clock), the emulation's speed in percent of its target, and whether the limiter is
		// at normal speed.
		Decision Frame(uint32_t vsyncs, uint32_t already, double ps2_hz, double display_hz, double now, double speed, bool nominal)
		{
			Decision d;
			if (vsyncs == 0)
				vsyncs = 1;
			if (!(ps2_hz >= 20.0 && ps2_hz <= 100.0))
				ps2_hz = 59.94;
			if (!(display_hz >= 20.0 && display_hz <= 250.0))
				display_hz = 59.94;
			const double ratio = display_hz / ps2_hz;

			// A long wait: the window starts again, and so does the interpolator.
			const bool gap = vsyncs > kGapVsyncs;
			if (gap)
			{
				m_count = 0;
				m_reset = true;
			}
			else
			{
				m_window[m_next] = vsyncs;
				m_next = (m_next + 1) % kWindow;
				m_count = m_count < kWindow ? m_count + 1 : kWindow;
			}
			m_median = Median();
			m_steady = UsualCount();
			m_refreshes = m_median * ratio;
			const double now_refreshes = vsyncs * ratio;

			// The generated frame's presents: about half the refreshes a frame has, kept from flickering between two counts.
			const double half = m_refreshes / 2.0;
			while (m_repeats < 3 && half >= m_repeats + 0.9)
				m_repeats++;
			while (m_repeats > 1 && half < m_repeats - 0.2)
				m_repeats--;

			Second(now, speed);
			if (m_held) // Forja 1.06
			{
				m_state = State::Held;
				m_reset = true;
				return d;
			}
			if (m_gave_up && !m_movie) // Forja 1.06
			{
				m_state = State::GaveUp;
				m_reset = true;
				return d;
			}
			if (m_fluid)
				return FluidFrame(d, gap, ratio, now_refreshes, already, nominal);

			const bool warm = m_count >= kWindow;
			m_room = warm && m_refreshes >= (m_room ? kDisengage : kEngage);
			// vk-285-135: steady enough, with the same hysteresis (the state just before says which threshold).
			const bool was_generating = m_state == State::Generating || m_state == State::Paused || m_state == State::NotNominal;
			const bool steady = warm && m_steady >= (was_generating ? kSteadyKeep : kSteadyStart);
			m_state = !warm ? State::Warming : !m_room ? State::NoRoom : !steady ? State::Unsteady : !nominal ? State::NotNominal :
			          Paused() ? State::Paused : State::Generating;
			d.engaged = m_state == State::Generating;
			if (!d.engaged)
			{
				m_reset = true; // the frames skipped meanwhile weren't given to the interpolator
				return d;
			}

			// This frame's presents: never more than the refreshes its own interval had, less the presents already made in it.
			const uint32_t fit = static_cast<uint32_t>(now_refreshes + 0.03);
			const uint32_t left = fit > already ? fit - already : 0;
			d.presents = std::min(m_repeats, left);
			d.reset = m_reset;
			m_reset = false;
			if (d.presents > 0)
				m_generated_this_second = true;
			return d;
		}

		Event TakeEvent()
		{
			const Event e = m_event;
			m_event = Event::None;
			return e;
		}

		State GetState() const { return m_state; }
		double MedianVsyncs() const { return m_median; }
		uint32_t SteadyFrames() const { return m_steady; } // vk-285-135: the window's intervals of its usual length
		double Refreshes() const { return m_refreshes; }
		uint32_t Repeats() const { return m_repeats; }
		uint32_t WarmFrames() const { return m_count; }
		bool Paused() const { return m_paused_until > 0.0; }
		double PauseSeconds() const { return m_pause_length; }
		double SlowSpeed() const { return m_fg_speed; }     // the speed that started the last pause
		double PauseFloor() const { return m_pause_floor; } // the floor it fell below
		double NoFgSpeed() const { return m_nofg_speed; }   // the speed during the last pause
		double NextPause() const { return m_next_pause; }
		double Floor(double now) const { return now < m_floor_until ? m_floor : kSlowSpeed; }

	private:
		// Forja SX2 (compilation 40): the fluid mode's frame (see the top of this file).
		Decision FluidFrame(Decision d, bool gap, double ratio, double now_refreshes, uint32_t already, bool nominal)
		{
			const bool warm = m_count >= kWindow;
			// Nothing to fill when every frame is one PS2 vsync on a display of the PS2's rate (a 60 fps game at 60 Hz).
			const bool all_single = m_median <= 1.0 && m_steady == m_count;
			const bool fills = !all_single || std::fabs(ratio - std::round(ratio)) > 0.02;
			m_room = warm && fills;
			m_state = !warm ? State::Warming : !m_room ? State::NoRoom : !nominal ? State::NotNominal :
			          Paused() ? State::Paused : State::Generating;
			d.engaged = m_state == State::Generating;
			if (!d.engaged || gap)
			{
				m_credit = 0.0;
				m_reset = true;
				return d;
			}
			// This interval's refreshes, less the presents made in it (the last frame of the game's own, and a repeat if any).
			m_credit = std::clamp(m_credit + now_refreshes - static_cast<double>(already), -1.0, kMaxCredit);
			uint32_t presents = 0;
			while (presents < kMaxFluidPresents && m_credit >= 0.97)
			{
				presents++;
				m_credit -= 1.0;
			}
			d.presents = presents;
			d.reset = m_reset;
			m_reset = false;
			if (presents > 0)
				m_generated_this_second = true;
			return d;
		}

		double Median() const
		{
			if (m_count == 0)
				return 0.0;
			uint32_t v[kWindow];
			for (uint32_t i = 0; i < m_count; i++)
				v[i] = m_window[(m_next + kWindow - 1 - i) % kWindow];
			std::sort(v, v + m_count);
			return (m_count & 1) ? v[m_count / 2] : (v[m_count / 2 - 1] + v[m_count / 2]) / 2.0;
		}

		// vk-285-135: how many of the window's intervals have its most common length.
		uint32_t UsualCount() const
		{
			uint32_t best = 0;
			for (uint32_t i = 0; i < m_count; i++)
			{
				uint32_t same = 0;
				for (uint32_t j = 0; j < m_count; j++)
					same += m_window[(m_next + kWindow - 1 - j) % kWindow] == m_window[(m_next + kWindow - 1 - i) % kWindow] ? 1 : 0;
				best = std::max(best, same);
			}
			return best;
		}

		// Once a second: the speed against the floor while frames are generated, and the pauses.
		void Second(double now, double speed)
		{
			if (!m_second_started)
			{
				m_second_started = true;
				m_second = now;
				return;
			}
			if (now - m_second < 1.0)
				return;
			m_second = now;
			const bool generated = m_generated_this_second;
			m_generated_this_second = false;

			if (m_movie) // compilation 66
			{
				if (Paused())
				{
					m_paused_until = 0.0;
					m_nofg_sum = 0.0;
					m_nofg_n = 0;
					m_reset = true;
				}
				m_slow = 0;
				m_slow_sum = 0.0;
				return;
			}

			if (Paused())
			{
				if (now >= m_pause_start + kSettle)
				{
					m_nofg_sum += speed;
					m_nofg_n++;
				}
				if (now < m_paused_until)
					return;
				m_paused_until = 0.0;
				m_last_pause_end = now;
				m_slow = 0;
				m_slow_sum = 0.0;
				m_nofg_speed = m_nofg_n > 0 ? m_nofg_sum / m_nofg_n : speed;
				m_nofg_sum = 0.0;
				m_nofg_n = 0;
				if (m_nofg_speed >= m_fg_speed + kCostly && ++m_costly_pauses >= kGiveUpPauses) // Forja 1.06 (NewGame)
				{
					m_gave_up = true;
					m_event = Event::GaveUp;
					m_reset = true;
					return;
				}
				if (m_nofg_speed >= kFullSpeed && m_nofg_speed >= m_fg_speed + kCostly)
				{
					m_next_pause = std::min(m_next_pause * 2.0, kMaxPause);
					m_event = Event::ResumedCostly;
				}
				else
				{
					m_floor = std::min(m_nofg_speed - kCostly, kSlowSpeed);
					m_floor_until = now + kFloorSeconds;
					m_next_pause = kFirstPause;
					m_event = Event::ResumedSlowGame;
				}
				m_reset = true;
				return;
			}

			if (m_last_pause_end > 0.0 && now - m_last_pause_end >= kPauseForget)
				m_next_pause = kFirstPause;
			if (generated && speed < Floor(now))
			{
				m_slow++;
				m_slow_sum += speed;
			}
			else
			{
				m_slow = 0;
				m_slow_sum = 0.0;
			}
			if (m_slow >= kSlowSeconds)
			{
				m_fg_speed = m_slow_sum / m_slow;
				m_pause_floor = Floor(now);
				m_pause_length = m_next_pause;
				m_pause_start = now;
				m_paused_until = now + m_pause_length;
				m_slow = 0;
				m_slow_sum = 0.0;
				m_event = Event::Paused;
			}
		}

		uint32_t m_window[kWindow] = {};
		uint32_t m_next = 0, m_count = 0;
		double m_median = 0.0;
		uint32_t m_steady = 0; // vk-285-135
		bool m_fluid = false;  // Forja SX2 (compilation 40)
		bool m_movie = false;  // Forja SX2 (compilation 66)
		bool m_held = false;   // Forja 1.06
		uint32_t m_costly_pauses = 0; // Forja 1.06: pauses in this game after which it ran faster without them (NewGame)
		bool m_gave_up = false;       // Forja 1.06
		double m_credit = 0.0; // the fluid mode's refreshes not yet presented
		double m_refreshes = 0.0;
		uint32_t m_repeats = 1;
		bool m_room = false;
		bool m_reset = true;
		bool m_generated_this_second = false;
		State m_state = State::Warming;
		Event m_event = Event::None;

		bool m_second_started = false;
		double m_second = 0.0;
		int m_slow = 0;
		double m_slow_sum = 0.0;
		double m_fg_speed = 0.0;
		double m_pause_floor = kSlowSpeed;
		double m_pause_start = 0.0;
		double m_paused_until = 0.0;
		double m_last_pause_end = 0.0;
		double m_pause_length = kFirstPause;
		double m_next_pause = kFirstPause;
		double m_nofg_sum = 0.0;
		int m_nofg_n = 0;
		double m_nofg_speed = 0.0;
		double m_floor = kSlowSpeed;
		double m_floor_until = 0.0;
	};
} // namespace orbis_fg
