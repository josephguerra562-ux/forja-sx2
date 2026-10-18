// SPDX-License-Identifier: GPL-3.0+
// Forja SX2: per-game profiles, in layers like Dolphin's game settings:
//   1. the global settings: ajustes.ini (the launcher's Settings page) and nucleos.txt;
//   2. the factory profile in the package, resources/perfiles/<SERIAL>.ini: what was measured on the console for that game;
//   3. the player's profile, PCSX2's own gamesettings/<SERIAL>_<CRC>.ini (written by the L3+R3 menu), which always wins.
// Both files use PCSX2's sections ([EmuCore/Speedhacks], [EmuCore/GS], ...) plus [Forja] for this port's own settings. What a
// profile sets explicitly also wins over the GameDB, key by key (VMManager::ApplyGameFixes), so a profile can drop one GameDB
// fix and keep the rest, which EnableGameFixes = false could not.
#pragma once

#include "Config.h"
#include "INISettingsInterface.h"
#include "VMManager.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "fmt/format.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace ForjaGameProfile
{
	// Where a value comes from, shown by the L3+R3 menu.
	enum class Origin : unsigned
	{
		Global = 0,
		Factory = 1,
		Player = 2,
	};

	struct Values
	{
		std::string serial;
		u32 crc = 0;
		bool has_factory = false;
		bool has_player = false;
		// [Forja] VideoMode: -1 not set (ajustes.ini decides), 0 off, 1 on.
		int video_mode = -1;
		Origin video_mode_origin = Origin::Global;
		// [Forja] VideoEECycleRate: the EE rate while a movie plays, -3..0; 1 = not set (-3).
		int video_ee_rate = 1;
		Origin video_ee_rate_origin = Origin::Global;
		// [Forja] FrameGen: -1 not set (ajustes.ini decides), 0 off, 1 on, 2 fluid (compilation 40: every refresh the game's
		// frames leave gets a generated one; OrbisFrameGenPacing.h).
		int frame_gen = -1;
		Origin frame_gen_origin = Origin::Global;
		// [Forja] Nucleos: nucleos.txt's syntax on one line ("EE=1 VU1=2 GS=3 AUDIO=5 AYUDANTES=4"); empty or "global" =
		// nucleos.txt (or the built-in cores).
		std::string cores;
		Origin cores_origin = Origin::Global;
		// [Forja] CpuAuto (compilation 35): the automatic PS2 CPU speed's floor, -6..-1 (27 % to 75 %); 0 off; 1 = not set
		// (off). The emulator lowers the EE rate towards it while the game cannot keep full speed (main.cpp, UpdateCpuAuto).
		int cpu_auto = 1;
		Origin cpu_auto_origin = Origin::Global;
		// [Forja] VU1Quitar (compilation 44): the VU1 programs the filter leaves out, their start addresses in hex ("0d30,0f48").
		std::string vu1_skip;
		Origin vu1_skip_origin = Origin::Global;
	};

	// Forja SX2 (compilation 44): the settings center's copy (/data/ForjaSX2/centro/perfiles, fetched from the project's GitHub by
	// ps4/vm/PatchDownload.cpp) wins over the package's, so a game's tested settings reach every player without a new package.
	inline std::string FactoryPath(std::string_view serial)
	{
		const std::string name = fmt::format("{}.ini", Path::SanitizeFileName(serial));
		const std::string center = Path::Combine(EmuFolders::DataRoot, "centro/perfiles/" + name);
		if (FileSystem::FileExists(center.c_str()))
			return center;
		return Path::Combine(EmuFolders::Resources, "perfiles/" + name);
	}

	inline std::string PlayerPath(std::string_view serial, u32 crc)
	{
		return (serial.empty() || crc == 0) ? std::string() : VMManager::GetGameSettingsPath(serial, crc);
	}

	inline std::string_view Trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
			text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
			text.remove_suffix(1);
		return text;
	}

	// Copies every key of the game's factory profile that `si` does not set yet; returns false when there is none. It has a
	// reader of its own (sections, "key = value", whole-line ';' or '#' comments), and nothing ever writes the file back.
	// An INISettingsInterface saves itself when destroyed dirty: callers merging into one call ClearDirty() afterwards.
	inline bool MergeFactory(SettingsInterface& si, std::string_view serial)
	{
		if (serial.empty())
			return false;
		std::FILE* file = std::fopen(FactoryPath(serial).c_str(), "r");
		if (!file)
			return false;
		char line[512];
		std::string section;
		while (std::fgets(line, sizeof(line), file))
		{
			const std::string_view text = Trim(line);
			if (text.empty() || text.front() == ';' || text.front() == '#')
				continue;
			if (text.front() == '[')
			{
				const size_t end = text.find(']');
				if (end != std::string_view::npos)
					section = std::string(Trim(text.substr(1, end - 1)));
				continue;
			}
			const size_t equals = text.find('=');
			if (equals == std::string_view::npos || section.empty())
				continue;
			const std::string key(Trim(text.substr(0, equals)));
			const std::string value(Trim(text.substr(equals + 1)));
			if (!key.empty() && !si.ContainsValue(section.c_str(), key.c_str()))
				si.SetStringValue(section.c_str(), key.c_str(), value.c_str());
		}
		std::fclose(file);
		return true;
	}

	// The game's [Forja] values, each with the layer it came from.
	inline Values Load(std::string_view serial, u32 crc)
	{
		Values v;
		v.serial = serial;
		v.crc = crc;
		const std::string player_path = PlayerPath(serial, crc);
		INISettingsInterface player(player_path);
		v.has_player = !player_path.empty() && FileSystem::FileExists(player_path.c_str()) && player.Load();
		MemorySettingsInterface factory;
		v.has_factory = MergeFactory(factory, serial);

		const auto read_int = [&](const char* key, int* value, Origin* origin) {
			if (v.has_player && player.GetIntValue("Forja", key, value))
				*origin = Origin::Player;
			else if (v.has_factory && factory.GetIntValue("Forja", key, value))
				*origin = Origin::Factory;
		};
		read_int("VideoMode", &v.video_mode, &v.video_mode_origin);
		read_int("VideoEECycleRate", &v.video_ee_rate, &v.video_ee_rate_origin);
		read_int("FrameGen", &v.frame_gen, &v.frame_gen_origin);
		read_int("CpuAuto", &v.cpu_auto, &v.cpu_auto_origin);
		if (v.has_player && player.GetStringValue("Forja", "Nucleos", &v.cores))
			v.cores_origin = Origin::Player;
		else if (v.has_factory && factory.GetStringValue("Forja", "Nucleos", &v.cores))
			v.cores_origin = Origin::Factory;
		if (v.has_player && player.GetStringValue("Forja", "VU1Quitar", &v.vu1_skip))
			v.vu1_skip_origin = Origin::Player;
		else if (v.has_factory && factory.GetStringValue("Forja", "VU1Quitar", &v.vu1_skip))
			v.vu1_skip_origin = Origin::Factory;

		if (v.video_mode >= 0)
			v.video_mode = v.video_mode != 0 ? 1 : 0;
		if (v.video_ee_rate <= 0)
			v.video_ee_rate = std::clamp(v.video_ee_rate, -3, 0);
		if (v.frame_gen >= 0)
			v.frame_gen = std::min(v.frame_gen, 2);
		if (v.cpu_auto <= 0)
			v.cpu_auto = std::clamp(v.cpu_auto, -6, 0);
		else
			v.cpu_auto = 0;
		return v;
	}

	// Edits the player's profile of a game and saves it (PCSX2 creates no file until something is set).
	template <typename Edit>
	inline bool EditPlayer(std::string_view serial, u32 crc, Edit&& edit)
	{
		const std::string path = PlayerPath(serial, crc);
		if (path.empty())
			return false;
		INISettingsInterface ini(path);
		ini.Load();
		edit(ini);
		return ini.Save();
	}

	// "Restablecer este juego": the player's profile becomes <file>.bak (the last one only), so a mistake can be undone by hand.
	inline bool ResetPlayer(std::string_view serial, u32 crc)
	{
		const std::string path = PlayerPath(serial, crc);
		if (path.empty() || !FileSystem::FileExists(path.c_str()))
			return false;
		return std::rename(path.c_str(), (path + ".bak").c_str()) == 0;
	}
} // namespace ForjaGameProfile
