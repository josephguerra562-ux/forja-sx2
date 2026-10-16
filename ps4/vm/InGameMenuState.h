// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Atomic CPU-to-GS menu snapshot, independent of renderer headers.
#pragma once

#include <algorithm>
#include <cstdint>

namespace PS4GameMenu
{
	// Forja SX2: rows of the L3+R3 menu.
	enum InGameMenuRow : unsigned
	{
		ROW_RESUME = 0,
		ROW_SAVE_STATE, // compilation 31: save states and restart
		ROW_LOAD_STATE,
		ROW_RESTART,
		ROW_RESOLUTION,
		ROW_ASPECT,
		ROW_OVERLAY,
		ROW_EE_RATE,
		ROW_VIDEO_MODE,
		ROW_FRAME_GEN,
		ROW_CORES,
		ROW_RESET_PROFILE,
		ROW_LIBRARY,
		ROW_EXIT,
		ROW_COUNT
	};

	struct InGameMenuState
	{
		bool visible = false;
		unsigned selected = 0;
		unsigned upscale = 1;
		unsigned max_upscale = 1;
		unsigned aspect = 1;
		bool show_overlay = true;
		int ee_rate = -1; // Forja SX2: EECycleRate, -3..3
		// Forja SX2: the game's profile. video: 0 off, 1 100 %, 2 75 %, 3 60 %, 4 50 % (EE rate during movies).
		unsigned video = 4;
		bool frame_gen = false;
		// cores: 0 global (nucleos.txt), 1 normal, 2 plan B, 3 another plan written by hand.
		unsigned cores = 0;
		// Where each one comes from: 0 global, 1 factory profile, 2 the player's profile (ForjaGameProfile::Origin).
		unsigned video_origin = 0;
		unsigned frame_gen_origin = 0;
		unsigned cores_origin = 0;
		bool has_factory = false;
		bool has_player = false;
		// Forja SX2: save states. slot 0-4 (shown as 1-5), whether it holds a state, and how the last action went.
		unsigned slot = 0;
		bool slot_used = false;
		unsigned action = 0; // StateAction
	};

	// What the last save state action did, for the rows' descriptions.
	enum StateAction : unsigned
	{
		STATE_NONE = 0,
		STATE_SAVED,
		STATE_SAVE_FAILED,
		STATE_LOAD_FAILED,
		STATE_EMPTY,
	};

	constexpr std::uint64_t PackInGameMenuState(InGameMenuState value)
	{
		if (!value.visible)
			return 0;
		const unsigned limit = std::clamp(value.max_upscale, 1u, 4u);
		return static_cast<std::uint64_t>((std::min(value.selected, ROW_COUNT - 1u) + 1u) |
				(std::clamp(value.upscale, 1u, limit) << 8) |
				(limit << 12) |
				(std::min(value.aspect, 4u) << 16) |
				(value.show_overlay ? (1u << 20) : 0u) |
				(static_cast<unsigned>(std::clamp(value.ee_rate, -3, 3) + 3) << 21) |
				(std::min(value.video, 4u) << 24) |
				(std::min(value.video_origin, 2u) << 27) |
				(value.frame_gen ? (1u << 29) : 0u) |
				(std::min(value.frame_gen_origin, 2u) << 30)) |
			(static_cast<std::uint64_t>(std::min(value.cores, 3u)) << 32) |
			(static_cast<std::uint64_t>(std::min(value.cores_origin, 2u)) << 34) |
			(value.has_factory ? (std::uint64_t{1} << 36) : 0u) |
			(value.has_player ? (std::uint64_t{1} << 37) : 0u) |
			(static_cast<std::uint64_t>(std::min(value.slot, 4u)) << 38) |
			(value.slot_used ? (std::uint64_t{1} << 41) : 0u) |
			(static_cast<std::uint64_t>(std::min(value.action, 7u)) << 42);
	}

	constexpr InGameMenuState UnpackInGameMenuState(std::uint64_t value)
	{
		// These fields occupy FOUR bits, not eight. Reading 0xff here mixed the
		// resolution with its limit (1 became 65), and aspect with overlay (0 became 16).
		const unsigned low = static_cast<unsigned>(value);
		const unsigned selection = low & 0xffu;
		const unsigned limit = std::clamp((low >> 12) & 0x0fu, 1u, 4u);
		InGameMenuState state;
		state.visible = selection != 0;
		state.selected = selection ? std::min(selection - 1u, ROW_COUNT - 1u) : 0u;
		state.upscale = std::clamp((low >> 8) & 0x0fu, 1u, limit);
		state.max_upscale = limit;
		state.aspect = std::min((low >> 16) & 0x0fu, 4u);
		state.show_overlay = (low & (1u << 20)) != 0;
		state.ee_rate = static_cast<int>(std::min((low >> 21) & 0x07u, 6u)) - 3;
		state.video = std::min((low >> 24) & 0x07u, 4u);
		state.video_origin = std::min((low >> 27) & 0x03u, 2u);
		state.frame_gen = (low & (1u << 29)) != 0;
		state.frame_gen_origin = std::min((low >> 30) & 0x03u, 2u);
		state.cores = static_cast<unsigned>((value >> 32) & 0x03u);
		state.cores_origin = std::min(static_cast<unsigned>((value >> 34) & 0x03u), 2u);
		state.has_factory = (value & (std::uint64_t{1} << 36)) != 0;
		state.has_player = (value & (std::uint64_t{1} << 37)) != 0;
		state.slot = std::min(static_cast<unsigned>((value >> 38) & 0x07u), 4u);
		state.slot_used = (value & (std::uint64_t{1} << 41)) != 0;
		state.action = static_cast<unsigned>((value >> 42) & 0x07u);
		return state;
	}
} // namespace PS4GameMenu
