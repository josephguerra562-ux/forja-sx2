// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja 1.06: the guard of "Saltar cuadros" (ps4/core/GSRendererPS4.cpp, VSync). Header-only, so a PC test runs it.
//
// Saltar cuadros leaves a frame of the game out and hides from the TV the framebuffer that holds it, so the TV repeats the last
// whole picture. That works where the game draws its frame into a framebuffer the TV then shows (Dragon Ball Z Budokai
// Tenkaichi 4's fights, compilation 71: 75 frames left out and 150 pictures hidden every 5 s, no flicker). Where it builds its
// picture another way (its victory screen, its pause menu, the start of a fight), frames were left out and none or few of
// their pictures hidden (0 to 88 of 300), and the TV showed them half drawn: the flicker. So the guard counts the pictures
// hidden for the frames left out, and pauses Saltar cuadros where they aren't:
// - frames left out are judged by groups of kGroup, kLagFrames frames after the group's last one (Dragon Ball shows a frame's
//   picture a frame after drawing it, so it is hidden then); a group whose pictures were hidden fewer than kMinHidden times
//   pauses it, for 2 s, then 8 s, then 30 s, back to 2 s after 30 s of groups all hidden. Compilation 72 judged each frame
//   alone, two frames after it: in the fights its picture was hidden later, or the first one after a wait not at all, and each
//   time Saltar cuadros went quiet for 30 s: "no noté ninguna diferencia" (2026-10-21);
// - after a load, a light frame (a menu, a message) or a pause, frames are left out only after kWarmupFrames heavy ones in a
//   row (about a second): the start of a fight and the screens that change are drawn whole.
#pragma once

#include <algorithm>
#include <cstdint>

namespace forja_skip
{
	class Guard
	{
	public:
		static constexpr uint32_t kHeavyDraws = 400;     // draws into la pantalla that make a frame heavy (FORJA_SKIP_MIN_PREVIOUS)
		static constexpr uint32_t kWarmupFrames = 30;    // heavy frames in a row before one is left out
		static constexpr uint32_t kGroup = 3;            // frames left out judged together
		static constexpr uint32_t kLagFrames = 3;        // frames after the group's last one before it is judged
		static constexpr uint32_t kMinHidden = 2;        // pictures hidden a group needs (the fights: ~2 a frame left out)
		static constexpr uint32_t kPauses[3] = {120, 480, 1800}; // vsyncs: 2 s, 8 s, 30 s at 60 Hz
		static constexpr uint32_t kForgiveVsyncs = 1800; // 30 s of groups all hidden: the next pause is the first again

		// A new frame of the game starts (VSync, the privileged registers written). previous_draws: the draws into la pantalla
		// of the frame that ended; hidden_total: the pictures hidden so far (it only grows); vsync: the vsync count now. True if
		// a pause began now.
		bool FrameStart(uint32_t previous_draws, uint32_t hidden_total, uint32_t vsync)
		{
			m_frame++;
			m_heavy = previous_draws >= kHeavyDraws ? std::min(m_heavy + 1, kWarmupFrames) : 0;
			if (m_group_count < kGroup || m_frame - m_group_last_frame < kLagFrames)
				return false;
			m_last_hidden = hidden_total - m_group_mark;
			m_group_count = 0;
			if (m_last_hidden >= kMinHidden)
			{
				if (m_masked_since == 0)
					m_masked_since = vsync | 1u;
				else if (vsync - m_masked_since >= kForgiveVsyncs)
					m_strike = 0;
				return false;
			}
			m_unmasked++;
			m_last_pause = kPauses[std::min<uint32_t>(m_strike, 2)];
			m_strike = std::min<uint32_t>(m_strike + 1, 2);
			m_paused_until = vsync + m_last_pause;
			m_masked_since = 0;
			m_heavy = 0; // and the warm-up again after it
			return true;
		}

		// Whether a frame may be left out now: warmed up and not in a pause.
		bool MayChoose(uint32_t vsync) const { return m_heavy >= kWarmupFrames && !Paused(vsync); }

		// A frame was chosen to be left out. It joins the group being judged unless that one is full (it waits its lag; the
		// pictures hidden meanwhile still count for it).
		void Chosen(uint32_t hidden_total)
		{
			if (m_group_count >= kGroup)
				return;
			if (m_group_count == 0)
				m_group_mark = hidden_total;
			m_group_count++;
			m_group_last_frame = m_frame;
		}

		// No new frame for a while (a load, a pause), or Saltar cuadros off: the warm-up starts over, and the group being
		// judged is dropped. The pauses' length stays.
		void Reset()
		{
			m_heavy = 0;
			m_group_count = 0;
		}

		bool Paused(uint32_t vsync) const { return static_cast<int32_t>(vsync - m_paused_until) < 0; }
		uint32_t LastPause() const { return m_last_pause; }   // in vsyncs
		uint32_t LastHidden() const { return m_last_hidden; } // pictures hidden in the last group judged
		uint32_t Unmasked() const { return m_unmasked; }      // groups that paused it (it only grows)

	private:
		uint32_t m_frame = 0;
		uint32_t m_heavy = 0;
		uint32_t m_group_count = 0;
		uint32_t m_group_mark = 0;
		uint32_t m_group_last_frame = 0;
		uint32_t m_last_hidden = 0;
		uint32_t m_paused_until = 0;
		uint32_t m_strike = 0;
		uint32_t m_last_pause = 0;
		uint32_t m_masked_since = 0; // the vsync since which every group was hidden (0: not now)
		uint32_t m_unmasked = 0;
	};
} // namespace forja_skip
