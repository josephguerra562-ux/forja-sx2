// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Non-blocking GitHub release notification for the PS4 launcher.
#pragma once

namespace PS4Release
{
	void Start();
	bool HasUpdate();
	const char* LatestVersion();
}
