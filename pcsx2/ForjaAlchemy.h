// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 47): more frames for the games on the Alchemy engine (Intrinsic Graphics' "Insight",
// later Vicarious Visions'), which Activision used for many of its PS2 games. Crash Nitro Kart showed how (2026-10-18): the
// engine keeps its picture settings as text inside the executable ("[INSIGHT]", "allocateVisualContextInScratchPad =
// true", "colorBits = 24"...) and reads them by name when it starts, swapInterval (TV refreshes per frame) and maxFrameRate
// among them. Writing "swapInterval = 1" and "maxFrameRate = 50.00000" over the first line, which has the same length,
// took its races from 25 to 50 frames a second (with the PS2 CPU at 36 %).
// When a game starts, OnGameStart looks for that text in the EE's memory and, if the game has no frame rate patch yet,
// writes patches/<SERIAL>_<CRC>_alchemy.pnach with the same change: off until the player turns it on in L3+R3 > Extras,
// and it applies from the game's next start (patch=0, once at load), like Crash's.
#pragma once

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "Patch.h"
#include "VMManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "fmt/format.h"

#include <cstring>
#include <string>
#include <string_view>

namespace ForjaAlchemy
{
	inline void OnGameStart()
	{
		const std::string serial = VMManager::GetDiscSerial();
		const u32 crc = VMManager::GetDiscCRC();
		if (serial.empty() || crc == 0 || !eeMem)
			return;
		const std::string_view ram(reinterpret_cast<const char*>(eeMem->Main), Ps2MemSize::MainRam);
		const size_t at = ram.find("[INSIGHT]\n");
		if (at == std::string_view::npos)
			return;
		const size_t end = ram.find('\0', at);
		if (end == std::string_view::npos || end - at > 4096)
			return;
		const std::string_view block = ram.substr(at, end - at);
		const bool pal = block.find("palVideoMode = true") != std::string_view::npos;
		Console.WriteLn("Forja SX2: Alchemy engine settings at 0x%08X (%s)", static_cast<unsigned>(at), pal ? "PAL" : "NTSC");
		if (block.find("swapInterval") != std::string_view::npos)
		{
			Console.WriteLn("Forja SX2: Alchemy: this game sets its own swapInterval; nothing to do");
			return;
		}
		constexpr std::string_view line = "allocateVisualContextInScratchPad = true\n";
		const size_t line_at = block.find(line);
		if (line_at == std::string_view::npos)
		{
			Console.WriteLn("Forja SX2: Alchemy: no line of the same length to write over; nothing to do");
			return;
		}
		for (const Patch::PatchInfo& info : Patch::GetPatchInfo(serial, crc, false, false, nullptr))
		{
			if (info.name.find("FPS") != std::string::npos)
			{
				Console.WriteLn("Forja SX2: Alchemy: the game already has a frame rate patch (\"%s\")", info.name.c_str());
				return;
			}
		}
		const int rate = pal ? 50 : 60;
		const std::string replacement = fmt::format("swapInterval = 1\nmaxFrameRate = {}.00000\n", rate);
		if (replacement.size() != line.size())
			return;

		// Whole words, as the pnach writes them: the bytes around the line keep their value.
		const u32 start = static_cast<u32>(at + line_at);
		const u32 first = start & ~3u;
		const u32 last = (start + static_cast<u32>(line.size()) + 3u) & ~3u;
		std::string words(ram.substr(first, last - first));
		words.replace(start - first, line.size(), replacement);
		std::string text = fmt::format(
			"gametitle={} {:08X}\n\n"
			"// Written by Forja SX2 when the game started: it runs on the Alchemy engine, whose settings text (0x{:08X}) does\n"
			"// not set swapInterval. This writes \"swapInterval = 1\" and \"maxFrameRate = {}\" over its first line, of the same\n"
			"// length, before the game reads it (as the \"50 FPS\" patch of Crash Nitro Kart, compilation 46).\n"
			"[{} FPS (motor Alchemy, experimental)]\n"
			"author=Forja SX2 (automático)\n"
			"description=Pide al motor un cuadro por refresco (hasta {} por segundo). Si el juego va acelerado, apágalo. Vale desde Partida > Reiniciar juego.\n",
			serial, crc, static_cast<unsigned>(at), rate, rate, rate);
		for (u32 address = first; address < last; address += 4)
		{
			u32 value, original;
			std::memcpy(&value, words.data() + (address - first), 4);
			std::memcpy(&original, ram.data() + address, 4);
			if (value != original)
				text += fmt::format("patch=0,EE,{:08X},word,{:08X}\n", address, value);
		}

		const std::string path = Path::Combine(EmuFolders::Patches, fmt::format("{}_{:08X}_alchemy.pnach", serial, crc));
		if (const std::optional<std::string> existing = FileSystem::ReadFileToString(path.c_str()); existing.has_value() && *existing == text)
			return;
		if (!FileSystem::WriteStringToFile(path.c_str(), text))
		{
			Console.Warning("Forja SX2: Alchemy: could not write %s", path.c_str());
			return;
		}
		Console.WriteLn("Forja SX2: Alchemy: wrote %s (%d FPS patch; turn it on in L3+R3 > Extras)", path.c_str(), rate);
		VMManager::ReloadPatches(true, false, false, false);
	}
} // namespace ForjaAlchemy
