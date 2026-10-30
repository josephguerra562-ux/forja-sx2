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
	// GSHardwareDownloadMode values for the readback choices: Enabled, Unsynchronized, NoReadbacks.
	constexpr int ReadbackModes[] = {0, 3, 2};
	// Analog sensitivity steps, in percent (133 is PCSX2's default for DualShock 2 pads).
	constexpr int StickSteps[] = {100, 110, 120, 133, 140, 150, 160, 170, 180, 190, 200};
	constexpr int StickStepCount = static_cast<int>(sizeof(StickSteps) / sizeof(StickSteps[0]));

	struct Values
	{
		// EECycleRate, -6..3. Compilation 45: -6 (27 %) was the default, as God of War II measured best (98 %, 2026-10-17)
		// and Crash Nitro Kart kept its own speed at it (2026-10-18); a game that needs more gets it in L3+R3, for that game.
		// Compilation 58: -4 (36 %), with the CPU automático below (27 %) under it. At 27 % the automatic speed had nowhere to
		// go; at 36 % a game gets a third more PS2 CPU where the PS4 has room for it, and comes down to 27 % by itself in its
		// heavy scenes: the recipe God of War II and Crash Nitro Kart's factory profiles have used since build 46 (~41 frames a
		// second in GoW2's Colossus fight). A player's ajustes.ini keeps what it says.
		int ee_rate = -4;
		// [Forja] CpuAuto for every game without its own (compilation 45): 0 off, -1..-6 the lowest EE rate it may step down
		// to (main.cpp, UpdateCpuAuto). -6: a game raised above 27 % in L3+R3 comes back down in its heavy scenes.
		int cpu_auto = -6;
		int upscale = 1;      // internal resolution, 1..4
		int aspect = 1;       // AspectRatioType: 0 Stretch, 1 Auto, 2 4:3, 3 16:9, 4 10:7
		bool overlay = true;  // FPS counter when a game starts
		int stick = 133;      // analog sensitivity, percent
		bool fast_cdvd = false;
		bool fast_boot = true;
		// GPU readbacks: 0 accurate (waits for the GPU), 1 don't wait (Unsynchronized), 2 none (NoReadbacks).
		int readback = 0;
		// Automatic video mode: while a game decodes a movie, run the EE at 50 % (EECycleRate -3).
		bool video_mode = true;
		// Frame generation (AMD FSR3 interpolation): a game steady at 30 fps is shown at 60. Off until tested on the console.
		bool frame_gen = false;
		// Compilation 53: what opens the emulator's menu while playing: 0 L3+R3, 1 a click on the touchpad's right half (its left
		// half stays the PS2's Select), 2 both.
		int menu_button = 0;
		// 2026-10-19: the emulator's language (ForjaLang.h code, "" until one is picked) and whether the library asks for it
		// at every start (Joseph: "al inicio de cada sesión te pide el idioma").
		char language[8] = "";
		bool ask_language = true;
		// Compilation 97: a second folder the library lists games from, besides /data/ForjaSX2/roms ("" none). Inside the app
		// the USB drives are /usb0../usb7.
		char games_folder[192] = "";
		bool from_file = false;
	};

	inline Values Clamp(Values v)
	{
		v.ee_rate = std::clamp(v.ee_rate, -6, 3);
		v.cpu_auto = std::clamp(v.cpu_auto, -6, 0);
		v.upscale = std::clamp(v.upscale, 1, 4);
		v.aspect = std::clamp(v.aspect, 0, 4);
		v.stick = std::clamp(v.stick, 100, 200);
		v.readback = std::clamp(v.readback, 0, 2);
		v.menu_button = std::clamp(v.menu_button, 0, 2);
		return v;
	}

	inline Values Load()
	{
		Values v;
		std::FILE* file = std::fopen(Path, "r");
		if (!file)
			return v;
		v.from_file = true;
		char line[256];
		while (std::fgets(line, sizeof(line), file))
		{
			char key[32] = {};
			int value = 0;
			char code[8] = {};
			if (std::sscanf(line, " idioma = %7[a-z]", code) == 1)
			{
				std::snprintf(v.language, sizeof(v.language), "%s", code);
				continue;
			}
			char folder[sizeof(v.games_folder)] = {};
			if (std::sscanf(line, " carpeta_juegos = %191[^\r\n]", folder) == 1)
			{
				std::snprintf(v.games_folder, sizeof(v.games_folder), "%s", folder);
				continue;
			}
			if (line[0] == ';' || line[0] == '#' || std::sscanf(line, " %31[a-z_] = %d", key, &value) != 2)
				continue;
			if (!std::strcmp(key, "ee_rate"))
				v.ee_rate = value;
			else if (!std::strcmp(key, "cpu_auto"))
				v.cpu_auto = value;
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
			else if (!std::strcmp(key, "readback"))
				v.readback = value;
			else if (!std::strcmp(key, "video_mode"))
				v.video_mode = value != 0;
			else if (!std::strcmp(key, "frame_gen"))
				v.frame_gen = value != 0;
			else if (!std::strcmp(key, "menu_boton"))
				v.menu_button = value;
			else if (!std::strcmp(key, "preguntar_idioma"))
				v.ask_language = value != 0;
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
			"ee_rate=%d\nupscale=%d\naspect=%d\noverlay=%d\nstick=%d\nfast_cdvd=%d\nfast_boot=%d\nreadback=%d\nvideo_mode=%d\n"
			"frame_gen=%d\ncpu_auto=%d\nmenu_boton=%d\nidioma=%s\npreguntar_idioma=%d\ncarpeta_juegos=%s\n",
			v.ee_rate, v.upscale, v.aspect, v.overlay ? 1 : 0, v.stick, v.fast_cdvd ? 1 : 0, v.fast_boot ? 1 : 0,
			v.readback, v.video_mode ? 1 : 0, v.frame_gen ? 1 : 0, v.cpu_auto, v.menu_button, v.language, v.ask_language ? 1 : 0,
			v.games_folder);
		const bool written = std::fclose(file) == 0;
		return written && std::rename(temporary, Path) == 0;
	}
} // namespace ForjaSettings
