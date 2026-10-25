// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 77): "Reportar este juego" (L3+R3 > Sistema). The player's report, a text file with the
// session's summary, the game's settings and the last of the log, is kept in /data/ForjaSX2/reportes/ and sent to the
// reports channel of Forja's Discord in the background. The channel's address is not in the source: the build takes it from
// the repository's FORJA_REPORT_URL secret (ps4/vm/CMakeLists.txt); a build without it only keeps the reports.
#pragma once

#include <string>

namespace ForjaReport
{
	enum class Status
	{
		Idle,    // nothing sent in this launch
		Sending, // the thread is sending it
		Sent,    // the channel took it
		Kept,    // kept in reportes/ only: no channel in this build, no network, or the channel refused it
	};

	// True when this build has a reports channel.
	bool HasChannel();
	// CPU thread: keeps `text` as /data/ForjaSX2/reportes/<name>.txt and sends it with `summary` as the message's line. False
	// when a report is still being sent.
	bool Send(const std::string& name, const std::string& summary, const std::string& text);
	Status GetStatus();
	// The name of the last report, for the menu.
	std::string LastName();
	// Compilation 86: seconds since the report being sent started (0 when none is), for the menu's "Mandando... 12 s".
	int SendingSeconds();
	// Compilation 86: a report that didn't reach the channel (no network, the channel refused it, or the game closed while it
	// was being sent) keeps a <name>.pendiente beside its .txt; this sends the oldest one again in the background (CPU thread,
	// at a game's start). Nothing when none is pending, one is being sent, or the build has no channel.
	void ResendPending();
} // namespace ForjaReport
