// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Small controller-driven launcher for the PS4 VM frontend.
#pragma once
#include "GamePlatform.h"
#include "InGameMenuState.h"
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
	// CPU thread: publish the pause panel and request a presentation while paused (a state with visible = false hides it).
	void SetInGameMenu(const InGameMenuState& state);
	// CPU thread (compilation 42): the Parches tab's row, which the u64 state can't carry: "Parche 1 de 3:  < name >  Sí" and its
	// description.
	void SetPatchMenuText(const std::string& label, const std::string& description);
	// CPU thread (compilation 44): any row's own text, for the rows whose values are names (Extras, Cambiar disco).
	void SetRowText(unsigned row, const std::string& label, const std::string& description);
} // namespace PS4GameMenu
