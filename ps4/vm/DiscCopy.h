// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 41): a PS2 DVD in the PS4's drive copied to /data/ForjaSX2/roms as an .iso. See
// DiscCopy.cpp.
#pragma once

#include "PhysicalDisc.h"

#include <memory>
#include <string>

namespace PS4DiscCopy
{
	enum class State
	{
		Idle,
		Copying,
		Done,
		Failed,
		Cancelled,
	};

	// Copies the disc `reader` was opened on, in a thread of its own, to roms/<title> (<serial>).iso. False, with `error`,
	// when a copy is running already or the thread can't start. A disc copied before (same file at full size) is done at once.
	bool Start(std::unique_ptr<PS4Disc::Reader> reader, const std::string& title, std::string& error);
	bool Busy();
	// The running copy stops at its next read and its .part file goes.
	void Cancel();
	State GetState();
	// True once after each copy ends (done, failed or cancelled): the library scans roms again.
	bool TakeFinished();
	// The finished copy's path; empty unless the last copy is done.
	std::string ResultPath();
	// "Copiando God of War II: 35 % · 4.4 MB/s · faltan 9 min", or how the last copy ended.
	std::string StatusText();
} // namespace PS4DiscCopy
