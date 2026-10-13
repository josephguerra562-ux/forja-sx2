// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Small controller-driven launcher for the PS4 VM frontend.
#pragma once
#include "GamePlatform.h"
#include <string>

namespace PS4GameMenu
{
	// CPU thread, after CPUThreadInitialize. Empty path means BIOS; false means exit/error.
	bool Select(int& pad, std::string& path, GamePlatform& platform);
	// GS thread only, called from RenderOSD for the launcher or pause panel.
	bool Render();
	// CPU thread: update the full-screen boot artwork and its real startup stage.
	void ShowLoading(unsigned progress, const char* status);
	void HideLoading();
	// CPU thread: publish the pause panel and request a presentation while paused.
	void SetInGameMenu(bool visible, unsigned selected, unsigned upscale = 1, unsigned max_upscale = 1,
		unsigned aspect = 1, bool show_performance_overlay = true);
} // namespace PS4GameMenu
