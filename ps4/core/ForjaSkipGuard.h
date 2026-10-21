// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja 1.06: the guard of "Saltar cuadros" (ps4/core/GSRendererPS4.cpp, VSync). Header-only, so a PC test runs it.
//
// Saltar cuadros leaves a frame of the game out and hides from the TV the framebuffer that holds it, so the TV repeats the last
// whole picture. That works where the game draws its frame into the framebuffer the TV shows (Dragon Ball Z Budokai Tenkaichi
// 4's fights, 2026-10-21: every frame left out hidden for 2 vsyncs, 150 of 300 pictures a 5 s, no flicker). Where a game builds
// its picture another way (the same game's victory screen, its pause menu, the start of a fight after a restart), frames were
// left out and none of their pictures hidden (0 of 300), and the TV showed them half drawn: the flicker. Whatever the game
// does, a frame left out whose picture is never hidden is one that may show, so:
// - each frame left out is checked two frames later (a game at 60 fps hides its picture at the next frame's start): if no
//   picture was hidden since it began, frames stop being left out for a while: 2 s the first time (the start of a fight
//   settles in that time) and 30 s from then on, so a screen that keeps showing them (a victory screen of 10-20 s) blinks
//   twice at most; back to 2 s after 30 s of frames all hidden;
// - after a load, a light frame (a menu, a message) or a pause, frames are left out only after kWarmupFrames heavy ones in a
//   row (about a second): the start of a fight and the screens that change are drawn whole.
// The fights, where every frame left out is hidden, skip as before.
#pragma once

#include <algorithm>
#include <cstdint>

namespace forja_skip
{
	class Guard
	{
	public:
		static constexpr uint32_t kHeavyDraws = 400;      // draws into la pantalla that make a frame heavy (FORJA_SKIP_MIN_PREVIOUS)
		static constexpr uint32_t kWarmupFrames = 30;     // heavy frames in a row before one is left out
		static constexpr uint32_t kFirstPause = 120;      // vsyncs (2 s at 60 Hz)
		static constexpr uint32_t kMaxPause = 1800;       // 30 s
		static constexpr uint32_t kForgiveVsyncs = 1800;  // 30 s of frames all hidden: the next pause is kFirstPause again
		static constexpr uint32_t kCheckFrames = 2;       // a frame left out is checked this many frame starts later

		// A new frame of the game starts (VSync, the privileged registers written). previous_draws: the draws into la pantalla
		// of the frame that ended; hidden_total: the pictures hidden so far (it only grows); vsync: the vsync count now. True if
		// a pause began now (a frame left out showed on the TV).
		bool FrameStart(uint32_t previous_draws, uint32_t hidden_total, uint32_t vsync)
		{
			m_heavy = previous_draws >= kHeavyDraws ? std::min(m_heavy + 1, kWarmupFrames) : 0;
			if (m_check_frames == 0 || --m_check_frames != 0)
				return false;
			if (hidden_total != m_check_mark)
			{
				if (m_masked_since == 0)
					m_masked_since = vsync | 1u;
				else if (vsync - m_masked_since >= kForgiveVsyncs)
					m_pause = kFirstPause;
				return false;
			}
			m_unmasked++;
			m_last_pause = m_pause;
			m_paused_until = vsync + m_pause;
			m_pause = kMaxPause;
			m_masked_since = 0;
			m_heavy = 0; // and the warm-up again after it
			return true;
		}

		// Whether a frame may be left out now: warmed up and not in a pause.
		bool MayChoose(uint32_t vsync) const { return m_heavy >= kWarmupFrames && !Paused(vsync); }

		// A frame was chosen to be left out: its check starts.
		void Chosen(uint32_t hidden_total)
		{
			m_check_mark = hidden_total;
			m_check_frames = kCheckFrames;
		}

		// No new frame for a while (a load, a pause), or Saltar cuadros off: the warm-up starts over. The pauses' length stays.
		void Reset()
		{
			m_heavy = 0;
			m_check_frames = 0;
		}

		bool Paused(uint32_t vsync) const { return static_cast<int32_t>(vsync - m_paused_until) < 0; }
		uint32_t LastPause() const { return m_last_pause; }   // in vsyncs
		uint32_t Unmasked() const { return m_unmasked; }      // frames left out that showed (it only grows)

	private:
		uint32_t m_heavy = 0;
		uint32_t m_check_frames = 0;
		uint32_t m_check_mark = 0;
		uint32_t m_paused_until = 0;
		uint32_t m_pause = kFirstPause;
		uint32_t m_last_pause = 0;
		uint32_t m_masked_since = 0; // the vsync the frames left out began to be all hidden (0: not now)
		uint32_t m_unmasked = 0;
	};
} // namespace forja_skip
