// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Non-blocking GitHub release notification for the PS4 launcher.
// Forja SX2: and the updater, as DolphinPS4 updates itself: the newest build's package is downloaded into /data/pkg and
// checked against the release's SHA256SUMS.txt; the player installs it with GoldHEN's Package Installer, over the app.
#pragma once

#include <string>

namespace PS4Release
{
	// current_build: this build's number (FORJA_BUILD_NUMBER, the GitHub Actions run). 0 = a local build: never "newer".
	void Start(int current_build);
	bool HasUpdate();
	int LatestBuild();

	enum class DownloadState
	{
		Idle,
		Downloading,
		Ready,
		Failed,
	};
	// Starts downloading the newest build's package (once at a time). No effect without an update.
	void StartDownload();
	DownloadState State();
	// The library's notice line: "Compilación 32 disponible...", the download's progress, or how it ended.
	std::string StatusText();
}
