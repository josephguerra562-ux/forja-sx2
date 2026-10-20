// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2: the PCSX2 community's patches (compilation 42) and the settings center's profile and patches
// (compilation 44) for the game that starts. See PatchDownload.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace ForjaPatches
{
	// CPU thread, once a game's serial and CRC are known: fetches its patch file in the background, unless it was fetched in
	// the last 7 days, this launch already asked for it, or /data/ForjaSX2/vm-sin-parches-comunidad exists.
	// force (compilation 44, L3+R3 > Extras > Ajustes recomendados): fetch now, however recent the files are.
	void FetchCommunity(const std::string& serial, uint32_t crc, bool force = false);
	// True once after a new file arrived: the CPU thread reloads the patches.
	bool TakeArrived();
	// Compilation 61: true while a download runs (main.cpp waits for it before using the recommended settings).
	bool Busy();
} // namespace ForjaPatches
