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
#include <set>
#include <string>
#include <string_view>

// 159 in the app: true in a build made on the lab's PC (ps4/vm/PatchDownload.cpp): the package's own profile comes first.
extern bool g_forja_build_casa;

namespace ForjaGameProfile
{
	// Where a value comes from, shown by the L3+R3 menu.
	enum class Origin : unsigned
	{
		Global = 0,
		Factory = 1,
		Player = 2,
		Lab = 3, // compilation 131: the lab's file, /data/ForjaSX2/pruebas.ini
		Safe = 4, // compilation 131: turned off by "Modo seguro"
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
		// [Forja] GSQuitar (compilation 51): the groups of draws the GS filter leaves out, their keys in hex ("1c07ff5,0e00a44";
		// GSRendererHW.cpp has what a key holds).
		std::string gs_filter;
		Origin gs_filter_origin = Origin::Global;
		// [Forja] SaltarCuadros (compilation 47): the GS frame skip, -1 not set (off), 0 off, 1 auto, 2 always (one frame in
		// two), 3 always with the guard's short pauses ("1 de 2 rápido", compilation 86; 82-85: without the guard). main.cpp,
		// UpdateGsSkip; GSRendererPS4.cpp, VSync.
		int gs_skip = -1;
		Origin gs_skip_origin = Origin::Global;
		// Compilation 146: [Forja] SaltarCuadrosOcultaPantalla, for a game that draws each frame into the picture the TV shows
		// (Dragon Ball Z BT4): that picture is hidden as soon as a frame is chosen to be left out (GSRendererPS4.cpp).
		int gs_skip_hide_shown = -1;
		Origin gs_skip_hide_shown_origin = Origin::Global;
		// 146 in the app: [Forja] SaltoCuidaCuadros, "Al vuelo cuida los cuadros" (ps4/vm/main.cpp): a step up of the cycle skip that
		// cuts the game's frames goes back. Off unless the game's profile turns it on (Downhill Domination, God of War II) until measured.
		int skip_keeps_frames = -1;
		Origin skip_keeps_frames_origin = Origin::Global;
		// 154 in the app: [Forja] SaltoTecho, "Techo del salto": the most sixteenths "Salto de ciclo: Al vuelo" may go up to (ps4/vm/main.cpp,
		// ForjaSkipTecho). -1 (or 48) none. Off unless the profile says.
		int skip_ceiling16 = -1;
		Origin skip_ceiling16_origin = Origin::Global;
		// 154 in the app: [Forja] FlipEnHilo, "Flip en su propio hilo" (ps4/core/ForjaFlipThread.h): 1 the GPU-side flip from a thread
		// of its own instead of the GS thread; 2 (155) that thread waits for the frame and flips from the CPU. Off unless the profile says.
		int flip_thread = -1;
		Origin flip_thread_origin = Origin::Global;
		// 157 in the app: [Forja] VU1RegistrosEntreBloques, "Registros entre bloques" (pcsx2/x86/microVU_Branch.inl): 1 the VU1's direct
		// branches hand their whole VF registers on to the next block. Off unless the profile says.
		int vu1_keep_regs = -1;
		Origin vu1_keep_regs_origin = Origin::Global;
		// 158 in the app: [Forja] VU1TresOperandos, "Tres operandos" (pcsx2/x86/microVU_Upper.inl): 1 the VU1's FMACs write their own
		// register with AVX instead of copying Fs first. [Forja] VU1MemoriaEnRegistro, "Memoria del VU1 en un registro"
		// (pcsx2/x86/microVU_Misc.inl): 1 VU memory's address in a GPR for the rest of the block instead of a movabs at each access.
		// Off unless the profile says.
		int vu1_three_op = -1;
		Origin vu1_three_op_origin = Origin::Global;
		int vu1_mem_reg = -1;
		Origin vu1_mem_reg_origin = Origin::Global;
		// 159 in the app: [Forja] VU1BanderasJustas, "Banderas justas" (pcsx2/x86/microVU_Flags.inl, microVU_Analyze.inl): 1 the VU1
		// computes only the MAC flags a read can see (not the last four of each block). Off unless the profile says.
		int vu1_mac_just = -1;
		Origin vu1_mac_just_origin = Origin::Global;
		// 161 in the app: [Forja] VU1CuentaCiclos, "Ciclos en una sola cuenta" (pcsx2/x86/microVU_Compile.inl): 1 each VU1 block checks
		// and counts its cycles with one subtraction. Off unless the profile says.
		int vu1_one_count = -1;
		Origin vu1_one_count_origin = Origin::Global;
		// 162 in the app: [Forja] VU1MacEnSaltos, "MAC justa en los saltos" (pcsx2/x86/microVU_Flags.inl): 1 a VU1 block before a JR/JALR
		// computes only the MAC instances the next program can read (2), not 4. Off unless the profile says.
		int vu1_mac_jump = -1;
		Origin vu1_mac_jump_origin = Origin::Global;
		// 163 in the app: [Forja] VU1MacAlFinal, "MAC al final" (pcsx2/x86/microVU_Upper.inl, microVU_Branch.inl): 1 each VU1 block saves its
		// last FMAC's result and the program's end computes the MAC flag the next program starts with. Off unless the profile says.
		int vu1_mac_end = -1;
		Origin vu1_mac_end_origin = Origin::Global;
		// 151 in the app: [Forja] VifMenosTurnos, "VIF1 en menos turnos" (pcsx2/R5900.cpp, CPU_INT): the VIF1 DMA's next step, due in fewer
		// cycles than this, runs in the same pass of the event scan instead of a new event test (PCSX2 does it under 4 cycles). Off (the 4)
		// unless the game's profile sets it: God of War II, measured.
		int vif1_fuse = -1;
		Origin vif1_fuse_origin = Origin::Global;
		// 167 in the app: [Forja] VifEnTramos, "VIF en tramos" (pcsx2/Vif_Transfer.cpp, MTVU.cpp): 1 the EE sends the VU thread the runs of
		// VIF1 codes only the VU1 needs in one piece (it starts checking and goes on by itself). Off unless the profile says.
		int vif_tramos = -1;
		Origin vif_tramos_origin = Origin::Global;
		// 169 in the app: [Forja] VU1ColaKB, "Cola corta del VU1" (pcsx2/MTVU.cpp, g_orbis_mtvu_ring_kb): the VU thread's ring lap in KB
		// (256..16384; the whole 16 MB unless the profile says). VU1ColaAlterna = 1 (a test): the lap goes 16384, 2048, 1024, 512 KB,
		// 20 s each, round and round, and the 5 s meter says which one ran, to compare them in the same scenes.
		int vu1_ring_kb = -1;
		Origin vu1_ring_kb_origin = Origin::Global;
		int vu1_ring_alt = -1;
		Origin vu1_ring_alt_origin = Origin::Global;
		// 170 in the app: [Forja] VifAdelanto, "VIF1 por adelantado" (pcsx2/Vif1_Dma.cpp, MTVU.cpp): 1 asks memory early for the whole VIF1
		// packet the next pass reads and for the VU ring's next lines. Off unless the profile says.
		int vif_adelanto = -1;
		Origin vif_adelanto_origin = Origin::Global;
		// 171 in the app: [Forja] GSVerticesUnaVez, "Vértices una vez" (pcsx2/GS/Renderers/Common/GSVertexTrace.cpp): 1 a draw's minimum and
		// maximum read each vertex once (it starts checking and goes on by itself). Off unless the profile says.
		int gs_vt_once = -1;
		Origin gs_vt_once_origin = Origin::Global;
		// 183 in the app: [Forja] GSDibujoAB, "A/B en un dibujo" (pcsx2/GS/GSState.cpp): 1 joins and draws the pieces back one by one (a
		// check), 2 one draw with both textures when the two configurations match (3: also with the second alpha pass).
		// 190: 2 for every game unless the profile says (ps4/vm/main.cpp).
		int gs_ab = -1;
		// 194 in the app: [Forja] VU1AvisoPorPedazo (pcsx2/MTVU.cpp g_orbis_mtvu_batch, the PS5 port's OrbisMTVUBatch, never set in Forja): 1
		// the EE tells the VU thread about new work once per VIF1 piece instead of once per packet. Off unless the profile says.
		int mtvu_batch = -1;
		Origin mtvu_batch_origin = Origin::Global;
		Origin gs_ab_origin = Origin::Global;
		// 173 in the app: [Forja] EEPlzcwRapido, "PLZCW rápido" (pcsx2/x86/iMMI.cpp): 1 the EE's PLZCW with LZCNT and no branches. Off unless
		// the profile says.
		int ee_lzcnt = -1;
		Origin ee_lzcnt_origin = Origin::Global;
		// 174 in the app: "Animación a medias" ([Forja] AnimMediasDir, AnimMediasFuncion, AnimMediasVacia; decimal EE addresses; ps4/vm/
		// main.cpp): at every vsync the word at AnimMediasDir (an entry of one of the game's function tables) goes between AnimMediasFuncion
		// and AnimMediasVacia (an empty function of the game), so that function runs one vsync out of two. Off unless all three are set.
		int anim_half_addr = -1, anim_half_fn = -1, anim_half_empty = -1;
		Origin anim_half_origin = Origin::Global;
		// 175 in the app: [Forja] VifSinCopia, "VIF sin copia" (pcsx2/MTVU.cpp, Vif_Unpack.cpp): 1 the VU thread reads each UNPACK from the
		// game's RAM (it starts checking and goes on by itself). Off unless the profile says.
		int vif_nocopy = -1;
		Origin vif_nocopy_origin = Origin::Global;
		// 152 in the app: [Forja] VU1Ahorro, "Ahorro del VU1" (pcsx2/MTVU.cpp, microVU_Execute.inl): the VU1's runs without the emitter's
		// pointers and with the PS5 port's lookup cache, and a microcode upload that changes no byte leaves the programs alone. Off
		// unless the game's profile turns it on: God of War II.
		int vu1_ahorro = -1;
		Origin vu1_ahorro_origin = Origin::Global;
		// 153 in the app: [Forja] VU1BanderasMuertas, "Banderas muertas del VU1" (pcsx2/x86/microVU_Flags.inl): 1 the status flag, 2 the
		// MAC flag, 3 both, that no VU1 code of the game reads across a JR/JALR (measured with vm-vu1-mapa). Off unless the profile says.
		int vu1_dead_flags = -1;
		Origin vu1_dead_flags_origin = Origin::Global;
		// [Forja] Huellas (compilation 55): the draw fingerprint rules turned on for the game, their huellas.ini ids with commas
		// (ps4/vm/main.cpp, LoadRules; pcsx2/ForjaGsRules.h).
		std::string rules_on;
		Origin rules_on_origin = Origin::Global;
		// 147 in the app: [Forja] BucleDeEspera, the game's own wait loops that span several blocks (iR5900.cpp, ForjaSetWaitLoops):
		// "<the block with the jump back, hex>:<gp offset of a call pointer that must be 0, decimal>", commas between.
		std::string wait_loops;
		// [Forja] PublicoCada (compilation 82): the Guitar Hero games' crowd, drawn in software (GSHwHack.cpp, GSC_GuitarHero) once
		// every 1, 2, 4 or 8 of its passes; 0 not set (main.cpp's default).
		int crowd_every = 0;
		Origin crowd_every_origin = Origin::Global;
		// [Forja] SaltoAprendido (compilation 139, "Al vuelo con memoria"): the cycle skip, in sixteenths, the game spent most of its last
		// session at with "Al vuelo" (main.cpp writes it); the next session starts there. -1: none yet.
		int learned_skip16 = -1;
		Origin learned_skip16_origin = Origin::Global;
		// [Forja] GrabarEnOtroHilo (compilation 116): the draws' Vulkan calls on their own thread (GS/Renderers/Vulkan/
		// ForjaVkRecorder.h): 0 off; 1 on; -1 not set, on since compilation 118 (tested in the six test games, 2026-11-04).
		int draw_thread = -1;
		Origin draw_thread_origin = Origin::Global;
		// [Forja] TraductorSoftware (compilation 117): the software draws with the generated code (the JIT) instead of the C
		// version (GS/Renderers/SW/GSDrawScanline.cpp): 0 off; 1 on; -1 not set, on since compilation 118 (Dragon Ball and
		// Guitar Hero, 2026-11-04).
		int sw_jit = -1;
		Origin sw_jit_origin = Origin::Global;
		// [Forja] IopRapido (compilation 120): an IOP write into its RAM clears translated code only where there is some
		// (pcsx2/IopMem.cpp, ForjaIopMayHaveCode): 0 off; 1 on; -1 not set, off until it is measured.
		int iop_fast = -1;
		Origin iop_fast_origin = Origin::Global;
		// Compilation 122, each 0 off, 1 on, -1 not set (off until measured): [Forja] IopDormido, the IOP asleep in its idle loop in
		// the game too, not only in movies (pcsx2/R3000A.cpp, ForjaIopMaySleep); [Forja] SaltoAutoAfinado and [Forja] CpuAutoSube,
		// the rules of main.cpp's UpdateCycleSkipAuto and UpdateCpuAuto tried in herramientas/simulaciones/simular_automaticos_escenas.py.
		int iop_sleep = -1;
		Origin iop_sleep_origin = Origin::Global;
		int skip_auto_fine = -1;
		Origin skip_auto_fine_origin = Origin::Global;
		int cpu_auto_up = -1;
		Origin cpu_auto_up_origin = Origin::Global;
		// [Forja] RevisionRapida (compilation 124): the EE recompiler without the check of every block's code: 0 off, 1 on, -1 not set (off).
		int fast_code = -1;
		Origin fast_code_origin = Origin::Global;
		// [Forja] EfectosListos (compilation 127): the effects of the game's list made at its start: 0 off, 1 on, -1 not set (off).
		int effects_ahead = -1;
		Origin effects_ahead_origin = Origin::Global;
		// [Forja] CpuSinTirones (compilation 128): a CPU speed change without a recompiler reset: 0 off, 1 on, -1 not set (off).
		int cpu_seamless = -1;
		Origin cpu_seamless_origin = Origin::Global;
		// [Forja] DiscoSinEsperas (compilation 130): no standing still for the console's disc: 0 off, 1 on, -1 not set (off).
		int disc_no_wait = -1;
		Origin disc_no_wait_origin = Origin::Global;
		// [EmuCore/Speedhacks] EECycleSkip * 4 + EECycleSkipQuarters of the factory profile only (compilation 109): where the cycle
		// skip's Auto starts and the least it goes to (main.cpp, UpdateCycleSkipAuto). 0 with no factory profile.
		int cycle_skip_factory = 0;
		// Compilation 131 (Joseph, 2026-11-13): the ten improvements above that were rows of No tocar (GrabarEnOtroHilo ... DiscoSinEsperas)
		// are on for every game, and Load gives each as 0 or 1: the lab's file wins, then "Modo seguro", then the game's factory profile
		// (GTA San Andreas does not start with RevisionRapida), then on. The player's own value counts only while the lab's file shows the
		// rows: hidden, a value saved by an older version would stay out of reach.
		// [Forja] ModoSeguro: 0 no; 1 partial, without the three that change how the game's code and disc are emulated (RevisionRapida,
		// CpuSinTirones, DiscoSinEsperas); 2 total, without all ten (and VMManager.cpp takes back the VU1's clamp and the repeated refreshes).
		int safe_mode = 0;
		Origin safe_mode_origin = Origin::Global;
		// The lab's file: read at all; [Forja] MostrarFilas = 1; [Forja] Guardian = 0 (main.cpp, ForjaGuardStart).
		bool has_lab = false;
		bool lab_rows = false;
		bool guard = true;
		// Compilation 140: [Forja] GuardianApagadas, the hidden improvements the guardian turned off for this game, one at a time
		// (main.cpp, ForjaGuardStart), comma-separated; and hidden_on, the hidden ones on for it now (what the guardian picks from).
		std::string guard_off;
		std::string hidden_on;
		// Compilation 132, three rows of No tocar; compilation 134 (Joseph, 2026-11-14, with the official 1.07: "siempre en sí ... e
		// invisibles al público"), on for every game and hidden like the ten above, after the 132 and the 133 on the console (God of War
		// II's VU1 95.2 -> 97.8 % speed; Shadow 9.8 -> 17.5 FPS on a push; "Código listo" made 62,000-250,000 blocks ahead a session).
		// 0 or 1 from Load; "Modo seguro" turns them off too: [Forja] CodigoListo, the EE's blocks of earlier sessions made ahead
		// (x86/ix86-32/iR5900.cpp); [Forja] SaltosVU1, a VU1 jump's cached target taken in the block's own code
		// (x86/microVU_Branch.inl); [Forja] EmpujonCuadros, a two-second raise of the PS2 CPU when the game stays at fewer frames with
		// room to spare (ps4/vm/main.cpp, UpdateFramePush).
		int code_ahead = 0;
		Origin code_ahead_origin = Origin::Global;
		int vu_jumps = 0;
		Origin vu_jumps_origin = Origin::Global;
		int frame_push = 0;
		Origin frame_push_origin = Origin::Global;
		// Compilation 136: [Forja] FGConMargen, "Cuadros generados con margen" (GSDeviceVK.cpp, OrbisFrameGenRecord): a row of No tocar,
		// in sight and off until it is tried on the console.
		int fg_margin = 0;
		Origin fg_margin_origin = Origin::Global;
	};

	// Forja SX2 (compilation 44): the settings center's copy (/data/ForjaSX2/centro/perfiles, fetched from the project's GitHub by
	// ps4/vm/PatchDownload.cpp) wins over the package's, so a game's tested settings reach every player without a new package.
	inline std::string FactoryPath(std::string_view serial)
	{
		const std::string name = fmt::format("{}.ini", Path::SanitizeFileName(serial));
		const std::string center = Path::Combine(EmuFolders::DataRoot, "centro/perfiles/" + name);
		if (!g_forja_build_casa && FileSystem::FileExists(center.c_str())) // 159 in the app: not in a build made on the PC
			return center;
		return Path::Combine(EmuFolders::Resources, "perfiles/" + name);
	}

	inline std::string PlayerPath(std::string_view serial, u32 crc)
	{
		return (serial.empty() || crc == 0) ? std::string() : VMManager::GetGameSettingsPath(serial, crc);
	}

	// Compilation 140: whether a comma-separated list (the guardian's) has this key.
	inline bool GuardListHas(const std::string& list, std::string_view key)
	{
		size_t at = 0;
		while (at <= list.size())
		{
			const size_t end = std::min(list.find(',', at), list.size());
			if (list.compare(at, end - at, key) == 0 && end - at == key.size())
				return true;
			at = end + 1;
		}
		return false;
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
	// A key the file repeats is a list, as PCSX2's own "Enable = A" / "Enable = B" (compilation 44: only the first was kept).
	// Compilation 131: the reader by itself (MergeFile), for the lab's file too; with `over` the file's keys win over what `si` has.
	inline bool MergeFile(SettingsInterface& si, const std::string& path, bool over = false)
	{
		std::FILE* file = std::fopen(path.c_str(), "r");
		if (!file)
			return false;
		char line[512];
		std::string section;
		std::set<std::string> own; // "section\nkey" this file set, so its repeats add to the list
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
			if (key.empty())
				continue;
			const std::string id = section + '\n' + key;
			if (own.count(id))
				si.AddToStringList(section.c_str(), key.c_str(), value.c_str());
			else if (over || !si.ContainsValue(section.c_str(), key.c_str()))
			{
				si.SetStringValue(section.c_str(), key.c_str(), value.c_str());
				own.insert(id);
			}
		}
		std::fclose(file);
		return true;
	}

	inline bool MergeFactory(SettingsInterface& si, std::string_view serial)
	{
		return !serial.empty() && MergeFile(si, FactoryPath(serial));
	}

	// Compilation 131: the lab's file, for tests on one console (Joseph, 2026-11-13: "si un día me requieras que desactive algo para
	// probarlo ... a través de mi Play"). Sent by FTP, never made by the app. Its keys win over the player's, the factory's and the
	// defaults, for every game: PCSX2's sections (VMManager.cpp merges them into the game's layer) and [Forja]'s (Load, below), where
	// MostrarFilas = 1 brings back the hidden rows of No tocar and Guardian = 0 keeps the guardian from touching "Modo seguro".
	inline std::string LabPath()
	{
		return Path::Combine(EmuFolders::DataRoot, "pruebas.ini");
	}

	inline bool MergeLab(SettingsInterface& si)
	{
		return MergeFile(si, LabPath(), true);
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
		read_int("SaltarCuadros", &v.gs_skip, &v.gs_skip_origin);
		read_int("SaltarCuadrosOcultaPantalla", &v.gs_skip_hide_shown, &v.gs_skip_hide_shown_origin); // compilation 146
		read_int("SaltoCuidaCuadros", &v.skip_keeps_frames, &v.skip_keeps_frames_origin); // 146 in the app
		read_int("SaltoTecho", &v.skip_ceiling16, &v.skip_ceiling16_origin); // 154 in the app
		read_int("FlipEnHilo", &v.flip_thread, &v.flip_thread_origin); // 154 in the app
		read_int("VU1RegistrosEntreBloques", &v.vu1_keep_regs, &v.vu1_keep_regs_origin); // 157 in the app
		read_int("VU1TresOperandos", &v.vu1_three_op, &v.vu1_three_op_origin); // 158 in the app
		read_int("VU1MemoriaEnRegistro", &v.vu1_mem_reg, &v.vu1_mem_reg_origin); // 158 in the app
		read_int("VU1BanderasJustas", &v.vu1_mac_just, &v.vu1_mac_just_origin); // 159 in the app
		read_int("VU1CuentaCiclos", &v.vu1_one_count, &v.vu1_one_count_origin); // 161 in the app
		read_int("VU1MacEnSaltos", &v.vu1_mac_jump, &v.vu1_mac_jump_origin); // 162 in the app
		read_int("VU1MacAlFinal", &v.vu1_mac_end, &v.vu1_mac_end_origin); // 163 in the app
		read_int("VifMenosTurnos", &v.vif1_fuse, &v.vif1_fuse_origin); // 151 in the app
		read_int("VifEnTramos", &v.vif_tramos, &v.vif_tramos_origin); // 167 in the app
		read_int("VU1ColaKB", &v.vu1_ring_kb, &v.vu1_ring_kb_origin); // 169 in the app
		read_int("VU1ColaAlterna", &v.vu1_ring_alt, &v.vu1_ring_alt_origin); // 169 in the app
		read_int("VifAdelanto", &v.vif_adelanto, &v.vif_adelanto_origin); // 170 in the app
		read_int("GSVerticesUnaVez", &v.gs_vt_once, &v.gs_vt_once_origin); // 171 in the app
		read_int("GSDibujoAB", &v.gs_ab, &v.gs_ab_origin); // 183 in the app
		read_int("VU1AvisoPorPedazo", &v.mtvu_batch, &v.mtvu_batch_origin); // 194 in the app
		read_int("EEPlzcwRapido", &v.ee_lzcnt, &v.ee_lzcnt_origin); // 173 in the app
		read_int("AnimMediasDir", &v.anim_half_addr, &v.anim_half_origin); // 174 in the app
		read_int("AnimMediasFuncion", &v.anim_half_fn, &v.anim_half_origin);
		read_int("AnimMediasVacia", &v.anim_half_empty, &v.anim_half_origin);
		read_int("VifSinCopia", &v.vif_nocopy, &v.vif_nocopy_origin); // 175 in the app
		read_int("VU1Ahorro", &v.vu1_ahorro, &v.vu1_ahorro_origin); // 152 in the app
		read_int("VU1BanderasMuertas", &v.vu1_dead_flags, &v.vu1_dead_flags_origin); // 153 in the app
		read_int("PublicoCada", &v.crowd_every, &v.crowd_every_origin);
		read_int("SaltoAprendido", &v.learned_skip16, &v.learned_skip16_origin); // compilation 139
		read_int("GrabarEnOtroHilo", &v.draw_thread, &v.draw_thread_origin);
		read_int("TraductorSoftware", &v.sw_jit, &v.sw_jit_origin);
		read_int("IopRapido", &v.iop_fast, &v.iop_fast_origin);
		read_int("IopDormido", &v.iop_sleep, &v.iop_sleep_origin); // compilation 122
		read_int("SaltoAutoAfinado", &v.skip_auto_fine, &v.skip_auto_fine_origin);
		read_int("CpuAutoSube", &v.cpu_auto_up, &v.cpu_auto_up_origin);
		read_int("RevisionRapida", &v.fast_code, &v.fast_code_origin); // compilation 124
		read_int("EfectosListos", &v.effects_ahead, &v.effects_ahead_origin); // compilation 127
		read_int("CpuSinTirones", &v.cpu_seamless, &v.cpu_seamless_origin); // compilation 128
		read_int("DiscoSinEsperas", &v.disc_no_wait, &v.disc_no_wait_origin); // compilation 130
		// Compilation 131: "Modo seguro", the lab's file, and the ten hidden improvements, each 0 or 1 from here on (see Values).
		read_int("ModoSeguro", &v.safe_mode, &v.safe_mode_origin);
		MemorySettingsInterface lab;
		v.has_lab = MergeFile(lab, LabPath());
		if (v.has_lab)
		{
			int shown = 0, guard = 1;
			lab.GetIntValue("Forja", "MostrarFilas", &shown);
			lab.GetIntValue("Forja", "Guardian", &guard);
			v.lab_rows = shown != 0;
			v.guard = guard != 0;
			if (lab.GetIntValue("Forja", "ModoSeguro", &v.safe_mode))
				v.safe_mode_origin = Origin::Lab;
		}
		v.safe_mode = std::clamp(v.safe_mode, 0, 2);
		if (v.has_player)
			player.GetStringValue("Forja", "GuardianApagadas", &v.guard_off); // compilation 140
		v.hidden_on.clear();
		struct Hidden
		{
			const char* key;
			int* value;
			Origin* origin;
			int safe_level; // the "Modo seguro" that turns it off
			int fallback; // with nothing said: 1 for the hidden ones, 0 for the rows still being tried (compilation 132)
			bool in_sight; // its row shows, so the player's value counts
		};
		const Hidden hidden[] = {
			{"GrabarEnOtroHilo", &v.draw_thread, &v.draw_thread_origin, 2, 1, false},
			{"TraductorSoftware", &v.sw_jit, &v.sw_jit_origin, 2, 1, false},
			{"IopRapido", &v.iop_fast, &v.iop_fast_origin, 2, 1, false},
			{"IopDormido", &v.iop_sleep, &v.iop_sleep_origin, 2, 1, false},
			{"SaltoAutoAfinado", &v.skip_auto_fine, &v.skip_auto_fine_origin, 2, 1, false},
			{"CpuAutoSube", &v.cpu_auto_up, &v.cpu_auto_up_origin, 2, 1, false},
			{"RevisionRapida", &v.fast_code, &v.fast_code_origin, 1, 1, false},
			{"EfectosListos", &v.effects_ahead, &v.effects_ahead_origin, 2, 1, false},
			{"CpuSinTirones", &v.cpu_seamless, &v.cpu_seamless_origin, 1, 1, false},
			{"DiscoSinEsperas", &v.disc_no_wait, &v.disc_no_wait_origin, 1, 1, false},
			{"CodigoListo", &v.code_ahead, &v.code_ahead_origin, 1, 1, false},
			{"SaltosVU1", &v.vu_jumps, &v.vu_jumps_origin, 1, 1, false},
			{"EmpujonCuadros", &v.frame_push, &v.frame_push_origin, 1, 1, false},
			{"FGConMargen", &v.fg_margin, &v.fg_margin_origin, 2, 0, true}, // compilation 136
		};
		for (const Hidden& h : hidden)
		{
			int value = h.fallback, read = 0;
			Origin origin = Origin::Global;
			if (v.has_lab && lab.GetIntValue("Forja", h.key, &read))
			{
				value = read != 0 ? 1 : 0;
				origin = Origin::Lab;
			}
			else if (v.safe_mode >= h.safe_level || GuardListHas(v.guard_off, h.key)) // compilation 140: or the guardian's list
			{
				value = 0;
				origin = Origin::Safe;
			}
			else if ((h.in_sight || v.lab_rows) && v.has_player && player.GetIntValue("Forja", h.key, &read))
			{
				value = read != 0 ? 1 : 0;
				origin = Origin::Player;
			}
			else if (v.has_factory && factory.GetIntValue("Forja", h.key, &read))
			{
				value = read != 0 ? 1 : 0;
				origin = Origin::Factory;
			}
			*h.value = value;
			*h.origin = origin;
			if (value != 0)
				v.hidden_on += std::string(v.hidden_on.empty() ? "" : ",") + h.key; // compilation 140
		}
		// Forja 1.06: [Forja] SaltarCuadrosConGuardian, a factory profile's frame skip for the versions with the guard (73 on),
		// which wins over its SaltarCuadros (never over the player's). The versions before 1.06 fetch the factory profiles from
		// main and read only SaltarCuadros: there it keeps what suits their skip without the guard (Dragon Ball: off).
		int guarded = -1;
		if (v.gs_skip_origin != Origin::Player && v.has_factory && factory.GetIntValue("Forja", "SaltarCuadrosConGuardian", &guarded) &&
			guarded >= 0)
		{
			v.gs_skip = guarded;
			v.gs_skip_origin = Origin::Factory;
		}
		if (v.has_player && player.GetStringValue("Forja", "Nucleos", &v.cores))
			v.cores_origin = Origin::Player;
		else if (v.has_factory && factory.GetStringValue("Forja", "Nucleos", &v.cores))
			v.cores_origin = Origin::Factory;
		if (!(v.has_player && player.GetStringValue("Forja", "BucleDeEspera", &v.wait_loops)) && v.has_factory) // 147 in the app
			factory.GetStringValue("Forja", "BucleDeEspera", &v.wait_loops);
		if (v.has_player && player.GetStringValue("Forja", "Huellas", &v.rules_on))
			v.rules_on_origin = Origin::Player;
		else if (v.has_factory && factory.GetStringValue("Forja", "Huellas", &v.rules_on))
			v.rules_on_origin = Origin::Factory;
		if (v.has_player && player.GetStringValue("Forja", "VU1Quitar", &v.vu1_skip))
			v.vu1_skip_origin = Origin::Player;
		else if (v.has_factory && factory.GetStringValue("Forja", "VU1Quitar", &v.vu1_skip))
			v.vu1_skip_origin = Origin::Factory;
		if (v.has_player && player.GetStringValue("Forja", "GSQuitar", &v.gs_filter))
			v.gs_filter_origin = Origin::Player;
		else if (v.has_factory && factory.GetStringValue("Forja", "GSQuitar", &v.gs_filter))
			v.gs_filter_origin = Origin::Factory;
		int skip_whole = 0, skip_quarters = 0;
		if (v.has_factory)
		{
			factory.GetIntValue("EmuCore/Speedhacks", "EECycleSkip", &skip_whole);
			factory.GetIntValue("EmuCore/Speedhacks", "EECycleSkipQuarters", &skip_quarters);
		}
		v.cycle_skip_factory = std::clamp(skip_whole, 0, 3) * 4 + std::clamp(skip_quarters, 0, 3);

		// Compilation 49: VideoMode 2 = auto (main.cpp, UpdateVideoMode), and movie rates down to -6 (27 %).
		if (v.video_mode >= 0)
			v.video_mode = std::min(v.video_mode, 2);
		if (v.video_ee_rate <= 0)
			v.video_ee_rate = std::clamp(v.video_ee_rate, -6, 0);
		if (v.frame_gen >= 0)
			v.frame_gen = std::min(v.frame_gen, 2);
		if (v.cpu_auto <= 0)
			v.cpu_auto = std::clamp(v.cpu_auto, -6, 0);
		else
			v.cpu_auto = 0;
		if (v.gs_skip >= 0)
			v.gs_skip = std::min(v.gs_skip, 5); // compilation 82: 3; 88: 4, two in three left out; 90: 5, on the fly
		v.learned_skip16 = std::clamp(v.learned_skip16, -1, 48); // compilation 139
		v.vif1_fuse = std::clamp(v.vif1_fuse, -1, 1024); // 151 in the app
		v.vu1_dead_flags = std::clamp(v.vu1_dead_flags, -1, 3); // 153 in the app
		v.skip_ceiling16 = std::clamp(v.skip_ceiling16, -1, 48); // 154 in the app
		v.flip_thread = std::clamp(v.flip_thread, -1, 2); // 154-155 in the app
		v.vu1_keep_regs = std::clamp(v.vu1_keep_regs, -1, 1); // 157 in the app
		v.vu1_three_op = std::clamp(v.vu1_three_op, -1, 1); // 158 in the app
		v.vu1_mem_reg = std::clamp(v.vu1_mem_reg, -1, 1); // 158 in the app
		v.vu1_mac_just = std::clamp(v.vu1_mac_just, -1, 1); // 159 in the app
		v.vu1_one_count = std::clamp(v.vu1_one_count, -1, 1); // 161 in the app
		v.vu1_mac_jump = std::clamp(v.vu1_mac_jump, -1, 1); // 162 in the app
		v.vu1_mac_end = std::clamp(v.vu1_mac_end, -1, 1); // 163 in the app
		v.vif_tramos = std::clamp(v.vif_tramos, -1, 1); // 167 in the app
		v.vu1_ring_kb = v.vu1_ring_kb < 0 ? -1 : std::clamp(v.vu1_ring_kb, 256, 16384); // 169 in the app
		v.vu1_ring_alt = std::clamp(v.vu1_ring_alt, -1, 1); // 169 in the app
		v.vif_adelanto = std::clamp(v.vif_adelanto, -1, 1); // 170 in the app
		v.gs_vt_once = std::clamp(v.gs_vt_once, -1, 1); // 171 in the app
		v.gs_ab = std::clamp(v.gs_ab, -1, 3); // 183 in the app (188: 3 also joins draws with the second alpha pass)
		v.mtvu_batch = std::clamp(v.mtvu_batch, -1, 1); // 194 in the app
		v.ee_lzcnt = std::clamp(v.ee_lzcnt, -1, 1); // 173 in the app
		v.vif_nocopy = std::clamp(v.vif_nocopy, -1, 1); // 175 in the app
		if (v.crowd_every != 0 && v.crowd_every != 99) // compilation 137: 99 is "Al vuelo"
			v.crowd_every = v.crowd_every >= 32 ? 32 : v.crowd_every >= 16 ? 16 : v.crowd_every >= 8 ? 8 : v.crowd_every >= 4 ? 4 :
			                v.crowd_every >= 2 ? 2 : 1; // compilation 88: 16 and 32
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
