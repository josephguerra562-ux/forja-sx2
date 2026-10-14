// SPDX-License-Identifier: GPL-3.0+
// Forja SX2: the launcher's Settings page, saved as /data/ForjaSX2/ajustes.ini ("key=value" lines).
// These are the defaults for every game. A game's own gamesettings/<serial>_<crc>.ini (written by the
// pause menu's CPU speed row) still wins for that game, because it is PCSX2's game settings layer.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ForjaSettings
{
	constexpr const char* Path = "/data/ForjaSX2/ajustes.ini";
	// Analog sensitivity steps, in percent (133 is PCSX2's default for DualShock 2 pads).
	constexpr int StickSteps[] = {100, 110, 120, 133, 140, 150, 160, 170, 180, 190, 200};
	constexpr int StickStepCount = static_cast<int>(sizeof(StickSteps) / sizeof(StickSteps[0]));

	struct Values
	{
		int ee_rate = -1;     // EECycleRate, -3..3 (-1 = 75 %, the port's default)
		int upscale = 1;      // internal resolution, 1..4
		int aspect = 1;       // AspectRatioType: 0 Stretch, 1 Auto, 2 4:3, 3 16:9, 4 10:7
		bool overlay = true;  // FPS counter when a game starts
		int stick = 133;      // analog sensitivity, percent
		bool fast_cdvd = false;
		bool fast_boot = true;
		bool from_file = false;
	};

	inline Values Clamp(Values v)
	{
		v.ee_rate = std::clamp(v.ee_rate, -3, 3);
		v.upscale = std::clamp(v.upscale, 1, 4);
		v.aspect = std::clamp(v.aspect, 0, 4);
		v.stick = std::clamp(v.stick, 100, 200);
		return v;
	}

	inline Values Load()
	{
		Values v;
		std::FILE* file = std::fopen(Path, "r");
		if (!file)
			return v;
		v.from_file = true;
		char line[128];
		while (std::fgets(line, sizeof(line), file))
		{
			char key[32] = {};
			int value = 0;
			if (line[0] == ';' || line[0] == '#' || std::sscanf(line, " %31[a-z_] = %d", key, &value) != 2)
				continue;
			if (!std::strcmp(key, "ee_rate"))
				v.ee_rate = value;
			else if (!std::strcmp(key, "upscale"))
				v.upscale = value;
			else if (!std::strcmp(key, "aspect"))
				v.aspect = value;
			else if (!std::strcmp(key, "overlay"))
				v.overlay = value != 0;
			else if (!std::strcmp(key, "stick"))
				v.stick = value;
			else if (!std::strcmp(key, "fast_cdvd"))
				v.fast_cdvd = value != 0;
			else if (!std::strcmp(key, "fast_boot"))
				v.fast_boot = value != 0;
		}
		std::fclose(file);
		return Clamp(v);
	}

	// Written through a temporary file and a rename, so closing the app mid-write keeps the old file.
	inline bool Save(const Values& in)
	{
		const Values v = Clamp(in);
		const char* temporary = "/data/ForjaSX2/ajustes.ini.tmp";
		std::FILE* file = std::fopen(temporary, "w");
		if (!file)
			return false;
		std::fprintf(file,
			"; Forja SX2: ajustes para todos los juegos (pestaña Ajustes de la biblioteca)\n"
			"ee_rate=%d\nupscale=%d\naspect=%d\noverlay=%d\nstick=%d\nfast_cdvd=%d\nfast_boot=%d\n",
			v.ee_rate, v.upscale, v.aspect, v.overlay ? 1 : 0, v.stick, v.fast_cdvd ? 1 : 0, v.fast_boot ? 1 : 0);
		const bool written = std::fclose(file) == 0;
		return written && std::rename(temporary, Path) == 0;
	}
} // namespace ForjaSettings
