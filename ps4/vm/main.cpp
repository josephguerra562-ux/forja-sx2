// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Boot the full PCSX2 VM on PS4, bypassing the incremental probe methodology.
#include "Config.h"
#include "PhysicalDisc.h"
#include "VMManager.h"
#include "Host.h"
#include "MTGS.h"
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "common/Console.h"
#include "common/Threading.h"
#include "common/HostSys.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "Memory.h"
#include "common/MemorySettingsInterface.h"
#include "INISettingsInterface.h"
#include "ForjaGameProfile.h"
#include "ForjaGsRules.h"
#include "Counters.h" // compilation 58: g_FrameCount, for the hang watch
#include "Patch.h"
#include "PatchDownload.h"
#include "ForjaReport.h"
#include "GameList.h"
#include "GameDatabase.h" // compilation 139: FPU exacto says when the game does not ask for it
#include "PerformanceMetrics.h"
#include "common/Timer.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "OrbisEEDiag.h" // 150 in the app: vm-eediag
#include "R3000A.h"
#include "IPU/IPU.h"
#include "IPU/IPU_MultiISA.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Pad/PadDualshock2.h"
#include <sys/stat.h>
#include "CDVD/CDVD.h"
#include "ps4/core/VulkanVideoOut.h"
#include "ps4/core/ForjaFlipThread.h" // 154 in the app
#include "ps4/core/ThreadCPUMetricsPS4.h"
#include "ps4/core/ThreadAffinityPS4.h"
#include "ps4/core/AudioStreamPS4.h"
#include "ps4/core/ProbeLoggerPS4.h"
#include "ps4/core/TelemetryPS4.h"
#include "ps4/core/IPUProfilePS4.h"
#include "ps4/core/EEEventProfilePS4.h"
#include "ps4/core/EEHotspotsPS4.h"
#include "ps4/core/GSProfilePS4.h"
#include "HostPollGate.h"
#include "GameMenu.h"
#include "ForjaLang.h"
#include "ForjaSettings.h"
#include "SessionLogs.h"
#include "SessionStats.h"
#include "ps4/core/WorkerProfilePS4.h"

#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <string>
#include <string_view>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>
#include <dirent.h>
#include <vector>

static int s_log = -1;

#if defined(PCSX2_PS4_VM_BOOT_JIT)
extern std::atomic<u32> g_orbis_rec_compiles, g_orbis_rec_discards, g_orbis_rec_page_resets, g_orbis_rec_full_resets;
extern bool g_ps4_vtlb_register_cache;
extern bool g_ps4_vtlb_bulk_register_cache;
extern bool g_ps4_skip_smc_checks;
#if defined(PCSX2_PS4_VM_BOOT_JIT)
// Compilation 141: "Código al día" (x86/ix86-32/iR5900.cpp).
extern std::atomic<u32> g_forja_fresh_loads, g_forja_fresh_load_blocks, g_forja_fresh_flushes, g_forja_fresh_scans;
extern std::atomic<u32> g_forja_fresh_flush_blocks, g_forja_fresh_scan_us;
#endif
extern bool g_ps4_bytecopy_fusion;
extern std::atomic<int> g_orbis_waitloop_ext;
extern std::atomic<u32> g_forja_resumes; // ps4/core/GSRendererPS4.cpp: the hitch detector skips the L3+R3 menu's gap
extern u64 g_ps4_vtlb_cached_quad_reads, g_ps4_vtlb_cached_writes;
extern u64 g_ps4_multiblock_wait_blocks, g_ps4_multiblock_wait_runs, g_ps4_multiblock_wait_iterations;
extern u64 g_ps4_bytecopy_blocks, g_ps4_bytecopy_runs, g_ps4_bytecopy_bytes;
extern u64 g_ps4_bytecopy_short, g_ps4_bytecopy_deadline, g_ps4_bytecopy_mapping, g_ps4_bytecopy_state, g_ps4_bytecopy_scratch;
extern u32 g_ps4_bytecopy_last_source, g_ps4_bytecopy_last_destination;
#endif

void ProbeLog(const char* format, ...)
{
	char text[4096];
	va_list args;
	va_start(args, format);
	const int length = std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (length <= 0)
		return;
	const auto bytes = std::min(static_cast<std::size_t>(length), sizeof(text) - 1);
	PS4ProbeLogger::Write(text, bytes);
}

static int MigrateLegacyLogs()
{
	static constexpr const char* names[] = {
		"PCSX2-vm-boot.log", "PCSX2-vm-stdout.log", "PCSX2-vm-stderr.log",
		"emulog.txt", "xmb-trace.log", "crash.log"};
	int migrated = 0;
	mkdir("/data/ForjaSX2/logs", 0777);
	for (const char* name : names)
	{
		char old_path[128];
		char new_path[128];
		snprintf(old_path, sizeof(old_path), "/data/ForjaSX2/%s", name);
		snprintf(new_path, sizeof(new_path), "/data/ForjaSX2/logs/%s", name);
		if (access(old_path, F_OK) != 0)
			continue;
		if (access(new_path, F_OK) == 0)
			snprintf(new_path, sizeof(new_path), "/data/ForjaSX2/logs/legacy-%s", name);
		if (rename(old_path, new_path) == 0)
		{
			migrated++;
			if (s_log >= 0)
				ProbeLog("Log migration: %s -> %s\n", old_path, new_path);
		}
	}
	return migrated;
}

static void ConsoleMessage(LOGLEVEL level, ConsoleColors, std::string_view message)
{
	// (AI-assisted) Preserve errors and lifecycle logs; omit successful shader plumbing.
	if (level > LOGLEVEL_WARNING &&
		(message.starts_with("PS4 shaderc:") || message.starts_with("PS4 shader cache:") ||
			message.starts_with("PS4 Vulkan shader module:")))
		return;
	ProbeLog("%.*s\n", static_cast<int>(message.size()), message.data());
}

// (AI-assisted) All control callbacks execute on the CPU thread, including interpreter polling.
extern void VMHostSetCPUThread();
extern void VMHostDrainCPUQueue();
extern u64 VMHostGetCPUQueueLockCount();
// PlatformVM.cpp: a "STAGE:" line with the PS4's free flexible memory; ps4/runtime/ps4_runtime.cpp: the heap's numbers
// and its lines (the overflow heap, a large mmap) since the last stage.
extern "C" void orbis_stage(const char* stage);
extern "C" size_t ps4rt_heap_high_water(void);
extern "C" int ps4rt_heap_log_next(char* out, size_t size);
static int s_pad = -1;
static std::string s_selected_game;
static bool s_use_vulkan_selector = false;
static u32 s_previous_pad_buttons = 0;
static u32 s_previous_host_buttons = 0;
// Forja SX2 (compilation 43): the second player. The PS4 ties each controller to a signed-in user: a second controller works
// once someone signs in with it (any user, or the PS4's temporary one: New User > Play, which isn't kept). UpdateSecondPad
// opens that user's pad and VMHostPoll gives it to the PS2's second port.
static int s_pad2 = -1;
static OrbisUserServiceUserId s_pad1_user = ORBIS_USER_SERVICE_USER_ID_INVALID;
static OrbisUserServiceUserId s_pad2_user = ORBIS_USER_SERVICE_USER_ID_INVALID;
static u32 s_previous_pad2_buttons = 0;
static double s_pad2_last_check = -10.0;
// Compilation 80: the controllers playing (1, or 2 with the second port open), for the session's summary (HostVM.cpp).
std::atomic<int> g_forja_pads{1};
static u32 s_blocked_pad_buttons = 0;
static bool s_menu_chord_latched = false;
// Forja SX2 (compilation 53): what opens this menu (ajustes.ini menu_boton): 0 L3+R3, 1 the touchpad's right half, 2 both. The
// touchpad is the PS2's Select (VMHostUpdateController), so only a click on its right half is the menu's and its left half
// stays Select; a press that opened the menu never reaches the game.
static int s_menu_button = 0;
static bool s_menu_touch_held = false;
static bool s_game_menu_open = false;
static unsigned s_game_menu_selection = 0;
// Forja SX2: the L3+R3 menu's save state slot (0-4, shown as 1-5), whether it holds a state, and the last action's result
// (PS4GameMenu::StateAction).
static unsigned s_state_slot = 0;
static bool s_state_slot_used = false;
static unsigned s_state_action = 0;
static unsigned s_game_upscale = 1;
static unsigned s_max_game_upscale = 1;
static unsigned s_game_aspect = 1;
static unsigned s_game_sharpness = 0; // Forja SX2 (compilation 44): the Nitidez row (SharpnessFromConfig)
static unsigned s_game_textures = 0; // Forja SX2 (compilation 44): the Texturas HD row (0 off, 1 on, 2 preloaded)
static bool s_game_show_performance_overlay = true;
static int s_game_ee_rate = -1;
// Forja SX2: console detection. The PS4's CPU runs at 1.6 GHz; a PS4 Pro runs it at ~2.13 GHz in Pro
// mode or with Boost Mode ("Modo optimizado") on. No system call tells an app which one it got, so the
// clock is measured: a chain of dependent adds takes one cycle each on Jaguar, so adds per second is the
// clock. ~40 ms, once, on the EE's own core before the VM starts.
static double s_console_cpu_ghz = 0.0;
static std::string s_console_system; // Forja 1.06: the system software's version (main), for the session's summary
static int s_console_neo = 0;        // ...and the Pro mode
static bool s_console_fast_cpu = false;
// Compilation 87: whether the console itself is a Pro, Boost Mode or not. The first report from another player (F-3420,
// FIFA Street 2, 2026-10-25) said "PS4 normal o Slim (o una Pro sin el Modo Boost)": the clock alone can't tell a Pro
// without Boost Mode from a Slim. The system says it: sceKernelHasNeoMode and sceKernelIsAuthenticNeo (both in the report,
// to check them against Joseph's Pro).
static int s_console_has_neo = 0, s_console_authentic_neo = 0;

// The console's name for the report and the channel's line.
static const char* ConsoleName()
{
	if (s_console_fast_cpu)
		return "PS4 Pro con el Modo Boost";
	if (s_console_has_neo > 0 || s_console_authentic_neo > 0)
		return "PS4 Pro sin el Modo Boost";
	return "PS4 normal o Slim";
}
// Compilation 59: for VMManager.cpp (LoadCoreSettings), which keeps a slow console at 2x at most. True until measured.
bool g_forja_console_fast_cpu = true;

static void MeasureConsoleCpu()
{
	constexpr u64 iterations = 20000000; // x4 dependent adds
	u64 value = 0;
	const auto start = Common::Timer::GetCurrentValue();
	for (u64 i = 0; i < iterations; ++i)
		asm volatile("add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0" : "+r"(value));
	const double seconds = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - start);
	s_console_cpu_ghz = seconds > 0.0 ? (static_cast<double>(iterations) * 4.0) / seconds / 1e9 : 0.0;
	s_console_fast_cpu = s_console_cpu_ghz >= 1.9;
	g_forja_console_fast_cpu = s_console_fast_cpu;
	ProbeLog("Console CPU: ~%.2f GHz (%s)\n", s_console_cpu_ghz,
		s_console_fast_cpu ? "PS4 Pro with Pro mode or Boost Mode" : "base PS4/Slim, or a Pro without Boost Mode");
}

// Forja SX2: automatic video mode state (see UpdateVideoMode).
extern u64 g_forja_ipu_decodes; // pcsx2/IPU/IPU.cpp: IDEC/BDEC commands since boot
static bool s_video_mode_enabled = true;
static bool s_video_active = false;
static int s_video_saved_rate = 0;
static u64 s_video_last_decodes = 0;
static double s_video_last_check = 0.0;
static double s_video_last_busy = -100.0;
static double s_video_since = 0.0;
// Compilation 87: when the last movie ended, and whether movies come and go (menus with movies behind them).
static double s_video_last_off = -100.0;
static bool s_video_flapping = false;
static unsigned s_video_busy_checks = 0;
static constexpr int VIDEO_EE_RATE = -3;
// Compilation 49: Modo video "Auto" ([Forja] VideoMode = 2, and the global "Sí" for a game whose profile does not set it).
// Compilation 51: it looks for the EE rate that shows the most movie pictures a second, from 31 to 100 %. The pictures come
// from the game's decoder, which needs EE cycles (too low a rate: fewer pictures, at full speed), while the PS4 runs the EE
// more slowly the higher the rate (too high: the whole movie slows down). Tales of the Abyss (2026-10-18): 17-20 pictures a
// second at 36 % (99 % speed), 21 at 50 % (94 %), 23 at 60 % (85 %) and 14 at 100 % (38 %); God of War II: 17-21 at 27 %,
// 23 at 60 % (92 %). The best rate differs by game and by movie, so it measures. Builds 51-64 compared the pictures a second
// between windows, which change with each scene; compilation 65 keeps the highest rate that plays the movie in real time
// (UpdateVideoMode). Builds 49-50 stepped on the speed alone, between 31 and 60 %: never above 60 %, and down for movies whose
// decoder was short of EE cycles; 65 also goes up, a step a movie, while the speed holds. The rate in use is where the game's
// next movie starts.
enum class VideoProbe
{
	Measure, // the rate in place
	Up,      // one step up, kept only if the movie still plays in real time
	Down,    // compilation 76: one step down, kept only if the movie keeps its pictures and runs faster
};
static bool s_video_auto = false;
static int s_video_auto_rate = VIDEO_EE_RATE; // where the game's next movie starts
// Compilation 65: the highest rate the game's movies may use: a rate found too slow (under 92 %, 90 since 66) lowers it for the rest of the
// game, so no movie tries it again (each change resets the EE recompiler, a stop of 100-250 ms: Crash Nitro Kart's intro had
// 8 to 12 in two minutes, 2026-10-20). And one step up a movie at most (compilation 68: a step a window while the movie is short of
// pictures, up to the ceiling).
static int s_video_auto_ceiling = 0;
// Compilation 68: the movie's pictures (in the PS2's seconds) before a step up, to see whether the step gave it more, and the
// windows in a row under the speed floor.
static double s_video_probe_pictures_before = 0.0;
static unsigned s_video_slow_windows = 0;
// Compilation 76: the other way. A movie with pictures to spare (30 % more than one every two PS2 vsyncs) that runs under 95 % tries
// a step down, and keeps it if it still has all its pictures and runs 4 points faster; otherwise that rate is the game's floor.
// Resident Evil 4 (2026-10-23): its movies showed 50 pictures a PS2 second, twice the 25 they need, at 85 % speed with the EE
// over its time, and the old Auto held -3 since it only went down under 78 %. The speed before the step, to compare.
static int s_video_auto_floor = -5; // VIDEO_AUTO_LOWEST
static double s_video_probe_speed_before = 0.0;
// Compilation 65: the video meter. The IPU's sampled profile (PS4IPUProfile: one worker call in 128, one register access in
// 1024; the GS thread writes "IPU profile" and "Video meter:" lines every 5 s) is on while a movie plays, to see how much of
// the EE thread the decoding takes; vm-profile-ipu keeps it on all the time and vm-no-ipu-profile off.
static bool s_ipu_profile_marker = false;
static bool s_ipu_profile_off = false;
// Compilation 65: the window's samples taken while the IPU decoded at a movie's pace. A window mostly without (a load, the gap
// between two movies) decides nothing: its speed says nothing about the movie, and a low one would lower the game's ceiling.
static unsigned s_video_probe_busy_samples = 0;

// Compilation 66: the EE thread's split as well (PS4EEEventProfile: one event in 1024, its phases and the game's code after it),
// in one line, "Video meter: EE thread"; vm-profile-ee / vm-profile-perf keep the full profile on all the time.
static bool s_ee_profile_marker = false;

static void ForjaVideoMeter(bool movie)
{
	const bool on = !s_ipu_profile_off && (s_ipu_profile_marker || movie);
	PS4IPUProfile::s_sampling.enabled = on;
	PS4IPUProfile::s_access_sampling.enabled = on;
	if (!s_ee_profile_marker)
	{
		PS4EEEventProfile::s_sampling.enabled = on;
		PS4EEEventProfile::s_sampling.gap_pending = false; // no gap from before the switch
		PS4EEEventProfile::s_sampling.gap_observer = nullptr; // no table of gaps, only the split
		PS4EEEventProfile::s_compact.store(on, std::memory_order_relaxed);
	}
}

static VideoProbe s_video_probe = VideoProbe::Measure;
static double s_video_probe_since = 0.0; // when the rate being measured was put in place
static double s_video_probe_fps_sum = 0.0;
static double s_video_probe_speed_sum = 0.0; // compilation 64: the speed over the same window
static unsigned s_video_probe_samples = 0;
static constexpr int VIDEO_AUTO_LOWEST = -5; // 31 %
static constexpr int VIDEO_AUTO_HIGHEST = 0; // 100 %
static constexpr double VIDEO_PROBE_SETTLE = 1.5;
static constexpr double VIDEO_PROBE_WINDOW = 3.0;
static constexpr double VIDEO_DECODES_PER_SECOND = 2000.0; // a movie: tens of thousands; gameplay: ~0
// Compilation 66: the Auto's speed floor, and "short of pictures": under 92 % of a picture every two PS2 vsyncs (30 at 60 Hz,
// 25 at 50 Hz, what PS2 movies use), counted in the PS2's own seconds (pictures a second over the speed).
// Compilation 68: 78 % and 95 %. A movie short of pictures is short of sound too: its player, short of EE cycles, leaves
// silences (Joseph's recordings of Tales of the Abyss, 2026-10-20: ~1.9 cuts a second at 36 %, ~1.2 at 44 %, "el audio
// sigue distorsionado"; at 57 % it had all its 30 pictures, and on 2026-10-15 no silences). So the movie gets the EE rate that
// gives it all its pictures, down to 78 % speed: the time stretching keeps its sound whole at that speed, a little slower.
static constexpr double VIDEO_SPEED_FLOOR = 78.0;
static constexpr double VIDEO_SHORT_OF_PICTURES = 0.95;
static constexpr double VIDEO_SPARE_PICTURES = 1.3; // compilation 76: s_video_auto_floor
static constexpr double VIDEO_SPARE_SLOW = 95.0;
static constexpr double VIDEO_DOWN_GAIN = 4.0;
extern double GetVerticalFrequency(); // pcsx2/GS.h (Counters.cpp): the PS2's refresh now
// Forja SX2: the running game's profile (pcsx2/ForjaGameProfile.h): its factory profile under the player's own. Read before
// the VM boots, from the library's scan of the image, and again if PCSX2 identifies another disc.
extern bool g_orbis_fg_wanted; // pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp: frame generation, read by the GS at every frame
extern bool g_orbis_fg_fluid;  // ...and its fluid mode (compilation 40: [Forja] FrameGen = 2)
extern bool g_orbis_fg_movie;  // ...and a movie playing (compilation 66: no pauses for the speed, OrbisFrameGenPacing.h)
extern bool g_orbis_fg_held;   // ...and held for a PAL game's movie on a 59.94 Hz TV (Forja 1.06, ForjaMovieSettings)
extern bool g_orbis_fg_gave_up; // ...off for this game, where it cost speed (Forja 1.06, OrbisFrameGenPacing.h GaveUp)
extern std::atomic<unsigned> g_orbis_fg_restart; // ...its "off for this game" starts over when this changes (Forja 1.06)
static ForjaGameProfile::Values s_profile;
static bool s_global_video_mode = true; // ajustes.ini
static bool s_global_frame_gen = false; // ajustes.ini
static int s_global_cpu_auto = ForjaSettings::Values{}.cpu_auto; // ajustes.ini (compilation 45)
static bool s_frame_gen_blocked = false; // the last try hung the GPU (fg-en-prueba): off for this launch
static int s_video_ee_rate = VIDEO_EE_RATE;
// Forja SX2 (compilation 35): the automatic PS2 CPU speed (UpdateCpuAuto). floor: 0 off, -1..-6 the lowest EE rate it may use;
// base: the game's own rate while it is lowered; steps: how many steps below base it runs now.
static int s_cpu_auto_floor = 0;
static int s_cpu_auto_base = 0;
static int s_cpu_auto_steps = 0;
static double s_cpu_auto_last_check = 0.0;
static double s_cpu_auto_last_change = -100.0;
static unsigned s_cpu_auto_slow_checks = 0;
static unsigned s_cpu_auto_room_checks = 0;
// Compilation 122: "CPU automático vuelve a subir" ([Forja] CpuAutoSube) and "Salto Auto afinado" ([Forja] SaltoAutoAfinado), both off until
// measured (herramientas/simulaciones/simular_automaticos_escenas.py). What the cycle skip's Auto tells UpdateCpuAuto: whether it can still
// help (a step up left and not blocked) and whether it is at the game's factory value. The CPU's tries up: when the last one was, how long
// until the next may be, and the wait after a failed one.
static bool s_cpu_auto_up = false, s_skip_auto_fine = false;
static bool s_skip_keeps_frames = false; // 146 in the app: [Forja] SaltoCuidaCuadros, "Al vuelo cuida los cuadros"
static int s_skip_techo16 = 48; // 154 in the app: [Forja] SaltoTecho, "Techo del salto" (48: none)
// Compilation 124: "Revisión rápida del código" for this game ([Forja] RevisionRapida), and the FTP marker vm-skip-smc (for every game).
static bool s_fast_code_game = false, s_fast_code_marker = false;
// Compilation 127: "Efectos listos al arrancar" for this game ([Forja] EfectosListos); the GS thread reads the atomic at every vsync.
static bool s_effects_ahead_game = false;
extern std::atomic<bool> g_forja_effects_ahead; // ps4/core/GSRendererPS4.cpp
// Compilation 128: "CPU sin tirones" for this game ([Forja] CpuSinTirones). The EE recompiler takes the mode at its next reset
// (x86/ix86-32/iR5900.cpp: g_forja_cpu_seamless is what is wanted, g_forja_cpu_seamless_active what its blocks were made with).
static bool s_cpu_seamless_game = false;
// Compilation 130: "Disco sin esperas" for this game ([Forja] DiscoSinEsperas); pcsx2/CDVD/CDVD.cpp reads the atomic at each read.
static bool s_disc_no_wait_game = false;
static int s_safe_mode = 0; // compilation 131: [Forja] ModoSeguro, 0 no, 1 partial, 2 total
static bool s_ps1_game = false; // compilation 131: the game chosen in the library is a PS1 one
// Compilation 132: the three new rows, and "Empujón de cuadros" while it has the PS2 CPU raised (the rate to go back to).
static bool s_code_ahead_game = false, s_vu_jumps_game = false, s_frame_push = false, s_push_active = false;
static int s_push_saved_rate = 0;
static std::string s_code_ahead_path;
#if defined(PCSX2_PS4_VM_BOOT_JIT)
extern std::atomic<bool> g_forja_code_ahead; // pcsx2/x86/ix86-32/iR5900.cpp
extern std::atomic<u32> g_forja_code_ahead_known, g_forja_code_ahead_done, g_forja_code_ahead_left;
extern std::atomic<u32> g_forja_code_ahead_started, g_forja_code_ahead_passes; // compilation 133
extern std::atomic<u32> g_forja_code_ahead_caught; // compilation 134
extern void ForjaCodeAheadBegin(const char* path, bool running);
extern bool ForjaCodeAheadSave(const char* path, bool force);
extern std::atomic<bool> g_forja_vu_jump_inline; // pcsx2/x86/microVU.cpp
#endif
static bool s_skip_fast_running = false; // compilation 131: "Salto de ciclo: Al vuelo" has the game's skip (UpdateCycleSkipFast)
extern std::atomic<int> g_forja_skip16; // pcsx2/MTVU.cpp (compilation 135)
extern std::atomic<s32> g_forja_vif1_fuse; // pcsx2/R5900.cpp (151 in the app, "VIF1 en menos turnos")
extern u64 g_forja_vif1_fused;
extern u64 g_forja_vif1_not_joined[5]; // pcsx2/R5900.cpp (152 in the app)
extern std::atomic<bool> g_forja_vu1_ahorro; // pcsx2/MTVU.cpp (152 in the app, "Ahorro del VU1")
extern std::atomic<int> g_orbis_vu1_dump_request; // pcsx2/MTVU.cpp: the VU thread runs OrbisVU1Dump (microVU.cpp) between two programs
static bool s_vu1_map_wanted = false; // 152 in the app: /data/ForjaSX2/vm-vu1-mapa at the game's start
static unsigned s_vu1_map_beats = 0;
extern u64 g_forja_mpg_total, g_forja_mpg_same;
extern std::atomic<int> g_forja_vu1_dead_flags; // pcsx2/MTVU.cpp (153 in the app, "Banderas muertas del VU1")
extern std::atomic<bool> g_forja_vu1_keep_regs; // pcsx2/MTVU.cpp (157 in the app, "Registros entre bloques")
extern std::atomic<u64> g_forja_vu1_kept_blocks, g_forja_vu1_kept_links;
extern std::atomic<bool> g_forja_vu1_three_op, g_forja_vu1_mem_reg; // pcsx2/MTVU.cpp (158 in the app)
extern std::atomic<u64> g_forja_vu1_three_op_copies, g_forja_vu1_mem_reg_loads, g_forja_vu1_mem_reg_reuses;
extern std::atomic<bool> g_forja_vu1_mac_just; // pcsx2/MTVU.cpp (159 in the app)
extern std::atomic<u64> g_forja_vu1_mac_left_out;
extern std::atomic<u64> g_forja_vu1_flag_risk_progs, g_forja_vu1_flag_missed; // pcsx2/MTVU.cpp (160 in the app)
extern std::atomic<bool> g_forja_vu1_one_count; // pcsx2/MTVU.cpp (161 in the app)
extern std::atomic<bool> g_forja_vu1_mac_jump; // pcsx2/MTVU.cpp (162 in the app)
extern std::atomic<bool> g_forja_vu1_mac_end; // pcsx2/MTVU.cpp (163 in the app)
extern std::atomic<int> g_forja_vif_tramos_mode; // pcsx2/Vif_Transfer.cpp (167 in the app, "VIF en tramos")
extern std::atomic<bool> g_forja_vif_tramos_check_only;
extern std::atomic<u64> g_forja_vif_tramos_runs, g_forja_vif_tramos_unpacks, g_forja_vif_tramos_words;
extern std::atomic<u64> g_forja_vif_tramos_checked, g_forja_vif_tramos_wrong;
extern u32 g_forja_vif_tramos_first_wrong[2][10];
extern std::atomic<u32> g_orbis_mtvu_ring_kb; // pcsx2/MTVU.cpp (169 in the app, "Cola corta del VU1")
extern std::atomic<bool> g_forja_vif_adelanto; // pcsx2/Vif1_Dma.cpp (170 in the app, "VIF1 por adelantado")
extern u64 g_orbis_vif1_tag_ids[8]; // pcsx2/Vif1_Dma.cpp (the VIF1 DMA tags read, per ID)
extern std::atomic<int> g_forja_gs_vt_once; // pcsx2/GS/Renderers/Common/GSVertexTrace.cpp (171 in the app, "Vértices una vez")
extern std::atomic<bool> g_forja_gs_vt_once_check_only;
extern std::atomic<u64> g_forja_gs_vt_once_checked, g_forja_gs_vt_once_wrong, g_forja_gs_vt_once_draws, g_forja_gs_vt_once_saved;
extern u32 g_forja_gs_vt_once_first_wrong[4];
extern std::atomic<int> g_forja_gs_ab; // pcsx2/GS/GSState.cpp (183 in the app, "A/B en un dibujo")
extern u64 g_forja_ab_switches, g_forja_ab_flushes, g_forja_ab_one_draws, g_forja_ab_one_segs, g_forja_ab_replays, g_forja_ab_replay_segs,
	g_forja_ab_capture_fail, g_forja_ab_mismatch;
extern u32 g_forja_ab_diff_offset[8], g_forja_ab_diff_n; // pcsx2/GS/Renderers/HW/GSRendererHW.cpp (183)
extern u64 g_forja_ab_switch_fail[8]; // pcsx2/GS/GSState.cpp (185)
extern std::atomic<bool> g_forja_ee_lzcnt; // pcsx2/x86/iMMI.cpp (173 in the app, "PLZCW rápido")
// 174 in the app, "Animación a medias" ([Forja] AnimMediasDir/AnimMediasFuncion/AnimMediasVacia). God of War II's map (the 171-173,
// herramientas/mapa_ee.py): the EE's busiest code in the Colossus is decoding animations (0x112380, ~13 %: each bone channel's keys for
// the current time). It is a virtual function: the scene's update walks its tree of objects and calls each one's slot 132; for the
// animation controllers that slot (the word at 0x2f57dc of their class's table) is 0x112380, and no code calls it any other way. At
// every vsync this puts that word to the game's own empty function (0x27b310, used by 79 entries of its tables) or back, by turns:
// the controllers decode one vsync out of two (their pose stays the last one in between). Only data changes: the recompiled code
// reads the word at each call. The word is only touched while it holds one of the two (anything else: not this program, left alone),
// and goes back to the function before every state save, so a saved state never keeps the animation off.
extern std::atomic<int> g_forja_vif_nocopy_mode; // pcsx2/MTVU.cpp (175 in the app, "VIF sin copia")
extern std::atomic<bool> g_forja_vif_nocopy_check_only;
extern std::atomic<u64> g_forja_vif_nocopy_checked, g_forja_vif_nocopy_wrong, g_forja_vif_nocopy_ptr, g_forja_vif_nocopy_late;
extern std::atomic<u64> g_forja_vif_nocopy_bytes_ram, g_forja_vif_nocopy_bytes_copied; // pcsx2/Vif_Unpack.cpp (176 in the app)
extern std::atomic<u16> g_forja_vif_page_good[];
extern std::atomic<u8> g_forja_vif_page_dynamic[];
extern u32 g_forja_vif_nocopy_first_wrong[2];
extern std::atomic<u32> g_forja_pal_detail; // pcsx2/GS/GSState.cpp (178 in the app, "Detalle de los cortes")
extern u64 g_forja_ab_joinable, g_forja_ab_runs, g_forja_pal_draws; // pcsx2/GS/GSState.cpp (180 in the app, "A/B en un dibujo")
extern u64 g_forja_ab_joinable_sc, g_forja_ab_broken, g_forja_ab_block[12], g_forja_ab_len[6], g_forja_draws_total; // the same (181)
static bool s_gs_cut_detail = false;
static bool s_anim_half_on = false;
static u32 s_anim_half_addr = 0, s_anim_half_fn = 0, s_anim_half_empty = 0;
static unsigned s_anim_half_n = 0;
static u64 s_anim_half_swaps = 0, s_anim_half_foreign = 0, s_anim_half_skipped = 0;

static void ForjaAnimHalfVsync()
{
	if (!s_anim_half_on)
		return;
	const u32 now = memRead32(s_anim_half_addr);
	if (now != s_anim_half_fn && now != s_anim_half_empty)
	{
		s_anim_half_foreign++;
		return;
	}
	const bool skip = (++s_anim_half_n & 1) != 0;
	const u32 want = skip ? s_anim_half_empty : s_anim_half_fn;
	if (now != want)
	{
		memWrite32(s_anim_half_addr, want);
		s_anim_half_swaps++;
	}
	s_anim_half_skipped += skip ? 1u : 0u;
}

// Before a state save (the VM paused): the table's word back to the function.
static void ForjaAnimHalfRestore()
{
	if (s_anim_half_on && memRead32(s_anim_half_addr) == s_anim_half_empty)
		memWrite32(s_anim_half_addr, s_anim_half_fn);
}
static bool s_vu1_ring_alternate = false; // 169 in the app: VU1ColaAlterna (the 5 s meter turns the lap)
static bool s_vu1_ring_logged = false;
extern std::atomic<bool> g_forja_fg_margin; // pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp (compilation 136)
extern std::atomic<unsigned> g_forja_vsync_load;
extern std::atomic<int> g_forja_deinterlace_factory; // compilation 137: pcsx2/VMManager.cpp, the game's factory deinterlace mode
extern std::atomic<bool> g_forja_deinterlace_auto;   // and whether the player's Auto was turned into it
static bool s_fg_margin = false; // compilation 136: [Forja] FGConMargen for this game
extern std::atomic<bool> g_forja_disc_no_wait; // pcsx2/CDVD/CDVD.cpp
extern std::atomic<u32> g_forja_disc_deferrals;
extern std::atomic<u64> g_forja_disc_wait_us; // ps4/core/GSRendererPS4.cpp
#if defined(PCSX2_PS4_VM_BOOT_JIT)
extern std::atomic<bool> g_forja_cpu_seamless;
extern bool g_forja_cpu_seamless_active;
#endif
// The video mode and the CPU automático change the rate here, not through the settings: with "CPU sin tirones" on, the EE blocks'
// cycle adds are rewritten in place (x86/ix86-32/iR5900.cpp; weak: the builds without it reset as before); otherwise, or when it
// cannot (rates over 130 %, too many adds), the EE recompiler starts over as before.
extern bool ForjaEERecRepatchCycleRate(int old_rate, int new_rate) __attribute__((weak));
static void ForjaEERateChanged(int old_rate)
{
	const int new_rate = EmuConfig.Speedhacks.EECycleRate;
	if (old_rate != new_rate && ForjaEERecRepatchCycleRate && ForjaEERecRepatchCycleRate(old_rate, new_rate))
		return;
	Cpu->Reset();
}
static bool s_skip_auto_can_help = false, s_skip_auto_at_factory = true;
static unsigned s_cpu_auto_good_checks = 0;
static double s_cpu_auto_probe_at = -1000.0, s_cpu_auto_no_probe_until = 0.0, s_cpu_auto_probe_wait = 60.0;
// Compilation 130: "CPU al vuelo", the CPU automático's fast mode while "CPU sin tirones" is in the EE blocks (a change costs ~0.5 ms
// instead of a recompile). Its guard against going up and down: when a step up has to come down within 3 s, no step up for a while
// (5 s, doubled up to 20 s; back to 5 after 30 s without one).
// Compilation 125: when the CPU last came down (no step up for 60 s after it).
static double s_cpu_auto_last_down = -1000.0;
// Forja SX2 (compilation 47): "Saltar cuadros" (UpdateGsSkip). mode: 0 off, 1 auto, 2 always; the GS reads the atomic.
extern std::atomic<bool> g_forja_gs_skip_active;     // ps4/core/GSRendererPS4.cpp
extern std::atomic<bool> g_forja_gs_skip_director;   // compilation 140: the director's gate (GSRendererPS4.cpp)
extern std::atomic<unsigned> g_forja_gs_skipped_frames;
extern std::atomic<unsigned> g_forja_gs_skip_stats[6]; // compilation 62: GSRendererPS4.cpp, VSync and ForjaGsSkipNote
extern std::atomic<bool> g_forja_gs_skip_paused;      // Forja 1.06: its guard has it paused (ForjaSkipGuard.h)
extern std::atomic<unsigned> g_forja_gs_skip_every;    // compilation 63: one frame in N left out
extern std::atomic<bool> g_forja_gs_skip_guard;       // compilation 82: the guard on (always since 86)
extern std::atomic<bool> g_forja_gs_skip_quick;       // compilation 86: "1 de 2 rápido" (SaltarCuadros 3), short pauses
extern std::atomic<bool> g_forja_gs_skip_two;         // compilation 88: "2 de 3" (SaltarCuadros 4)
extern std::atomic<bool> g_forja_gs_skip_hide_shown;  // compilation 146: [Forja] SaltarCuadrosOcultaPantalla (GSRendererPS4.cpp)
extern std::atomic<bool> g_forja_gs_skip_lag;         // compilation 90: "Al vuelo" (SaltarCuadros 5)
extern std::atomic<unsigned> g_forja_gs_skip_lag_percent; // compilation 105: at most this share of the frames (UpdateGsSkip)
// Compilation 82: the Guitar Hero games' crowd, drawn in software once every N of its passes (GSHwHack.cpp, GSC_GuitarHero;
// [Forja] PublicoCada). Without a value, one in two: every one is PCSX2's, and cost Guitar Hero III ~36 ms a frame.
extern std::atomic<u32> g_forja_crowd_every;
// Compilation 116: "Grabar dibujos en otro hilo" (No tocar; [Forja] GrabarEnOtroHilo): the draws' Vulkan calls on their own
// thread. The GS thread takes it at its next command buffer (GS/Renderers/Vulkan/ForjaVkRecorder.h, FrameBoundary).
extern std::atomic<bool> g_forja_draw_thread_wanted;
static bool s_draw_thread = false;
// Compilation 117: "Dibujo por software con traductor" (No tocar; [Forja] TraductorSoftware): the software draws with the
// generated code (the JIT) instead of the C version (GS/Renderers/SW/GSDrawScanline.cpp, read at each draw's setup).
extern std::atomic<bool> g_forja_sw_jit;
static bool s_sw_jit = false;
// Compilation 120: "Escrituras rápidas del IOP" (No tocar; [Forja] IopRapido, off until measured): an IOP write into its RAM
// clears translated code only where the IOP has some (pcsx2/IopMem.cpp, x86/iR3000A.cpp ForjaIopMayHaveCode). Bully's EE
// spent 1.9 % in recClearIOP and 1.9 % in iopMemWrite32 (the 119's sampler, 2026-11-04), nearly all of it on RAM with no code.
extern std::atomic<bool> g_forja_iop_fast;
static bool s_iop_fast = false;
// Compilation 88: 16 and 32 too (Joseph, 2026-10-25, Guitar Hero III: "el público sigue siendo el problema").
// Compilation 137: "Al vuelo" ([Forja] PublicoCada = 99) after them: ForjaCrowdVsync, below, picks 1, 2 or 4 at each vsync.
static constexpr unsigned CROWD_AUTO = 99u;
static constexpr unsigned CROWD_EVERY_STEPS[] = {1u, 2u, 4u, 8u, 16u, 32u, CROWD_AUTO};
static constexpr unsigned CROWD_EVERY_DEFAULT = 2u;
static unsigned s_crowd_every = CROWD_EVERY_DEFAULT;
static bool s_crowd_auto = false; // compilation 137: Al vuelo chosen and the game draws its crowd in software
static int s_gs_skip_mode = 0;
// Compilation 63: Auto's steps, one frame in N left out (index 0: every frame drawn), and the one it is on.
static constexpr unsigned GS_SKIP_STEPS[] = {0u, 8u, 6u, 5u, 4u, 3u, 2u};
static unsigned s_gs_skip_step = 0;
// Compilation 64: Auto's samples (every 0.5 s) since its last decision.
static double s_gs_skip_speed_sum = 0.0, s_gs_skip_gs_sum = 0.0, s_gs_skip_others_sum = 0.0;
static unsigned s_gs_skip_samples = 0;
static double s_gs_skip_last_check = 0.0;
static double s_gs_skip_last_change = -100.0;
static double s_gs_skip_last_report = 0.0;
static unsigned s_gs_skip_slow_checks = 0;
static unsigned s_gs_skip_room_checks = 0;
// Compilation 76: Auto checks that it really leaves frames out. A game that draws its picture into a buffer of its own and copies
// it to la pantalla at the end leaves the skip nothing to leave out: Shadow of the Colossus (its costliest draws go to 01c00)
// went up to one in 2 with no frame skipped and no change in speed (2026-10-23), as God of War II had. After three of the 5 s
// reports with Auto on, outside a movie, with nothing left out and no pause of the guard, it draws every frame again and stays
// so for this game (until another game, or the row changed by hand).
static double s_gs_skip_on_since = -1.0; // when Auto left step 0 (-1: at step 0)
static unsigned s_gs_skip_reports_on = 0; // the reports since then, outside movies
static bool s_gs_skip_left_any = false;   // anything left out (or the guard paused) since then
static bool s_gs_skip_useless = false;    // this game leaves the skip nothing to leave out
// Compilation 76: when frame generation last generated a frame (g_orbis_fg_engaged), to tell "on and generating" from "on and
// idle": a 50 fps game on a 59.94 Hz TV has no room for generated frames ("Sí"), and in a pause the game runs without them.
extern bool g_orbis_fg_engaged; // pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp, written by the GS thread at each frame
static double s_fg_engaged_seen = -100.0;

// Compilation 65: what a movie gets besides its EE rate, the game's own settings back when it ends. Both use other parts of the
// PS4 than the EE's core, the one a movie fills:
// - frame generation, when the game has it on ("Sí" or "Fluida"), in its fluid mode: at the rate that plays a movie in real
//   time its decoder drops some pictures, so the movie's frames are of uneven lengths, which "Sí" waits out (Tales of the
//   Abyss, 2026-10-20: "the game's frames aren't of one length" through most of its movies); the fluid mode fills each
//   refresh the TV would repeat, on the GPU and the GS thread, both nearly idle in a movie (GS 7 %).
// - Saltar cuadros paused: a movie's frames are cheap for the GS, and each one left out is a picture of the movie lost. Auto
//   had kept the step the game was on when the movie began (UpdateGsSkip doesn't run in a movie).
static bool s_fg_fluid_profile = false; // the game's own fluid mode (FrameGen = 2), ApplyGameProfile
static bool s_iop_sleep_allowed = true; // compilation 67: vm-no-iop-sleep
static bool s_movie_settings = false;
// Compilation 122: "IOP dormido en el juego" ([Forja] IopDormido, off until measured): the IOP's sleep of compilation 67 in the game too,
// not only in movies. The IOP spends most of its time in its idle loop waiting for the next event, and each of its turns costs the EE
// thread (~60 ms a second in Bully, the 119's sampler, 2026-11-04). Its wake-ups are the same as in a movie (R3000A.cpp, ForjaIopMaySleep).
static bool s_iop_sleep_game = false;
static void ForjaApplyIopSleep()
{
	g_forja_iop_sleep = (s_movie_settings || s_iop_sleep_game) && s_iop_sleep_allowed;
	g_forja_iop_sleep_poll = s_iop_sleep_game && s_iop_sleep_allowed; // compilation 123: the reading loops, with the row only
}

static void ForjaApplyFgFluid()
{
	g_orbis_fg_fluid = s_fg_fluid_profile || (s_movie_settings && g_orbis_fg_wanted && !g_orbis_fg_held);
}

static void ForjaMovieSettings(bool movie)
{
	if (movie == s_movie_settings)
		return;
	s_movie_settings = movie;
	const bool fluid_before = g_orbis_fg_fluid;
	// Forja 1.06: no generated frames in a PAL game's movie while the TV is at 59.94 Hz (the PS4 refuses 50 Hz, VulkanVideoOut.h):
	// its 25 pictures a second are 2.4 refreshes each and the in-between frame came by turns (OrbisFrameGenPacing.h, SetHeld).
	// God of War II's movies played whole, 25 pictures of 25 at full speed, and Joseph still saw them fail (2026-10-21): of the
	// games tested, the only one with PAL movies and frame generation on (Crash Nitro Kart's, PAL without it, and the NTSC
	// ones with it were fine). The game's own setting is back after the movie.
	const double ps2_hz = GetVerticalFrequency();
	const int tv_millihz = PS4Port::g_output_millihz.load(std::memory_order_relaxed);
	g_orbis_fg_held = movie && ps2_hz > 49.0 && ps2_hz < 51.0 && (tv_millihz < 49000 || tv_millihz > 51000);
	ForjaApplyFgFluid();
	// Compilation 66: in a movie the EE's core is the limit and frame generation runs on the GPU and the GS thread, so its
	// safety net (a pause after 3 s under 95 %) only took the generated frames away: Tales of the Abyss's movies at 91-93 %,
	// 2026-10-20 ("about as with them"), 10 s of the game's own uneven frames each time.
	g_orbis_fg_movie = movie;
	// Compilation 67: the IOP asleep in its idle loop between the events it needs (pcsx2/R3000A.cpp, ForjaIopMaySleep).
	ForjaApplyIopSleep(); // compilation 122: in the game too, with its row
	if (movie)
	{
		g_forja_gs_skip_active.store(false, std::memory_order_relaxed);
	}
	else if (s_gs_skip_mode >= 2) // compilation 82: 3 too, without the guard
	{
		g_forja_gs_skip_active.store(true, std::memory_order_relaxed);
	}
	else if (s_gs_skip_mode == 1)
	{
		// Auto starts over from drawing every frame, as after the game's start.
		s_gs_skip_step = 0;
		s_gs_skip_slow_checks = s_gs_skip_room_checks = 0;
		s_gs_skip_speed_sum = s_gs_skip_gs_sum = s_gs_skip_others_sum = 0.0;
		s_gs_skip_samples = 0;
		g_forja_gs_skip_every.store(2u, std::memory_order_relaxed);
		g_forja_gs_skip_active.store(false, std::memory_order_relaxed);
	}
	ProbeLog("Movie settings: %s (frame generation %s%s; Saltar cuadros %s)\n", movie ? "on" : "off",
		!g_orbis_fg_wanted ? "off" : g_orbis_fg_held ? "held, a PAL movie on a 59.94 Hz TV" : g_orbis_fg_fluid ? "fluid" : "steady",
		fluid_before != g_orbis_fg_fluid ? ", changed" : "",
		movie ? "paused" : s_gs_skip_mode == 5 ? "on the fly again" : s_gs_skip_mode == 4 ? "two in three again" :
		s_gs_skip_mode >= 2 ? "one in two again" :
		s_gs_skip_mode == 1 ? "auto from every frame" : "off");
}

// Compilation 67: a movie cut short by the menu (a state loaded, the game restarted, its settings reset or recommended): what it
// had changed besides its EE rate goes back too. Until now only s_video_active did, and the movie's settings (frame generation
// fluid, Saltar cuadros paused, the meter, the IOP asleep) stayed on in the game until the next movie ended.
static void ForjaEndMovieSettings()
{
	ForjaVideoMeter(false);
	ForjaMovieSettings(false);
}
// Forja SX2 (compilation 37): the TV at 50 Hz for PAL games (UpdateOutputRate); vm-sin-50hz turns it off.
static bool s_output_50hz_enabled = true;
static double s_output_last_check = 0.0;
static double s_output_pal_since = -1.0;
static double s_profile_last_check = 0.0;
static u32 s_game_menu_direction = 0;
static double s_game_menu_next_repeat = 0.0;
static std::atomic<bool> s_return_to_main_menu{false};
static Common::Timer s_run_timer;
static double s_last_log = -5.0;
static double s_last_input = -1.0;
static double s_last_pause_log = -5.0;
static u32 s_last_ee_cycle = 0;
static u64 s_host_poll_calls = 0;
static bool s_log_direct_boot = false; // compilation 67: the log line by line while a game starts (PS4ProbeLogger::SetDirect)
static u64 s_last_host_poll_calls = 0;
static HostPollGate s_host_poll_gate;
static u64 s_host_clock_checks = 0;
static u64 s_last_host_clock_checks = 0;
static bool s_verbose_diag = false;

// Forja SX2: analog sensitivity, as PCSX2's "Analog Sensitivity" (133% by default for DualShock 2 pads).
// A real DS2 stick reaches its end values well before the end of its travel, and on the diagonals too; a
// DS4 passed through 1:1 often stays short of them, so games that pick walk or run from how far the stick
// is pushed (Mega Man X Command Mission) walked with the stick held at the edge. Optional override:
// /data/ForjaSX2/sensibilidad.txt with a percentage (100-200).
static float s_stick_scale = 1.33f;

// Compilation 146: the base, once. ajustes.ini written before it gets the new base (ForjaSettings::Load), and the players' per-game
// profiles lose the CPU, cycle skip and Saltar cuadros keys of the old tests, so the base and the factory profiles count again (a copy
// of each file goes to gamesettings/antes-146 first). Resolution, aspect, patches, the crowd, the guardian's list, the learnt skip and
// the rest stay.
// 147 in the app, once (marker /data/ForjaSX2/ajustes-147): every game with the PS2 CPU native. Joseph (2026-11-19): "que todos los juegos
// vayan nativos con el 100 % en la CPU del emulador". The players' own CPU and CPU automático for a game ([EmuCore/Speedhacks] EECycleRate,
// [Forja] CpuAuto, from L3+R3 or Options) go, a copy of each file first in gamesettings/antes-147; ajustes.ini's base is 147 (ForjaSettings.h).
// Nothing else of the player's is touched (huellas, resolution, frame generation, patches): Dragon Ball's huella stays as the player left it
// (its fights without it, the 124: 92 % and 28.2 frames, against 94.8 % and 28.4 with it: the same).
// 150 in the app, once (marker /data/ForjaSX2/ajustes-150): a game's own "CPU automático: No" ([Forja] CpuAuto = 0) goes (a copy of
// the file first, in gamesettings/antes-150). The 147 took every game's own CPU out (ForjaApplyNative147); in the 147 and the 148 the
// base had the CPU automático off, and a "No" saved then (God of War II and Need for Speed, 2026-11-19: L3+R3 > CPU automático pressed
// round to "No") is the base of that time, not a choice against the fallback. Joseph chose the fallback for every game (2026-11-19,
// "Sí, nativa con respaldo"); with the "No" those two kept the CPU native even where the EE held them back. A game's own floor (-1 to
// -6) stays: that one the player picked.
static void ForjaApplyRespaldo150()
{
	constexpr const char* marker = "/data/ForjaSX2/ajustes-150";
	if (access(marker, F_OK) == 0)
		return;
	const std::string dir = "/data/ForjaSX2/gamesettings";
	const std::string backup = dir + "/antes-150";
	unsigned files = 0;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			const std::string name = e->d_name;
			if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ini") != 0)
				continue;
			const std::string path = dir + "/" + name;
			std::string text;
			if (std::FILE* f = std::fopen(path.c_str(), "rb"))
			{
				char buffer[4096];
				size_t n;
				while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
					text.append(buffer, n);
				std::fclose(f);
			}
			else
				continue;
			std::string section, kept;
			bool dropped = false;
			size_t at = 0;
			while (at < text.size())
			{
				size_t end = text.find('\n', at);
				const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
				at = end == std::string::npos ? text.size() : end + 1;
				std::string bare = line;
				while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r' || bare.back() == ' ' || bare.back() == '\t'))
					bare.pop_back();
				const size_t lead = bare.find_first_not_of(" \t");
				bare = lead == std::string::npos ? std::string() : bare.substr(lead);
				if (!bare.empty() && bare.front() == '[' && bare.back() == ']')
					section = bare.substr(1, bare.size() - 2);
				else if (const size_t eq = bare.find('='); section == "Forja" && eq != std::string::npos && bare[0] != ';' && bare[0] != '#')
				{
					std::string key = bare.substr(0, eq), value = bare.substr(eq + 1);
					while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
						key.pop_back();
					const size_t v = value.find_first_not_of(" \t");
					value = v == std::string::npos ? std::string() : value.substr(v);
					if (key == "CpuAuto" && (value == "0" || value == "-0"))
					{
						dropped = true;
						continue;
					}
				}
				kept += line;
			}
			if (!dropped)
				continue;
			mkdir(backup.c_str(), 0777);
			if (std::FILE* f = std::fopen((backup + "/" + name).c_str(), "wb"))
			{
				std::fwrite(text.data(), 1, text.size(), f);
				std::fclose(f);
			}
			else
				continue; // no copy, no change
			const std::string temporary = path + ".tmp";
			if (std::FILE* f = std::fopen(temporary.c_str(), "wb"))
			{
				const bool written = std::fwrite(kept.data(), 1, kept.size(), f) == kept.size();
				if (std::fclose(f) == 0 && written && std::rename(temporary.c_str(), path.c_str()) == 0)
				{
					files++;
					ProbeLog("Respaldo 150: %s had \"CPU automático: No\" from the 147-148; the fallback is back \n", name.c_str());
				}
			}
		}
		closedir(d);
	}
	if (std::FILE* f = std::fopen(marker, "w"))
	{
		std::fprintf(f, "150: %u perfiles sin su \"CPU automatico: No\" de la 147-148 (copia en gamesettings/antes-150)\n", files);
		std::fclose(f);
	}
}

// 149 in the app, once (marker /data/ForjaSX2/ajustes-149): a player's deinterlace for a game whose factory profile has a measured one
// goes (a copy of the file first, in gamesettings/antes-149). Guitar Hero III (2026-11-19, the 148): its factory Blend (6) was hidden by
// a Bob (4) left from trying the modes in the 145, and Bob makes the thin lines shimmer by itself (it shows each field alone, one line
// up or down every refresh): the neck, the crowd, the grass. With Blend the 148's fix for its flicker applies (GSRendererPS4.cpp).
// L3+R3 > Imagen still changes it for whoever wants another.
static void ForjaApplyDeinterlace149()
{
	constexpr const char* marker = "/data/ForjaSX2/ajustes-149";
	if (access(marker, F_OK) == 0)
		return;
	const std::string dir = "/data/ForjaSX2/gamesettings";
	const std::string backup = dir + "/antes-149";
	unsigned files = 0;
	const auto read_all = [](const std::string& path, std::string& text) {
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f)
			return false;
		char buffer[4096];
		size_t n;
		while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
			text.append(buffer, n);
		std::fclose(f);
		return true;
	};
	// The text without [EmuCore/GS] deinterlace_mode, and whether it had one.
	const auto without_mode = [](const std::string& text, bool* had) {
		std::string section, kept;
		*had = false;
		size_t at = 0;
		while (at < text.size())
		{
			size_t end = text.find('\n', at);
			const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
			at = end == std::string::npos ? text.size() : end + 1;
			std::string bare = line;
			while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r' || bare.back() == ' ' || bare.back() == '\t'))
				bare.pop_back();
			const size_t lead = bare.find_first_not_of(" \t");
			bare = lead == std::string::npos ? std::string() : bare.substr(lead);
			if (!bare.empty() && bare.front() == '[' && bare.back() == ']')
				section = bare.substr(1, bare.size() - 2);
			else if (const size_t eq = bare.find('='); section == "EmuCore/GS" && eq != std::string::npos && bare[0] != ';')
			{
				std::string key = bare.substr(0, eq);
				while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
					key.pop_back();
				if (key == "deinterlace_mode")
				{
					*had = true;
					continue;
				}
			}
			kept += line;
		}
		return kept;
	};
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			const std::string name = e->d_name;
			const size_t cut = name.find('_');
			if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ini") != 0 || cut == std::string::npos)
				continue;
			const std::string path = dir + "/" + name;
			std::string text, factory;
			bool player_has = false, factory_has = false;
			if (!read_all(path, text))
				continue;
			const std::string kept = without_mode(text, &player_has);
			if (!player_has || !read_all(ForjaGameProfile::FactoryPath(name.substr(0, cut)), factory))
				continue;
			without_mode(factory, &factory_has);
			if (!factory_has)
				continue;
			mkdir(backup.c_str(), 0777);
			if (std::FILE* f = std::fopen((backup + "/" + name).c_str(), "wb"))
			{
				std::fwrite(text.data(), 1, text.size(), f);
				std::fclose(f);
			}
			else
				continue; // no copy, no change
			const std::string temporary = path + ".tmp";
			if (std::FILE* f = std::fopen(temporary.c_str(), "wb"))
			{
				const bool written = std::fwrite(kept.data(), 1, kept.size(), f) == kept.size();
				if (std::fclose(f) == 0 && written && std::rename(temporary.c_str(), path.c_str()) == 0)
				{
					files++;
					ProbeLog("Deinterlace 149: %s had its own mode; the game's measured one comes back \n", name.c_str());
				}
			}
		}
		closedir(d);
	}
	if (std::FILE* f = std::fopen(marker, "w"))
	{
		std::fprintf(f, "149: %u perfiles con el desentrelazado medido de vuelta (copia en gamesettings/antes-149)\n", files);
		std::fclose(f);
	}
}

static void ForjaApplyNative147()
{
	constexpr const char* marker = "/data/ForjaSX2/ajustes-147";
	if (access(marker, F_OK) == 0)
		return;
	static constexpr const char* drop[][2] = {{"EmuCore/Speedhacks", "EECycleRate"}, {"Forja", "CpuAuto"}};
	const std::string dir = "/data/ForjaSX2/gamesettings";
	const std::string backup = dir + "/antes-147";
	unsigned files = 0, keys = 0;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			const std::string name = e->d_name;
			if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ini") != 0)
				continue;
			const std::string path = dir + "/" + name;
			std::string text;
			if (std::FILE* f = std::fopen(path.c_str(), "rb"))
			{
				char buffer[4096];
				size_t n;
				while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
					text.append(buffer, n);
				std::fclose(f);
			}
			else
				continue;
			std::string section, kept;
			unsigned dropped = 0;
			size_t at = 0;
			while (at < text.size())
			{
				size_t end = text.find('\n', at);
				const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
				at = end == std::string::npos ? text.size() : end + 1;
				std::string bare = line;
				while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r' || bare.back() == ' '))
					bare.pop_back();
				const size_t lead = bare.find_first_not_of(" \t");
				bare = lead == std::string::npos ? std::string() : bare.substr(lead);
				if (!bare.empty() && bare.front() == '[' && bare.back() == ']')
					section = bare.substr(1, bare.size() - 2);
				else if (const size_t eq = bare.find('='); eq != std::string::npos && bare[0] != ';' && bare[0] != '#')
				{
					std::string key = bare.substr(0, eq);
					while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
						key.pop_back();
					bool remove = false;
					for (const auto& pair : drop)
						remove |= section == pair[0] && key == pair[1];
					if (remove)
					{
						dropped++;
						continue;
					}
				}
				kept += line;
			}
			if (dropped == 0)
				continue;
			mkdir(backup.c_str(), 0777);
			if (std::FILE* f = std::fopen((backup + "/" + name).c_str(), "wb"))
			{
				std::fwrite(text.data(), 1, text.size(), f);
				std::fclose(f);
			}
			else
				continue; // no copy, no change
			const std::string temporary = path + ".tmp";
			if (std::FILE* f = std::fopen(temporary.c_str(), "wb"))
			{
				const bool written = std::fwrite(kept.data(), 1, kept.size(), f) == kept.size();
				if (std::fclose(f) == 0 && written && std::rename(temporary.c_str(), path.c_str()) == 0)
				{
					files++;
					keys += dropped;
				}
			}
		}
		closedir(d);
	}
	if (std::FILE* f = std::fopen(marker, "w"))
	{
		std::fprintf(f, "147: CPU nativa; %u perfiles sin su CPU o CPU automatico propios (%u claves; copia en gamesettings/antes-147)\n", files, keys);
		std::fclose(f);
	}
	ProbeLog("Native 147: %u player profiles had their own CPU or CPU automático taken out (%u keys; copies in %s)\n", files, keys,
		backup.c_str());
}

static void ForjaApplyBase()
{
	ForjaSettings::Values settings = ForjaSettings::Load();
	if (settings.migrated)
		ProbeLog("Base 149: ajustes.ini given the new base (CPU 100 %% native, the CPU automático down to 31 %% as its fallback), saved=%d\n",
			ForjaSettings::Save(settings) ? 1 : 0);
	constexpr const char* marker = "/data/ForjaSX2/ajustes-base-146";
	if (access(marker, F_OK) == 0)
		return;
	static constexpr const char* drop[][2] = {{"EmuCore/Speedhacks", "EECycleRate"}, {"EmuCore/Speedhacks", "EECycleSkip"},
		{"EmuCore/Speedhacks", "EECycleSkipQuarters"}, {"EmuCore/Speedhacks", "EECycleSkipAuto"}, {"EmuCore/Speedhacks", "fastCDVD"},
		{"EmuCore/GS", "HWDownloadMode"}, {"Forja", "CpuAuto"}, {"Forja", "SaltarCuadros"}, {"Forja", "CpuAutoSube"},
		{"Forja", "SaltoAutoAfinado"}, {"Forja", "IopRapido"}, {"Forja", "IopDormido"}, {"Forja", "GrabarEnOtroHilo"},
		{"Forja", "TraductorSoftware"}}; // frame generation stays: each player's own choice for a game
	const std::string dir = "/data/ForjaSX2/gamesettings";
	const std::string backup = dir + "/antes-146";
	mkdir(backup.c_str(), 0777);
	unsigned files = 0, keys = 0;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			const std::string name = e->d_name;
			if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ini") != 0)
				continue;
			const std::string path = dir + "/" + name;
			std::string text;
			if (std::FILE* f = std::fopen(path.c_str(), "rb"))
			{
				char buffer[4096];
				size_t n;
				while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
					text.append(buffer, n);
				std::fclose(f);
			}
			else
				continue;
			std::string section, kept;
			unsigned dropped = 0;
			size_t at = 0;
			while (at < text.size())
			{
				size_t end = text.find('\n', at);
				const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
				at = end == std::string::npos ? text.size() : end + 1;
				std::string bare = line;
				while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r' || bare.back() == ' '))
					bare.pop_back();
				const size_t lead = bare.find_first_not_of(" \t");
				bare = lead == std::string::npos ? std::string() : bare.substr(lead);
				if (!bare.empty() && bare.front() == '[' && bare.back() == ']')
					section = bare.substr(1, bare.size() - 2);
				else if (const size_t eq = bare.find('='); eq != std::string::npos && bare[0] != ';' && bare[0] != '#')
				{
					std::string key = bare.substr(0, eq);
					while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
						key.pop_back();
					bool remove = false;
					for (const auto& pair : drop)
						remove |= section == pair[0] && key == pair[1];
					if (remove)
					{
						dropped++;
						continue;
					}
				}
				kept += line;
			}
			if (dropped == 0)
				continue;
			if (std::FILE* f = std::fopen((backup + "/" + name).c_str(), "wb"))
			{
				std::fwrite(text.data(), 1, text.size(), f);
				std::fclose(f);
			}
			else
				continue; // no copy, no change
			const std::string temporary = path + ".tmp";
			if (std::FILE* f = std::fopen(temporary.c_str(), "wb"))
			{
				const bool written = std::fwrite(kept.data(), 1, kept.size(), f) == kept.size();
				if (std::fclose(f) == 0 && written && std::rename(temporary.c_str(), path.c_str()) == 0)
				{
					files++;
					keys += dropped;
				}
			}
		}
		closedir(d);
	}
	if (std::FILE* f = std::fopen(marker, "w"))
	{
		std::fprintf(f, "base 146: %u perfiles, %u claves viejas quitadas (copia en gamesettings/antes-146)\n", files, keys);
		std::fclose(f);
	}
	ProbeLog("Base 146: %u player profiles had their old CPU and skip keys taken out (%u keys; copies in %s)\n", files, keys,
		backup.c_str());
}

static void LoadStickSensitivity()
{
	s_stick_scale = static_cast<float>(ForjaSettings::Load().stick) / 100.0f;
	if (std::FILE* file = std::fopen("/data/ForjaSX2/sensibilidad.txt", "r"))
	{
		int percent = 0;
		if (std::fscanf(file, "%d", &percent) == 1 && percent >= 100 && percent <= 200)
			s_stick_scale = static_cast<float>(percent) / 100.0f;
		std::fclose(file);
	}
	ProbeLog("Controller: analog sensitivity %.0f%% (/data/ForjaSX2/sensibilidad.txt overrides, 100-200)\n",
		s_stick_scale * 100.0f);
}

// Converts one DS4 stick (0..255 per axis, centre 128) to the DS2's values: a small round deadzone for
// centre noise, then the sensitivity scale on the whole vector, clamped per axis so the diagonals reach the
// corners like a DS2.
static std::pair<u8, u8> MapStick(u8 raw_x, u8 raw_y)
{
	constexpr float deadzone = 8.0f;
	float dx = static_cast<float>(raw_x) - 128.0f;
	float dy = static_cast<float>(raw_y) - 128.0f;
	const float length = std::sqrt(dx * dx + dy * dy);
	if (length <= deadzone)
		return {Pad::ANALOG_NEUTRAL_POSITION, Pad::ANALOG_NEUTRAL_POSITION};
	// Take the deadzone off the length so movement starts at 0 just outside it.
	const float factor = (length - deadzone) / (128.0f - deadzone) * 128.0f / length * s_stick_scale;
	dx *= factor;
	dy *= factor;
	const auto to_u8 = [](float d) {
		return static_cast<u8>(std::clamp(static_cast<int>(std::lround(128.0f + d)), 0, 255));
	};
	return {to_u8(dx), to_u8(dy)};
}

// (AI-assisted) Forward a PS4 controller to an emulated DualShock 2 (port 0, and port 1 for the second player since 43).
static void VMHostUpdateController(u32 port, const OrbisPadData& data, u32& previous_buttons)
{
	struct ButtonMapping
	{
		u32 host_mask;
		u32 ps2_button;
	};
	static constexpr ButtonMapping mappings[] = {
		{ORBIS_PAD_BUTTON_UP, PadDualshock2::Inputs::PAD_UP},
		{ORBIS_PAD_BUTTON_RIGHT, PadDualshock2::Inputs::PAD_RIGHT},
		{ORBIS_PAD_BUTTON_DOWN, PadDualshock2::Inputs::PAD_DOWN},
		{ORBIS_PAD_BUTTON_LEFT, PadDualshock2::Inputs::PAD_LEFT},
		{ORBIS_PAD_BUTTON_TRIANGLE, PadDualshock2::Inputs::PAD_TRIANGLE},
		{ORBIS_PAD_BUTTON_CIRCLE, PadDualshock2::Inputs::PAD_CIRCLE},
		{ORBIS_PAD_BUTTON_CROSS, PadDualshock2::Inputs::PAD_CROSS},
		{ORBIS_PAD_BUTTON_SQUARE, PadDualshock2::Inputs::PAD_SQUARE},
		{ORBIS_PAD_BUTTON_L1, PadDualshock2::Inputs::PAD_L1},
		{ORBIS_PAD_BUTTON_R1, PadDualshock2::Inputs::PAD_R1},
		{ORBIS_PAD_BUTTON_L3, PadDualshock2::Inputs::PAD_L3},
		{ORBIS_PAD_BUTTON_R3, PadDualshock2::Inputs::PAD_R3},
		{ORBIS_PAD_BUTTON_OPTIONS, PadDualshock2::Inputs::PAD_START},
		{ORBIS_PAD_BUTTON_TOUCH_PAD, PadDualshock2::Inputs::PAD_SELECT},
	};
	for (const ButtonMapping& mapping : mappings)
	{
		const bool was_pressed = (previous_buttons & mapping.host_mask) != 0;
		const bool is_pressed = (data.buttons & mapping.host_mask) != 0;
		if (was_pressed != is_pressed)
			Pad::SetControllerState(port, mapping.ps2_button, is_pressed ? 1.0f : 0.0f);
	}
	previous_buttons = data.buttons;

	if (PadBase* pad = Pad::GetPad(static_cast<u8>(port)))
	{
		const auto left = MapStick(data.leftStick.x, data.leftStick.y);
		const auto right = MapStick(data.rightStick.x, data.rightStick.y);
		pad->SetRawAnalogs({left.first, left.second}, {right.first, right.second});
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_L2, data.analogButtons.l2 / 255.0f);
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_R2, data.analogButtons.r2 / 255.0f);
	}
}

// Forja SX2 (compilation 43): once a second, the signed-in users: the first one that isn't player 1's gets the second port.
// A user who signs out (or whose controller goes) frees it for the next one.
static void UpdateSecondPad(double elapsed)
{
	if (elapsed - s_pad2_last_check < 1.0)
		return;
	s_pad2_last_check = elapsed;
	OrbisUserServiceLoginUserIdList list{};
	if (sceUserServiceGetLoginUserIdList(&list) != 0)
		return;
	bool still_signed_in = false;
	OrbisUserServiceUserId candidate = ORBIS_USER_SERVICE_USER_ID_INVALID;
	for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++)
	{
		const OrbisUserServiceUserId user = list.userId[i];
		if (user == ORBIS_USER_SERVICE_USER_ID_INVALID || user == s_pad1_user)
			continue;
		if (user == s_pad2_user)
			still_signed_in = true;
		else if (candidate == ORBIS_USER_SERVICE_USER_ID_INVALID)
			candidate = user;
	}
	if (s_pad2 >= 0 && !still_signed_in)
	{
		scePadClose(s_pad2);
		ProbeLog("Player 2: user %#x signed out, port 2 free\n", static_cast<unsigned>(s_pad2_user));
		s_pad2 = -1;
		s_pad2_user = ORBIS_USER_SERVICE_USER_ID_INVALID;
	}
	if (s_pad2 < 0 && candidate != ORBIS_USER_SERVICE_USER_ID_INVALID)
	{
		const int handle = scePadOpen(candidate, 0, 0, nullptr);
		ProbeLog("Player 2: user %#x signed in, pad open=%d\n", static_cast<unsigned>(candidate), handle);
		if (handle >= 0)
		{
			s_pad2 = handle;
			s_pad2_user = candidate;
		}
	}
	g_forja_pads.store(s_pad2 >= 0 ? 2 : 1, std::memory_order_relaxed);
}

// (AI-assisted) Called only with the VM paused. Device limits and GSConfig belong
// to the GS thread; wait for that snapshot before showing the applied value.
static void ReadGameResolution()
{
	// Compilation 99: the options a player's profile can change that the report's settings line doesn't show (the flicker's tests).
	ProbeLog("GS image options: deinterlace=%d texture_filter=%d cycle_skip=%d\n", static_cast<int>(GSConfig.InterlaceMode),
		static_cast<int>(GSConfig.TextureFiltering), EmuConfig.Speedhacks.EECycleSkip * 4 + EmuConfig.Speedhacks.EECycleSkipQuarters);
	MTGS::RunOnGSThread([]() {
		// Compilation 59: 3x and 4x only on a fast console (a PS4 Pro with Boost Mode). A PS4 or a Slim (or a Pro without Boost
		// Mode) has ~25 % less CPU and half the GPU, and those resolutions are what the hint already called "solo para PS4 Pro".
		s_max_game_upscale = (g_gs_device && GSIsHardwareRenderer()) ?
			std::clamp(GSGetMaxUpscaleMultiplier(g_gs_device->GetMaxTextureSize()), 1u, s_console_fast_cpu ? 4u : 2u) : 1u;
		const float configured = GSConfig.UpscaleMultiplier;
		// A corrupted/uninitialized value must never be exposed as a 68x or 20x
		// menu choice. The launcher intentionally exposes only 1x..4x.
		s_game_upscale = (std::isfinite(configured) && configured >= 1.0f && configured <= 4.0f) ?
			static_cast<unsigned>(configured + 0.5f) : 1u;
		s_game_upscale = std::min(s_game_upscale, s_max_game_upscale);
		s_game_aspect = std::clamp(static_cast<unsigned>(GSConfig.AspectRatio), 0u, 4u);
		s_game_textures = GSConfig.LoadTextureReplacements ? (GSConfig.PrecacheTextureReplacements ? 2u : 1u) : 0u;
		s_game_sharpness = GSConfig.CASMode != GSCASMode::Disabled ? (GSConfig.CAS_Sharpness >= 75 ? 3u : 2u) :
		                   GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp ? 1u :
		                   GSConfig.LinearPresent == GSPostBilinearMode::Off ? 4u : 0u;
	});
	if (s_verbose_diag)
	{
		if (!MTGS::TimedWaitGS(2000, false, false, false))
			ProbeLog("pause step: ReadGameResolution TIMEOUT vsyncq=%d\n", MTGS::GetCurrentVsyncQueueSize());
	}
	else
		MTGS::WaitGS(false, false, false);
}

// Forja SX2: the L3+R3 menu's values for the game's profile rows (InGameMenuState.h). Video mode: 0 off, else 1 - the EE
// rate while a movie plays (1 = 100 % ... 4 = 50 %).
static unsigned VideoMenuValue()
{
	// 0 off, 1-5 a fixed rate (100, 75, 60, 50 and 36 %), 6 auto (compilation 49).
	if (!s_video_mode_enabled)
		return 0u;
	return s_video_auto ? 6u : static_cast<unsigned>(1 - std::clamp(s_video_ee_rate, -4, 0));
}

// The Núcleos row's choices: global (nucleos.txt), the built-in cores, and plan B (the best one in God of War II).
static constexpr const char* CORE_PLANS[] = {"global", "EE=2 VU1=3 GS=4 AUDIO=5", "EE=1 VU1=2 GS=3 AUDIO=5 AYUDANTES=4"};

static std::string SquashPlan(std::string_view plan)
{
	std::string squashed;
	for (const char c : plan)
	{
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '=')
			squashed += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return squashed;
}

// 0 global, 1 normal, 2 plan B, 3 another plan written by hand in the profile.
static unsigned CoresMenuValue()
{
	const std::string plan = SquashPlan(s_profile.cores);
	if (plan.empty() || plan == "GLOBAL")
		return 0;
	for (unsigned i = 1; i < std::size(CORE_PLANS); i++)
	{
		if (plan == SquashPlan(CORE_PLANS[i]))
			return i;
	}
	return 3;
}

// Forja SX2 (compilation 42): the L3+R3 menu's Parches tab. The game's patch groups (the player's file, the package's and the
// community's, Patch::GetPatchInfo), one at a time: Left/Right pick, X turns it on or off in the player's profile ([Patches]
// Enable, and Disable so a factory profile's can be turned off too).
static std::vector<Patch::PatchInfo> s_patch_infos;
static std::vector<int> s_patch_rule; // compilation 55: for each entry, the fingerprint rule it is (-1: a pnach patch group)
static std::vector<std::string> s_patch_enabled, s_patch_disabled;
static unsigned s_patch_index = 0;
static u32 s_patch_fetch_crc = 0;

// Forja SX2 (compilation 55): draw fingerprints ("huellas", pcsx2/ForjaGsRules.h): Forja's own patches, at the end of the list,
// found by what a game draws and not by addresses, so each one holds for every game drawn the same way, any copy or region
// (Joseph, 2026-10-18: "que todo sea nativo del emulador para aplicar a cualquier juego con los mismos esquemas"). They come
// from huellas.ini: the settings center's copy (fetched from the project's GitHub at every game start) or the package's, so
// an effect found in one game reaches every other game without a new package. A rule is offered where it is named (a
// serial, or a GameDB draw hook such as OI_DBZBTGames, Spike's Budokai Tenkaichi engine: BT2, BT3 and the BT4 mod in every
// region) and in any game where it takes its "ofrecer_desde" share of the GS or more while it plays; the ones the player
// turns on go in [Forja] Huellas (their ids). The descriptor's layout is in GSRendererHW.cpp.
struct ForjaRule
{
	std::string id, name, description;
	ForjaGsRule gs{};
	u32 offer_permille = 150;
	std::vector<std::string> games;
};
static std::vector<ForjaRule> s_rules;
static u32 s_rules_on = 0;              // a bit per rule: on for this game
static u32 s_rule_share_seen[16] = {};  // the largest share of the GS seen in this game, permille

static std::string RuleTrim(std::string text)
{
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
		text.pop_back();
	size_t start = 0;
	while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
		start++;
	return text.substr(start);
}

static std::string RuleLower(std::string text)
{
	for (char& c : text)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

static std::vector<std::string> RuleSplit(const std::string& text)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find(',', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string part = RuleTrim(text.substr(start, end - start));
		if (!part.empty())
			parts.push_back(part);
		start = end + 1;
	}
	return parts;
}

static int RulePsm(const std::string& name)
{
	static constexpr std::pair<const char*, int> names[] = {{"c32", 0x00}, {"c24", 0x01}, {"c16", 0x02}, {"c16s", 0x0a}, {"t8", 0x13},
		{"t4", 0x14}, {"t8h", 0x1b}, {"t4hl", 0x24}, {"t4hh", 0x2c}, {"z32", 0x30}, {"z24", 0x31}, {"z16", 0x32}, {"z16s", 0x3a}};
	for (const auto& [text, psm] : names)
	{
		if (name == text)
			return psm;
	}
	return -1;
}

static int RuleLog2(int value)
{
	for (int bit = 0; bit < 11; bit++)
	{
		if (value == (1 << bit))
			return bit;
	}
	return -1;
}

static void PublishRules()
{
	g_forja_gs_rule_count.store(0, std::memory_order_release);
	const u32 count = static_cast<u32>(std::min<size_t>(s_rules.size(), 16));
	for (u32 i = 0; i < count; i++)
		g_forja_gs_rules[i] = s_rules[i].gs;
	g_forja_gs_rules_off.store(s_rules_on, std::memory_order_relaxed);
	g_forja_gs_rule_count.store(count, std::memory_order_release);
}

static void ApplyRulesOn(const std::string& list)
{
	s_rules_on = 0;
	for (const std::string& id : RuleSplit(list))
	{
		for (size_t i = 0; i < s_rules.size() && i < 16; i++)
		{
			if (RuleLower(s_rules[i].id) == RuleLower(id))
				s_rules_on |= 1u << i;
		}
	}
	g_forja_gs_rules_off.store(s_rules_on, std::memory_order_relaxed);
}

static std::string RulesOnList()
{
	std::string list;
	for (size_t i = 0; i < s_rules.size() && i < 16; i++)
	{
		if (s_rules_on & (1u << i))
			list += (list.empty() ? "" : ",") + s_rules[i].id;
	}
	return list;
}

// huellas.ini: one [section] a rule (its id), "key = value" lines, ';' comments. Keys: nombre, descripcion, juegos (serials or
// GameDB draw hooks, with commas), ofrecer_desde (percent of the GS), and what the draw looks like, each optional: destino
// (pantalla / fuera), textura (no / si / T4 / T8 / C32 / C16 / Z32...), funcion (modulate / decal / highlight / highlight2),
// mezcla (si / no), ecuacion (up to four "ABCD" blends, digits 0-2, with commas), primitiva (puntos / lineas / triangulos /
// sprites), escribe_z (si / no), tamano (256x256).
static void LoadRules()
{
	s_rules.clear();
	s_rules_on = 0;
	std::fill(std::begin(s_rule_share_seen), std::end(s_rule_share_seen), 0u);
	const std::string center = Path::Combine(EmuFolders::DataRoot, "centro/huellas.ini");
	const std::string package = Path::Combine(EmuFolders::Resources, "huellas.ini");
	const std::string path = FileSystem::FileExists(center.c_str()) ? center : package;
	struct Pending
	{
		ForjaRule rule;
		u64 mask = 0, value = 0;
		std::vector<u32> blends;
		bool ok = true;
	};
	std::vector<Pending> pending;
	if (std::FILE* file = std::fopen(path.c_str(), "r"))
	{
		char buffer[512];
		bool skipping = false;
		while (std::fgets(buffer, sizeof(buffer), file))
		{
			const std::string line = RuleTrim(buffer);
			if (line.empty() || line[0] == ';' || line[0] == '#')
				continue;
			if (line.front() == '[' && line.back() == ']')
			{
				skipping = pending.size() >= 16;
				if (!skipping)
				{
					pending.emplace_back();
					pending.back().rule.id = RuleTrim(line.substr(1, line.size() - 2));
				}
				continue;
			}
			const size_t equals = line.find('=');
			if (skipping || pending.empty() || equals == std::string::npos)
				continue;
			Pending& p = pending.back();
			const std::string key = RuleLower(RuleTrim(line.substr(0, equals)));
			const std::string value = RuleTrim(line.substr(equals + 1));
			const std::string low = RuleLower(value);
			const auto set = [&p](u64 mask, u64 bits) {
				p.mask |= mask;
				p.value = (p.value & ~mask) | bits;
			};
			const auto pick = [&p, &low](std::initializer_list<const char*> names) -> int {
				int index = 0;
				for (const char* name : names)
				{
					if (low == name)
						return index;
					index++;
				}
				p.ok = false;
				return 0;
			};
			if (key == "nombre")
				p.rule.name = value;
			else if (key == "descripcion")
				p.rule.description = value;
			else if (key == "juegos")
				p.rule.games = RuleSplit(value);
			else if (key == "ofrecer_desde")
				p.rule.offer_permille = static_cast<u32>(std::clamp(std::atoi(value.c_str()), 1, 100)) * 10;
			else if (key == "destino")
				set(1, pick({"fuera", "pantalla"}) ? 1 : 0);
			else if (key == "textura")
			{
				if (low == "no" || low == "si")
					set(2, low == "si" ? 2 : 0);
				else if (const int psm = RulePsm(low); psm >= 0)
					set(2 | (u64{0x3f} << 2), 2 | (static_cast<u64>(psm) << 2));
				else
					p.ok = false;
			}
			else if (key == "funcion")
				set(u64{3} << 8, static_cast<u64>(pick({"modulate", "decal", "highlight", "highlight2"})) << 8);
			else if (key == "mezcla")
				set(u64{1} << 11, pick({"no", "si"}) ? (u64{1} << 11) : 0);
			else if (key == "ecuacion")
			{
				for (const std::string& blend : RuleSplit(value))
				{
					if (blend.size() != 4 || blend.find_first_not_of("012") != std::string::npos || p.blends.size() >= 4)
					{
						p.ok = false;
						break;
					}
					p.blends.push_back(static_cast<u32>(blend[0] - '0') | (static_cast<u32>(blend[1] - '0') << 2) |
									   (static_cast<u32>(blend[2] - '0') << 4) | (static_cast<u32>(blend[3] - '0') << 6));
				}
			}
			else if (key == "primitiva")
				set(u64{3} << 20, static_cast<u64>(pick({"puntos", "lineas", "triangulos", "sprites"})) << 20);
			else if (key == "escribe_z")
				set(u64{1} << 22, pick({"no", "si"}) ? (u64{1} << 22) : 0);
			else if (key == "tamano")
			{
				const size_t x = low.find('x');
				const int tw = x == std::string::npos ? -1 : RuleLog2(std::atoi(low.c_str()));
				const int th = x == std::string::npos ? -1 : RuleLog2(std::atoi(low.c_str() + x + 1));
				if (tw < 0 || th < 0)
					p.ok = false;
				else
					set((u64{0xf} << 23) | (u64{0xf} << 27), (static_cast<u64>(tw) << 23) | (static_cast<u64>(th) << 27));
			}
			else
			{
				ProbeLog("Huellas: [%s] unknown key \"%s\", ignored\n", p.rule.id.c_str(), key.c_str());
			}
		}
		std::fclose(file);
	}
	std::string ids;
	for (Pending& p : pending)
	{
		if (!p.ok || p.rule.id.empty() || p.rule.name.empty() || p.mask == 0)
		{
			ProbeLog("Huellas: [%s] left out (a value it doesn't know, or no name or nothing to match)\n", p.rule.id.c_str());
			continue;
		}
		p.rule.gs.mask = p.mask;
		if (p.blends.empty())
		{
			p.rule.gs.values[0] = p.value;
			p.rule.gs.count = 1;
		}
		else
		{
			p.rule.gs.mask |= u64{0xff} << 12;
			for (size_t k = 0; k < p.blends.size(); k++)
				p.rule.gs.values[k] = (p.value & ~(u64{0xff} << 12)) | (static_cast<u64>(p.blends[k]) << 12);
			p.rule.gs.count = static_cast<u32>(p.blends.size());
		}
		ids += (ids.empty() ? "" : ", ") + p.rule.id;
		s_rules.push_back(std::move(p.rule));
	}
	PublishRules();
	ProbeLog("Huellas: %zu from %s (%s)\n", s_rules.size(), path.c_str(), ids.empty() ? "none" : ids.c_str());
}

// Called with the heartbeat: the largest share each rule took in this game (a rule left out takes none while it is out).
static void UpdateRuleShares()
{
	for (size_t i = 0; i < s_rules.size() && i < 16; i++)
		s_rule_share_seen[i] = std::max(s_rule_share_seen[i], g_forja_gs_rule_share[i].load(std::memory_order_relaxed));
}

static bool RuleOffered(size_t i)
{
	if ((s_rules_on & (1u << i)) || s_rule_share_seen[i] >= s_rules[i].offer_permille)
		return true;
	const std::string serial = VMManager::GetDiscSerial();
	for (const std::string& game : s_rules[i].games)
	{
		if (game == serial)
			return true;
		if (game.rfind("OI_", 0) == 0)
		{
			const s16 hook = GSLookupBeforeDrawFunctionId(game);
			if (hook >= 0 && EmuConfig.GS.BeforeDrawFunctionId == hook)
				return true;
		}
	}
	return false;
}

static bool PatchIsOn(size_t index)
{
	if (index < s_patch_rule.size() && s_patch_rule[index] >= 0)
		return (s_rules_on & (1u << s_patch_rule[index])) != 0;
	const std::string& name = s_patch_infos[index].name;
	return std::find(s_patch_enabled.begin(), s_patch_enabled.end(), name) != s_patch_enabled.end() &&
	       std::find(s_patch_disabled.begin(), s_patch_disabled.end(), name) == s_patch_disabled.end();
}

static void PublishPatchMenuText()
{
	// 2026-10-19: the menu's texts in the language picked in the library (ForjaLang.h); the patches' own names and
	// descriptions are their authors'.
	using ForjaLang::T;
	using ForjaLang::TF;
	if (s_patch_infos.empty())
	{
		PS4GameMenu::SetPatchMenuText(T("Parches:  ninguno para este juego"),
			T("Al empezar un juego se bajan los de la comunidad de PCSX2 (con internet). Los tuyos van en /data/ForjaSX2/patches"));
		return;
	}
	const Patch::PatchInfo& patch = s_patch_infos[s_patch_index];
	const bool on = PatchIsOn(s_patch_index);
	char label[512];
	std::snprintf(label, sizeof(label), TF("Parche %u de %zu:  < %s >  %s"), s_patch_index + 1, s_patch_infos.size(), patch.name.c_str(),
		on ? T("Sí") : T("No"));
	std::string description = patch.description.empty() ? std::string(T("Sin descripción")) : patch.description;
	if (!patch.author.empty())
		description += "  ·  " + patch.author;
	description += on ? T("  ·  X: quitar (algunos piden reiniciar el juego)") : T("  ·  X: poner");
	PS4GameMenu::SetPatchMenuText(label, ForjaLang::Cut(description, 600));
}

static void ReloadPatchMenu()
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	s_patch_infos = crc != 0 ? Patch::GetPatchInfo(serial, crc, false, false, nullptr) : std::vector<Patch::PatchInfo>{};
	s_patch_rule.assign(s_patch_infos.size(), -1);
	UpdateRuleShares();
	for (size_t i = 0; crc != 0 && i < s_rules.size() && i < 16; i++)
	{
		if (!RuleOffered(i))
			continue;
		// The fingerprints' names and descriptions (huellas.ini) are the settings center's Spanish ones; the language files
		// translate the ones it has (assets/lang/idiomas.py reads assets/huellas.ini too).
		Patch::PatchInfo rule;
		rule.name = ForjaLang::T(s_rules[i].name) + " (Forja)";
		rule.description = ForjaLang::T(s_rules[i].description);
		if (s_rule_share_seen[i] > 0)
		{
			char seen[160];
			std::snprintf(seen, sizeof(seen), ForjaLang::TF("  ·  aquí: %.0f %% del GS"), s_rule_share_seen[i] / 10.0);
			rule.description += seen;
		}
		rule.author = "Forja SX2";
		s_patch_infos.push_back(std::move(rule));
		s_patch_rule.push_back(static_cast<int>(i));
	}
	s_patch_enabled = Host::GetStringListSetting(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY);
	s_patch_disabled = Host::GetStringListSetting(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_DISABLE_CONFIG_KEY);
	if (s_patch_index >= s_patch_infos.size())
		s_patch_index = 0;
	PublishPatchMenuText();
}

template <typename Edit>
static void SavePlayerProfile(const char* what, Edit&& edit, bool reload_pcsx2);

static void TogglePatch()
{
	if (s_patch_infos.empty())
		return;
	const std::string name = s_patch_infos[s_patch_index].name;
	const bool turning_on = !PatchIsOn(s_patch_index);
	if (s_patch_rule[s_patch_index] >= 0) // compilation 55: a fingerprint rule, in [Forja] Huellas
	{
		const u32 bit = 1u << s_patch_rule[s_patch_index];
		s_rules_on = turning_on ? (s_rules_on | bit) : (s_rules_on & ~bit);
		const std::string list = RulesOnList();
		SavePlayerProfile("Huellas", [list](INISettingsInterface& ini) { ini.SetStringValue("Forja", "Huellas", list.c_str()); }, false);
		ProbeLog("Patches: \"%s\" %s; huellas on: [%s]\n", name.c_str(), turning_on ? "on" : "off", list.c_str());
		ReloadPatchMenu();
		return;
	}
	std::vector<std::string> enabled = s_patch_enabled, disabled = s_patch_disabled;
	enabled.erase(std::remove(enabled.begin(), enabled.end(), name), enabled.end());
	disabled.erase(std::remove(disabled.begin(), disabled.end(), name), disabled.end());
	(turning_on ? enabled : disabled).push_back(name);
	SavePlayerProfile("Patches", [enabled, disabled](INISettingsInterface& ini) {
		ini.SetStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, enabled);
		ini.SetStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_DISABLE_CONFIG_KEY, disabled);
	}, true);
	VMManager::ReloadPatches(false, true, false, true);
	ProbeLog("Patches: \"%s\" %s\n", name.c_str(), turning_on ? "on" : "off");
	ReloadPatchMenu();
}

// Forja SX2 (compilation 58): "Trucos" (L3+R3 > Extras), an idea from M33's quick menu: the game's cheat groups (PCSX2's cheats
// folder, /data/ForjaSX2/cheats/<SERIAL>_<CRC>.pnach, written as a patch file), one at a time like the patches; X turns one on
// or off in the player's profile ([Cheats] Enable, with [EmuCore] EnableCheats while any is on).
static std::vector<Patch::PatchInfo> s_cheat_infos;
static std::vector<std::string> s_cheats_enabled;
static unsigned s_cheat_index = 0;

static bool CheatIsOn(const std::string& name)
{
	return std::find(s_cheats_enabled.begin(), s_cheats_enabled.end(), name) != s_cheats_enabled.end();
}

static void PublishCheatText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	if (s_cheat_infos.empty())
	{
		char description[512];
		std::snprintf(description, sizeof(description),
			TF("Pon tus trucos en /data/ForjaSX2/cheats/%s_%08X.pnach (escritos como un parche) y vuelve a abrir este menú"),
			VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC());
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_CHEATS, T("Trucos:  ninguno para este juego"), description);
		return;
	}
	const Patch::PatchInfo& cheat = s_cheat_infos[s_cheat_index];
	const bool on = CheatIsOn(cheat.name);
	char label[512];
	std::snprintf(label, sizeof(label), TF("Truco %u de %zu:  < %s >  %s"), s_cheat_index + 1, s_cheat_infos.size(), cheat.name.c_str(),
		on ? T("Sí") : T("No"));
	std::string description = cheat.description.empty() ? std::string(T("Sin descripción")) : cheat.description;
	if (!cheat.author.empty())
		description += "  ·  " + cheat.author;
	description += on ? T("  ·  X: quitar") : T("  ·  X: poner");
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CHEATS, label, ForjaLang::Cut(description, 600));
}

static void ReloadCheats()
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	s_cheat_infos = crc != 0 ? Patch::GetPatchInfo(serial, crc, true, false, nullptr) : std::vector<Patch::PatchInfo>{};
	s_cheats_enabled = Host::GetStringListSetting(Patch::CHEATS_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY);
	if (s_cheat_index >= s_cheat_infos.size())
		s_cheat_index = 0;
	PublishCheatText();
}

static void ToggleCheat()
{
	if (s_cheat_infos.empty())
		return;
	const std::string name = s_cheat_infos[s_cheat_index].name;
	const bool turning_on = !CheatIsOn(name);
	std::vector<std::string> enabled = s_cheats_enabled;
	enabled.erase(std::remove(enabled.begin(), enabled.end(), name), enabled.end());
	if (turning_on)
		enabled.push_back(name);
	const bool any = !enabled.empty();
	SavePlayerProfile("Cheats", [enabled, any](INISettingsInterface& ini) {
		ini.SetStringList(Patch::CHEATS_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, enabled);
		ini.SetBoolValue("EmuCore", "EnableCheats", any);
	}, true);
	VMManager::ReloadPatches(false, true, false, true);
	ProbeLog("Cheats: \"%s\" %s (%zu on)\n", name.c_str(), turning_on ? "on" : "off", enabled.size());
	ReloadCheats();
}


// Forja SX2 (compilation 132): "Código listo al arrancar": the game's list of EE blocks (pcsx2/x86/ix86-32/iR5900.cpp) is read when it
// starts (or when its row is turned on) and written when the menu opens and when the game ends.
static void ForjaCodeAheadStart(const std::string& serial, u32 crc, bool running)
{
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	s_code_ahead_path.clear();
	if (s_code_ahead_game && !serial.empty() && crc != 0)
	{
		mkdir("/data/ForjaSX2/cache", 0777);
		mkdir("/data/ForjaSX2/cache/codigo", 0777);
		char path[200];
		std::snprintf(path, sizeof(path), "/data/ForjaSX2/cache/codigo/%s_%08X.bin", Path::SanitizeFileName(serial).c_str(), crc);
		s_code_ahead_path = path;
		// Compilation 137: "Código listo de fábrica". A player without a list of his own for this game (its first session) starts
		// from the package's (resources/codigo/<SERIAL>_<CRC>.bin, made on Joseph's console), copied to his: the first session of a
		// game is then as ready as the tenth, and from there the list grows and is saved as always. Same serial and CRC (the same
		// disc); each block is checked against the game's memory before it is compiled (ForjaCodeAheadTick), as with his own.
		if (!FileSystem::FileExists(path))
		{
			char factory_name[96];
			std::snprintf(factory_name, sizeof(factory_name), "codigo/%s_%08X.bin", Path::SanitizeFileName(serial).c_str(), crc);
			const std::string factory = Path::Combine(EmuFolders::Resources, factory_name);
			if (FileSystem::FileExists(factory.c_str()))
			{
				const bool copied = FileSystem::CopyFilePath(factory.c_str(), path, false);
				ProbeLog("Code ahead (compilation 137): the package's list for this game %s (%s)\n", copied ? "copied" : "could not be copied",
					factory.c_str());
			}
		}
	}
	ForjaCodeAheadBegin(s_code_ahead_path.c_str(), running);
	if (!s_code_ahead_path.empty())
		ProbeLog("Code ahead (compilation 132): %u blocks known from earlier sessions (%s)\n",
			g_forja_code_ahead_known.load(std::memory_order_relaxed), s_code_ahead_path.c_str());
#else
	(void)serial;
	(void)crc;
	(void)running;
#endif
}

static void ForjaCodeAheadStore(bool force)
{
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	if (!s_code_ahead_path.empty() && ForjaCodeAheadSave(s_code_ahead_path.c_str(), force))
		ProbeLog("Code ahead: list saved, %u blocks\n", g_forja_code_ahead_known.load(std::memory_order_relaxed));
#else
	(void)force;
#endif
}

// Forja SX2 (compilation 132): a new build drops the settings center's downloaded profiles. They win over the package's, and a game's
// profile is fetched when the game starts and used "from its next start": Need for Speed Most Wanted ran its first session with the
// 131 (2026-11-13) on the copy fetched with the 130, without the two keys the 131's package had just turned off for it.
static void ForjaCenterFreshForBuild()
{
#ifdef FORJA_BUILD_NUMBER
	if (FORJA_BUILD_NUMBER == 0)
		return; // a local build
	static constexpr const char* stamp = "/data/ForjaSX2/centro/compilacion.txt";
	int seen = 0;
	if (std::FILE* file = std::fopen(stamp, "r"))
	{
		if (std::fscanf(file, "%d", &seen) != 1)
			seen = 0;
		std::fclose(file);
	}
	if (seen == FORJA_BUILD_NUMBER)
		return;
	unsigned removed = 0;
	FileSystem::FindResultsArray files;
	if (FileSystem::FindFiles("/data/ForjaSX2/centro/perfiles", "*.ini", FILESYSTEM_FIND_FILES, &files))
	{
		for (const FILESYSTEM_FIND_DATA& found : files)
			removed += FileSystem::DeleteFilePath(found.FileName.c_str()) ? 1u : 0u;
	}
	mkdir("/data/ForjaSX2/centro", 0777);
	if (std::FILE* file = std::fopen(stamp, "w"))
	{
		std::fprintf(file, "%d\n", FORJA_BUILD_NUMBER);
		std::fclose(file);
	}
	ProbeLog("Settings center: build %d runs for the first time (the last one was %d): %u downloaded profiles removed; the package's are "
			 "used until each game's is fetched again\n", FORJA_BUILD_NUMBER, seen, removed);
#endif
}

// Forja SX2 (compilation 131): the guardian. The improvements that were rows of No tocar are on for every game and hidden, so a game
// that cannot take one of them needs a way out that asks nothing of the player. A session that ends badly leaves a mark for its game
// (forja_guard_mark, ps4/runtime/ps4_crashlog.cpp: a signal, an EE exception, a hang); the game's next start finds it and puts "Modo
// seguro" in Parcial (never further by itself), which the player sees in the menu and can put back. Need for Speed Most Wanted
// (2026-11-13, the 130) stopped on an EE exception 80 s after "Disco sin esperas" and "CPU sin tirones" were turned on.
extern "C" char g_forja_guard_mark[160];
extern "C" void forja_guard_mark(const char* reason);

static void ForjaGuardStart(const std::string& serial, u32 crc)
{
	g_forja_guard_mark[0] = '\0';
	if (serial.empty() || crc == 0)
		return;
	mkdir("/data/ForjaSX2/guardian", 0777);
	char path[160];
	std::snprintf(path, sizeof(path), "/data/ForjaSX2/guardian/%s.txt", Path::SanitizeFileName(serial).c_str());
	if (std::FILE* file = std::fopen(path, "r"))
	{
		char reason[64] = "";
		if (!std::fgets(reason, sizeof(reason), file))
			reason[0] = '\0';
		std::fclose(file);
		std::remove(path);
		reason[std::strcspn(reason, "\r\n")] = '\0';
		const ForjaGameProfile::Values before = ForjaGameProfile::Load(serial, crc);
		if (!before.guard)
			ProbeLog("Guardian: the last session of %s ended badly (%s); off in the lab's file, nothing changed\n", serial.c_str(), reason);
		else if (before.safe_mode != 0)
			ProbeLog("Guardian: the last session of %s ended badly (%s); Modo seguro was already %d, nothing changed\n", serial.c_str(), reason,
				before.safe_mode);
		else
		{
			// Compilation 140: one suspect at a time, the likeliest for that end first, and the others stay on. Need for Speed (2026-11-18):
			// the Parcial of the 131-139 turned six off at once and its hitches went from 7.5 to 23 a minute (26 of the disc, 27 of code);
			// with "CPU sin tirones" alone off it ran 5:45 with 3.7 a minute and no stop. Parcial only when no suspect is left.
			static constexpr const char* ee_order[] = {"CpuSinTirones", "RevisionRapida", "CodigoListo", "DiscoSinEsperas", "SaltosVU1",
				"EmpujonCuadros"};
			static constexpr const char* hang_order[] = {"DiscoSinEsperas", "IopDormido", "CpuSinTirones", "RevisionRapida", "CodigoListo",
				"SaltosVU1"};
			static constexpr const char* other_order[] = {"CpuSinTirones", "RevisionRapida", "CodigoListo", "SaltosVU1", "DiscoSinEsperas",
				"EmpujonCuadros", "TraductorSoftware", "GrabarEnOtroHilo"};
			const bool ee = std::strstr(reason, "ee exception") != nullptr;
			const bool hang = std::strstr(reason, "hang") != nullptr;
			const char* const* order = ee ? ee_order : hang ? hang_order : other_order;
			const size_t count = ee ? std::size(ee_order) : hang ? std::size(hang_order) : std::size(other_order);
			const char* next = nullptr;
			for (size_t i = 0; i < count && !next; i++)
			{
				if (!ForjaGameProfile::GuardListHas(before.guard_off, order[i]) && ForjaGameProfile::GuardListHas(before.hidden_on, order[i]))
					next = order[i];
			}
			if (next)
			{
				const std::string list = before.guard_off.empty() ? std::string(next) : before.guard_off + "," + next;
				const bool saved = ForjaGameProfile::EditPlayer(serial, crc, [&list](INISettingsInterface& ini) {
					ini.SetStringValue("Forja", "GuardianApagadas", list.c_str());
				});
				ProbeLog("Guardian: the last session of %s ended badly (%s): %s off for this game, one at a time (off so far: %s; saved=%d)\n",
					serial.c_str(), reason, next, list.c_str(), saved ? 1 : 0);
			}
			else
			{
				const bool saved = ForjaGameProfile::EditPlayer(serial, crc, [](INISettingsInterface& ini) { ini.SetIntValue("Forja", "ModoSeguro", 1); });
				ProbeLog("Guardian: the last session of %s ended badly (%s), no suspect left: Modo seguro -> Parcial for this game (saved=%d)\n",
					serial.c_str(), reason, saved ? 1 : 0);
			}
		}
	}
	std::snprintf(g_forja_guard_mark, sizeof(g_forja_guard_mark), "%s", path);
}

// Forja SX2 (compilation 58): the hang log, an idea from M33's hang.log: a thread checks once a second that the game's frames
// still advance (g_FrameCount, the PS2's vsyncs). After 10 s without one while the game runs (not paused, no menu) it writes
// where the EE and the IOP were to logs/hang.log and to the session log, once per stop, so a freeze leaves something to read.
static std::atomic<bool> s_hang_watch_started{false};

static void* HangWatch(void*)
{
	pthread_setname_np(pthread_self(), "Forja hang watch");
	unsigned last_frame = 0, still = 0;
	bool reported = false;
	for (;;)
	{
		sleep(1);
		const bool running = VMManager::GetState() == VMState::Running && !s_game_menu_open;
		const unsigned frame = g_FrameCount;
		if (!running || frame != last_frame)
		{
			if (reported && running)
			{
				ProbeLog("Hang watch: frames again after %u s without one\n", still);
				// Compilation 131: the game came back (the PS4's own menu over it, a long stall): the guardian's mark goes away.
				if (g_forja_guard_mark[0])
					std::remove(g_forja_guard_mark);
			}
			last_frame = frame;
			still = 0;
			reported = false;
			continue;
		}
		if (++still < 10 || reported)
			continue;
		reported = true;
		forja_guard_mark("hang"); // compilation 131
		ForjaSession::AddHang(); // compilation 80: the summary's "Cuelgues"
		char when[32] = "?";
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		if (localtime_r(&now, &local)) // the reentrant one: the CPU thread uses localtime too
			std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
		char line[400];
		std::snprintf(line, sizeof(line),
			"%s: %u s sin cuadros nuevos en %s %08X: cuadro %u, EE_PC=%08x IOP_PC=%08x EE_cycle=%llu vsyncq=%d\n",
			when, still, VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC(), frame, cpuRegs.pc,
			psxRegs.pc, static_cast<unsigned long long>(cpuRegs.cycle), MTGS::GetCurrentVsyncQueueSize());
		ProbeLog("Hang watch: %s", line);
		if (std::FILE* file = std::fopen("/data/ForjaSX2/logs/hang.log", "a"))
		{
			std::fputs(line, file);
			std::fclose(file);
		}
	}
	return nullptr;
}

static void StartHangWatch()
{
	if (s_hang_watch_started.exchange(true))
		return;
	// Compilation 59: a stack of its own (it only reads a few numbers), so it does not take the default one's memory.
	pthread_t thread{};
	pthread_attr_t attr;
	int result = pthread_attr_init(&attr);
	if (result == 0)
	{
		pthread_attr_setstacksize(&attr, 128 * 1024);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		result = pthread_create(&thread, &attr, HangWatch, nullptr);
		pthread_attr_destroy(&attr);
	}
	if (result != 0)
	{
		s_hang_watch_started.store(false);
		ProbeLog("Hang watch: the thread could not start (error %d)\n", result);
	}
}


// Forja SX2 (compilation 44): the VU1 filter (MTVU.cpp), an effects lab for any game. The menu offers the VU1's costliest programs
// of the last seconds (and those already left out); X leaves one out or brings it back, and the list goes in the player's profile
// ([Forja] VU1Quitar, start addresses in hex). A program that draws an effect, a shadow or the far scenery can be left out to
// save its VU1 time: what it drew isn't drawn. One the game needs (its main 3D) leaves the screen empty: X again brings it back.
extern std::atomic<u32> g_forja_vu1_skip[64];
extern std::atomic<u32> g_forja_vu1_top[8];
extern std::atomic<u32> g_forja_vu1_top_count;
static std::vector<u32> s_vu1_candidates; // programs (start address / 8)
static std::vector<u32> s_vu1_shares;     // their share of the VU1's time, in permille (0 for one already left out)
static unsigned s_vu1_index = 0;

static bool Vu1Skipped(u32 program)
{
	return ((g_forja_vu1_skip[(program & 2047) >> 5].load(std::memory_order_relaxed) >> (program & 31)) & 1) != 0;
}

static void SetVu1Skipped(u32 program, bool skip)
{
	const u32 bit = 1u << (program & 31);
	if (skip)
		g_forja_vu1_skip[(program & 2047) >> 5].fetch_or(bit, std::memory_order_relaxed);
	else
		g_forja_vu1_skip[(program & 2047) >> 5].fetch_and(~bit, std::memory_order_relaxed);
}

static std::string Vu1SkipList()
{
	std::string list;
	for (u32 program = 0; program < 2048; program++)
	{
		if (!Vu1Skipped(program))
			continue;
		char text[16];
		std::snprintf(text, sizeof(text), "%s%04x", list.empty() ? "" : ",", program * 8);
		list += text;
	}
	return list;
}

static void ApplyVu1SkipList(const std::string& list)
{
	for (auto& word : g_forja_vu1_skip)
		word.store(0, std::memory_order_relaxed);
	const char* p = list.c_str();
	while (*p)
	{
		char* end = nullptr;
		const unsigned long start = std::strtoul(p, &end, 16);
		if (end == p)
		{
			++p;
			continue;
		}
		if (start < 0x4000 && (start & 7) == 0)
			SetVu1Skipped(static_cast<u32>(start / 8), true);
		p = end;
	}
}

static void PublishVu1FilterText()
{
	using ForjaLang::T;
	if (s_vu1_candidates.empty())
	{
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_VU1_FILTER, T("Filtro VU1:  sin datos todavía"),
			T("Juega unos segundos: el medidor del VU1 lista sus programas más costosos y aquí los apagas uno por uno (experimental)"));
		return;
	}
	const u32 program = s_vu1_candidates[s_vu1_index];
	const bool skipped = Vu1Skipped(program);
	char label[512];
	std::snprintf(label, sizeof(label), ForjaLang::TF("Filtro VU1 %u de %zu:  < programa %04x · %.1f %% >  %s"), s_vu1_index + 1,
		s_vu1_candidates.size(), program * 8, s_vu1_shares[s_vu1_index] / 10.0, skipped ? T("QUITADO") : T("Activo"));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_VU1_FILTER, label,
		skipped ? T("No se dibuja lo que hace este programa. X lo devuelve; Cuadrado devuelve todos. Se guarda para este juego") :
				  T("X lo apaga: mira qué deja de verse y cuánto sube la velocidad (experimental). Se guarda para este juego"));
}

static void ReloadVu1Filter()
{
	const u32 previous = s_vu1_candidates.empty() ? ~0u : s_vu1_candidates[s_vu1_index];
	s_vu1_candidates.clear();
	s_vu1_shares.clear();
	const u32 count = std::min(g_forja_vu1_top_count.load(std::memory_order_acquire), 8u);
	for (u32 k = 0; k < count; k++)
	{
		const u32 entry = g_forja_vu1_top[k].load(std::memory_order_relaxed);
		s_vu1_candidates.push_back(entry >> 16);
		s_vu1_shares.push_back(entry & 0xffff);
	}
	for (u32 program = 0; program < 2048; program++)
	{
		if (Vu1Skipped(program) && std::find(s_vu1_candidates.begin(), s_vu1_candidates.end(), program) == s_vu1_candidates.end())
		{
			s_vu1_candidates.push_back(program);
			s_vu1_shares.push_back(0);
		}
	}
	const auto again = std::find(s_vu1_candidates.begin(), s_vu1_candidates.end(), previous);
	s_vu1_index = again != s_vu1_candidates.end() ? static_cast<unsigned>(again - s_vu1_candidates.begin()) : 0u;
	PublishVu1FilterText();
}

static void ToggleVu1Program()
{
	if (s_vu1_candidates.empty())
		return;
	const u32 program = s_vu1_candidates[s_vu1_index];
	const bool skip = !Vu1Skipped(program);
	SetVu1Skipped(program, skip);
	const std::string list = Vu1SkipList();
	SavePlayerProfile("VU1 filter", [list](INISettingsInterface& ini) { ini.SetStringValue("Forja", "VU1Quitar", list.c_str()); }, false);
	ProbeLog("VU1 filter: program %04x %s; left out now: [%s]\n", program * 8, skip ? "left out" : "back", list.c_str());
	PublishVu1FilterText();
}

// Compilation 52: Cuadrado on the row: every program back at once.
static void BringBackVu1Programs()
{
	for (auto& word : g_forja_vu1_skip)
		word.store(0, std::memory_order_relaxed);
	SavePlayerProfile("VU1 filter", [](INISettingsInterface& ini) { ini.SetStringValue("Forja", "VU1Quitar", ""); }, false);
	ProbeLog("VU1 filter: every program back\n");
	ReloadVu1Filter();
}

// Forja SX2 (compilation 51): the GS filter (GSRendererHW.cpp), the VU1 filter's twin for the games the GS holds back. The menu
// offers the GS's eight costliest groups of draws of the last seconds (and those already left out); X leaves one out or brings
// it back, and the list goes in the player's profile ([Forja] GSQuitar, the groups' keys in hex). A group that draws an effect
// (a glow, an aura, a shadow) can be left out to save its GS time; one with the main picture leaves the screen wrong, and X
// brings it back. Dragon Ball Z Budokai Tenkaichi 4 is held back by its ~2,150 draws a frame, not by its VU1.
// Compilation 54: 64-bit keys with the texture's format and size (GSRendererHW.cpp has the layout); keys of builds 51-53 have
// no bit 63 and are dropped when a profile is read.
extern std::atomic<u64> g_forja_gs_filter[16];
extern std::atomic<u32> g_forja_gs_filter_count;
extern std::atomic<u64> g_forja_gs_top[8];
extern std::atomic<u32> g_forja_gs_top_info[8];
extern std::atomic<u32> g_forja_gs_top_count;
static constexpr u64 GS_KEY_MARK = u64{1} << 63;
static std::vector<u64> s_gs_filter_keys; // the groups left out (16 at most)
static std::vector<u64> s_gs_candidates;  // the menu's groups, in their places
static std::vector<u32> s_gs_infos;       // their share in permille << 16 | draws a second
static unsigned s_gs_index = 0;
// Compilation 52: each group's last measure, kept for the ones left out (the GS doesn't time what it doesn't draw).
static std::vector<std::pair<u64, u32>> s_gs_known;

static u32 GsKnownInfo(u64 key)
{
	for (const auto& [known, info] : s_gs_known)
	{
		if (known == key)
			return info;
	}
	return 0;
}

static void GsRemember(u64 key, u32 info)
{
	for (auto& [known, known_info] : s_gs_known)
	{
		if (known == key)
		{
			known_info = info;
			return;
		}
	}
	s_gs_known.emplace_back(key, info);
}

static bool GsFiltered(u64 key)
{
	return std::find(s_gs_filter_keys.begin(), s_gs_filter_keys.end(), key) != s_gs_filter_keys.end();
}

// The GS thread reads the count and then the keys, so the count is 0 while they change.
static void PublishGsFilter()
{
	g_forja_gs_filter_count.store(0, std::memory_order_release);
	const u32 count = static_cast<u32>(std::min<size_t>(s_gs_filter_keys.size(), 16));
	for (u32 i = 0; i < count; i++)
		g_forja_gs_filter[i].store(s_gs_filter_keys[i], std::memory_order_relaxed);
	g_forja_gs_filter_count.store(count, std::memory_order_release);
}

static std::string GsFilterList()
{
	std::string list;
	for (const u64 key : s_gs_filter_keys)
	{
		char text[24];
		std::snprintf(text, sizeof(text), "%s%llx", list.empty() ? "" : ",", static_cast<unsigned long long>(key));
		list += text;
	}
	return list;
}

static void ApplyGsFilterList(const std::string& list)
{
	s_gs_filter_keys.clear();
	const char* p = list.c_str();
	while (*p)
	{
		char* end = nullptr;
		const unsigned long long key = std::strtoull(p, &end, 16);
		if (end == p)
		{
			++p;
			continue;
		}
		if ((key & GS_KEY_MARK) != 0 && s_gs_filter_keys.size() < 16 && !GsFiltered(key))
			s_gs_filter_keys.push_back(key);
		p = end;
	}
	PublishGsFilter();
}

// "En la pantalla con la textura 03d40 (256x256, 256 colores), triángulos transparentes" (GS block addresses).
// 2026-10-19: in the library's language for the menu; the log keeps the Spanish (translated = false).
static std::string GsGroupText(u64 key, bool translated = true)
{
	const auto tr = [translated](const char* text) { return translated ? ForjaLang::T(text) : text; };
	const auto trf = [translated](const char* text) { return translated ? ForjaLang::TF(text) : text; };
	static constexpr const char* prims[] = {N_("puntos"), N_("líneas"), N_("triángulos"), N_("sprites")};
	const u32 fbp = static_cast<u32>(key >> 31) & 0x1ffu;
	const u32 tbp = static_cast<u32>(key >> 17) & 0x3fffu;
	const u32 psm = static_cast<u32>(key >> 11) & 0x3fu;
	const u32 tw = static_cast<u32>(key >> 7) & 0xfu, th = static_cast<u32>(key >> 3) & 0xfu;
	const char* prim = tr(prims[(key >> 1) & 3u]);
	const char* blend = (key & 1u) ? tr(N_(" transparentes")) : "";
	char where[128];
	std::snprintf(where, sizeof(where), "%s", tr(N_("En la pantalla"))); // compilation 52: the framebuffers the TV shows
	if (fbp != 0x1ffu)
		std::snprintf(where, sizeof(where), trf(N_("En %05x")), fbp << 5);
	char text[512];
	if (tbp == 0x3fffu)
	{
		std::snprintf(text, sizeof(text), trf(N_("%s sin textura, %s%s")), where, prim, blend);
		return text;
	}
	// The texture's colours: 4- and 8-bit ones are palettes, the others full colour or a depth buffer read as a texture.
	const char* colours = (psm == 0x14 || psm == 0x24 || psm == 0x2c) ? tr(N_("16 colores")) :
	                      (psm == 0x13 || psm == 0x1b)                ? tr(N_("256 colores")) :
	                      (psm >= 0x30)                               ? tr(N_("profundidad")) : tr(N_("color"));
	std::snprintf(text, sizeof(text), trf(N_("%s con la textura %05x (%ux%u, %s), %s%s")), where, tbp, 1u << tw, 1u << th, colours,
		prim, blend);
	return text;
}

static void PublishGsFilterText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	if (s_gs_candidates.empty())
	{
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_GS_FILTER, T("Filtro GS:  sin datos todavía"),
			T("Juega unos segundos: el GS lista sus grupos de dibujos más costosos y aquí los apagas uno por uno (experimental)"));
		return;
	}
	const u64 key = s_gs_candidates[s_gs_index];
	const bool skipped = GsFiltered(key);
	const u32 info = s_gs_infos[s_gs_index];
	char label[512];
	if (skipped && info == 0)
		std::snprintf(label, sizeof(label), TF("Filtro GS %u de %zu:  < grupo quitado >  QUITADO"), s_gs_index + 1, s_gs_candidates.size());
	else if (skipped)
		std::snprintf(label, sizeof(label), TF("Filtro GS %u de %zu:  < era el %.1f %% del GS · %u dibujos/s >  QUITADO"), s_gs_index + 1,
			s_gs_candidates.size(), (info >> 16) / 10.0, info & 0xffffu);
	else
		std::snprintf(label, sizeof(label), TF("Filtro GS %u de %zu:  < %.1f %% del GS · %u dibujos/s >  Activo"), s_gs_index + 1,
			s_gs_candidates.size(), (info >> 16) / 10.0, info & 0xffffu);
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_GS_FILTER, label,
		GsGroupText(key) + (skipped ? T(": no se dibuja. X lo devuelve; Cuadrado devuelve todos") :
									  T(". X lo apaga: mira qué deja de verse")));
}

// Compilation 54: the list keeps its places while a game runs. Builds 52-53 sorted it again by the last measure each time the
// menu opened, and two groups that cost about the same swapped places: in God of War II (2026-10-18) "the 2" left out what
// had been "the 1". Now a group stays where it first showed; one that no longer costs much gives its place to the next
// costliest; a group left out keeps its place; new ones go at the end while fewer than 8 are on.
static void ResetGsFilterList()
{
	s_gs_candidates.clear();
	s_gs_infos.clear();
	s_gs_known.clear();
	s_gs_index = 0;
}

static void ReloadGsFilter()
{
	const u64 previous = s_gs_candidates.empty() ? 0 : s_gs_candidates[s_gs_index];
	std::vector<std::pair<u64, u32>> measured; // costliest first, the ones still on
	const u32 count = std::min(g_forja_gs_top_count.load(std::memory_order_acquire), 8u);
	for (u32 k = 0; k < count; k++)
	{
		const u64 key = g_forja_gs_top[k].load(std::memory_order_relaxed);
		const u32 info = g_forja_gs_top_info[k].load(std::memory_order_relaxed);
		if (GsFiltered(key))
			continue; // still measured from before it was left out: its last measure is kept
		GsRemember(key, info);
		measured.emplace_back(key, info);
	}
	const auto take_measured = [&](u64 key) {
		for (auto it = measured.begin(); it != measured.end(); ++it)
		{
			if (it->first == key)
			{
				const u32 info = it->second;
				measured.erase(it);
				return info;
			}
		}
		return u32{0};
	};
	// Places already in the list: a group left out stays, one measured again gets its new measure, the others free their place
	// for the next costliest one measured now.
	std::vector<bool> free_place(s_gs_candidates.size(), false);
	for (size_t i = 0; i < s_gs_candidates.size(); i++)
	{
		const u64 key = s_gs_candidates[i];
		if (GsFiltered(key))
			s_gs_infos[i] = GsKnownInfo(key);
		else if (const u32 info = take_measured(key))
			s_gs_infos[i] = info;
		else
			free_place[i] = true;
	}
	for (size_t i = 0; i < s_gs_candidates.size(); i++)
	{
		if (!free_place[i] || measured.empty())
			continue;
		s_gs_candidates[i] = measured.front().first;
		s_gs_infos[i] = measured.front().second;
		measured.erase(measured.begin());
		free_place[i] = false;
	}
	// Compilation 56: a place nobody new takes keeps its group (with its last measure) instead of closing up: in build 54 the
	// ones after it moved up one, and Dragon Ball's "sin textura" sprites, left out as the 6, came back as the 5 (2026-10-19).
	size_t on = 0;
	for (const u64 key : s_gs_candidates)
		on += GsFiltered(key) ? 0 : 1;
	for (const auto& [key, info] : measured)
	{
		if (on >= 8)
			break;
		s_gs_candidates.push_back(key);
		s_gs_infos.push_back(info);
		on++;
	}
	// Groups left out that aren't listed yet (from the player's profile, at the game's start).
	for (const u64 key : s_gs_filter_keys)
	{
		if (std::find(s_gs_candidates.begin(), s_gs_candidates.end(), key) == s_gs_candidates.end())
		{
			s_gs_candidates.push_back(key);
			s_gs_infos.push_back(GsKnownInfo(key));
		}
	}
	const auto again = std::find(s_gs_candidates.begin(), s_gs_candidates.end(), previous);
	s_gs_index = again != s_gs_candidates.end() ? static_cast<unsigned>(again - s_gs_candidates.begin()) : 0u;
	PublishGsFilterText();
}

// Compilation 52: Cuadrado on the row: every group back at once.
static void BringBackGsGroups()
{
	if (s_gs_filter_keys.empty())
		return;
	s_gs_filter_keys.clear();
	PublishGsFilter();
	SavePlayerProfile("GS filter", [](INISettingsInterface& ini) { ini.SetStringValue("Forja", "GSQuitar", ""); }, false);
	ProbeLog("GS filter: every group back\n");
	ReloadGsFilter();
}

static void ToggleGsGroup()
{
	if (s_gs_candidates.empty())
		return;
	const u64 key = s_gs_candidates[s_gs_index];
	const bool skip = !GsFiltered(key);
	if (skip && s_gs_filter_keys.size() >= 16)
		return;
	if (skip)
		s_gs_filter_keys.push_back(key);
	else
		s_gs_filter_keys.erase(std::remove(s_gs_filter_keys.begin(), s_gs_filter_keys.end(), key), s_gs_filter_keys.end());
	PublishGsFilter();
	const std::string list = GsFilterList();
	SavePlayerProfile("GS filter", [list](INISettingsInterface& ini) { ini.SetStringValue("Forja", "GSQuitar", list.c_str()); }, false);
	ProbeLog("GS filter: group %u %llx (%s) %s; left out now: [%s]\n", s_gs_index + 1, static_cast<unsigned long long>(key),
		GsGroupText(key, false).c_str(), skip ? "left out" : "back", list.c_str());
	PublishGsFilterText();
}

// Forja SX2 (compilation 44): HD texture packs, PCSX2's texture replacements: /data/ForjaSX2/textures/<SERIAL>/replacements.
// 0 off, 1 on (loaded as they're needed), 2 on and preloaded at start (no stutter, more memory), as Dolphin for PS4 (v04.12).
// (s_game_textures is declared with s_game_sharpness, which ReadGameResolution fills too.)

static void PublishTexturesText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	static constexpr const char* names[] = {N_("No"), N_("Sí"), N_("Sí, precargadas")};
	const std::string serial = VMManager::GetDiscSerial();
	const std::string folder = Path::Combine(EmuFolders::Textures, serial + "/replacements");
	const bool pack = !serial.empty() && FileSystem::DirectoryExists(folder.c_str());
	char label[256], description[512];
	std::snprintf(label, sizeof(label), TF("Texturas HD:  < %s >"), T(names[std::min(s_game_textures, 2u)]));
	if (pack)
		std::snprintf(description, sizeof(description), TF("Pack encontrado (textures/%s/replacements). Precargar evita tirones pero usa más memoria"),
			serial.c_str());
	else
		std::snprintf(description, sizeof(description), TF("No hay pack: cópialo por FTP a /data/ForjaSX2/textures/%s/replacements"),
			serial.empty() ? "<SERIAL>" : serial.c_str());
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_TEXTURES, label, description);
}

static void ApplyGameTextures(unsigned value)
{
	s_game_textures = std::min(value, 2u);
	// Compilation 59: the choice is saved, but it only turns on with a pack for this game (VMManager.cpp, LoadCoreSettings).
	const std::string serial = VMManager::GetDiscSerial();
	const bool pack = !serial.empty() && FileSystem::DirectoryExists(Path::Combine(EmuFolders::Textures, serial + "/replacements").c_str());
	EmuConfig.GS.LoadTextureReplacements = pack && s_game_textures != 0;
	EmuConfig.GS.PrecacheTextureReplacements = pack && s_game_textures == 2;
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	const bool load = s_game_textures != 0, precache = s_game_textures == 2;
	SavePlayerProfile("HD textures", [load, precache](INISettingsInterface& ini) {
		ini.SetBoolValue("EmuCore/GS", "LoadTextureReplacements", load);
		ini.SetBoolValue("EmuCore/GS", "PrecacheTextureReplacements", precache);
	}, false);
	ProbeLog("HD textures: %u\n", s_game_textures);
	PublishTexturesText();
}

// Compilation 61: X on "Ajustes recomendados" fetches them and uses them at once (Joseph, 2026-10-20: "yo aplasto eso y me
// configuro a la mejor versión que hemos probado"). Until 60 it only fetched them, for the next start, under the player's own
// file, which won key by key: nothing changed. s_recommended_wait_since: when X asked (0 = not waiting); VMHostPoll uses them
// once the download ends. s_recommended_applied: the row says so until the menu opens again.
static double s_recommended_wait_since = 0.0;
static bool s_recommended_applied = false;

// Forja SX2 (compilation 44): where the game's recommended settings come from: the settings center (fetched from GitHub,
// PatchDownload.cpp), the package, or none. X fetches them again now.
static void PublishRecommendedText(bool fetching = false)
{
	const std::string serial = VMManager::GetDiscSerial();
	const std::string center = Path::Combine(EmuFolders::DataRoot, "centro/perfiles/" + serial + ".ini");
	const std::string package = Path::Combine(EmuFolders::Resources, "perfiles/" + serial + ".ini");
	using ForjaLang::T;
	struct stat info{};
	char label[512];
	if (fetching)
		std::snprintf(label, sizeof(label), "%s", T("Ajustes recomendados:  buscando en GitHub..."));
	else if (s_recommended_applied)
		std::snprintf(label, sizeof(label), "%s", T("Ajustes recomendados:  aplicados ahora"));
	else if (!serial.empty() && stat(center.c_str(), &info) == 0)
	{
		char date[32] = "?";
		const std::time_t when = info.st_mtime;
		if (const std::tm* tm = std::localtime(&when))
			std::strftime(date, sizeof(date), "%d/%m %H:%M", tm);
		std::snprintf(label, sizeof(label), ForjaLang::TF("Ajustes recomendados:  del centro (GitHub), %s"), date);
	}
	else if (!serial.empty() && FileSystem::FileExists(package.c_str()))
		std::snprintf(label, sizeof(label), "%s", T("Ajustes recomendados:  del PKG"));
	else
		std::snprintf(label, sizeof(label), "%s", T("Ajustes recomendados:  ninguno para este juego"));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_RECOMMENDED, label,
		T("X: traer de GitHub los mejores ajustes probados y usarlos ya. Los tuyos quedan en una copia; la resolución no cambia"));
}

// Forja SX2 (compilation 44): the other discs of a game of several, as Dolphin for PS4 changes them (v04.13). Its files sit
// together in roms/ named "<game> (Disco 1).iso", "(Disc 2)", "CD 3"...: the same name before the word and another number.
static std::vector<std::string> s_disc_paths;
static unsigned s_disc_index = 0;

static std::pair<std::string, int> DiscKey(std::string name)
{
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (const char* word : {"disco", "disc", "cd", "dvd"})
	{
		const size_t at = lower.rfind(word);
		if (at == std::string::npos)
			continue;
		size_t digit = at + std::strlen(word);
		while (digit < lower.size() && (lower[digit] == ' ' || lower[digit] == '_' || lower[digit] == '-' || lower[digit] == '.'))
			digit++;
		if (digit < lower.size() && lower[digit] >= '1' && lower[digit] <= '9')
		{
			std::string prefix = lower.substr(0, at);
			while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '(' || prefix.back() == '[' || prefix.back() == '-' ||
										  prefix.back() == '_'))
				prefix.pop_back();
			return {prefix, lower[digit] - '0'};
		}
	}
	return {std::string(), 0};
}

static void PublishDiscText()
{
	using ForjaLang::T;
	if (s_disc_paths.size() < 2)
	{
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_CHANGE_DISC, T("Cambiar disco:  este juego tiene un solo disco"),
			T("Para juegos de varios discos: ponlos en roms/ como \"Juego (Disco 1).iso\", \"Juego (Disco 2).iso\"..."));
		return;
	}
	const int number = DiscKey(std::string(Path::GetFileName(s_disc_paths[s_disc_index]))).second;
	const bool current = s_disc_paths[s_disc_index] == s_selected_game;
	char label[512];
	std::snprintf(label, sizeof(label), ForjaLang::TF("Cambiar disco:  < Disco %d de %zu >%s"), number, s_disc_paths.size(),
		current ? T("  (puesto)") : "");
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CHANGE_DISC, label,
		T("X: cambia al disco elegido, como abrir la bandeja de la PS2 y meter otro. Hazlo cuando el juego lo pida"));
}

static void ReloadDiscList()
{
	s_disc_paths.clear();
	s_disc_index = 0;
	const auto key = DiscKey(std::string(Path::GetFileName(s_selected_game)));
	if (key.second != 0)
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(std::string(Path::GetDirectory(s_selected_game)).c_str(), "*", FILESYSTEM_FIND_FILES, &files);
		std::vector<std::pair<int, std::string>> discs;
		for (const FILESYSTEM_FIND_DATA& file : files)
		{
			const auto other = DiscKey(std::string(Path::GetFileName(file.FileName)));
			if (other.second != 0 && other.first == key.first)
				discs.emplace_back(other.second, file.FileName);
		}
		std::sort(discs.begin(), discs.end());
		for (const auto& disc : discs)
		{
			if (disc.second == s_selected_game)
				s_disc_index = static_cast<unsigned>(s_disc_paths.size());
			s_disc_paths.push_back(disc.second);
		}
	}
	PublishDiscText();
}

static void ChangeToSelectedDisc()
{
	if (s_disc_paths.size() < 2 || s_disc_paths[s_disc_index] == s_selected_game)
		return;
	const std::string path = s_disc_paths[s_disc_index];
	const bool changed = VMManager::ChangeDisc(CDVD_SourceType::Iso, path);
	ProbeLog("Change disc: %s %s\n", path.c_str(), changed ? "in" : "FAILED");
	if (changed)
		s_selected_game = path;
	PublishDiscText();
}

// Forja SX2 (compilation 44): the settings the game runs with now, for the session's analysis (the log said the EE rate only
// when it changed, so where a session started was a guess).
static bool Vu1FastOn(); // compilation 126
static void LogGameSettings(const char* why)
{
	ProbeLog("Game settings (%s): ee_rate=%d frame_gen=%d upscale=%u sharpness=%u hd_textures=%u vu1_left_out=[%s] gs_skip=%d "
			 "gs_left_out=[%s] huellas=[%s] draw_thread=%d sw_jit=%d fpu_full=%d iop_fast=%d iop_sleep=%d skip_auto_fine=%d cpu_auto_up=%d fast_code=%d vu1_fast=%d effects_ahead=%d skip_repeats=%d cpu_seamless=%d disc_no_wait=%d safe_mode=%d skip_mode=%d code_ahead=%d vu_jumps=%d frame_push=%d fg_margin=%d\n", why,
		static_cast<int>(EmuConfig.Speedhacks.EECycleRate), g_orbis_fg_wanted ? (g_orbis_fg_fluid ? 2 : 1) : 0, s_game_upscale,
		s_game_sharpness, s_game_textures, Vu1SkipList().c_str(), s_gs_skip_mode, GsFilterList().c_str(), RulesOnList().c_str(),
		s_draw_thread ? 1 : 0, s_sw_jit ? 1 : 0, EmuConfig.Cpu.Recompiler.fpuFullMode ? 1 : 0, s_iop_fast ? 1 : 0, s_iop_sleep_game ? 1 : 0,
		s_skip_auto_fine ? 1 : 0, s_cpu_auto_up ? 1 : 0, g_ps4_skip_smc_checks ? 1 : 0, Vu1FastOn() ? 1 : 0, s_effects_ahead_game ? 1 : 0,
		EmuConfig.GS.SkipDuplicateFrames ? 1 : 0, s_cpu_seamless_game ? 1 : 0, s_disc_no_wait_game ? 1 : 0, s_safe_mode,
		static_cast<int>(EmuConfig.Speedhacks.EECycleSkipAuto), s_code_ahead_game ? 1 : 0, s_vu_jumps_game ? 1 : 0, s_frame_push ? 1 : 0,
		s_fg_margin ? 1 : 0);
}

// Compilation 76: two rows say so when their Auto can do nothing in this game, in place of their usual line (GameMenu.cpp's
// own text comes back when it can again). Saltar cuadros: UpdateGsSkip found nothing to leave out. CPU automático: it only
// lowers the PS2 CPU down to its floor, and the game's own is already there or lower (on 2026-10-23 it did nothing in any of
// five games, all at 27-31 % with floors of 27 % or 50 %, while the row said "Hasta 50 %").
static int EERatePercent(int rate);

static bool CpuAutoIdle()
{
	return s_cpu_auto_floor != 0 && s_cpu_auto_steps == 0 && s_game_ee_rate <= s_cpu_auto_floor;
}

static void PublishAutoNotes()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	if (s_gs_skip_mode == 1 && s_gs_skip_useless)
	{
		std::snprintf(label, sizeof(label), TF("Saltar cuadros (GS):  < %s >"), T("Auto"));
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_GS_SKIP, label,
			T("Auto se apagó: este juego dibuja su imagen aparte y después la copia a la pantalla, así que no hay cuadros que saltar  ·  ahora: dibuja todos"));
	}
	else
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_GS_SKIP, "", "");
	if (CpuAutoIdle())
	{
		// GameMenu.cpp's names for the row's values.
		static constexpr const char* names[] = {N_("No"), N_("Hasta 75 %"), N_("Hasta 60 %"), N_("Hasta 50 %"), N_("Hasta 36 % (exp.)"),
			N_("Hasta 31 % (exp.)"), N_("Hasta 27 % (exp.)")};
		std::snprintf(label, sizeof(label), TF("CPU automático:  < %s >"), T(names[std::clamp(-s_cpu_auto_floor, 0, 6)]));
		char description[512];
		std::snprintf(description, sizeof(description),
			TF("No actúa: el CPU de la PS2 de este juego (%d %%) ya está igual o más bajo. Elige un límite más bajo o sube el CPU de la PS2"),
			EERatePercent(s_game_ee_rate));
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_CPU_AUTO, label, description);
	}
	else
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_CPU_AUTO, "", "");
}

// Compilation 82: whether the running game draws its crowd in software (the GameDB's GSC_GuitarHero, render fixes on).
static bool GameDrawsCrowdInSoftware()
{
	static const s16 guitar_hero = GSLookupGetSkipCountFunctionId("GSC_GuitarHero");
	return guitar_hero >= 0 && EmuConfig.GS.GetSkipCountFunctionId == guitar_hero && !EmuConfig.GS.UserHacks_DisableRenderFixes;
}

// Compilation 100: "Salto de ciclo" (EECycleSkip): each VU1 program's cycles are also taken from the EE (MTVU.cpp), as if it had
// waited for it. Guitar Hero III went from 87 to 99.8 % with 1 (2026-10-31): the EE goes from 23 to 18 ms a frame, and the game
// shows some frames for two refreshes instead of the whole song running slow.
// Compilation 102: in quarters too, up to 1 (ForjaCycleSkipSteps): between 90 % with every frame and 100 % with 18 % of them shown twice.
static constexpr u8 ForjaCycleSkipSteps[] = {0, 1, 2, 3, 4, 8, 12};

// Compilation 103: Auto, after them. Compilation 140: only "Al vuelo" there (the old Auto is gone).
static constexpr unsigned ForjaCycleSkipAutoStep = std::size(ForjaCycleSkipSteps);

static unsigned ForjaCycleSkipStep()
{
	if (EmuConfig.Speedhacks.EECycleSkipAuto)
		return ForjaCycleSkipAutoStep; // "Al vuelo"
	const unsigned quarters = EmuConfig.Speedhacks.EECycleSkip * 4u + EmuConfig.Speedhacks.EECycleSkipQuarters;
	unsigned step = 0;
	while (step + 1 < std::size(ForjaCycleSkipSteps) && ForjaCycleSkipSteps[step] < quarters)
		step++;
	return step;
}

static void PublishCycleSkipText()
{
	char label[256];
	static constexpr const char* names[] = {N_("No"), "1/4", "1/2", "3/4", "1", "2", "3"};
	char value[64];
	if (EmuConfig.Speedhacks.EECycleSkipAuto) // compilation 131: "Al vuelo", in quarters (140: the only automatic one)
	{
		const unsigned quarters = EmuConfig.Speedhacks.EECycleSkip * 4u + EmuConfig.Speedhacks.EECycleSkipQuarters;
		static constexpr const char* parts[] = {"", "1/4", "1/2", "3/4"};
		char amount[32];
		if (quarters == 0)
			std::snprintf(amount, sizeof(amount), "%s", ForjaLang::T("No"));
		else if (quarters % 4 == 0)
			std::snprintf(amount, sizeof(amount), "%u", quarters / 4);
		else if (quarters < 4)
			std::snprintf(amount, sizeof(amount), "%s", parts[quarters]);
		else
			std::snprintf(amount, sizeof(amount), "%u %s", quarters / 4, parts[quarters % 4]);
		std::snprintf(value, sizeof(value), "%s: %s", ForjaLang::T("Al vuelo"), amount);
	}
	else
		std::snprintf(value, sizeof(value), "%s", ForjaLang::T(names[ForjaCycleSkipStep()]));
	std::snprintf(label, sizeof(label), ForjaLang::TF("Salto de ciclo:  < %s >"), value);
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CYCLE_SKIP, label,
		ForjaLang::T("Quita trabajo a la consola: la velocidad se acerca al 100 %, pero el juego puede repetir cuadros. Al vuelo lo ajusta solo en cada refresco y recuerda lo de la última partida. Se guarda para este juego"));
}

// Compilation 100 (Joseph liked God of War II without the limiter, as a setting and not as "fast forward"): the normal speed of a game
// (PCSX2's NominalScalar, in its profile): 100 %, 130 % or no limit in practice (1000 %). It gives no extra power: where the PS4 has
// some left, the game and its sound run faster. In Guitar Hero's songs the limit changed nothing (87 and 88 %, 2026-10-31).
static void PublishSpeedLimitText()
{
	const float scalar = EmuConfig.EmulationSpeed.NominalScalar;
	char label[256];
	std::snprintf(label, sizeof(label), ForjaLang::TF("Límite de velocidad:  < %s >"),
		scalar > 2.0f ? ForjaLang::T("Sin límite") : scalar > 1.1f ? "130 %" : "100 %");
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_SPEED_LIMIT, label,
		ForjaLang::T("100 % es la velocidad real. Más alto: el juego y su sonido van más rápido si a la PS4 le sobra potencia; no da más rendimiento. Se guarda para este juego"));
}

// Compilation 104: the two options that took a file by FTP until now (Joseph, 2026-10-31, Guitar Hero III: "ya no parpadea casi nada ... pero se ve
// borroso"). The picture of a game that draws by fields: Auto (adaptive) flickers in Guitar Hero III's guitar, a blend of the two fields doesn't and
// it's softer, and without a blend (bob) nothing is mixed. Both take effect at once, without a reload.
static constexpr std::pair<GSInterlaceMode, const char*> ForjaDeinterlaceSteps[] = {{GSInterlaceMode::Automatic, "Auto"},
	{GSInterlaceMode::BlendTFF, N_("Mezcla")}, {GSInterlaceMode::BobTFF, N_("Sin mezcla")}};
static constexpr std::pair<BiFiltering, const char*> ForjaTextureFilterSteps[] = {{BiFiltering::PS2, N_("Normal")}, {BiFiltering::Forced, N_("Suave")}};

template <typename Value, size_t N>
static unsigned ForjaImageStep(const std::pair<Value, const char*> (&steps)[N], Value now)
{
	for (unsigned i = 0; i < N; i++)
	{
		if (steps[i].first == now)
			return i;
	}
	return 0;
}

// Compilation 137: whether the row is on "Auto" turned into the game's factory mode (VMManager.cpp, UpdateGameSettingsLayer).
static bool ForjaDeinterlaceAutoFactory()
{
	const int factory = g_forja_deinterlace_factory.load(std::memory_order_relaxed);
	return g_forja_deinterlace_auto.load(std::memory_order_relaxed) && factory > 0 && static_cast<int>(EmuConfig.GS.InterlaceMode) == factory;
}

static void PublishImageOptionsText()
{
	char label[256];
	char value[96];
	const char* name = ForjaLang::T(ForjaDeinterlaceSteps[ForjaImageStep(ForjaDeinterlaceSteps, EmuConfig.GS.InterlaceMode)].second);
	if (ForjaDeinterlaceAutoFactory())
		std::snprintf(value, sizeof(value), "Auto (%s)", name); // compilation 137
	else
		std::snprintf(value, sizeof(value), "%s", name);
	std::snprintf(label, sizeof(label), ForjaLang::TF("Desentrelazado:  < %s >"), value);
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_DEINTERLACE, label,
		ForjaLang::T("Si la imagen parpadea: Mezcla lo quita, pero se ve un poco más suave. Auto usa lo que se midió para este juego, si lo hay. Se guarda para este juego"));
	std::snprintf(label, sizeof(label), ForjaLang::TF("Filtro de texturas:  < %s >"),
		ForjaLang::T(ForjaTextureFilterSteps[ForjaImageStep(ForjaTextureFilterSteps, EmuConfig.GS.TextureFiltering)].second));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_TEXTURE_FILTER, label,
		ForjaLang::T("Suave alisa lo pixelado, pero puede verse borroso. Normal es como en la PS2. Se guarda para este juego"));
}

static void PublishCrowdText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	if (!GameDrawsCrowdInSoftware())
	{
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_CROWD, T("Público por software:  este juego no lo usa"),
			T("Solo los Guitar Hero dibujan su público por software. En este juego no cambia nada"));
		return;
	}
	char label[256];
	if (s_crowd_every == CROWD_AUTO) // compilation 137
	{
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_CROWD, T("Público por software:  < Al vuelo >"),
			T("Dibuja el público en cada cuadro mientras a la consola le sobra tiempo, y menos veces (hasta 1 de 4) solo cuando le falta. Así se mueve fluido sin quitar velocidad. Se guarda para este juego"));
		return;
	}
	if (s_crowd_every <= 1)
		std::snprintf(label, sizeof(label), "%s", T("Público por software:  < todos los cuadros (lento) >"));
	else
		std::snprintf(label, sizeof(label), TF("Público por software:  < 1 de cada %u cuadros >"), s_crowd_every);
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CROWD, label,
		T("Este juego dibuja su público con la CPU, lo que más lo frena. Menos veces = más velocidad; el público se mueve menos fluido. Se guarda para este juego"));
}

// Compilation 144: the network rows (Extras). PCSX2's [DEV9/Eth] and Forja's [DEV9/ForjaLAN] for this game, read from the running settings.
static int s_keyboard_target = 0; // what the PS4's keyboard writes: 0 the report's comment, 1 the DNS, 2 the room's host, 3 its key

static int NetMode()
{
	if (!EmuConfig.DEV9.EthEnable)
		return 0;
	return EmuConfig.DEV9.EthApi == Pcsx2Config::DEV9Options::NetApi::ForjaLAN ? 2 : 1;
}

static void PublishNetText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	static constexpr const char* modes[] = {N_("No"), N_("Internet"), N_("Sala LAN")};
	char label[320];
	std::snprintf(label, sizeof(label), TF("Juego en línea:  < %s >"), T(modes[NetMode()]));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_NET_MODE, label,
		T("Internet: el online original del juego, en los servidores de fans que lo revivieron (pon abajo su DNS). Sala LAN: el modo LAN del juego con amigos por internet; una PS4 crea la sala y las demás entran. Reinicia el juego para que lo vea. Se guarda para este juego"));
	char dns[32] = {};
	const u8* d = EmuConfig.DEV9.DNS1;
	if (EmuConfig.DEV9.ModeDNS1 == Pcsx2Config::DEV9Options::DnsMode::Manual)
		std::snprintf(dns, sizeof(dns), "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
	std::snprintf(label, sizeof(label), TF("DNS del servidor:  < %s >"), dns[0] ? dns : T("Auto (PS2Online)"));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_NET_DNS, label,
		T("Solo para Internet. Auto usa el de PS2Online, la comunidad que revivió el online de muchos juegos (Mortal Kombat Armageddon ya entra). Si a tu juego lo revivió otro grupo, escribe su DNS con X; Cuadrado vuelve a Auto"));
	std::snprintf(label, sizeof(label), TF("Sala LAN, anfitrión:  < %s >"),
		EmuConfig.DEV9.LanHost.empty() ? T("esta PS4 crea la sala") : EmuConfig.DEV9.LanHost.c_str());
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_NET_HOST, label,
		T("Vacío: esta PS4 crea la sala. Para entrar a la de un amigo, escribe su IP o su dominio. X lo escribe, Cuadrado lo borra"));
	const std::string stars(std::min<size_t>(EmuConfig.DEV9.LanKey.size(), 12), '*');
	std::snprintf(label, sizeof(label), TF("Sala LAN, clave:  < %s >"), stars.empty() ? T("ninguna (hace falta una)") : stars.c_str());
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_NET_KEY, label,
		T("La misma clave para todos los de la sala; sin clave la sala no se abre. X la escribe, Cuadrado la borra"));
	std::snprintf(label, sizeof(label), TF("Abrir el puerto del router (UPnP):  < %s >"), T(EmuConfig.DEV9.LanUPnP ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_NET_UPNP, label,
		T("Solo para quien crea la sala: Forja le pide al router que abra el puerto UDP 41020 mientras la sala está abierta. Con No, ábrelo tú en el router"));
}

// A dotted IPv4 address (four numbers up to 255), for the DNS row.
static bool NetIsIPv4(const std::string& text)
{
	unsigned a, b, c, d;
	char extra;
	return std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
}

// What the keyboard wrote for a network row (target 1-3), or its Cuadrado (an empty text): saved for this game.
static void SaveNetText(int target, std::string text)
{
	while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	while (!text.empty() && text.front() == ' ')
		text.erase(text.begin());
	if (target == 1)
	{
		if (!text.empty() && !NetIsIPv4(text))
		{
			ProbeLog("Network: \"%s\" is not an IPv4 address; the DNS stays as it was\n", text.c_str());
			return;
		}
		SavePlayerProfile("Network DNS", [text](INISettingsInterface& ini) {
			ini.SetStringValue("DEV9/Eth", "ModeDNS1", text.empty() ? "Auto" : "Manual");
			if (!text.empty())
				ini.SetStringValue("DEV9/Eth", "DNS1", text.c_str());
		}, true);
	}
	else if (target == 2)
	{
		text = text.substr(0, 120);
		SavePlayerProfile("Network room host", [text](INISettingsInterface& ini) { ini.SetStringValue("DEV9/ForjaLAN", "Host", text.c_str()); }, true);
	}
	else if (target == 3)
	{
		text = text.substr(0, 64);
		SavePlayerProfile("Network room key", [text](INISettingsInterface& ini) { ini.SetStringValue("DEV9/ForjaLAN", "Key", text.c_str()); }, true);
	}
}

// 147 in the app: [Forja] BucleDeEspera, "<block hex>:<gp offset of the call pointer, decimal>" with commas, for the EE recompiler
// (iR5900.cpp checks each loop again when it makes the block).
extern void ForjaSetWaitLoops(const u32* pcs, const u32* guards, u32 count);
static void ApplyWaitLoops(const std::string& list)
{
	u32 pcs[4] = {}, guards[4] = {}, count = 0;
	size_t at = 0;
	while (at < list.size() && count < 4)
	{
		const size_t end = std::min(list.find(',', at), list.size());
		const std::string item = list.substr(at, end - at);
		at = end + 1;
		unsigned pc = 0;
		int offset = 0;
		const int got = std::sscanf(item.c_str(), " %x : %d", &pc, &offset);
		if (got < 1 || pc == 0 || (pc & 3) != 0)
			continue;
		pcs[count] = pc;
		guards[count] = got >= 2 && offset >= -32768 && offset <= 32767 ? (0x80000000u | (static_cast<u32>(offset) & 0xffffu)) : 0u;
		count++;
	}
	ForjaSetWaitLoops(pcs, guards, count);
	if (count)
		ProbeLog("Wait loops (147 in the app): %u listed (%s)\n", count, list.c_str());
}

// Compilation 116: the GS recorder's row, in No tocar until it is measured in the six test games.
// Compilation 117: the software draws' JIT row, in No tocar until it is measured.
static void PublishSwJitText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Dibujo por software con traductor:  < %s >"), T(s_sw_jit ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_SW_JIT, label,
		T("Prueba: los dibujos que el juego hace con la CPU (el público de Guitar Hero, las paletas de Dragon Ball) usan código traducido en vez del lento. Si el juego se cierra o se ve mal, ponlo en No. Se guarda para este juego"));
}

// Compilation 120: "FPU exacto" (PCSX2's fpuFullMode, from the GameDB's eeClampMode 3 or the player's profile) and "Escrituras
// rápidas del IOP", both in No tocar until they are measured.
static void PublishFpuExactText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	// Compilation 139: on in a game whose GameDB entry does not ask for it (eeClampMode 3), the row says so. God of War II and Shadow
	// of the Colossus (2026-11-14/17): it was left on from the 136 in the player's profile, costing work for nothing.
	const GameDatabaseSchema::GameEntry* entry = GameDatabase::findGame(VMManager::GetDiscSerial());
	const bool asked = entry && entry->eeClampMode == GameDatabaseSchema::ClampMode::Full;
	const char* value = !EmuConfig.Cpu.Recompiler.fpuFullMode ? N_("No") : asked ? N_("Sí") : N_("Sí (más lento: este juego no lo pide)");
	std::snprintf(label, sizeof(label), TF("FPU exacto:  < %s >"), T(value));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_FPU_EXACT, label,
		T("Prueba: las cuentas con decimales del juego, exactas pero más lentas. La base de datos lo prende en algunos juegos (Bully) por un detalle puntual. En No puede ir más rápido; si algo sale mal, vuelve a Sí. Se guarda para este juego"));
}

static void PublishIopFastText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Escrituras rápidas del IOP:  < %s >"), T(s_iop_fast ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_IOP_FAST, label,
		T("Prueba: cuando el IOP (el procesador del disco y del sonido) escribe en su memoria, solo borra código traducido donde de verdad lo hay. Debería verse y sonar igual. Si el juego se cierra o falla, ponlo en No. Se guarda para este juego"));
}

// Compilation 122: the three rows of No tocar for Shadow of the Colossus's "aumentan los FPS pero disminuye la velocidad" (Joseph, 2026-11-07).
static void PublishIopSleepText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("IOP dormido en el juego:  < %s >"), T(s_iop_sleep_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_IOP_SLEEP, label,
		T("Prueba: el IOP (el procesador del disco y del sonido) duerme mientras espera, como ya hace en los videos, y deja más tiempo al juego. Si el sonido se corta o el juego se traba, ponlo en No. Se guarda para este juego"));
}

static void PublishCpuAutoUpText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("CPU automático vuelve a subir:  < %s >"), T(s_cpu_auto_up ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CPU_AUTO_UP, label,
		T("Prueba, con el CPU automático: cuando el juego va bien, vuelve a subir la CPU de la PS2 hasta la que elegiste. Con el Salto en Auto, primero salta y la CPU baja solo si el salto no alcanza. Cada cambio de CPU es un tirón corto. Se guarda para este juego"));
}

// Compilation 126: "VU1 rápido", read from the running settings (the profile's keys win over the GameDB's).
static bool Vu1FastOn()
{
	return EmuConfig.Speedhacks.vuFlagHack && !EmuConfig.Cpu.Recompiler.vu1Overflow;
}

static void PublishVu1FastText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("VU1 rápido:  < %s >"), T(Vu1FastOn() ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_VU1_FAST, label,
		T("El VU1 deja de calcular datos que casi ningún programa usa y de recortar números, y trabaja menos. Viene en Sí, salvo en los juegos que la base de datos marca como delicados. Si aparecen polígonos estirados o formas raras, ponlo en No. Se guarda para este juego"));
}

// Compilation 127: "Efectos listos al arrancar".
static void PublishEffectsAheadText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Efectos listos al arrancar:  < %s >"), T(s_effects_ahead_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_EFFECTS_AHEAD, label,
		T("Al arrancar el juego, la gráfica prepara los efectos que usó en las partidas anteriores, para que no se armen en pleno juego y den tirones. Viene en Sí; la primera partida de cada juego arma la lista. Se guarda para este juego"));
}

// Compilation 128: "Refrescos sin repetir" (PCSX2's SkipDuplicateFrames, read from the running settings).
static void PublishSkipRepeatsText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Refrescos sin repetir:  < %s >"), T(EmuConfig.GS.SkipDuplicateFrames ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_SKIP_REPEATS, label,
		T("Prueba: cuando el juego no hizo un cuadro nuevo, el emulador no vuelve a mostrar el mismo y el GS se ahorra ese trabajo. Sirve en los juegos lentos que frena el GS (Shadow of the Colossus). Si la imagen da saltos raros, ponlo en No. Se guarda para este juego"));
}

// Compilation 132: the three new rows of No tocar.
static void PublishTestRowsText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Código listo al arrancar:  < %s >"), T(s_code_ahead_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CODE_AHEAD, label,
		T("Prueba: Forja recuerda qué partes del juego tradujo en partidas anteriores y las traduce de antemano en los ratos libres, para que no den tirones al llegar a ellas. La primera partida solo aprende; se nota desde la segunda. Si el juego falla, ponlo en No. Se guarda para este juego"));
	std::snprintf(label, sizeof(label), TF("Saltos rápidos del VU1:  < %s >"), T(s_vu_jumps_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_VU_JUMPS, label,
		T("Prueba: los saltos de los programas del VU1 se resuelven sin salir del código traducido. Alivia el VU1 en los juegos que lo llenan (God of War). Si la imagen se rompe o el juego falla, ponlo en No. Se guarda para este juego"));
	std::snprintf(label, sizeof(label), TF("Cuadros generados con margen:  < %s >"), T(s_fg_margin ? N_("Sí") : N_("No"))); // compilation 136
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_FG_MARGIN, label,
		T("Prueba: con la generación de cuadros prendida, solo arma un cuadro cuando a la consola le sobra tiempo. Así generar nunca le quita velocidad al juego y no se apaga sola. Se guarda para este juego"));
	std::snprintf(label, sizeof(label), TF("Empujón de cuadros:  < %s >"), T(s_frame_push ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_FRAME_PUSH, label,
		T("Prueba: cuando el juego queda dando menos cuadros que hace un momento aunque a la consola le sobre fuerza, sube la CPU de la PS2 dos segundos y la devuelve. Sirve con la CPU de la PS2 por debajo de 100 %. Se guarda para este juego"));
}

// Compilation 131: "Modo seguro".
static void PublishSafeModeText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	static constexpr const char* names[] = {N_("No"), N_("Parcial"), N_("Total")};
	char label[256];
	std::snprintf(label, sizeof(label), TF("Modo seguro:  < %s >"), T(names[std::clamp(s_safe_mode, 0, 2)]));
	// Compilation 140: the guardian goes one suspect at a time; the row says which it turned off (changing the row clears them).
	std::string description = T("Si este juego se cierra, se traba o se ve mal, prueba Parcial y después Total: apagan mejoras internas de velocidad, y el juego puede ir más lento. Si una partida termina mal, Forja apaga por su cuenta una mejora sospechosa por vez. Se guarda para este juego");
	if (!s_profile.guard_off.empty())
	{
		char off[256];
		std::snprintf(off, sizeof(off), TF("Forja ya apagó en este juego: %s"), s_profile.guard_off.c_str());
		description += std::string(". ") + off;
	}
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_SAFE_MODE, label, description.c_str());
}

// Compilation 130: "Disco sin esperas".
static void PublishDiscNoWaitText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Disco sin esperas:  < %s >"), T(s_disc_no_wait_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_DISC_NO_WAIT, label,
		T("Prueba: cuando el juego pide algo del disco y la PS4 todavía no lo leyó, el emulador sigue andando en vez de congelarse a esperarlo. Sirve con las cargas rápidas del disco (Bully). Si una carga se traba o un video se corta, ponlo en No. Se guarda para este juego"));
}

// Compilation 128: "CPU sin tirones".
static void PublishCpuSeamlessText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("CPU sin tirones:  < %s >"), T(s_cpu_seamless_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_CPU_SEAMLESS, label,
		T("Prueba: al cambiar la velocidad de la CPU de la PS2, el emulador ya no borra el código traducido del juego y cambia sin dar tirones. Con el CPU automático prendido, este sigue la escena en un segundo: baja en lo pesado y sube al salir. Si el juego se cierra o se traba, ponlo en No. Se guarda para este juego"));
}

// Compilation 124: "Revisión rápida del código".
static void PublishFastCodeText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Revisión rápida del código:  < %s >"), T(s_fast_code_game ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_FAST_CODE_CHECK, label,
		T("Prueba: el EE deja de revisar el código del juego antes de cada bloque y trabaja menos. Algunos juegos que cambian su propio código (GTA San Andreas) no arrancan así: si el juego se cierra o se traba, ponlo en No. Se guarda para este juego"));
}

static void PublishDrawThreadText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Grabar dibujos en otro hilo:  < %s >"), T(s_draw_thread ? N_("Sí") : N_("No")));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_DRAW_THREAD, label,
		T("Prueba: la gráfica se graba en un núcleo libre y el GS sigue. Si el juego se cierra o se ve mal, ponlo en No. Se guarda para este juego"));
}

// Compilation 77: "Reportar este juego" (SendReport, PublishReportText): the player's word for it, and when the last one went.
// Compilation 79 (Joseph, 2026-10-23: "también tienen que poner mal, bien, bueno ... perfecto"): five words, worst to best, starting
// in the middle so the word is the player's, not ours.
static constexpr const char* REPORT_RATINGS[] = {N_("Pésimo"), N_("Mal"), N_("Regular"), N_("Bien"), N_("Perfecto")};
static constexpr int REPORT_RATING_COUNT = static_cast<int>(std::size(REPORT_RATINGS));
static int s_report_rating = 2; // "Regular"
// Compilation 110: whether the player ever moved the word. The 19 reports from Joseph's PS4 of 2026-11-02/03 all said "Regular", the word
// the row starts on: X sends at once, so an untouched word said nothing. It now goes as "Regular (sin elegir)".
static bool s_report_rating_chosen = false;
// Compilation 80: what went wrong, the half of "compatible" no meter sees (the picture, the sound, the controls).
static constexpr const char* REPORT_ISSUES[] = {N_("Nada en especial"), N_("Va lento"), N_("Fallos de imagen"),
	N_("Fallos de sonido"), N_("Los controles"), N_("Se cuelga o se cierra")};
static constexpr int REPORT_ISSUE_COUNT = static_cast<int>(std::size(REPORT_ISSUES));
static int s_report_issue = 0;
// Compilation 86: the player's own words for the report (Joseph, 2026-10-24: "para agregar comentarios, un comentario
// adicional"), UTF-8, 200 characters at most; it goes with the next report and is cleared after it.
static std::string s_report_comment;
// Compilation 87: ready phrases for the comment, with Left/Right on its row, for when the keyboard doesn't open (in the 86
// it never did) or for a quick one. In the player's language.
static constexpr const char* REPORT_PHRASES[] = {N_("Se ve pixelado"), N_("La imagen parpadea"), N_("Faltan cosas en la pantalla"),
	N_("Colores raros"), N_("Va lento en las peleas o en el juego"), N_("Va lento en los menús o en los videos"), N_("Da tirones"),
	N_("Se corta el sonido")};
static constexpr int REPORT_PHRASE_COUNT = static_cast<int>(std::size(REPORT_PHRASES));
static int s_report_phrase = -1;
// Compilation 88: where it shows, the part only the player sees (Joseph, 2026-10-25: "queremos comentarios específicos...
// lo que nosotros no podemos ver"). Left/Right on its row; it goes in Spanish in the report, as these lists are written.
static constexpr const char* REPORT_WHERE[] = {N_("En los personajes"), N_("En el fondo o el escenario"), N_("En el público"),
	N_("En los textos o los menús"), N_("En las luces, los brillos o las sombras"), N_("En toda la pantalla"),
	N_("Al empezar o al terminar una pelea o canción"), N_("En los videos")};
static constexpr int REPORT_WHERE_COUNT = static_cast<int>(std::size(REPORT_WHERE));
static int s_report_where = -1;

// Compilation 86: the PS4's own keyboard (the system's IME dialog) for the comment. The SDK has no stub library for it
// (libSceImeDialog), so its module and the common dialogs' are loaded and their functions found by name the first time; if
// anything fails the row says so, and the report goes without a comment. While it is open the menu reads no buttons.
namespace ForjaKeyboard
{
	// OrbisImeDialogSetting (orbis/_types/ime_dialog.h) with the PS4's 16-bit characters: the header's wchar_t is 32-bit here.
	struct Setting
	{
		uint32_t user_id;
		int32_t type;
		uint64_t supported_languages;
		int32_t enter_label;
		int32_t input_method;
		void* filter;
		uint32_t option;
		uint32_t max_text_length;
		char16_t* input_text_buffer;
		float posx, posy;
		int32_t horizontal_alignment, vertical_alignment;
		const char16_t* placeholder;
		const char16_t* title;
		int8_t reserved[16];
	};
	static_assert(sizeof(Setting) == 96, "the IME dialog's setting");
	struct Result
	{
		int32_t end_status; // 0 OK, 1 cancelled, 2 aborted
		int8_t reserved[12];
	};

	static constexpr uint32_t MaxLength = 200;
	static int32_t (*s_init)(const Setting*, void*) = nullptr;
	static int32_t (*s_status)() = nullptr; // 0 none, 1 running, 2 finished
	static int32_t (*s_result)(Result*) = nullptr;
	static int32_t (*s_term)() = nullptr;
	static bool s_loaded = false, s_failed = false, s_open = false;
	static char16_t s_buffer[MaxLength + 1];
	static char16_t s_title[64];
	static std::string s_opened_with; // compilation 88: the text it opened with

	static void ToUtf16(const std::string& in, char16_t* out, size_t capacity)
	{
		size_t n = 0;
		for (size_t i = 0; i < in.size() && n + 2 < capacity;)
		{
			const unsigned char c = static_cast<unsigned char>(in[i]);
			uint32_t code = c;
			int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
			if (extra)
				code = c & (0x3F >> extra);
			i++;
			for (; extra > 0 && i < in.size(); extra--, i++)
				code = (code << 6) | (static_cast<unsigned char>(in[i]) & 0x3F);
			if (code >= 0x10000)
			{
				code -= 0x10000;
				out[n++] = static_cast<char16_t>(0xD800 + (code >> 10));
				out[n++] = static_cast<char16_t>(0xDC00 + (code & 0x3FF));
			}
			else
				out[n++] = static_cast<char16_t>(code);
		}
		out[n] = 0;
	}

	static std::string ToUtf8(const char16_t* in)
	{
		std::string out;
		for (size_t i = 0; in[i] != 0; i++)
		{
			uint32_t code = in[i];
			if (code >= 0xD800 && code < 0xDC00 && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
				code = 0x10000 + ((code - 0xD800) << 10) + (in[++i] - 0xDC00);
			if (code < 0x80)
				out += static_cast<char>(code);
			else if (code < 0x800)
			{
				out += static_cast<char>(0xC0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			else if (code < 0x10000)
			{
				out += static_cast<char>(0xE0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (code >> 18));
				out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
		}
		return out;
	}

	// Compilation 87: a loaded module's handle, found among the app's modules by its name ("libSceImeDialog" is
	// "libSceImeDialog.sprx" there). -1 if it isn't loaded.
	static int FindModule(const char* name)
	{
		OrbisKernelModule handles[256];
		size_t count = 0;
		if (sceKernelGetModuleList(handles, std::size(handles), &count) != 0)
			return -1;
		const size_t length = std::strlen(name);
		for (size_t i = 0; i < std::min(count, std::size(handles)); i++)
		{
			OrbisKernelModuleInfo info{};
			info.size = sizeof(info);
			if (sceKernelGetModuleInfo(handles[i], &info) == 0 && std::strncmp(info.name, name, length) == 0 &&
				(info.name[length] == '.' || info.name[length] == '\0'))
				return static_cast<int>(handles[i]);
		}
		return -1;
	}

	static bool Load()
	{
		if (s_loaded || s_failed)
			return s_loaded;
		// Compilation 87: through the system's module loader, as the PS4's own apps load them, and then found by name. The 86
		// loaded them from /system/common/lib, and that path isn't there for this app (0x80020002, "no such file", both;
		// Joseph, 2026-10-25): the keyboard never opened. That way stays as the last try.
		const int common_load = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
		const int ime_load = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
		int common = FindModule("libSceCommonDialog");
		int ime = FindModule("libSceImeDialog");
		if (common < 0)
			common = static_cast<int>(sceKernelLoadStartModule("/system/common/lib/libSceCommonDialog.sprx", 0, nullptr, 0, nullptr, nullptr));
		if (ime < 0)
			ime = static_cast<int>(sceKernelLoadStartModule("/system/common/lib/libSceImeDialog.sprx", 0, nullptr, 0, nullptr, nullptr));
		ProbeLog("Keyboard: system loader %#x (common dialogs) and %#x (keyboard)\n", static_cast<unsigned>(common_load),
			static_cast<unsigned>(ime_load));
		void* common_init = nullptr;
		if (common >= 0)
			sceKernelDlsym(common, "sceCommonDialogInitialize", &common_init);
		if (ime >= 0)
		{
			sceKernelDlsym(ime, "sceImeDialogInit", reinterpret_cast<void**>(&s_init));
			sceKernelDlsym(ime, "sceImeDialogGetStatus", reinterpret_cast<void**>(&s_status));
			sceKernelDlsym(ime, "sceImeDialogGetResult", reinterpret_cast<void**>(&s_result));
			sceKernelDlsym(ime, "sceImeDialogTerm", reinterpret_cast<void**>(&s_term));
		}
		const bool found = common_init && s_init && s_status && s_result && s_term;
		const int32_t common_result = found ? reinterpret_cast<int32_t (*)()>(common_init)() : -1;
		ProbeLog("Keyboard: common dialog module %#x, keyboard module %#x, functions %s, common dialogs %#x\n",
			static_cast<unsigned>(common), static_cast<unsigned>(ime), found ? "found" : "missing", static_cast<unsigned>(common_result));
		s_loaded = found;
		s_failed = !found;
		return s_loaded;
	}

	// Opens the keyboard over the menu with `current` in it. False if it can't.
	static bool Open(OrbisUserServiceUserId user, const char* title, const std::string& current)
	{
		if (s_open || !Load())
			return false;
		ToUtf16(current, s_buffer, MaxLength + 1);
		s_opened_with = current; // compilation 88
		ToUtf16(title, s_title, std::size(s_title));
		Setting setting{};
		setting.user_id = static_cast<uint32_t>(user);
		setting.max_text_length = MaxLength;
		setting.input_text_buffer = s_buffer;
		setting.posx = 960.0f;
		setting.posy = 540.0f;
		setting.horizontal_alignment = 1; // centre
		setting.vertical_alignment = 1;
		setting.title = s_title;
		const int32_t result = s_init(&setting, nullptr);
		OrbisUserServiceUserId initial = ORBIS_USER_SERVICE_USER_ID_INVALID;
		sceUserServiceGetInitialUser(&initial);
		ProbeLog("Keyboard: open %#x (user %#x, the console's first user %#x)\n", static_cast<unsigned>(result),
			static_cast<unsigned>(user), static_cast<unsigned>(initial)); // compilation 88
		s_open = result == 0;
		return s_open;
	}

	// While open, and on the poll it closes on: true. When it closes with OK, `text` gets what was written (`done` true).
	static bool Busy(std::string& text, bool& done)
	{
		done = false;
		if (!s_open)
			return false;
		if (s_status() == 1)
			return true;
		Result result{};
		const int32_t got = s_result(&result);
		s_term();
		s_open = false;
		done = got == 0 && result.end_status == 0;
		// Compilation 88: closed with Circle ("cancel"), what was written stays if the system left it in the buffer. Joseph's
		// test of the 87 (2026-10-25): the keyboard opened, and 9 of its 10 closes were cancels with nothing kept; on the PS4's
		// keyboard Circle closes it as a cancel, and only R2 or its "Listo" key closes it with the text.
		const std::string buffer = ToUtf8(s_buffer);
		const bool kept = !done && got == 0 && result.end_status == 1 && !buffer.empty() && buffer != s_opened_with;
		if (done || kept)
			text = buffer;
		done = done || kept;
		ProbeLog("Keyboard: closed (result %#x, end %d, %zu bytes%s)\n", static_cast<unsigned>(got), result.end_status,
			done ? text.size() : buffer.size(), kept ? ", closed with Circle: kept" : done ? "" : " in the buffer, not kept");
		return true; // the button that closed it is not the menu's
	}

	static bool Failed() { return s_failed; }
} // namespace ForjaKeyboard
static double s_report_last_sent = -1000.0;
static bool s_report_too_soon = false;
static ForjaReport::Status s_report_shown = ForjaReport::Status::Idle;
static void PublishReportText();

static void PublishGameMenu()
{
	PublishAutoNotes(); // compilation 76
	PublishCrowdText(); // compilation 82
	PublishNetText(); // compilation 144
	PublishDrawThreadText(); // compilation 116
	PublishSwJitText(); // compilation 117
	PublishFpuExactText(); // compilation 120
	PublishIopFastText(); // compilation 120
	PublishIopSleepText(); // compilation 122
	PublishCpuAutoUpText();
	PublishFastCodeText(); // compilation 124
	PublishVu1FastText(); // compilation 126
	PublishEffectsAheadText(); // compilation 127
	PublishSkipRepeatsText(); // compilation 128
	PublishCpuSeamlessText(); // compilation 128
	PublishDiscNoWaitText(); // compilation 130
	PublishSafeModeText(); // compilation 131
	PublishTestRowsText(); // compilation 132
	PublishCycleSkipText(); // compilation 100
	PublishSpeedLimitText();
	PublishImageOptionsText(); // compilation 104
	PublishReportText();
	PS4GameMenu::InGameMenuState state;
	state.visible = s_game_menu_open;
	state.selected = s_game_menu_selection;
	state.upscale = s_game_upscale;
	state.max_upscale = s_max_game_upscale;
	state.aspect = s_game_aspect;
	state.sharpness = s_game_sharpness;
	state.show_overlay = s_game_show_performance_overlay;
	state.ee_rate = s_game_ee_rate;
	state.video = VideoMenuValue();
	state.video_origin = static_cast<unsigned>(std::max(s_profile.video_mode_origin, s_profile.video_ee_rate_origin));
	state.frame_gen = g_orbis_fg_wanted ? (g_orbis_fg_fluid ? 2u : 1u) : 0u;
	state.frame_gen_origin = static_cast<unsigned>(s_profile.frame_gen_origin);
	state.cores = CoresMenuValue();
	state.cores_origin = static_cast<unsigned>(s_profile.cores_origin);
	state.has_factory = s_profile.has_factory;
	state.has_player = s_profile.has_player;
	state.slot = s_state_slot;
	state.slot_used = s_state_slot_used;
	state.action = s_state_action;
	state.cpu_auto = static_cast<unsigned>(-s_cpu_auto_floor);
	state.cpu_auto_steps = static_cast<unsigned>(s_cpu_auto_steps);
	state.cpu_auto_origin = static_cast<unsigned>(s_profile.cpu_auto_origin);
	state.gs_skip = static_cast<unsigned>(s_gs_skip_mode);
	state.gs_skip_active = g_forja_gs_skip_active.load(std::memory_order_relaxed);
	state.gs_skip_origin = static_cast<unsigned>(s_profile.gs_skip_origin);
	PS4GameMenu::SetInGameMenu(state);
}

namespace ImGuiManager
{
	void SetPerformanceHUDVisible(bool visible);
}

static void ApplyPerformanceOverlay()
{
	const bool visible = s_game_show_performance_overlay;
	MTGS::RunOnGSThread([visible]() { ImGuiManager::SetPerformanceHUDVisible(visible); });
	MTGS::WaitGS(false, false, false);
	ProbeLog("Performance overlay: %s (session only)\n", visible ? "shown" : "hidden");
}

// Forja SX2 (compilation 44): the picture's sharpness, PCSX2's own presentation options. 0 smooth: the scaled picture blended
// (PCSX2's default); 1 sharp bilinear; 2 and 3 AMD's FidelityFX CAS, sharpening while it scales to the TV (Dolphin for PS4
// got sharper pictures from AMD's FSR 1 in v04.14; CAS is the same family and PCSX2 has it), at 50 and 90; 4 no filter.
// Joseph, 2026-10-18: the picture looked "blurry, like old TVs".
static void ApplyGameSharpness(unsigned sharpness, bool save)
{
	const unsigned value = std::min(sharpness, 4u);
	const GSPostBilinearMode present = value == 1 ? GSPostBilinearMode::BilinearSharp :
	                                   value == 4 ? GSPostBilinearMode::Off : GSPostBilinearMode::BilinearSmooth;
	const GSCASMode cas = (value == 2 || value == 3) ? GSCASMode::SharpenAndResize : GSCASMode::Disabled;
	const u8 amount = value == 3 ? 90 : 50;
	EmuConfig.GS.LinearPresent = present;
	EmuConfig.GS.CASMode = cas;
	EmuConfig.GS.CAS_Sharpness = amount;
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	ProbeLog("Sharpness applied: %u (present=%d cas=%d amount=%u)\n", value, static_cast<int>(present), static_cast<int>(cas), amount);
	if (save)
	{
		SavePlayerProfile("Sharpness", [present, cas, amount](INISettingsInterface& ini) {
			ini.SetIntValue("EmuCore/GS", "linear_present_mode", static_cast<int>(present));
			ini.SetIntValue("EmuCore/GS", "CASMode", static_cast<int>(cas));
			ini.SetIntValue("EmuCore/GS", "CASSharpness", amount);
		}, false);
	}
}

static void ApplyGameAspect(unsigned aspect)
{
	const auto value = static_cast<AspectRatioType>(std::clamp(aspect, 0u, 4u));
	// ApplySettings copies the CPU-side EmuConfig into GSConfig on the GS
	// thread. Calling ApplySettings from inside a GS callback queues a nested
	// callback and leaves the displayed aspect unchanged.
	EmuConfig.GS.AspectRatio = value;
	EmuConfig.CurrentAspectRatio = value;
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	ProbeLog("Aspect ratio applied: index=%u\n", aspect);
}

static const char* ProfileOriginName(ForjaGameProfile::Origin origin)
{
	return origin == ForjaGameProfile::Origin::Player ? "player" : origin == ForjaGameProfile::Origin::Factory ? "factory" :
	       origin == ForjaGameProfile::Origin::Lab ? "pruebas.ini" : origin == ForjaGameProfile::Origin::Safe ? "modo seguro" : "global";
}

// Forja SX2: makes the game's profile current: video mode, frame generation and cores. Its PCSX2 keys are not here: PCSX2
// reads them itself, as the game settings layer (VMManager::UpdateGameSettingsLayer).
static void ApplyGameProfile(ForjaGameProfile::Values profile)
{
	const bool new_game = profile.serial != s_profile.serial || profile.crc != s_profile.crc; // compilation 76
	s_profile = std::move(profile);
	// Compilation 131: a PS1 game keeps what it had until the 130. Of the improvements that are now on for every game, only the GS's
	// three stay on for it (the recorder, the software draws' JIT, the effects ready: the same renderer, on since the 118 and the 128).
	// The others change how the EE's code, the IOP and the disc are emulated, which PS1DRV uses in its own way, and none was tried
	// with a PS1 game (its start asks for the exact code validation, below in this file).
	if (s_ps1_game)
		s_profile.iop_fast = s_profile.iop_sleep = s_profile.skip_auto_fine = s_profile.cpu_auto_up = s_profile.fast_code =
			s_profile.cpu_seamless = s_profile.disc_no_wait = 0;
	const int previous_video_ee_rate = s_video_ee_rate;
	s_video_mode_enabled = s_profile.video_mode >= 0 ? s_profile.video_mode != 0 : s_global_video_mode;
	s_video_ee_rate = s_profile.video_ee_rate <= 0 ? s_profile.video_ee_rate : VIDEO_EE_RATE;
	// Compilation 49: auto when the profile says 2, or when it leaves the video mode to the Settings page's "Sí".
	{
		const bool video_auto = s_video_mode_enabled && (s_profile.video_mode == 2 || s_profile.video_mode < 0);
		// Compilation 51: it starts from the fixed rate last chosen for the game (its VideoEECycleRate), 31 to 100 %.
		// Compilation 65: also when a new profile brings another one. Until 64 a game already on auto kept the old start:
		// Crash Nitro Kart's Ajustes recomendados left its movies starting at the player's 100 % (2026-10-20).
		if (video_auto && (!s_video_auto || s_video_ee_rate != previous_video_ee_rate))
		{
			s_video_auto_rate = std::clamp(s_video_ee_rate, VIDEO_AUTO_LOWEST, VIDEO_AUTO_HIGHEST);
			s_video_auto_ceiling = VIDEO_AUTO_HIGHEST;
			s_video_auto_floor = VIDEO_AUTO_LOWEST; // compilation 76
		}
		s_video_auto = video_auto;
	}
	const int frame_gen = s_profile.frame_gen >= 0 ? s_profile.frame_gen : (s_global_frame_gen ? 1 : 0);
	g_orbis_fg_wanted = !s_frame_gen_blocked && frame_gen != 0;
	s_fg_fluid_profile = frame_gen == 2;
	ForjaApplyFgFluid(); // compilation 65: fluid in a movie too
	// Compilation 45: a game without its own CpuAuto (player or factory) takes the Settings page's.
	s_cpu_auto_floor = s_profile.cpu_auto_origin == ForjaGameProfile::Origin::Global ? s_global_cpu_auto : s_profile.cpu_auto;
	// Compilation 47: "Saltar cuadros". Always on at 2; auto (1) starts off and UpdateGsSkip turns it on when the GS holds the game back.
	{
		// Compilation 140: the old Auto (1) is gone: a profile's Auto runs as "Al vuelo" (5), which the director lets act only with the
		// cycle skip at its top. The old one left out nothing in God of War II, Shadow and Need for Speed (2026-11-17/18).
		// Compilation 146: with nothing said, Al vuelo, the base for every game (the director lets it act only with the cycle skip
		// 3 s at its top; God of War II, Need for Speed and Guitar Hero never got there).
		const int gs_skip = s_profile.gs_skip == 1 ? 5 : s_profile.gs_skip >= 0 ? s_profile.gs_skip : 5;
		// Compilation 76: and for another game, which starts drawing every frame; "no use in this game" (UpdateGsSkip) lasts until
		// then, or until the row is changed by hand.
		if (gs_skip != s_gs_skip_mode || new_game)
		{
			s_gs_skip_useless = false;
			s_gs_skip_on_since = -1.0;
			s_gs_skip_mode = gs_skip;
			s_gs_skip_slow_checks = s_gs_skip_room_checks = 0;
			s_gs_skip_step = 0; // compilation 63: Auto starts drawing every frame; "1 de 2" is one frame in two
			s_gs_skip_speed_sum = s_gs_skip_gs_sum = s_gs_skip_others_sum = 0.0;
			s_gs_skip_samples = 0;
			g_forja_gs_skip_every.store(2u, std::memory_order_relaxed);
			g_forja_gs_skip_active.store(gs_skip >= 2 && !s_movie_settings, std::memory_order_relaxed); // compilation 65
		}
		// Compilation 86: 3 is "1 de 2 rápido", the guard with its short pauses (until 85 it had none: flicker in menus).
		g_forja_gs_skip_guard.store(true, std::memory_order_relaxed);
		g_forja_gs_skip_hide_shown.store(s_profile.gs_skip_hide_shown == 1, std::memory_order_relaxed); // compilation 146
		// Compilation 88: 4 is "2 de 3", two frames left out of every three, with the same short pauses.
		// Compilation 140: 3, 4 and Al vuelo (5) again, as until the 136. The 137 gave Al vuelo the normal pauses (to 30 s) for fewer
		// flickers, and Dragon Ball Z BT4 with the 139 (2026-11-18) spent more than half its time paused (120, 480, 1800, 1800 vsyncs),
		// left out 23 frames in 2 minutes against 1,548 in 6 with the 1.07, and ran at 67 % against 89 %.
		g_forja_gs_skip_quick.store(gs_skip >= 3, std::memory_order_relaxed);
		g_forja_gs_skip_two.store(gs_skip == 4, std::memory_order_relaxed);
		// Compilation 90: 5 is "Al vuelo", a frame left out only while the GS is behind (GSRendererPS4.cpp).
		g_forja_gs_skip_lag.store(gs_skip == 5, std::memory_order_relaxed);
	}
	// Compilation 82: the Guitar Hero games' crowd (GSC_GuitarHero reads the atomic on the GS thread).
	s_crowd_every = s_profile.crowd_every > 0 ? static_cast<unsigned>(s_profile.crowd_every) : CROWD_EVERY_DEFAULT;
	// Compilation 137: Al vuelo starts at one in two and ForjaCrowdVsync moves it.
	const bool crowd_auto = s_crowd_every == CROWD_AUTO && GameDrawsCrowdInSoftware();
	if (crowd_auto != s_crowd_auto || !crowd_auto)
		g_forja_crowd_every.store(s_crowd_every == CROWD_AUTO ? 2u : s_crowd_every, std::memory_order_relaxed);
	s_crowd_auto = crowd_auto;
	if (GameDrawsCrowdInSoftware() && s_crowd_every == CROWD_AUTO)
		ProbeLog("Crowd: drawn in software al vuelo (compilation 137), one pass in 1, 2 or 4 by the last vsyncs' load (%s)\n",
			ProfileOriginName(s_profile.crowd_every_origin));
	else if (GameDrawsCrowdInSoftware())
		ProbeLog("Crowd: drawn in software one pass in %u (%s)\n", s_crowd_every, ProfileOriginName(s_profile.crowd_every_origin));
	// Compilation 116: the GS recorder. Compilation 118 (Joseph, 2026-11-04, after the 116 in the six test games: Shadow -27 %,
	// Most Wanted -22 %, God of War II -17 % a draw on the GS): on unless the profile says 0.
	s_draw_thread = s_profile.draw_thread != 0;
	g_forja_draw_thread_wanted.store(s_draw_thread, std::memory_order_relaxed);
	if (s_draw_thread || s_profile.draw_thread_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("GS recorder wanted: %s (%s)\n", s_draw_thread ? "on" : "off", ProfileOriginName(s_profile.draw_thread_origin));
	// Compilation 117: the software draws' JIT. Compilation 118 (Joseph, 2026-11-04, after the 117: a software draw -52 % in
	// Dragon Ball and -38 % in Guitar Hero III, both drawn the same): on unless the profile says 0.
	s_sw_jit = s_profile.sw_jit != 0;
	g_forja_sw_jit.store(s_sw_jit, std::memory_order_relaxed);
	if (s_sw_jit || s_profile.sw_jit_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("Software draws JIT: %s (%s)\n", s_sw_jit ? "on" : "off", ProfileOriginName(s_profile.sw_jit_origin));
	// Compilation 120: the IOP's writes, off unless the profile says 1.
	s_iop_fast = s_profile.iop_fast == 1;
	g_forja_iop_fast.store(s_iop_fast, std::memory_order_relaxed);
	if (s_iop_fast || s_profile.iop_fast_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("IOP fast writes: %s (%s)\n", s_iop_fast ? "on" : "off", ProfileOriginName(s_profile.iop_fast_origin));
	// Compilation 122: the three rows, each off unless the profile says 1.
	s_iop_sleep_game = s_profile.iop_sleep == 1;
	ForjaApplyIopSleep();
	s_skip_auto_fine = s_profile.skip_auto_fine == 1;
	s_cpu_auto_up = s_profile.cpu_auto_up == 1;
	s_fast_code_game = s_profile.fast_code == 1; // compilation 124: the recompiler takes it at the game's start (or from the row, with a reset)
	if (s_fast_code_game || s_profile.fast_code_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("Fast code check (compilation 124): %s (%s)\n", s_fast_code_game ? "on" : "off", ProfileOriginName(s_profile.fast_code_origin));
	s_skip_keeps_frames = s_profile.skip_keeps_frames == 1; // 146 in the app
	if (s_skip_keeps_frames || s_profile.skip_keeps_frames_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("Cycle skip keeps the frames (146 in the app): %s (%s)\n", s_skip_keeps_frames ? "on" : "off",
			ProfileOriginName(s_profile.skip_keeps_frames_origin));
	{
		// 154 in the app, "Techo del salto" ([Forja] SaltoTecho; /data/ForjaSX2/vm-salto-sin-techo turns it off). The cycle skip adds each
		// VU1 program's cycles times the skip to the EE's clock (MTVU.cpp, ExecuteVU): in God of War II that is ~85 % of each frame's PS2
		// time at 2.3 skips, and the frame lasts 1.5 vsyncs (33 FPS) with no thread full (the 153, 2026-11-20: EE 70 % and asleep 24 %,
		// VU1 70 %, GS 61 %). Al vuelo goes up at each vsync lost, but this game loses ~1 % of them at any skip (940 stretches of 44
		// sessions: 0.8-1.3 % from 1 to 2.5 skips), so it only goes up: 3 s without one lost is rare. Its geometry a second (VU1 cycles)
		// was 146 millions at 1.5 skips and 107 at 2.5, at the same speed. herramientas/simulaciones/simular_techo_salto.py: the 153's
		// heavy stretches 33.5 -> 42 frames a second with 24 sixteenths. 10 of 50 vsyncs lost at the ceiling (real slow motion) lift it 10 s.
		const bool no_ceiling = access("/data/ForjaSX2/vm-salto-sin-techo", F_OK) == 0;
		s_skip_techo16 = (no_ceiling || s_profile.skip_ceiling16 < 0) ? 48 : std::clamp(s_profile.skip_ceiling16, 0, 48);
		if (s_skip_techo16 < 48 || no_ceiling || s_profile.skip_ceiling16_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("Cycle skip ceiling (154 in the app): %d/16 (%s)%s\n", s_skip_techo16, ProfileOriginName(s_profile.skip_ceiling16_origin),
				no_ceiling ? ", vm-salto-sin-techo" : "");
	}
	{
		// 154 in the app, "Flip en su propio hilo" ([Forja] FlipEnHilo; /data/ForjaSX2/vm-flip-hilo turns it on for any game and
		// vm-flip-sin-hilo off): ps4/core/ForjaFlipThread.h. The VideoOut lives across games, so this only says which way each flip goes.
		// 155 in the app: 2, "Flip sin candado" (the flip thread waits for the frame's fence and flips from the CPU); vm-flip-hilo now
		// means 2 for any game, vm-flip-hilo-gpu the 154's way (1).
		const bool force_on = access("/data/ForjaSX2/vm-flip-hilo", F_OK) == 0;
		const bool force_gpu = access("/data/ForjaSX2/vm-flip-hilo-gpu", F_OK) == 0;
		const bool force_off = access("/data/ForjaSX2/vm-flip-sin-hilo", F_OK) == 0;
		// 156 in the app: 2 for every game unless its profile says otherwise ([Forja] FlipEnHilo = 0 turns it off for a game). The 155 on
		// the console (2026-11-20, God of War II at 2x): the GS's end of frame 22 -> 2-3 %, ~39.5 -> ~43 frames a second, the picture
		// right; and every other game's GS still spent 7-16 % of its time in the GPU-side flip (Dragon Ball BT4 12 %, Downhill 16 %,
		// Guitar Hero 10 %, Need for Speed 8 %, Shadow 7 %). While frame generation is on, GSDeviceVK keeps the GPU-side flip.
		const int mode = force_off ? 0 : force_gpu ? 1 : force_on ? 2 : (s_profile.flip_thread >= 0 ? s_profile.flip_thread : 2);
		ForjaFlipThread::wanted.store(mode, std::memory_order_relaxed);
		ProbeLog("GS flip in its own thread (154-156 in the app): %s (%s)%s%s%s\n",
			mode == 2 ? "on, from the CPU once the frame is done" : mode == 1 ? "on, GPU-side (154)" : "off",
			s_profile.flip_thread >= 0 ? ProfileOriginName(s_profile.flip_thread_origin) : "factory for every game, 156",
			force_on ? ", vm-flip-hilo" : "", force_gpu ? ", vm-flip-hilo-gpu" : "", force_off ? ", vm-flip-sin-hilo" : "");
	}
	{
		// 151 in the app, "VIF1 en menos turnos" ([Forja] VifMenosTurnos, cycles; pcsx2/R5900.cpp, CPU_INT). With it, the PS5 port's simple
		// VIF codes in the transfer loop too (STCYCL, STMOD, BASE, OFFSET, ITOP without the handler table's call: Vif_Transfer.cpp,
		// vk-285-102; God of War II sends ~260,000 of them a second). /data/ForjaSX2/vm-vif-turnos-exactos turns both off.
		const bool exact = access("/data/ForjaSX2/vm-vif-turnos-exactos", F_OK) == 0;
		const int cycles = exact ? 0 : s_profile.vif1_fuse;
		g_forja_vif1_fuse.store(cycles > 4 ? cycles : 4, std::memory_order_relaxed);
		g_orbis_vif_fast.store(cycles > 4 ? 1 : 0, std::memory_order_relaxed);
		if (cycles > 4 || exact || s_profile.vif1_fuse_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VIF1 in fewer turns (151 in the app): %s, steps due in under %d cycles join the scan (%s)%s\n",
				cycles > 4 ? "on" : "off", cycles > 4 ? cycles : 4, ProfileOriginName(s_profile.vif1_fuse_origin),
				exact ? ", vm-vif-turnos-exactos" : "");
	}
	{
		// 167 in the app, "VIF en tramos" ([Forja] VifEnTramos; pcsx2/Vif_Transfer.cpp, MTVU.cpp). 1 starts it checking (the EE predicts
		// each run's UNPACKs and the normal path compares them; it goes on by itself after 100,000 right and off at the first wrong).
		// /data/ForjaSX2/vm-vif-tramos-no turns it off, vm-vif-tramos-comprobar only checks, vm-vif-tramos-directo goes on without it.
		const bool no = access("/data/ForjaSX2/vm-vif-tramos-no", F_OK) == 0;
		const bool check_only = !no && access("/data/ForjaSX2/vm-vif-tramos-comprobar", F_OK) == 0;
		const bool direct = !no && !check_only && access("/data/ForjaSX2/vm-vif-tramos-directo", F_OK) == 0;
		const bool want = !no && (s_profile.vif_tramos == 1 || check_only || direct);
		g_forja_vif_tramos_checked.store(0, std::memory_order_relaxed);
		g_forja_vif_tramos_wrong.store(0, std::memory_order_relaxed);
		g_forja_vif_tramos_check_only.store(check_only, std::memory_order_relaxed);
		g_forja_vif_tramos_mode.store(want ? (direct ? 2 : 1) : 0, std::memory_order_relaxed);
		if (want || no || s_profile.vif_tramos_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VIF in runs (167 in the app): %s (%s)%s%s%s\n", want ? (direct ? "on" : "checking first") : "off",
				ProfileOriginName(s_profile.vif_tramos_origin), no ? ", vm-vif-tramos-no" : "",
				check_only ? ", vm-vif-tramos-comprobar" : "", direct ? ", vm-vif-tramos-directo" : "");
	}
	{
		// 169 in the app, "Cola corta del VU1" ([Forja] VU1ColaKB, VU1ColaAlterna; pcsx2/MTVU.cpp). The ring lap the EE writes and the
		// VU thread reads, in KB: shorter, it stays in the L2 the EE, VU1 and GS cores share; too short, the EE waits for room when the
		// VU thread falls behind ("EE-VU1 waits: EE_writes_ring_pct"). Taken at the ring's next wrap. /data/ForjaSX2/vm-vu1-cola-larga
		// keeps the whole 16 MB and no turning.
		const bool longq = access("/data/ForjaSX2/vm-vu1-cola-larga", F_OK) == 0;
		const int kb = (!longq && s_profile.vu1_ring_kb > 0) ? s_profile.vu1_ring_kb : 16384;
		g_orbis_mtvu_ring_kb.store(static_cast<u32>(kb), std::memory_order_relaxed);
		s_vu1_ring_alternate = !longq && s_profile.vu1_ring_alt == 1;
		s_vu1_ring_logged = s_vu1_ring_alternate || kb != 16384;
		if (s_vu1_ring_logged || longq || s_profile.vu1_ring_kb_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 ring lap (169 in the app): %d KB%s (%s)%s\n", kb, s_vu1_ring_alternate ? ", turning 16384/2048/1024/512 every 20 s" : "",
				ProfileOriginName(s_profile.vu1_ring_kb_origin), longq ? ", vm-vu1-cola-larga" : "");
	}
	{
		// 170 in the app, "VIF1 por adelantado" ([Forja] VifAdelanto; pcsx2/Vif1_Dma.cpp, MTVU.cpp): the whole VIF1 packet and the VU
		// ring's next lines asked from memory early. /data/ForjaSX2/vm-vif-adelanto-no turns it off.
		const bool no = access("/data/ForjaSX2/vm-vif-adelanto-no", F_OK) == 0;
		const bool on = !no && s_profile.vif_adelanto == 1;
		g_forja_vif_adelanto.store(on, std::memory_order_relaxed);
		if (on || no || s_profile.vif_adelanto_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VIF1 early (170 in the app): %s (%s)%s\n", on ? "on" : "off", ProfileOriginName(s_profile.vif_adelanto_origin),
				no ? ", vm-vif-adelanto-no" : "");
	}
	// 178 in the app, "Detalle de los cortes": /data/ForjaSX2/vm-gs-cortes-detalle, read at the game's start; the 5 s meter re-arms it.
	s_gs_cut_detail = access("/data/ForjaSX2/vm-gs-cortes-detalle", F_OK) == 0;
	g_forja_pal_detail.store(0, std::memory_order_relaxed);
	if (s_gs_cut_detail)
		ProbeLog("GS cut detail (178 in the app): on, the next 12 palette cuts every 30 s (vm-gs-cortes-detalle)\n");
	{
		// 175 in the app, "VIF sin copia" ([Forja] VifSinCopia; pcsx2/MTVU.cpp). 1 starts checking (the copy against the game's RAM when
		// the VU thread would read it; on by itself after 200,000 the same, off at the first different). /data/ForjaSX2/vm-vif-sincopia-no
		// turns it off, vm-vif-sincopia-comprobar only checks, vm-vif-sincopia-directo goes on without checking.
		const bool no = access("/data/ForjaSX2/vm-vif-sincopia-no", F_OK) == 0;
		const bool check_only = !no && access("/data/ForjaSX2/vm-vif-sincopia-comprobar", F_OK) == 0;
		const bool direct = !no && !check_only && access("/data/ForjaSX2/vm-vif-sincopia-directo", F_OK) == 0;
		const bool want = !no && (s_profile.vif_nocopy == 1 || check_only || direct);
		g_forja_vif_nocopy_checked.store(0, std::memory_order_relaxed);
		g_forja_vif_nocopy_wrong.store(0, std::memory_order_relaxed);
		g_forja_vif_nocopy_check_only.store(check_only, std::memory_order_relaxed);
		// 176 in the app: per page now; 2 learns and uses, 1 (vm-vif-sincopia-comprobar) only learns. vm-vif-sincopia-directo is the same as 2.
		g_forja_vif_nocopy_late.store(0, std::memory_order_relaxed);
		for (u32 p = 0; p < (Ps2MemSize::MainRam >> 12); p++)
		{
			g_forja_vif_page_good[p].store(0, std::memory_order_relaxed);
			g_forja_vif_page_dynamic[p].store(0, std::memory_order_relaxed);
		}
		g_forja_vif_nocopy_mode.store(want ? (check_only ? 1 : 2) : 0, std::memory_order_relaxed);
		if (want || no || s_profile.vif_nocopy_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VIF without copy (175-176 in the app): %s (%s)%s%s%s\n", want ? (check_only ? "learning pages only" : "learning pages and using them") : "off",
				ProfileOriginName(s_profile.vif_nocopy_origin), no ? ", vm-vif-sincopia-no" : "", check_only ? ", vm-vif-sincopia-comprobar" : "",
				direct ? ", vm-vif-sincopia-directo" : "");
	}
	{
		// 174 in the app, "Animación a medias" ([Forja] AnimMediasDir/AnimMediasFuncion/AnimMediasVacia). /data/ForjaSX2/vm-anim-completa
		// turns it off.
		const bool full = access("/data/ForjaSX2/vm-anim-completa", F_OK) == 0;
		const bool set = s_profile.anim_half_addr > 0 && s_profile.anim_half_fn > 0 && s_profile.anim_half_empty > 0;
		s_anim_half_on = set && !full;
		s_anim_half_addr = set ? static_cast<u32>(s_profile.anim_half_addr) : 0;
		s_anim_half_fn = set ? static_cast<u32>(s_profile.anim_half_fn) : 0;
		s_anim_half_empty = set ? static_cast<u32>(s_profile.anim_half_empty) : 0;
		s_anim_half_n = 0;
		s_anim_half_swaps = s_anim_half_foreign = s_anim_half_skipped = 0;
		if (set || full)
			ProbeLog("Animation at half rate (174 in the app): %s, table word %08x: %08x / empty %08x (%s)%s\n", s_anim_half_on ? "on" : "off",
				s_anim_half_addr, s_anim_half_fn, s_anim_half_empty, ProfileOriginName(s_profile.anim_half_origin),
				full ? ", vm-anim-completa" : "");
	}
	{
		// 173 in the app, "PLZCW rápido" ([Forja] EEPlzcwRapido; pcsx2/x86/iMMI.cpp): before the EE's first block is compiled.
		// /data/ForjaSX2/vm-ee-plzcw-viejo keeps the old code.
		const bool old = access("/data/ForjaSX2/vm-ee-plzcw-viejo", F_OK) == 0;
		const bool on = !old && s_profile.ee_lzcnt == 1;
		g_forja_ee_lzcnt.store(on, std::memory_order_relaxed);
		if (on || old || s_profile.ee_lzcnt_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("EE PLZCW with LZCNT (173 in the app): %s (%s)%s\n", on ? "on" : "off", ProfileOriginName(s_profile.ee_lzcnt_origin),
				old ? ", vm-ee-plzcw-viejo" : "");
	}
	{
		// 183 in the app, "A/B en un dibujo" ([Forja] GSDibujoAB; pcsx2/GS/GSState.cpp, informes/AB_EN_UN_DIBUJO_PLAN_2026-11-22.md).
		// /data/ForjaSX2/vm-gs-ab-no turns it off; vm-gs-ab-ensayo only joins and draws the pieces back one by one (the check).
		const bool no = access("/data/ForjaSX2/vm-gs-ab-no", F_OK) == 0;
		const bool trial = !no && access("/data/ForjaSX2/vm-gs-ab-ensayo", F_OK) == 0;
		const int mode = no ? 0 : (trial ? 1 : std::max(0, s_profile.gs_ab));
		g_forja_gs_ab.store(mode, std::memory_order_relaxed);
		if (mode != 0 || no || s_profile.gs_ab_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("GS A/B in one draw (183 in the app): mode %d (%s)%s%s\n", mode, ProfileOriginName(s_profile.gs_ab_origin),
				no ? ", vm-gs-ab-no" : "", trial ? ", vm-gs-ab-ensayo" : "");
	}
	{
		// 171 in the app, "Vértices una vez" ([Forja] GSVerticesUnaVez; GSVertexTrace.cpp). 1 starts checking (both ways on each draw, the
		// old one kept; on by itself after 20,000 draws the same, off at the first different). /data/ForjaSX2/vm-gs-vertices-no turns it
		// off, vm-gs-vertices-comprobar only checks, vm-gs-vertices-directo goes on without checking.
		const bool no = access("/data/ForjaSX2/vm-gs-vertices-no", F_OK) == 0;
		const bool check_only = !no && access("/data/ForjaSX2/vm-gs-vertices-comprobar", F_OK) == 0;
		const bool direct = !no && !check_only && access("/data/ForjaSX2/vm-gs-vertices-directo", F_OK) == 0;
		const bool want = !no && (s_profile.gs_vt_once == 1 || check_only || direct);
		g_forja_gs_vt_once_checked.store(0, std::memory_order_relaxed);
		g_forja_gs_vt_once_wrong.store(0, std::memory_order_relaxed);
		g_forja_gs_vt_once_check_only.store(check_only, std::memory_order_relaxed);
		g_forja_gs_vt_once.store(want ? (direct ? 2 : 1) : 0, std::memory_order_relaxed);
		if (want || no || s_profile.gs_vt_once_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("GS vertices once (171 in the app): %s (%s)%s%s%s\n", want ? (direct ? "on" : "checking first") : "off",
				ProfileOriginName(s_profile.gs_vt_once_origin), no ? ", vm-gs-vertices-no" : "", check_only ? ", vm-gs-vertices-comprobar" : "",
				direct ? ", vm-gs-vertices-directo" : "");
	}
	{
		// 152 in the app, "Ahorro del VU1" ([Forja] VU1Ahorro; /data/ForjaSX2/vm-vu1-ahorro-no turns it off). Set before the game's
		// first VU1 program: the VU thread reads it at every run.
		// The VU1's map (vm-vu1-mapa): its programs, their blocks and their x86 entries, after ~60 and ~120 s of play, to tell which VU1
		// microcode the samples' JIT addresses are (OrbisVU1Dump, the PS5 port's vk-285-74).
		s_vu1_map_wanted = access("/data/ForjaSX2/vm-vu1-mapa", F_OK) == 0;
		s_vu1_map_beats = 0;
		if (s_vu1_map_wanted)
			ProbeLog("VU1 map (152 in the app): on (vm-vu1-mapa), dumps at ~60 and ~120 s of play to logs/vu1dump-0.bin and -1.bin\n");
		const bool no = access("/data/ForjaSX2/vm-vu1-ahorro-no", F_OK) == 0;
		const bool on = !no && s_profile.vu1_ahorro == 1;
		g_forja_vu1_ahorro.store(on, std::memory_order_relaxed);
		// 153 in the app, "Banderas muertas del VU1" ([Forja] VU1BanderasMuertas; /data/ForjaSX2/vm-vu1-banderas-exactas turns it off).
		// Before the game's first VU1 compile: microVU reads it as it compiles.
		const bool exact_flags = access("/data/ForjaSX2/vm-vu1-banderas-exactas", F_OK) == 0;
		const int dead = (exact_flags || s_profile.vu1_dead_flags < 0) ? 0 : (s_profile.vu1_dead_flags & 3);
		g_forja_vu1_dead_flags.store(dead, std::memory_order_relaxed);
		if (dead || exact_flags || s_profile.vu1_dead_flags_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 dead flags (153 in the app): %s%s (%s)%s\n", dead & 1 ? "status " : "", dead & 2 ? "MAC" : (dead ? "" : "none"),
				ProfileOriginName(s_profile.vu1_dead_flags_origin), exact_flags ? ", vm-vu1-banderas-exactas" : "");
		if (on || no || s_profile.vu1_ahorro_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 savings (152 in the app): %s (%s)%s\n", on ? "on" : "off", ProfileOriginName(s_profile.vu1_ahorro_origin),
				no ? ", vm-vu1-ahorro-no" : "");
		// 157 in the app, "Registros entre bloques" ([Forja] VU1RegistrosEntreBloques; /data/ForjaSX2/vm-vu1-regs-no turns it off). Before the
		// game's first VU1 compile: microVU reads it as it compiles.
		const bool regs_no = access("/data/ForjaSX2/vm-vu1-regs-no", F_OK) == 0;
		const bool keep_regs = !regs_no && s_profile.vu1_keep_regs == 1;
		g_forja_vu1_keep_regs.store(keep_regs, std::memory_order_relaxed);
		if (keep_regs || regs_no || s_profile.vu1_keep_regs_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 registers between blocks (157 in the app): %s (%s)%s\n", keep_regs ? "on" : "off",
				ProfileOriginName(s_profile.vu1_keep_regs_origin), regs_no ? ", vm-vu1-regs-no" : "");
		// 158 in the app, "Tres operandos" ([Forja] VU1TresOperandos; /data/ForjaSX2/vm-vu1-3op-no turns it off) and "Memoria del VU1
		// en un registro" ([Forja] VU1MemoriaEnRegistro; /data/ForjaSX2/vm-vu1-memreg-no). Before the game's first VU1 compile.
		const bool three_no = access("/data/ForjaSX2/vm-vu1-3op-no", F_OK) == 0;
		const bool three_op = !three_no && s_profile.vu1_three_op == 1;
		g_forja_vu1_three_op.store(three_op, std::memory_order_relaxed);
		if (three_op || three_no || s_profile.vu1_three_op_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 three operands (158 in the app): %s (%s)%s\n", three_op ? "on" : "off",
				ProfileOriginName(s_profile.vu1_three_op_origin), three_no ? ", vm-vu1-3op-no" : "");
		const bool memreg_no = access("/data/ForjaSX2/vm-vu1-memreg-no", F_OK) == 0;
		const bool mem_reg = !memreg_no && s_profile.vu1_mem_reg == 1;
		g_forja_vu1_mem_reg.store(mem_reg, std::memory_order_relaxed);
		if (mem_reg || memreg_no || s_profile.vu1_mem_reg_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 memory register (158 in the app): %s (%s)%s\n", mem_reg ? "on" : "off",
				ProfileOriginName(s_profile.vu1_mem_reg_origin), memreg_no ? ", vm-vu1-memreg-no" : "");
		// 159 in the app, "Banderas justas" ([Forja] VU1BanderasJustas; /data/ForjaSX2/vm-vu1-banderas-justas-no turns it off). Before the
		// game's first VU1 compile.
		const bool just_no = access("/data/ForjaSX2/vm-vu1-banderas-justas-no", F_OK) == 0;
		const bool mac_just = !just_no && s_profile.vu1_mac_just == 1;
		g_forja_vu1_mac_just.store(mac_just, std::memory_order_relaxed);
		if (mac_just || just_no || s_profile.vu1_mac_just_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 just MAC flags (159 in the app): %s (%s)%s\n", mac_just ? "on" : "off",
				ProfileOriginName(s_profile.vu1_mac_just_origin), just_no ? ", vm-vu1-banderas-justas-no" : "");
		// 161 in the app, "Ciclos en una sola cuenta" ([Forja] VU1CuentaCiclos; /data/ForjaSX2/vm-vu1-cuenta-ciclos-no turns it off).
		const bool count_no = access("/data/ForjaSX2/vm-vu1-cuenta-ciclos-no", F_OK) == 0;
		const bool one_count = !count_no && s_profile.vu1_one_count == 1;
		g_forja_vu1_one_count.store(one_count, std::memory_order_relaxed);
		if (one_count || count_no || s_profile.vu1_one_count_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 one cycle count (161 in the app): %s (%s)%s\n", one_count ? "on" : "off",
				ProfileOriginName(s_profile.vu1_one_count_origin), count_no ? ", vm-vu1-cuenta-ciclos-no" : "");
		// 162 in the app, "MAC justa en los saltos" ([Forja] VU1MacEnSaltos; /data/ForjaSX2/vm-vu1-mac-saltos-no turns it off).
		const bool macj_no = access("/data/ForjaSX2/vm-vu1-mac-saltos-no", F_OK) == 0;
		const bool mac_jump = !macj_no && s_profile.vu1_mac_jump == 1;
		g_forja_vu1_mac_jump.store(mac_jump, std::memory_order_relaxed);
		if (mac_jump || macj_no || s_profile.vu1_mac_jump_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 just MAC at jumps (162 in the app): %s (%s)%s\n", mac_jump ? "on" : "off",
				ProfileOriginName(s_profile.vu1_mac_jump_origin), macj_no ? ", vm-vu1-mac-saltos-no" : "");
		// 163 in the app, "MAC al final" ([Forja] VU1MacAlFinal; /data/ForjaSX2/vm-vu1-mac-final-no turns it off). Before the first compile.
		const bool mace_no = access("/data/ForjaSX2/vm-vu1-mac-final-no", F_OK) == 0;
		const bool mac_end = !mace_no && s_profile.vu1_mac_end == 1;
		g_forja_vu1_mac_end.store(mac_end, std::memory_order_relaxed);
		if (mac_end || mace_no || s_profile.vu1_mac_end_origin != ForjaGameProfile::Origin::Global)
			ProbeLog("VU1 MAC at the end (163 in the app): %s (%s)%s\n", mac_end ? "on" : "off",
				ProfileOriginName(s_profile.vu1_mac_end_origin), mace_no ? ", vm-vu1-mac-final-no" : "");
	}
	// Compilation 128: on unless the profile says 0 (measured on 2026-11-08: Bully's effect hitches 6 -> 1, Shadow's 14 -> 2).
	s_effects_ahead_game = s_profile.effects_ahead != 0; // compilation 127
	g_forja_effects_ahead.store(s_effects_ahead_game, std::memory_order_relaxed);
	if (s_effects_ahead_game || s_profile.effects_ahead_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("Effects ready ahead (compilation 127): %s (%s)\n", s_effects_ahead_game ? "on" : "off",
			ProfileOriginName(s_profile.effects_ahead_origin));
	// Compilation 128: "CPU sin tirones". When the mode the EE blocks were made with is not the one wanted, the recompiler starts over
	// (at the game's start it does anyway; from the row, once).
	s_cpu_seamless_game = s_profile.cpu_seamless == 1;
	s_disc_no_wait_game = s_profile.disc_no_wait == 1; // compilation 130
	g_forja_disc_no_wait.store(s_disc_no_wait_game, std::memory_order_relaxed);
	if (s_disc_no_wait_game || s_profile.disc_no_wait_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("Disc without waits (compilation 130): %s (%s)\n", s_disc_no_wait_game ? "on" : "off",
			ProfileOriginName(s_profile.disc_no_wait_origin));
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	g_forja_cpu_seamless.store(s_cpu_seamless_game, std::memory_order_relaxed);
	if (s_cpu_seamless_game != g_forja_cpu_seamless_active && VMManager::GetDiscCRC() != 0)
	{
		Cpu->Reset();
		ProbeLog("CPU seamless (compilation 128): %s (%s); EE recompiler reset for the new mode\n", s_cpu_seamless_game ? "on" : "off",
			ProfileOriginName(s_profile.cpu_seamless_origin));
	}
#endif
	if (s_iop_sleep_game || s_skip_auto_fine || s_cpu_auto_up || s_profile.iop_sleep_origin != ForjaGameProfile::Origin::Global ||
		s_profile.skip_auto_fine_origin != ForjaGameProfile::Origin::Global || s_profile.cpu_auto_up_origin != ForjaGameProfile::Origin::Global)
		ProbeLog("Compilation 122 rows: IOP asleep in the game %s (%s), cycle skip auto fine %s (%s), CPU auto back up %s (%s)\n",
			s_iop_sleep_game ? "on" : "off", ProfileOriginName(s_profile.iop_sleep_origin), s_skip_auto_fine ? "on" : "off",
			ProfileOriginName(s_profile.skip_auto_fine_origin), s_cpu_auto_up ? "on" : "off", ProfileOriginName(s_profile.cpu_auto_up_origin));
	// Compilation 131 (Joseph, 2026-11-13: "siempre en sí, de fábrica ... y que no esté al ojo público"): the ten improvements read above
	// are on for every game unless its factory profile, "Modo seguro" or the lab's file (/data/ForjaSX2/pruebas.ini) says otherwise
	// (ForjaGameProfile::Load gives each as 0 or 1), and their rows show only when that file asks for them.
	s_safe_mode = s_profile.safe_mode;
	PS4GameMenu::g_lab_rows_shown.store(s_profile.lab_rows, std::memory_order_relaxed);
	// Compilation 132: the three new rows; compilation 134, on unless the game's profile says 0 (and never for a PS1 game).
	{
		const bool code_ahead = s_profile.code_ahead == 1 && !s_ps1_game;
		const bool turned_on = code_ahead && !s_code_ahead_game;
		s_code_ahead_game = code_ahead;
		s_vu_jumps_game = s_profile.vu_jumps == 1 && !s_ps1_game;
		s_frame_push = s_profile.frame_push == 1 && !s_ps1_game;
		s_fg_margin = s_profile.fg_margin == 1; // compilation 136
		g_forja_fg_margin.store(s_fg_margin, std::memory_order_relaxed);
#if defined(PCSX2_PS4_VM_BOOT_JIT)
		g_forja_code_ahead.store(code_ahead, std::memory_order_relaxed);
		g_forja_vu_jump_inline.store(s_vu_jumps_game, std::memory_order_relaxed);
		if (turned_on && !new_game && VMManager::GetDiscCRC() != 0)
			ForjaCodeAheadStart(s_profile.serial, s_profile.crc, true); // from its row, the game already going: the list is read now
#endif
		if (code_ahead || s_vu_jumps_game || s_frame_push)
			ProbeLog("Compilation 132 rows: code ahead %s (%s), VU1 inline jumps %s (%s), frame push %s (%s)\n", code_ahead ? "on" : "off",
				ProfileOriginName(s_profile.code_ahead_origin), s_vu_jumps_game ? "on" : "off", ProfileOriginName(s_profile.vu_jumps_origin),
				s_frame_push ? "on" : "off", ProfileOriginName(s_profile.frame_push_origin));
	}
	if (s_safe_mode != 0 || s_profile.has_lab)
		ProbeLog("Safe mode (compilation 131): %d (%s); lab file pruebas.ini: %s, hidden rows %s, guardian %s\n", s_safe_mode,
			ProfileOriginName(s_profile.safe_mode_origin), s_profile.has_lab ? "read" : "none", s_profile.lab_rows ? "shown" : "hidden",
			s_profile.guard ? "on" : "off");
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	// "Revisión rápida del código" changed under the running game ("Modo seguro", its row, or the profile of the game that really
	// runs): the recompiler starts over in the new mode. Until the 130 only its row did this.
	{
		const bool skip = s_fast_code_marker || s_fast_code_game;
		if (skip != g_ps4_skip_smc_checks && VMManager::GetDiscCRC() != 0)
		{
			g_ps4_skip_smc_checks = skip;
			g_ps4_bytecopy_fusion = skip && access("/data/ForjaSX2/vm-no-bytecopy-fusion", F_OK) != 0;
			Cpu->Reset();
			ProbeLog("EE JIT manual SMC validation: %s (%s; recompiler reset)\n", skip ? "relaxed" : "exact",
				ProfileOriginName(s_profile.fast_code_origin));
		}
	}
#endif
	PS4ThreadAffinity::SetGamePlan(s_profile.cores.c_str());
	ApplyVu1SkipList(s_profile.vu1_skip); // compilation 44: the VU1 filter
	ApplyGsFilterList(s_profile.gs_filter); // compilation 51: the GS filter
	ApplyRulesOn(s_profile.rules_on); // compilation 55: the fingerprint rules the player turned on
	ApplyWaitLoops(s_profile.wait_loops); // 147 in the app: "Esperas sin vueltas"
	ProbeLog("Game profile: %s %08X factory=%d player=%d video_mode=%d (%s) video_ee_rate=%d (%s) frame_gen=%d (%s) "
			 "cores=\"%s\" (%s) cpu_auto=%d (%s)\n",
		s_profile.serial.empty() ? "(no serial)" : s_profile.serial.c_str(), s_profile.crc, s_profile.has_factory ? 1 : 0,
		s_profile.has_player ? 1 : 0, s_video_mode_enabled ? 1 : 0, ProfileOriginName(s_profile.video_mode_origin), s_video_ee_rate,
		ProfileOriginName(s_profile.video_ee_rate_origin), g_orbis_fg_wanted ? (g_orbis_fg_fluid ? 2 : 1) : 0,
		ProfileOriginName(s_profile.frame_gen_origin),
		s_profile.cores.c_str(), ProfileOriginName(s_profile.cores_origin), s_cpu_auto_floor, ProfileOriginName(s_profile.cpu_auto_origin));
	LogGameSettings("profile");
}

// The EE is the thread that calls this; the GS moves on its own thread (or pins itself with the plan when it starts). The VU1
// and the audio thread move from their loops.
static void RepinWorkersForProfile()
{
	PS4ThreadAffinity::RepinIfPlanChanged("EE", 2);
	if (MTGS::IsOpen())
		MTGS::RunOnGSThread([]() { PS4ThreadAffinity::RepinIfPlanChanged("GS", 4); });
}

// Forja SX2: the L3+R3 menu's per-game rows save into the player's profile of the running game (PCSX2's own
// gamesettings/<serial>_<crc>.ini). With reload_pcsx2 the game layer is reloaded so PCSX2's keys take effect at once:
// ReloadGameSettings rebuilds EmuConfig from the settings layers, which drops the video mode's movie rate (it re-arms if a
// movie is still on), so the resolution and aspect shown in the menu are put back afterwards.
template <typename Edit>
static void SavePlayerProfile(const char* what, Edit&& edit, bool reload_pcsx2)
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	if (crc == 0)
	{
		ProbeLog("%s: no disc CRC yet, not saved\n", what);
		return;
	}
	const bool saved = ForjaGameProfile::EditPlayer(serial, crc, std::forward<Edit>(edit));
	bool reloaded = false;
	if (reload_pcsx2)
	{
		reloaded = VMManager::ReloadGameSettings();
		EmuConfig.GS.UpscaleMultiplier = static_cast<float>(s_game_upscale);
		EmuConfig.GS.AspectRatio = static_cast<AspectRatioType>(std::clamp(s_game_aspect, 0u, 4u));
		EmuConfig.CurrentAspectRatio = EmuConfig.GS.AspectRatio;
		MTGS::ApplySettings();
		MTGS::WaitGS(false, false, false);
		s_video_active = false;
		ForjaEndMovieSettings(); // compilation 67
		s_cpu_auto_steps = 0;
		s_game_ee_rate = EmuConfig.Speedhacks.EECycleRate;
	}
	ApplyGameProfile(ForjaGameProfile::Load(serial, crc));
	ProbeLog("%s: saved=%d reloaded=%d file=%s\n", what, saved ? 1 : 0, reloaded ? 1 : 0,
		ForjaGameProfile::PlayerPath(serial, crc).c_str());
}

// Forja SX2: the pause menu's "PS2 CPU speed" row (EECycleRate, a PCSX2 key: the recompilers need the reload).
static void ApplyGameEECycleRate(int rate)
{
	rate = std::clamp(rate, -6, 0);
	SavePlayerProfile("EE cycle rate", [rate](INISettingsInterface& ini) { ini.SetIntValue("EmuCore/Speedhacks", "EECycleRate", rate); },
		true);
	ProbeLog("EE cycle rate: requested=%d active=%d\n", rate, s_game_ee_rate);
}

// Forja SX2: after the player's profile changed under the running game (Restablecer, Ajustes recomendados): PCSX2's layers
// and Forja's profile are read again, and the menu's values with them.
static void ReloadResetGameProfile(const std::string& serial, u32 crc, bool reset)
{
	const bool reloaded = VMManager::ReloadGameSettings();
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	ReadGameResolution();
	s_video_active = false;
	ForjaEndMovieSettings(); // compilation 67
	s_cpu_auto_steps = 0;
	s_game_ee_rate = EmuConfig.Speedhacks.EECycleRate;
	ApplyGameProfile(ForjaGameProfile::Load(serial, crc));
	RepinWorkersForProfile();
	ProbeLog("Game profile: reset=%d reloaded=%d upscale=%ux aspect=%u ee_rate=%d\n", reset ? 1 : 0, reloaded ? 1 : 0,
		s_game_upscale, s_game_aspect, s_game_ee_rate);
}

// Forja SX2: "Restablecer este juego". The player's profile becomes a .bak; the factory profile and the global settings stay.
static void ResetGameProfile()
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	const bool reset = ForjaGameProfile::ResetPlayer(serial, crc);
	ReloadResetGameProfile(serial, crc, reset);
}

// Compilation 61: true when the player's file sets something besides the picture, the two keys Ajustes recomendados keeps.
static bool PlayerFileHasOwnSettings(const std::string& path)
{
	std::FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
	if (!file)
		return false;
	char line[512];
	bool own = false;
	while (!own && std::fgets(line, sizeof(line), file))
	{
		const std::string_view text = ForjaGameProfile::Trim(line);
		if (text.empty() || text.front() == ';' || text.front() == '#' || text.front() == '[')
			continue;
		const std::string_view key = ForjaGameProfile::Trim(text.substr(0, text.find('=')));
		own = key != "upscale_multiplier" && key != "AspectRatio";
	}
	std::fclose(file);
	return own;
}

// Compilation 61: "Ajustes recomendados" used at once, with the VM paused (the L3+R3 menu). As Restablecer, but the picture
// the player chose stays (resolution and aspect: the profiles leave them to the console, 2x on a PS4, up to 4x on a Pro).
// False when the game has no recommended settings: then the player's own stay as they are.
static bool ApplyRecommendedProfile()
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	if (serial.empty() || crc == 0 || !FileSystem::FileExists(ForjaGameProfile::FactoryPath(serial).c_str()))
	{
		ProbeLog("Recommended settings: none for %s, the player's stay\n", serial.empty() ? "(no serial)" : serial.c_str());
		return false;
	}
	float upscale = 0.0f;
	std::string aspect;
	const std::string path = ForjaGameProfile::PlayerPath(serial, crc);
	{
		INISettingsInterface player(path);
		if (player.Load())
		{
			player.GetFloatValue("EmuCore/GS", "upscale_multiplier", &upscale);
			player.GetStringValue("EmuCore/GS", "AspectRatio", &aspect);
		}
	}
	// Pressed a second time the file holds only the kept picture: it does not go over the .bak with the player's own settings.
	const bool reset = PlayerFileHasOwnSettings(path) && ForjaGameProfile::ResetPlayer(serial, crc);
	if (upscale > 0.0f || !aspect.empty())
	{
		ForjaGameProfile::EditPlayer(serial, crc, [upscale, &aspect](INISettingsInterface& ini) {
			if (upscale > 0.0f)
				ini.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
			if (!aspect.empty())
				ini.SetStringValue("EmuCore/GS", "AspectRatio", aspect.c_str());
		});
	}
	ProbeLog("Recommended settings: %s from %s; the player's file %s (kept: upscale %.0f, aspect '%s')\n", serial.c_str(),
		ForjaGameProfile::FactoryPath(serial).c_str(), reset ? "is now its .bak" : "had nothing of the player's own", upscale,
		aspect.c_str());
	ReloadResetGameProfile(serial, crc, reset);
	return true;
}

// Forja SX2: automatic video mode. Measured on a PS4 Pro with Mega Man X Command Mission: during a movie
// the EE thread was the limit (game code polling the IPU ~130k times a second, plus IOP/SPU2, DMA and IPU
// work), and the intro ran at 55 % speed with EECycleRate 0, 66-72 % with -1 and ~83 % with -3, while
// gameplay ran fine at any of them. So while the game decodes pictures at a movie's pace the EE runs at
// 50 %, and the game's own rate comes back when the movie ends. The rate is baked into recompiled EE
// blocks, so each switch resets the EE recompiler (recResetEE defers that to a safe point).

// Forja SX2: the EE rate for movies can be set per game, in its profile ([Forja] VideoEECycleRate = -2; s_video_ee_rate). Tales
// of the Abyss at -3 (50 %) dropped about a sixth of its movie's pictures and left ~5 short silences a second in the music, as
// its player could not feed the sound in time; at -2 (60 %) it showed as many pictures a second and the silences were gone
// (PS4 Pro, 2026-10-15). God of War II and Mega Man X Command Mission play their movies cleanly at -3, the default.
static void UpdateVideoMode(double elapsed)
{
	if (!s_video_mode_enabled)
	{
		// Turned off for this game from the L3+R3 menu while a movie played: the game's own rate comes back now.
		if (s_video_active)
		{
			s_video_active = false;
			s_video_busy_checks = 0;
			if (EmuConfig.Speedhacks.EECycleRate != s_video_saved_rate)
			{
				const int rate_before = EmuConfig.Speedhacks.EECycleRate;
				EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(s_video_saved_rate);
				ForjaEERateChanged(rate_before); // compilation 128 (was Cpu->Reset())
			}
			ProbeLog("Video mode: turned off for this game, EE rate back to %d\n", s_video_saved_rate);
			ForjaVideoMeter(false); // compilation 65
			ForjaMovieSettings(false); // compilation 65
		}
		return;
	}
	if (elapsed - s_video_last_check < 0.5)
		return;
	const double interval = elapsed - s_video_last_check;
	s_video_last_check = elapsed;
	const u64 decodes = g_forja_ipu_decodes;
	const double per_second = static_cast<double>(decodes - s_video_last_decodes) / interval;
	s_video_last_decodes = decodes;
	if (per_second >= VIDEO_DECODES_PER_SECOND)
	{
		s_video_last_busy = elapsed;
		++s_video_busy_checks;
	}
	else
	{
		s_video_busy_checks = 0;
	}

	// Compilation 51: the auto mode's measuring, while a movie plays. Compilation 65: guided by the speed, the steady sign of
	// whether the movie plays in real time; its pictures a second change with each scene (Crash Nitro Kart's intro: 29 at
	// 100 % in a light scene, 21 in a heavy one), so comparing them across windows chased the scenes. Each window (3 s, after
	// 1.5 s to settle a change): under 92 % the rate goes a step down (two under 80 %) and that rate becomes the game's
	// ceiling; at 98 % or more it tries a step up, once a movie and never to the ceiling, and keeps it only at 95 % or more.
	// Tales of the Abyss (2026-10-20): 98-104 % at 36 %, 89-93 % at 50 %, 77-85 % at 60 %, 81 % at 100 %.
	// Compilation 66: real time alone was not enough. At 36 % Tales's movies played at 100 % with 19-22 pictures a second of
	// their 30, and the player heard its sound fail with them ("se va el audio, se va el video"): its player, short of EE
	// cycles, drops pictures and leaves silences (as at 50 % on 2026-10-15); God of War II's at 25 of their 25 were fine. Now
	// the pictures count too, in the PS2's seconds (over the speed): a movie short of them (under 92 % of one every two
	// vsyncs) at 96 % speed or more tries a step up, once a movie and never to the ceiling, and keeps it down to 90 %; the
	// floor is 90 % (it was 92) whatever the pictures, and the step up that falls under it goes back one step. A movie with
	// all its pictures never tries more, so God of War II stays where it is without a trial and its stop.
	if (s_video_active && s_video_auto && elapsed - s_video_probe_since >= VIDEO_PROBE_SETTLE)
	{
		s_video_probe_fps_sum += PerformanceMetrics::GetInternalFPS();
		s_video_probe_speed_sum += PerformanceMetrics::GetSpeed();
		s_video_probe_samples++;
		if (per_second >= VIDEO_DECODES_PER_SECOND)
			s_video_probe_busy_samples++;
		if (elapsed - s_video_probe_since >= VIDEO_PROBE_SETTLE + VIDEO_PROBE_WINDOW &&
			s_video_probe_busy_samples * 2 < s_video_probe_samples)
		{
			ProbeLog("Video mode: auto window with %u of %u samples decoding a movie: no decision" "\n", s_video_probe_busy_samples,
				s_video_probe_samples);
			s_video_probe_since = elapsed;
			s_video_probe_fps_sum = 0.0;
			s_video_probe_speed_sum = 0.0;
			s_video_probe_samples = 0;
			s_video_probe_busy_samples = 0;
		}
		else if (elapsed - s_video_probe_since >= VIDEO_PROBE_SETTLE + VIDEO_PROBE_WINDOW)
		{
			const int rate = EmuConfig.Speedhacks.EECycleRate;
			const double fps = s_video_probe_fps_sum / s_video_probe_samples;
			const double speed = s_video_probe_speed_sum / s_video_probe_samples;
			// Compilation 66: the movie's pictures in the PS2's seconds, against one every two of its vsyncs.
			const double hz = GetVerticalFrequency();
			const double full = (hz > 40.0 && hz < 70.0 ? hz : 59.94) / 2.0;
			const double pictures = speed > 1.0 ? fps * 100.0 / speed : fps;
			const bool short_of_pictures = pictures < full * VIDEO_SHORT_OF_PICTURES;
			const char* what = short_of_pictures ? "holds, short of pictures" : "holds";
			int next = rate;
			// Compilation 68: pictures first (and the sound with them), within the speed floor. A step up stays if the movie
			// still runs at 78 % or more and got more pictures from it (4 % or more); one that gave none goes back, for good in
			// this game: the movie had them all (one of 24 or 15 a second) or the EE was not what held it. Out of a step up, a
			// rate goes down only after two windows in a row under the floor, so one heavy scene does not undo it.
			const bool probing_up = s_video_probe == VideoProbe::Up;
			VideoProbe probe = VideoProbe::Measure;
			s_video_slow_windows = speed < VIDEO_SPEED_FLOOR ? s_video_slow_windows + 1 : 0;
			if (s_video_probe == VideoProbe::Down) // compilation 76
			{
				if (short_of_pictures || speed < s_video_probe_speed_before + VIDEO_DOWN_GAIN)
				{
					next = rate + 1;
					s_video_auto_floor = std::max(s_video_auto_floor, rate + 1);
					what = short_of_pictures ? "the step down lost pictures, back" : "the step down gave no speed, back";
				}
				else
					what = "the step down kept: all its pictures, and faster";
			}
			else if (speed < VIDEO_SPEED_FLOOR && rate > VIDEO_AUTO_LOWEST && (probing_up || s_video_slow_windows >= 2))
			{
				next = probing_up ? rate - 1 : std::max(rate - (speed < 70.0 ? 2 : 1), VIDEO_AUTO_LOWEST);
				s_video_auto_ceiling = std::min(s_video_auto_ceiling, rate - 1);
				s_video_slow_windows = 0;
				what = probing_up ? "the step up was too slow, back" : "slowed down, lower";
			}
			else if (probing_up && pictures < s_video_probe_pictures_before * 1.04)
			{
				next = rate - 1;
				s_video_auto_ceiling = std::min(s_video_auto_ceiling, rate - 1);
				what = "the step up gave no more pictures, back";
			}
			else if (short_of_pictures && speed >= VIDEO_SPEED_FLOOR && rate < std::min(s_video_auto_ceiling, VIDEO_AUTO_HIGHEST))
			{
				next = rate + 1;
				s_video_probe_pictures_before = pictures;
				probe = VideoProbe::Up;
				what = "short of pictures (and of sound), a step up";
			}
			else if (!probing_up && pictures >= full * VIDEO_SPARE_PICTURES && speed < VIDEO_SPARE_SLOW &&
					 rate > std::max(s_video_auto_floor, VIDEO_AUTO_LOWEST)) // compilation 76
			{
				next = rate - 1;
				s_video_probe_speed_before = speed;
				probe = VideoProbe::Down;
				what = "pictures to spare and under full speed, a step down";
			}
			s_video_probe = probe;
			ProbeLog("Video mode: auto at %d: %.1f pictures/s (%.1f a PS2 second, of %.0f), speed %.0f %% (ceiling %d, floor %d): %s -> %d\n",
				rate, fps, pictures, full, speed, s_video_auto_ceiling, std::max(s_video_auto_floor, VIDEO_AUTO_LOWEST), what, next);
			s_video_auto_rate = next;
			s_video_probe_since = elapsed;
			s_video_probe_fps_sum = 0.0;
			s_video_probe_speed_sum = 0.0;
			s_video_probe_samples = 0;
			s_video_probe_busy_samples = 0;
			if (next != rate)
			{
				const int rate_before = EmuConfig.Speedhacks.EECycleRate;
				EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(next);
				ForjaEERateChanged(rate_before); // compilation 128
			}
		}
	}

	// On after two busy checks in a row (~1 s of movie); off 4 s after the last busy check (2 s until compilation 62), and never
	// sooner than 3 s after switching on, so short decode bursts (loading screens) and the gaps between the short movies of an
	// intro (Crash Nitro Kart's) don't flip it back and forth, each flip a reset of the EE recompiler.
	// Compilation 87: a movie that comes back within 20 s of the last one's end makes the next end wait 12 s. FIFA Street 2
	// (report F-3420, 2026-10-25): its menus play movies with gaps of a few seconds, and the mode went on and off 4 times in
	// a minute, each one a reset of the EE recompiler and a hitch.
	bool want = s_video_active;
	if (!s_video_active && s_video_busy_checks >= 2)
		want = true;
	else if (s_video_active && elapsed - s_video_last_busy >= (s_video_flapping ? 12.0 : 4.0) && elapsed - s_video_since >= 3.0)
		want = false;
	if (want == s_video_active)
		return;
	if (want)
		s_video_flapping = elapsed - s_video_last_off < 20.0;
	else
		s_video_last_off = elapsed;
	ForjaVideoMeter(want); // compilation 65
	ForjaMovieSettings(want); // compilation 65
	const int rate_before = EmuConfig.Speedhacks.EECycleRate; // compilation 128

	if (want)
	{
		const int video_rate = s_video_auto ? s_video_auto_rate : s_video_ee_rate;
		s_video_saved_rate = EmuConfig.Speedhacks.EECycleRate;
		s_video_since = elapsed;
		s_video_probe = VideoProbe::Measure;
		s_video_slow_windows = 0; // compilation 68
		s_video_probe_since = elapsed;
		s_video_probe_fps_sum = 0.0;
		s_video_probe_speed_sum = 0.0;
		s_video_probe_samples = 0;
		s_video_probe_busy_samples = 0;
		s_video_active = true;
		// Compilation 48: up as well as down. It was made to lower a game at 100 % to 50-60 % during its movies; since the
		// games start at 27 % (compilation 45) the movie's rate is above theirs, and "already lower" left God of War II's
		// movies at 27 %: 17-21 frames a second instead of 25, at full speed, the decoder short of EE cycles (2026-10-18).
		if (s_video_saved_rate == video_rate)
		{
			ProbeLog("Video mode: movie detected (%.0f decodes/s), EE rate already %d\n", per_second, s_video_saved_rate);
			return;
		}
		EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(video_rate);
	}
	else
	{
		s_video_active = false;
		if (EmuConfig.Speedhacks.EECycleRate == s_video_saved_rate)
		{
			ProbeLog("Video mode: movie ended, EE rate unchanged (%d)\n", s_video_saved_rate);
			return;
		}
		EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(s_video_saved_rate);
	}
	ForjaEERateChanged(rate_before); // compilation 128
	ProbeLog("Video mode: %s (%.0f decodes/s), EE rate now %d (game's own %d)\n", want ? "ON" : "OFF", per_second,
		static_cast<int>(EmuConfig.Speedhacks.EECycleRate), s_video_saved_rate);
}

// Forja SX2 (compilation 37): the TV at 50 Hz while a PAL game runs (VulkanVideoOut.h has why). The PS2's own rate is checked
// once a second, and it takes 3 s at ~50 Hz, so the BIOS's NTSC logo or a game's own 50/60 Hz question don't switch the TV
// back and forth. The GS thread does the switch before its next frame.
static void UpdateOutputRate(double elapsed)
{
	if (elapsed - s_output_last_check < 1.0)
		return;
	s_output_last_check = elapsed;
	const double ps2_hz = GetVerticalFrequency();
	const bool pal = s_output_50hz_enabled && ps2_hz > 49.0 && ps2_hz < 51.0;
	if (!pal)
		s_output_pal_since = -1.0;
	else if (s_output_pal_since < 0.0)
		s_output_pal_since = elapsed;
	const int wanted = pal && elapsed - s_output_pal_since >= 3.0 ? 50 : 0;
	if (PS4Port::g_output_hz_request.exchange(wanted, std::memory_order_relaxed) != wanted)
		ProbeLog("Output rate: %s (the PS2 at %.2f Hz)\n", wanted == 50 ? "50 Hz for this PAL game" : "the system's rate", ps2_hz);
}

// Forja SX2 (compilation 35): automatic PS2 CPU speed, as DolphinPS4's cpu_clock=auto. In God of War II's heaviest fights
// the VU1 is the limit (2026-10-16: 28-33 ms a frame, with the GS and the EE waiting for it) and the whole game slows down,
// its music and its moves too. A lower EE rate gives the game fewer cycles a second: it draws fewer frames, so the VU1 and
// the GS have less to do each second, and the game keeps its real speed with fewer frames, as a PS2 does when it cannot keep
// up. One step down, to the floor of [Forja] CpuAuto at most, after ~3 s below 92 % speed; one step back up after ~8 s at
// full speed in which the busiest of the EE, VU1 and GS threads would still have room at the faster rate. At most one change
// every 6 s: each one resets the EE recompiler (a short hitch). Movies are the video mode's (UpdateVideoMode).
// EECycleRate -6..3. -1..-3 as PCSX2 names them; -4..-6 (compilation 36) as the recompiler scales them (11, 13, 15 / 32).
static constexpr int EE_RATE_PERCENT[] = {27, 31, 36, 50, 60, 75, 100, 130, 180, 300};

static int EERatePercent(int rate)
{
	return EE_RATE_PERCENT[std::clamp(rate, -6, 3) + 6];
}

static void SetCpuAutoRate(int rate, const char* why, double speed, double busiest)
{
	const int before = EmuConfig.Speedhacks.EECycleRate;
	EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(rate);
	ForjaEERateChanged(before); // compilation 128
	ProbeLog("CPU auto: %s at %.0f %% speed (busiest thread %.0f %%): EE rate %d -> %d (game's own %d, floor %d)\n", why, speed,
		busiest, before, rate, s_cpu_auto_base, s_cpu_auto_floor);
}

// Forja SX2 (compilation 47): "Saltar cuadros" on auto. When the GS thread is what holds the game back, the GS leaves frames of
// the game out (GSRendererPS4.cpp, VSync): the game keeps its speed, its music and its moves, with less GS work. Dragon Ball Z
// Budokai Tenkaichi 4 (2026-10-18): ~2,100 draws a frame in its fights, 25 us each for the PS4's CPU, 31 ms a vsync for the GS,
// 48 % speed. Compilation 63: Auto goes through GS_SKIP_STEPS.
static void UpdateGsSkip(double elapsed)
{
	if (s_gs_skip_mode == 0)
		return;
	if (elapsed - s_gs_skip_last_report >= 5.0)
	{
		// Compilation 62: with what VSync did with them: the draws into la pantalla left out, the TV's pictures hidden (a
		// framebuffer holding a frame left out) and shown, and the frames drawn whole because two in a row were all hidden.
		const unsigned skipped = g_forja_gs_skipped_frames.exchange(0, std::memory_order_relaxed);
		const unsigned draws = g_forja_gs_skip_stats[0].exchange(0, std::memory_order_relaxed);
		const unsigned hidden = g_forja_gs_skip_stats[1].exchange(0, std::memory_order_relaxed);
		const unsigned shown = g_forja_gs_skip_stats[2].exchange(0, std::memory_order_relaxed);
		const unsigned forced = g_forja_gs_skip_stats[3].exchange(0, std::memory_order_relaxed);
		// Forja 1.06: and its guard's (ForjaSkipGuard.h): frames left out that showed on the TV, and the vsyncs it kept it paused.
		const unsigned unmasked = g_forja_gs_skip_stats[4].exchange(0, std::memory_order_relaxed);
		const unsigned paused = g_forja_gs_skip_stats[5].exchange(0, std::memory_order_relaxed);
		ForjaSession::AddSkip(skipped, unmasked); // Forja 1.06
		if (skipped || draws || hidden || paused)
			ProbeLog("GS skip: %u frames of the game not drawn in the last %.0f s (%u draws into la pantalla left out; TV "
					 "pictures %u hidden, %u shown; %u frames drawn whole by force; %u shown though left out, paused %u vsyncs)\n",
				skipped, elapsed - s_gs_skip_last_report, draws, hidden, shown, forced, unmasked, paused);
		s_gs_skip_last_report = elapsed;
		// Compilation 76: Auto on, a report outside a movie (Saltar cuadros is paused in one) while the game draws and the GS
		// works (a loading screen, black or still, has nothing to leave out either).
		if (s_gs_skip_on_since >= 0.0 && !s_video_active && PerformanceMetrics::GetInternalFPS() >= 5.0f &&
			PerformanceMetrics::GetGSThreadUsage() >= 50.0)
		{
			s_gs_skip_reports_on++;
			if (skipped || draws || hidden || paused)
				s_gs_skip_left_any = true;
		}
	}
	// Compilation 105: "Al vuelo" leaves out as few frames as the speed needs. "Behind" alone was true in nearly every frame of a game at 30 FPS, where
	// a frame is two vsyncs: Dragon Ball Z BT4 left out 45 % of its frames with the GS at 76 %, ~17 new pictures a second and holes of 70-80 ms, where
	// without it it ran at 84 % with 25 and holes of 42 (Joseph, 2026-11-01: "el saltar cuadros fallaba demasiado").
	if (s_gs_skip_mode == 5 && !s_video_active)
	{
		static double s_lag_last = 0.0, s_lag_logged = 0.0;
		if (elapsed - s_lag_last >= 1.0)
		{
			s_lag_last = elapsed;
			const double speed = PerformanceMetrics::GetSpeed();
			const unsigned before = g_forja_gs_skip_lag_percent.load(std::memory_order_relaxed);
			// Compilation 140: nothing while the director holds it (it would climb to 66 % meanwhile and leave many frames out at once)
			// or while frame generation makes frames (frames left out make the game's frames uneven, which it waits out: the old Auto's
			// rule, from Forja 1.06 and compilation 76).
			if (g_orbis_fg_engaged)
				s_fg_engaged_seen = elapsed;
			const bool held = !g_forja_gs_skip_director.load(std::memory_order_relaxed) ||
							  (g_orbis_fg_wanted && !g_orbis_fg_gave_up && elapsed - s_fg_engaged_seen < 2.0);
			const unsigned percent = held ? 0u : speed < 97.0 ? std::min(before + 5u, 66u) : speed >= 99.5 ? (before >= 5u ? before - 5u : 0u) : before;
			g_forja_gs_skip_lag_percent.store(percent, std::memory_order_relaxed);
			if (percent != before && elapsed - s_lag_logged >= 5.0)
			{
				s_lag_logged = elapsed;
				ProbeLog("GS skip on the fly: at most %u %% of the frames (speed %.0f %%)\n", percent, speed);
			}
		}
	}
	// Compilation 140: the old Auto (47-76, one in 8 up to one in 2 on 2 s averages) removed; Al vuelo above.
}

static void UpdateCpuAuto(double elapsed)
{
	if (s_video_active || s_push_active) // compilation 132: "Empujón de cuadros" has the rate for two seconds
	{
		s_cpu_auto_slow_checks = s_cpu_auto_room_checks = 0;
		return;
	}
	const int rate = EmuConfig.Speedhacks.EECycleRate;
	// Turned off, or a higher floor picked in the menu: back to the game's rate, or to the new floor.
	if (s_cpu_auto_steps > 0 && (s_cpu_auto_floor == 0 || rate < s_cpu_auto_floor))
	{
		const int target = s_cpu_auto_floor == 0 ? s_cpu_auto_base : std::min(s_cpu_auto_floor, s_cpu_auto_base);
		s_cpu_auto_steps = s_cpu_auto_base - target;
		SetCpuAutoRate(target, s_cpu_auto_floor == 0 ? "turned off" : "new floor", PerformanceMetrics::GetSpeed(), 0.0);
		s_cpu_auto_last_change = elapsed;
		return;
	}
	if (s_cpu_auto_floor == 0)
		return;
	if (elapsed - s_cpu_auto_last_check < 0.5)
		return;
	s_cpu_auto_last_check = elapsed;
	if (s_cpu_auto_steps == 0)
		s_cpu_auto_base = rate;
	// Compilation 130: "CPU al vuelo". Shadow of the Colossus' heavy view (2026-11-09, the 129): turning the camera to "that place"
	// doubles the draws a frame (3,581 against 1,358) and the GS holds it back at 89 %; there a lower PS2 CPU keeps the frames (the GS
	// decides them) and brings the speed back, while in the light view it would take frames away. So with every change free, the CPU
	// automático follows the view: down after 1 s slow, up after 1 s with room, one change a second at most, no 60 s calm, no tries up.
	// The audit (2026-11-09) against Shadow's session: in its heaviest view the game makes its frames at a nearly fixed pace (3-4
	// refreshes), so a lower CPU barely helps there (44 -> 36 % with the cycle skip at 4: 39-57 -> 49-71 % speed); it helps where the frames
	// follow the CPU (Bully, whose frames barely grow with it: a lower CPU is speed for free). herramientas/simulaciones/simular_tres_juegos.py.
	// Compilation 140: the fast mode of the compilation 130 ("CPU al vuelo": a change a second with "CPU sin tirones") removed. "Salto de
	// ciclo: Al vuelo" does that work without changing the CPU, and every change of the CPU rewrites the translated code (the suspect of
	// Need for Speed's stops). The CPU automático is the last resort: 6 s between changes, and with Al vuelo only at its top (139).
	const double speed = PerformanceMetrics::GetSpeed();
	const double busiest = std::max({PerformanceMetrics::GetCPUThreadUsage(), static_cast<double>(PerformanceMetrics::GetVUThreadUsage()),
		static_cast<double>(PerformanceMetrics::GetGSThreadUsage())});
	// Compilation 122 ("CPU automático vuelve a subir", [Forja] CpuAutoSube; Joseph, 2026-11-07, Shadow of the Colossus: 57 -> 44 % in a
	// cave and never back up in the open air). The room below assumes the whole busiest thread grows with the rate, and with the EE at
	// 85-92 % that never happens (in Bully ~75-80 % of it doesn't grow, simular_techos_cpu.py). With the row on:
	// - slow is under 95 % (92 until now): with tries up and 92, the speed stayed lower (Bully 12.5 FPS but 69 % of the time at 95 % or more);
	// - with the cycle skip's Auto on, the CPU comes down only when the skip can't help (at its top or blocked): a skip changes with no hitch;
	// - a try up: 20 s at 97 % or more, with the skip's Auto (if on) at the game's factory value, one step up, to the rate the player chose at
	//   most. Down again within 30 s: no try for 60 s, doubled with each failed try up to 300; a try that held 120 s brings the wait back to 60.
	// herramientas/simulaciones/simular_automaticos_escenas.py, 57 % chosen, with "Salto Auto afinado": Bully 11.4 -> 12.2 FPS (74 -> 73 % of
	// the time at 95 % or more), Shadow 13.1 -> 13.5 FPS (79 -> 74 %); a hitch every ~2 minutes in Bully.
	// Compilation 139: slow counts only with the game drawing in 3D. Shadow of the Colossus (2026-11-17, the 138) went 100 -> 80 % in its
	// loading screen at 73 % speed with every thread at 9 %: a load is not a reason to take the CPU down, and the game then ran with less.
	const bool drawing_3d = PerformanceMetrics::GetVUThreadUsage() >= 8.0f && PerformanceMetrics::GetInternalFPS() >= 5.0f;
	s_cpu_auto_slow_checks = (speed < (s_cpu_auto_up ? 95.0 : 92.0) && drawing_3d) ? s_cpu_auto_slow_checks + 1 : 0;
	// Compilation 125 (Downhill Domination, 2026-11-08: 38 changes of the CPU in an hour, between 44 and 100 %, each one a reset of the EE
	// recompiler and a hitch): a loading screen or a menu has every thread nearly idle (busiest 20-42 %), the room looked enormous, the CPU went
	// up and the race brought it down again. Room counts only with the game drawing in 3D (the VU1 at work) and not within 60 s of a step down.
	const bool in_game_3d = PerformanceMetrics::GetVUThreadUsage() >= 8.0f && PerformanceMetrics::GetInternalFPS() >= 5.0f;
	const bool calm = elapsed - s_cpu_auto_last_down >= 60.0;
	const bool room = s_cpu_auto_steps > 0 && speed >= 98.0 && in_game_3d && calm &&
					  (EmuConfig.Speedhacks.EECycleSkipAuto < 2 || s_skip_auto_at_factory) && // compilation 131: "Salto al vuelo" comes down first
					  busiest * EERatePercent(rate + 1) / EERatePercent(rate) < 90.0;
	s_cpu_auto_room_checks = room ? s_cpu_auto_room_checks + 1 : 0;
	s_cpu_auto_good_checks = speed >= 97.0 ? s_cpu_auto_good_checks + 1 : 0;
	const bool skip_auto = EmuConfig.Speedhacks.EECycleSkipAuto;
	if (s_cpu_auto_probe_at > 0.0 && elapsed - s_cpu_auto_probe_at >= 120.0)
	{
		s_cpu_auto_probe_wait = 60.0;
		s_cpu_auto_probe_at = -1000.0;
	}
	// Compilation 125: one try at a time (Downhill went 57 -> 80 -> 100 % in 56 s, three tries before the first was judged): the next try waits
	// until the last one held 120 s, and none in a loading screen or a menu, nor within 60 s of a step down.
	const bool probe = s_cpu_auto_up && s_cpu_auto_steps > 0 && s_cpu_auto_good_checks >= 40 && elapsed >= s_cpu_auto_no_probe_until &&
					   (!skip_auto || s_skip_auto_at_factory) && s_cpu_auto_probe_at < 0.0 && in_game_3d && calm;
	if (elapsed - s_cpu_auto_last_change < 6.0)
		return;
	if (s_cpu_auto_slow_checks >= 6u && rate > s_cpu_auto_floor && (!s_cpu_auto_up || !skip_auto || !s_skip_auto_can_help))
	{
		if (s_cpu_auto_up && elapsed - s_cpu_auto_probe_at < 30.0)
		{
			s_cpu_auto_no_probe_until = elapsed + s_cpu_auto_probe_wait;
			ProbeLog("CPU auto: the try up didn't hold; no other for %.0f s\n", s_cpu_auto_probe_wait);
			s_cpu_auto_probe_wait = std::min(s_cpu_auto_probe_wait * 2.0, 300.0);
		}
		s_cpu_auto_probe_at = -1000.0;
		s_cpu_auto_last_down = elapsed; // compilation 125: no step up for 60 s
		// Compilation 87: as far down as the speed says, up to 4 steps at once. Each change resets the EE's recompiler: a
		// hitch of 100-600 ms, and then every block of the game compiled again. FIFA Street 2 (report F-3420, another
		// player's PS4, 2026-10-25) started at 300 % (its PCSX2 database entry) and 13 % speed; one step every 6-10 s took 11
		// changes and 2 minutes to get to 31 %, with 105 hitches. Less work a second goes with fewer cycles, so the rate that
		// would reach ~95 % is about this one times the speed.
		const double wanted = EERatePercent(rate) * std::max(speed, 1.0) / 95.0;
		int next = rate - 1;
		// Compilation 132: with "Salto al vuelo" one step at a time. Shadow of the Colossus (2026-11-13, the 131): in the same second
		// the skip went 5 -> 8 and the CPU 36 -> 27 %, both on one slow reading, and the CPU took a minute to come back (8-15 FPS with
		// every thread under 65 %).
		while (next > s_cpu_auto_floor && rate - next < (EmuConfig.Speedhacks.EECycleSkipAuto >= 2 ? 1 : 4) && EERatePercent(next) > wanted)
			next--;
		s_cpu_auto_steps += rate - next;
		SetCpuAutoRate(next, rate - next > 1 ? "slow, several steps at once" : "slow", speed, busiest);
	}
	else if (s_cpu_auto_room_checks >= 16u || probe)
	{
		s_cpu_auto_steps--;
		if (probe && s_cpu_auto_room_checks < 16)
			s_cpu_auto_probe_at = elapsed;
		SetCpuAutoRate(rate + 1, s_cpu_auto_room_checks >= 16 ? "room" : "a try up (20 s at full speed)", speed,
			busiest);
	}
	else
		return;
	s_cpu_auto_last_change = elapsed;
	s_cpu_auto_slow_checks = s_cpu_auto_room_checks = s_cpu_auto_good_checks = 0;
}

// Compilation 131: "Salto de ciclo: Al vuelo" (EECycleSkipAuto = 2), the row's value after Auto, chosen by hand. A change of the skip
// costs nothing (MTVU.cpp reads it at every VU1 program). The 131 and the 132 decided every half second, on PerformanceMetrics' speed
// (itself a mean of the last half second), after two slow readings and with a second between changes: 1 to 2.5 s to react, and as much
// again to finish a climb (Joseph, 2026-11-14: "tarda bastante ... quiero que sea directo").
// Compilation 135: it decides at every vsync (ForjaSkipInstantVsync, called by VMManager::Internal::Throttle on the EE thread, the same
// thread as this file's loop). The idea, from herramientas/simulaciones/simular_salto_inmediato.py: when one part of the emulator is at its
// limit (God of War II's VU1, Shadow's GS in its heavy view, Bully's EE), the frames that reach the screen are what that part can make. The
// skip only decides whether the game wants more than that and runs in slow motion, or wants just that and runs at full speed with the same
// frames, or less and loses frames. So the right skip, at once, gives full speed with the same frames. Each vsync:
//   u = the EE's work since the last vsync (without the limiter's sleep) / the vsync's time (20 ms PAL). Over 1, that vsync ran late.
//   With the median of the last 7 (a lone hitch doesn't count):
//   - over 1.00: up at once to what the model asks for u = 0.97 (16 sixteenths at once at most), then 3 vsyncs to see it;
//   - under 0.91 for 12 vsyncs (0.24 s): down to what the model allows for u = 0.94 (8 sixteenths at most); a step down that is over 1
//     again within 10 vsyncs goes back, and no step down for 0.5 s (doubled up to 4 s, back to 0.5 after 10 s without one);
//   - the model: u changes with the load (1 + 0.177 quarters) to the power -e, and e is learnt from each change (6 vsyncs after it).
//   In sixteenths (g_forja_skip16), from the game's factory value to 12 quarters. Nothing in a movie or during "Empujón de cuadros"; a
//   vsync of more than 10 times its time (a pause, a load) starts over.
// Simulated (10 minutes of scenes that change at once, with dips of ~1 s and the hitches of the 133's sessions), against the 132's rules:
// slow motion God of War II 15.5 -> 4.3 %, Shadow 17.4 -> 4.9 %, Bully 8.4 -> 3.6 %, back to full speed in 0.2-0.4 s, with the frames 1 to
// 5 % under the best this console can give (the right skip known at once: slow motion 3.7, 4.7, 3.6 %). What is left are the hitches,
// which no skip can fix. afinar_salto_inmediato.py chose the thresholds.
// Compilation 137: "Al vuelo por atraso". The 136 on the console (2026-11-14: God of War II, Shadow and Need for Speed, CPU 57-100 %) kept
// the skip at ~2 whole skips on average (1.81-2.08) with the EE's mean use at 0.79-0.89 (room to spare), 15-22 % of the vsyncs late and
// ~60 changes a minute; God of War II at CPU 100 % went from 98.7 % and 37.0 FPS (the 1.07) to 100 % and 32.2 FPS, nothing at its limit
// (Joseph: "ya va más rápido, pero los FPS cayeron bastante, en casi todos los juegos"). The 136 went up when the MEDIAN of the last 7
// vsyncs was over 1. But a late vsync is not slow motion: the limiter (VMManager::Internal::Throttle) carries the delay into the next
// vsync and only loses time when the delay adds up to a whole vsync. With frames of 1-2 vsyncs, half the vsyncs of a stretch can be late
// without losing anything. So this file keeps the limiter's own delay, vsync by vsync:
//   delay = max(0, delay + u - 1); over 1, those whole vsyncs are lost (real slow motion) and the fraction stays.
//   - up only when vsyncs were lost to the added-up delay (not to a lone hitch, u > 2.5) in the last half second, or the delay stayed over
//     0.6 of a vsync for 6 vsyncs: to what the model asks for a mean use (of the last 25 vsyncs) of 0.95, 1 to 8 sixteenths; then 6 vsyncs;
//   - down when nothing was lost for a second, the delay is under 0.3 and the mean use under 0.88: to what the model allows for
//     0.94, 4 sixteenths at most; a step down that loses vsyncs within half a second goes back, and the next waits 0.5 s
//     (doubled up to 4 s, 0.5 again after 10 s without one). The model's exponent is 0.35 (the 136's learnt one swung 0.37-1.00).
// Simulated (herramientas/simulaciones/simular_al_vuelo_atraso.py, 10 minutes, the 136's per-vsync spread): God of War II 99.6 % and 32.6 FPS (136: 99.7 %, 30.8; no skip: 98.8 %, 33.1), Shadow 99.3 % and 17.8 (136: 99.6 %, 16.7), Need for Speed 99.4 % and 22.4 (136: 99.7 %, 20.8); 17-32 changes a minute (136: 76-87).
static bool s_skip_instant_on = false;
static int s_skip16_floor = 0;
// Compilation 139 (the 138 on the console, 2026-11-17: ~20 ups and 35-49 downs a minute, 8-11 of them back, where the simulation said
// 17-32 changes): down only under a mean use of 0.85 (was 0.88) and after 2 s without a vsync lost (was 1 s).
// Compilation 140 (herramientas/simulaciones/simular_con_traza.py, the console's own traces of the 139: Guitar Hero III and Need for
// Speed replayed to the console's own skip and changes): down under 0.82 after 3 s without a vsync lost. Guitar Hero 41 -> 31 changes a
// minute, Need for Speed 44 -> 39, the same speed and fewer vsyncs lost, -0.1 to -0.3 % frames.
// Compilation 141 (the 140's traces too, 8 in all): the 3 s are what calm it; the 0.82 only held the skip higher (God of War II -1.2 %
// frames). Down under 0.85 again after the 3 s: the 139's frames with 7 % fewer changes.
static constexpr double SI_UP_TARGET = 0.95, SI_DOWN_BELOW = 0.85, SI_DOWN_TARGET = 0.94, SI_DEBT_HIGH = 0.6, SI_E = 0.35;
static constexpr int SI_MAX_UP = 8, SI_MAX_DOWN = 4;
static struct
{
	int value = 0;
	double us[25] = {};
	unsigned count = 0, next = 0, wait = 0, highs = 0;
	double debt = 0.0;
	u64 n = 0, lost_at[16] = {}; // vsyncs (n) when vsyncs were lost to the added-up delay, the last 16 (140: 3 s of them)
	unsigned lost_next = 0;
	int down_from = -1;
	u64 last_down = 0, no_down_until = 0, down_wait = 50; // compilation 139: 0.5 -> 1 s after a step down that did not hold
	u64 last_long = 0; // compilation 139: the last vsync over 2.5 (a hitch alone, or the scene's pace when they come close together)
	u32 time_at[49] = {}; // compilation 139: vsyncs at each value, for "Al vuelo con memoria"
	// for the 5 s line
	unsigned ups = 0, downs = 0, backs = 0, vsyncs = 0, late = 0, lost = 0, hitches = 0;
	int low_seen = 1000, high_seen = -1;
	double u_sum = 0.0;
	// 146 in the app, "Al vuelo cuida los cuadros": the game's frames and the lost vsyncs, added up, at each of the last 64 vsyncs; the
	// step up being watched (from what value, at which vsync, frames and lost vsyncs in the 50 before); and the ceiling it left.
	u64 frames_at[64] = {}, lost_at_n[64] = {}, lost_total = 0;
	int watch_from = -1, ceiling = 48;
	u64 watch_n = 0, watch_frames = 0, watch_lost = 0, ceiling_until = 0;
	unsigned guards = 0;
	// 154 in the app, "Techo del salto": until which vsync the ceiling is lifted (0: not lifted); for the 5 s line, the vsyncs at it
	// and the lifts.
	u64 techo_lifted_until = 0;
	unsigned techo_at = 0, techo_lifts = 0;
	unsigned techo_lift_count = 0; // in this game (each lift twice as long; the sixth turns the ceiling off)
	// 164 in the app: until which vsync lost vsyncs don't lift the ceiling (after a state load, after it comes back down), and the vsync of
	// the last lift (a minute without one starts the count again).
	u64 techo_grace_until = 0, techo_last_lift_n = 0;
} s_si;
// 164 in the app: a state was loaded (the menu's load, or its undo); the vsync handler starts the ceiling's grace from it.
static std::atomic<bool> s_forja_techo_load_grace{false};

// 154 in the app: the most Al vuelo may go up to now (48: no ceiling, or lifted).
static int ForjaSkipTecho()
{
	return (s_skip_techo16 < 48 && s_si.techo_lifted_until == 0) ? s_skip_techo16 : 48;
}

extern std::atomic<u64> g_forja_frames_reg, g_forja_frames_blit; // pcsx2/PerformanceMetrics.cpp (GS thread)

static void ForjaSkipInstantSet(int value16)
{
	s_si.value = std::clamp(value16, s_skip16_floor, 48);
	g_forja_skip16.store(s_si.value, std::memory_order_relaxed);
	const int quarters = (s_si.value + 2) / 4; // what the row shows and the CPU automático reads
	EmuConfig.Speedhacks.EECycleSkip = static_cast<u8>(quarters / 4);
	EmuConfig.Speedhacks.EECycleSkipQuarters = static_cast<u8>(quarters % 4);
	s_si.low_seen = std::min(s_si.low_seen, s_si.value);
	s_si.high_seen = std::max(s_si.high_seen, s_si.value);
}

static void ForjaSkipInstantReset()
{
	s_si.count = s_si.next = s_si.wait = s_si.highs = 0;
	s_si.debt = 0.0;
	s_si.down_from = -1;
	std::fill(std::begin(s_si.lost_at), std::end(s_si.lost_at), 0);
	s_si.watch_from = -1; // 146 in the app: a pause or a load in the middle is not the step's doing
}

// Compilation 139: the trace of the last minute's vsyncs (the use, the limiter's delay, the skip), written when the menu opens to
// /data/ForjaSX2/logs/traza-al-vuelo-<SERIAL>.csv: herramientas/simulaciones/simular_al_vuelo_atraso.py takes the console's own spread from
// it (the 138's churn was 55-70 changes a minute where the simulation said 17-32: its spread was made up).
static struct
{
	u16 use[3000] = {}, debt[3000] = {};
	u8 skip[3000] = {};
	unsigned next = 0, count = 0;
} s_trace;

static void ForjaSkipTraceNote(double u, double debt, int skip16)
{
	s_trace.use[s_trace.next] = static_cast<u16>(std::clamp(u * 1000.0, 0.0, 65535.0));
	s_trace.debt[s_trace.next] = static_cast<u16>(std::clamp(debt * 1000.0, 0.0, 65535.0));
	s_trace.skip[s_trace.next] = static_cast<u8>(std::clamp(skip16, 0, 255));
	s_trace.next = (s_trace.next + 1) % 3000;
	s_trace.count = std::min(s_trace.count + 1, 3000u);
}

static void ForjaSkipTraceDump()
{
	const std::string serial = VMManager::GetDiscSerial();
	if (s_trace.count < 500 || serial.empty())
		return;
	char path[200];
	std::snprintf(path, sizeof(path), "/data/ForjaSX2/logs/traza-al-vuelo-%s.csv", Path::SanitizeFileName(serial).c_str());
	const std::string tmp = std::string(path) + ".tmp";
	std::FILE* f = std::fopen(tmp.c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "uso,atraso,salto16\n");
	const unsigned first = (s_trace.next + 3000 - s_trace.count) % 3000;
	for (unsigned k = 0; k < s_trace.count; k++)
	{
		const unsigned i = (first + k) % 3000;
		std::fprintf(f, "%.3f,%.3f,%u\n", s_trace.use[i] / 1000.0, s_trace.debt[i] / 1000.0, static_cast<unsigned>(s_trace.skip[i]));
	}
	const bool ok = std::fclose(f) == 0;
	if (ok && std::rename(tmp.c_str(), path) == 0)
		ProbeLog("Cycle skip al vuelo: the last %u vsyncs written to %s\n", s_trace.count, path);
	else
		std::remove(tmp.c_str());
	s_trace.count = s_trace.next = 0;
}

// Vsyncs lost to the added-up delay within the last `window` vsyncs.
static unsigned ForjaSkipInstantLostWithin(u64 window)
{
	unsigned count = 0;
	for (const u64 at : s_si.lost_at)
		count += (at != 0 && s_si.n - at < window) ? 1u : 0u;
	return count;
}

// Compilation 137: "Público al vuelo" ([Forja] PublicoCada = 99), for every game whose crowd is drawn in software (the GameDB's
// GSC_GuitarHero: the Guitar Hero games). One in 8 fixed (Guitar Hero III's factory value) keeps the speed but the crowd moves 6 times
// a second (Joseph, 2026-11-14: "se atoran los miembros del fondo"); every pass costs the GS 21.5 ms a frame against 19.8 (the 1.07's
// session) and slows the song where the GS is at its limit. With the median of the last 7 vsyncs' load (u, the same g_forja_vsync_load):
//   over 0.98: half as often (down to one in 4, never 8), before "Al vuelo" of the cycle skip has to step in at 1.00;
//   under 0.93 for 8 vsyncs: twice as often (up to every pass); 3 vsyncs to see each change;
//   a step to more passes that is over 0.98 again within 0.3 s goes back, and the next waits 0.5 s (doubled up to 4 s, 0.5 again
//   after 10 s without one).
// Simulated (simular_sin_parpadeo.py, 10-minute songs, with the 136's cycle skip al vuelo on the same measure): the crowd up to date
// in 87-89 % of the passes (one in 8: 12.5 %), 48.5 game frames a second (one in 8: 48.6; every pass: 48.1), no slow motion.
static struct
{
	unsigned wait = 0, low = 0, down_from = 0;
	double clock = 0.0, no_down_until = 0.0, down_wait = 0.5, last_down = -100.0, last_line = 0.0;
	unsigned vsyncs = 0, passes[3] = {}, ups = 0, downs = 0, backs = 0; // for the 5 s line: vsyncs at 1, 2 and 4
} s_ca;

static void ForjaCrowdVsync(double m, double budget_ms, unsigned filled) // compilation 137: m is the mean of the last 12 vsyncs
{
	if (!s_crowd_auto || s_video_active || budget_ms < 1.0)
		return;
	unsigned every = std::clamp(g_forja_crowd_every.load(std::memory_order_relaxed), 1u, 4u);
	const auto set = [&every](unsigned next) {
		every = next;
		g_forja_crowd_every.store(next, std::memory_order_relaxed);
	};
	s_ca.clock += budget_ms / 1000.0;
	s_ca.vsyncs++;
	s_ca.passes[every >= 4 ? 2 : every >= 2 ? 1 : 0]++;
	if (s_ca.clock - s_ca.last_line >= 5.0)
	{
		s_ca.last_line = s_ca.clock;
		const double n = std::max(s_ca.vsyncs, 1u);
		ProbeLog("Crowd al vuelo: 5 s: now 1 in %u; vsyncs at 1 in 1 %.0f %%, 1 in 2 %.0f %%, 1 in 4 %.0f %%; fewer %u, more %u, back %u; "
				 "load %.2f\n", every, 100.0 * s_ca.passes[0] / n, 100.0 * s_ca.passes[1] / n, 100.0 * s_ca.passes[2] / n, s_ca.ups,
			s_ca.downs, s_ca.backs, m);
		s_ca.vsyncs = s_ca.passes[0] = s_ca.passes[1] = s_ca.passes[2] = s_ca.ups = s_ca.downs = s_ca.backs = 0;
	}
	if (filled < 5)
		return;
	if (s_ca.wait > 0)
	{
		s_ca.wait--;
		return;
	}
	if (s_ca.down_from != 0 && m > 0.98)
	{
		set(s_ca.down_from);
		s_ca.down_from = 0;
		s_ca.no_down_until = s_ca.clock + s_ca.down_wait;
		s_ca.down_wait = std::min(s_ca.down_wait * 2.0, 4.0);
		s_ca.wait = 3;
		s_ca.backs++;
		return;
	}
	if (s_ca.down_from != 0 && s_ca.clock - s_ca.last_down > 0.3)
		s_ca.down_from = 0; // it held
	if (m > 0.98 && every < 4)
	{
		set(every * 2);
		s_ca.wait = 3;
		s_ca.low = 0;
		s_ca.ups++;
		return;
	}
	s_ca.low = m < 0.93 ? s_ca.low + 1 : 0;
	if (s_ca.low >= 8 && every > 1 && s_ca.clock >= s_ca.no_down_until)
	{
		if (s_ca.clock - s_ca.last_down > 10.0)
			s_ca.down_wait = 0.5;
		s_ca.down_from = every;
		s_ca.last_down = s_ca.clock;
		set(every / 2);
		s_ca.wait = 3;
		s_ca.low = 0;
		s_ca.downs++;
	}
}

// Compilation 137: what the others read (GSDeviceVK.cpp's "Cuadros generados con margen", ForjaCrowdVsync's "Público al vuelo"): the MEAN
// use of the last 12 vsyncs and the limiter's delay, for the same reason (the 136 gave them the median of 7).
extern std::atomic<unsigned> g_forja_vsync_debt;

void ForjaSkipInstantVsync(double work_ms, double budget_ms)
{
	ForjaAnimHalfVsync(); // 174 in the app
	if (budget_ms < 1.0)
		return;
	const double u_now = work_ms / budget_ms;
	{
		static double s_ring[12] = {};
		static unsigned s_filled = 0, s_at = 0;
		static double s_debt = 0.0;
		if (u_now > 10.0)
		{
			s_filled = s_at = 0; // a pause, a load, the menu
			s_debt = 0.0;
		}
		else
		{
			s_ring[s_at] = u_now;
			s_at = (s_at + 1) % 12;
			s_filled = std::min(s_filled + 1, 12u);
			double sum = 0.0;
			for (unsigned i = 0; i < s_filled; i++)
				sum += s_ring[i];
			const double mean = sum / s_filled;
			s_debt = std::max(0.0, s_debt + u_now - 1.0);
			if (s_debt >= 1.0)
				s_debt -= std::floor(s_debt);
			g_forja_vsync_load.store(static_cast<unsigned>(std::lround(mean * 1000.0)), std::memory_order_relaxed);
			g_forja_vsync_debt.store(static_cast<unsigned>(std::lround(s_debt * 1000.0)), std::memory_order_relaxed);
			ForjaCrowdVsync(mean, budget_ms, s_filled);
		}
	}
	if (!s_skip_instant_on || s_video_active || s_push_active)
		return;
	static constexpr double K = 0.177;
	if (u_now > 10.0)
	{
		ForjaSkipInstantReset();
		return;
	}
	s_si.n++;
	s_si.vsyncs++;
	s_si.late += u_now > 1.0 ? 1u : 0u;
	s_si.u_sum += u_now;
	// The limiter's delay.
	s_si.debt = std::max(0.0, s_si.debt + u_now - 1.0);
	if (s_si.debt >= 1.0)
	{
		const double whole = std::floor(s_si.debt);
		s_si.debt -= whole;
		// Compilation 139: a vsync over 2.5 is a lone hitch (no skip fixes it) only when it is alone; another one within 2 s means the
		// scene's own pace (a GS-bound view whose frames take 3 vsyncs), and that is real slow motion the skip can help. Shadow's loading
		// and heavy view (2026-11-17): 10-13 "lone hitches" in some 5 s.
		const bool alone = u_now >= 2.5 && s_si.n - s_si.last_long > 100;
		if (!alone)
		{
			s_si.lost_at[s_si.lost_next] = s_si.n;
			s_si.lost_next = (s_si.lost_next + 1) % 16;
			s_si.lost += static_cast<unsigned>(whole);
			s_si.lost_total += static_cast<u64>(whole); // 146 in the app
		}
		else
			s_si.hitches++; // a lone hitch: no skip fixes it
	}
	if (u_now >= 2.5)
		s_si.last_long = s_si.n;
	// Compilation 141: what is learnt is where the game works (a use of 0.7 or more): in menus the skip goes to 0 and Dragon Ball learnt
	// 0 from a session whose fights wanted 48.
	if (u_now >= 0.7)
		s_si.time_at[std::clamp(s_si.value, 0, 48)]++;
	// 146 in the app, "Al vuelo cuida los cuadros" ([Forja] SaltoCuidaCuadros). Al vuelo (140) goes up when a vsync is lost and does not
	// look at the game's frames. Downhill Domination at 1x (the 145, 2026-11-19; Joseph: "muchos tirones"): every step to 40-48
	// sixteenths cut its frames from 50 to 25-36, to save 1-6 vsyncs of 250 (under 2 % of speed); the old Auto (102-122) compared
	// the frames and went back. Here: 50 vsyncs after a step up, if the game's frames fell over 20 % and no more than 5 of the 50 vsyncs
	// before it were lost, the skip goes back and that value is its ceiling for 20 s; 10 of 50 lost under the ceiling (real slow
	// motion) lift it. herramientas/simulaciones/simular_cuadros_salto.py, the console's traces calibrated to its own frames: God of
	// War II 38.9 -> 44.0 frames a second (99.1 -> 98.9 %), Downhill 48.3 -> 48.7 (99.3 -> 98.5 %) without the 50/25 swings; Need for
	// Speed, GTA, Dragon Ball, Guitar Hero and Resident Evil 4 the same (it never acts in them).
	{
		const bool blit = EmuConfig.Gamefixes.BlitInternalFPSHack;
		const u64 reg = g_forja_frames_reg.load(std::memory_order_relaxed), fb = g_forja_frames_blit.load(std::memory_order_relaxed);
		const unsigned at = static_cast<unsigned>(s_si.n % 64);
		s_si.frames_at[at] = blit ? fb : (reg != 0 ? reg : fb);
		s_si.lost_at_n[at] = s_si.lost_total;
	}
	// 154 in the app, "Techo del salto" ([Forja] SaltoTecho): 10 of the last 50 vsyncs lost at the ceiling (real slow motion, not the ~1 %
	// God of War II loses at any skip) lift it for 10 s; then the skip goes back under it.
	// 164 in the app: the ceiling's grace. The 161-163 at the Colossus (2026-11-21/22) spent most of their time over it, at 28-31
	// sixteenths, ~36 frames instead of ~42: every state load is a stall (everything translated again) that lost 10 of 50 vsyncs and
	// lifted it, each lift twice as long (10, 20, 40, 80, 160 s); and each time it came back down the game caught up on the time it had
	// fallen behind and lost vsyncs again at once. So no lift for 15 s after a state load or 10 s after it comes back, the lift no
	// longer doubles past 20 s, and a minute without a lift starts the count again.
	if (s_forja_techo_load_grace.exchange(false, std::memory_order_relaxed))
		s_si.techo_grace_until = s_si.n + 750;

	if (s_skip_techo16 < 48)
	{
		if (s_si.techo_lifted_until != 0 && s_si.n >= s_si.techo_lifted_until)
		{
			s_si.techo_lifted_until = 0;
			s_si.techo_grace_until = std::max(s_si.techo_grace_until, s_si.n + 500); // 164 in the app
		}
		if (s_si.techo_lift_count && s_si.n - s_si.techo_last_lift_n > 3000) // 164 in the app: a minute without a lift
			s_si.techo_lift_count = 0;
		// Over the ceiling and not lifted (the lift's 10 s ended, or the profile came in after Al vuelo had started): back under it at
		// once. Never under the game's floor (ForjaSkipInstantSet keeps it), so a floor over the ceiling does not loop here.
		if (s_si.techo_lifted_until == 0 && s_si.value > std::max(s_skip_techo16, s_skip16_floor))
		{
			ProbeLog("Cycle skip ceiling (154 in the app): back to %d/16 (from %d)\n", s_skip_techo16, s_si.value);
			ForjaSkipInstantSet(s_skip_techo16);
			s_si.down_from = -1;
			s_si.wait = 25;
			return;
		}
		if (s_si.value >= s_skip_techo16)
			s_si.techo_at++;
		if (s_si.techo_lifted_until == 0 && s_si.n >= 50 && s_si.value >= s_skip_techo16 && s_si.n >= s_si.techo_grace_until &&
			s_si.lost_at_n[s_si.n % 64] - s_si.lost_at_n[(s_si.n - 50) % 64] >= 10)
		{
			// Each lift in the same game twice as long (10, 20, 40, 80, 160 s); the sixth leaves the game without a ceiling: a place
			// where the PS4 cannot hold it (at 2x before the flip in its own thread, the GS was full at ~34 frames) would otherwise
			// come back to slow motion after every lift.
			const unsigned k = std::min(s_si.techo_lift_count, 1u); // 164 in the app: 10 s, then 20 s at most
			s_si.techo_lifted_until = s_si.n + (500ull << k);
			s_si.techo_lifts++;
			s_si.techo_lift_count++;
			s_si.techo_last_lift_n = s_si.n; // 164 in the app
			ProbeLog("Cycle skip ceiling (154 in the app): %d/16 lifted for %u s (%llu of 50 vsyncs lost; lift %u of 6 in this game)\n",
				s_skip_techo16, 10u << k, static_cast<unsigned long long>(s_si.lost_at_n[s_si.n % 64] - s_si.lost_at_n[(s_si.n - 50) % 64]),
				s_si.techo_lift_count);
			if (s_si.techo_lift_count >= 6)
			{
				ProbeLog("Cycle skip ceiling (154 in the app): off for the rest of this game (6 lifts)\n");
				s_skip_techo16 = 48;
				s_si.techo_lifted_until = 0;
			}
		}
	}
	if (s_si.ceiling < 48 && s_si.n >= s_si.ceiling_until)
		s_si.ceiling = 48;
	if (s_skip_keeps_frames && s_si.n >= 50)
	{
		const auto frames_in = [](u64 end, u64 span) { return s_si.frames_at[end % 64] - s_si.frames_at[(end - span) % 64]; };
		const auto lost_in = [](u64 end, u64 span) { return s_si.lost_at_n[end % 64] - s_si.lost_at_n[(end - span) % 64]; };
		if (s_si.ceiling < 48 && lost_in(s_si.n, 50) >= 10)
		{
			ProbeLog("Cycle skip keeps the frames: ceiling %d/16 lifted (%llu of 50 vsyncs lost)\n", s_si.ceiling,
				static_cast<unsigned long long>(lost_in(s_si.n, 50)));
			s_si.ceiling = 48;
		}
		if (s_si.watch_from >= 0 && s_si.n - s_si.watch_n >= 50)
		{
			const u64 after = frames_in(s_si.n, 50);
			const int from = s_si.watch_from;
			s_si.watch_from = -1;
			if (s_si.watch_frames >= 10 && after * 10 < s_si.watch_frames * 8 && s_si.watch_lost <= 5)
			{
				ProbeLog("Cycle skip keeps the frames: %d -> %d/16 cut the frames %llu -> %llu in 50 vsyncs (%llu lost before); back to %d, "
						 "its ceiling for 20 s\n", from, s_si.value, static_cast<unsigned long long>(s_si.watch_frames),
					static_cast<unsigned long long>(after), static_cast<unsigned long long>(s_si.watch_lost), from);
				s_si.ceiling = from;
				s_si.ceiling_until = s_si.n + 1000;
				s_si.guards++;
				ForjaSkipInstantSet(from);
				s_si.down_from = -1;
				s_si.wait = 25;
				return;
			}
		}
	}
	ForjaSkipTraceNote(u_now, s_si.debt, s_si.value);
	s_si.highs = s_si.debt > SI_DEBT_HIGH ? s_si.highs + 1 : 0;
	s_si.us[s_si.next] = u_now;
	s_si.next = (s_si.next + 1) % 25;
	s_si.count = std::min(s_si.count + 1, 25u);
	if (s_si.count < 12)
		return;
	double sum = 0.0;
	for (unsigned i = 0; i < s_si.count; i++)
		sum += s_si.us[i];
	const double mean = sum / s_si.count;
	const double q = s_si.value / 4.0;
	const auto quarters_for = [&](double target) {
		const double load = (1.0 + K * q) * std::pow(mean / target, 1.0 / SI_E);
		return (load - 1.0) / K;
	};
	if (s_si.wait > 0)
	{
		s_si.wait--;
		return;
	}
	const bool losing = ForjaSkipInstantLostWithin(25) > 0 || s_si.highs >= 6;
	// A step down that lost vsyncs.
	if (s_si.down_from >= 0 && losing)
	{
		ForjaSkipInstantSet(s_si.down_from);
		s_si.down_from = -1;
		s_si.no_down_until = s_si.n + s_si.down_wait;
		s_si.down_wait = std::min<u64>(s_si.down_wait * 2, 400); // compilation 139: up to 8 s (was 4)
		s_si.wait = 6;
		s_si.backs++;
		return;
	}
	if (s_si.down_from >= 0 && s_si.n - s_si.last_down > 25)
		s_si.down_from = -1; // it held
	const int top = std::min(s_skip_keeps_frames ? s_si.ceiling : 48, ForjaSkipTecho()); // 146 in the app; 154: the ceiling
	if (losing && s_si.value < top)
	{
		const int wanted = static_cast<int>(std::ceil(quarters_for(SI_UP_TARGET) * 4.0));
		if (s_skip_keeps_frames && s_si.watch_from < 0 && s_si.n >= 50) // 146 in the app: watch this step's next 50 vsyncs
		{
			s_si.watch_from = s_si.value;
			s_si.watch_n = s_si.n;
			s_si.watch_frames = s_si.frames_at[s_si.n % 64] - s_si.frames_at[(s_si.n - 50) % 64];
			s_si.watch_lost = s_si.lost_at_n[s_si.n % 64] - s_si.lost_at_n[(s_si.n - 50) % 64];
		}
		ForjaSkipInstantSet(std::min({top, s_si.value + SI_MAX_UP, std::max(s_si.value + 1, wanted)}));
		std::fill(std::begin(s_si.lost_at), std::end(s_si.lost_at), 0);
		s_si.wait = 6;
		s_si.highs = 0;
		s_si.ups++;
		return;
	}
	if (ForjaSkipInstantLostWithin(150) == 0 && s_si.debt < 0.3 && mean < SI_DOWN_BELOW && s_si.value > s_skip16_floor && // 140: 3 s
		s_si.n >= s_si.no_down_until)
	{
		const int allowed = static_cast<int>(std::floor(quarters_for(SI_DOWN_TARGET) * 4.0));
		const int next = std::max({s_skip16_floor, s_si.value - SI_MAX_DOWN, std::min(s_si.value - 1, allowed)});
		if (s_si.n - s_si.last_down > 500)
			s_si.down_wait = 50;
		s_si.down_from = s_si.value;
		s_si.last_down = s_si.n;
		ForjaSkipInstantSet(next);
		s_si.wait = 10;
		s_si.downs++;
	}
}

// This file's loop, each vsync: Al vuelo on or off, the game's floor, the 5 s line, the row's text, and what UpdateCpuAuto reads.
static void UpdateCycleSkipFast(double elapsed)
{
	static double s_last_line = 0.0;
	static int s_published = -1;
	static u32 s_crc = 0;
	const u32 crc = VMManager::GetDiscCRC();
	const int floor16 = (s_profile.crc == crc ? std::min(s_profile.cycle_skip_factory, 12) : 0) * 4;
	if (!s_skip_fast_running || crc != s_crc)
	{
		s_skip_fast_running = true;
		s_crc = crc;
		s_skip16_floor = floor16;
		s_si.no_down_until = 0;
		s_si.down_wait = 50;
		ForjaSkipInstantReset();
		// Compilation 139: "Al vuelo con memoria": where the game spent most of its last session (God of War II, 2026-11-17: it started
		// at 0 and spent its first minutes climbing to ~1.4 skips, losing vsyncs on the way).
		const int learned = s_profile.crc == crc ? s_profile.learned_skip16 : -1;
		// 146 in the app: with "cuida los cuadros", at most 2 whole skips to start (Downhill learnt 48, the top, from a session that
		// swung 0-48, and its first 8 s ran at 24 frames a second).
		const int start_learned = learned >= 0 ? std::max(floor16, s_skip_keeps_frames ? std::min(learned, 32) : learned) : floor16;
		s_si.techo_lifted_until = 0; // 154 in the app: and never over the ceiling
		s_si.techo_lift_count = 0;
		s_si.techo_grace_until = s_si.techo_last_lift_n = 0; // 164 in the app
		const int start = std::max(floor16, std::min(start_learned, s_skip_techo16));
		ForjaSkipInstantSet(start);
		std::fill(std::begin(s_si.time_at), std::end(s_si.time_at), 0u);
		s_skip_auto_can_help = true; // compilation 139: not "at the top" before it has measured anything
		s_last_line = elapsed;
		ProbeLog("Cycle skip al vuelo (compilation 139, by the limiter's delay): starts at %d sixteenths (%s; factory %d)%s\n", start,
			learned >= 0 ? "where the last session spent most of its time" : "the game's factory value", floor16,
			s_skip_keeps_frames ? "; keeps the frames (146 in the app)" : "");
	}
	s_skip16_floor = floor16;
	s_skip_instant_on = true;
	if (s_si.value < floor16)
		ForjaSkipInstantSet(floor16);
	// A reload of the game's settings brought the profile's quarters back: the controller's value rules.
	g_forja_skip16.store(s_si.value, std::memory_order_relaxed);
	const int quarters = (s_si.value + 2) / 4;
	EmuConfig.Speedhacks.EECycleSkip = static_cast<u8>(quarters / 4);
	EmuConfig.Speedhacks.EECycleSkipQuarters = static_cast<u8>(quarters % 4);
	s_skip_auto_at_factory = s_si.value <= floor16 + 8;
	// Compilation 139: the CPU automático comes down only after Al vuelo has been at its top for 3 s and the game is still slow (until
	// the 138, one vsync at the top was enough). Need for Speed (2026-11-17): the CPU went 100 -> 80 % and the race lost frames (20.3 FPS
	// at 80 % against 23.1 at 100 %), and every change of the CPU rewrites the translated code ("CPU sin tirones").
	static double s_top_since = -1.0;
	if (s_si.value >= 48)
	{
		if (s_top_since < 0.0)
			s_top_since = elapsed;
	}
	else
		s_top_since = -1.0;
	s_skip_auto_can_help = s_si.value < 48 || elapsed - s_top_since < 3.0;
	// Compilation 140: the director. Each knob that takes frames away to get the speed back has its turn: the extras give way first (the
	// crowd, generated frames: they read the same load), then this skip (no hitch, no flicker), and only after 3 s at its top Saltar
	// cuadros (the GS) and the CPU automático (the EE). Dragon Ball's fights keep the skip at its top: Saltar cuadros works there as before.
	const bool at_top = s_si.value >= 48 && s_top_since >= 0.0 && elapsed - s_top_since >= 3.0;
	g_forja_gs_skip_director.store(at_top, std::memory_order_relaxed);
	if (quarters != s_published)
	{
		s_published = quarters;
		PublishCycleSkipText();
	}
	if (elapsed - s_last_line >= 5.0)
	{
		if (s_si.vsyncs > 0)
			ProbeLog("Cycle skip al vuelo: 5 s: now %d/16 (from %d to %d), up %u, down %u, back %u; EE late in %u of %u vsyncs, mean use %.2f; "
					 "vsyncs lost to the delay %u, to lone hitches %u times\n", s_si.value, s_si.low_seen, s_si.high_seen, s_si.ups, s_si.downs,
				s_si.backs, s_si.late, s_si.vsyncs, s_si.u_sum / s_si.vsyncs, s_si.lost, s_si.hitches);
		if (s_skip_techo16 < 48 && s_si.vsyncs > 0) // 154 in the app
			ProbeLog("Cycle skip ceiling (154 in the app): %d/16, at it in %u of %u vsyncs, lifted %u times%s\n", s_skip_techo16,
				s_si.techo_at, s_si.vsyncs, s_si.techo_lifts, s_si.techo_lifted_until ? " (lifted now)" : "");
		// Compilation 140: who had which knob in these 5 s.
		ProbeLog("Director (140): speed %.0f %%; cycle skip %d/16 (%s); CPU %d %% (automático %d steps down); Saltar cuadros %s (mode %d, "
				 "at most %u %%); crowd 1 in %u; frame generation %s\n",
			PerformanceMetrics::GetSpeed(), s_si.value, at_top ? "at its top 3 s" : "working", EERatePercent(EmuConfig.Speedhacks.EECycleRate),
			s_cpu_auto_steps, at_top ? "allowed" : "held", s_gs_skip_mode, g_forja_gs_skip_lag_percent.load(std::memory_order_relaxed),
			g_forja_crowd_every.load(std::memory_order_relaxed), !g_orbis_fg_wanted ? "off" : g_orbis_fg_engaged ? "making frames" : "on, idle");
		s_last_line = elapsed;
		s_si.ups = s_si.downs = s_si.backs = s_si.vsyncs = s_si.late = s_si.lost = s_si.hitches = 0;
		s_si.techo_at = s_si.techo_lifts = 0; // 154 in the app
		s_si.u_sum = 0.0;
		s_si.low_seen = 1000;
		s_si.high_seen = -1;
	}
}

// Compilation 139: "Al vuelo con memoria". The value Al vuelo spent most vsyncs at (30 s of game at least) goes to the player's profile
// ([Forja] SaltoAprendido) when the menu opens and when the game ends; UpdateCycleSkipFast starts the next session there. Written
// straight into the file (no settings reload), only when it moved 2 sixteenths or more.
static void ForjaSkipRemember()
{
	// Compilation 140: not in Modo seguro or with the guardian's list: Need for Speed learnt 48 (the top) in a Parcial session where the
	// skip could not help, and its next session started there.
	if (!s_skip_fast_running || s_safe_mode != 0 || !s_profile.guard_off.empty())
		return;
	u64 total = 0;
	int best = 0;
	for (int v = 0; v <= 48; v++)
	{
		total += s_si.time_at[v];
		if (s_si.time_at[v] > s_si.time_at[best])
			best = v;
	}
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	if (total < 1500 || serial.empty() || crc == 0 || (s_profile.learned_skip16 >= 0 && std::abs(best - s_profile.learned_skip16) < 2))
		return;
	const bool saved = ForjaGameProfile::EditPlayer(serial, crc, [best](INISettingsInterface& ini) {
		ini.SetIntValue("Forja", "SaltoAprendido", best);
	});
	if (saved)
		s_profile.learned_skip16 = best;
	ProbeLog("Cycle skip al vuelo: learnt %d sixteenths for %s (%llu vsyncs measured), saved=%d\n", best, serial.c_str(),
		static_cast<unsigned long long>(total), saved ? 1 : 0);
}

// Compilation 132: "Empujón de cuadros" ([Forja] EmpujonCuadros; a row of No tocar, off). Bully (2026-11-13, the 130): 15-18 FPS before
// its pause menu and 8-12 after it, with the same PS2 CPU, the same speed and the EE as busy; the game decides how many refreshes each
// frame takes and stayed at the slow pace. Joseph, by hand, in Shadow of the Colossus: the CPU up to 100 % and back, and the frames
// came back. With "CPU sin tirones" a change of the CPU is free, so: at full speed (98 % or more), every thread under 80 %, the game
// drawing in 3D, and the frames of the last 3 s at 80 % or less of the best 3 s of the last two minutes, for 3 s in a row, the PS2 CPU
// goes up one or two steps (while the busiest thread would stay under ~110 %) for 2 s and comes back. Four seconds later it is judged:
// 15 % more frames than before, it worked (another only after 15 s); if not, none for 30 s, doubled with each one that fails, up to
// 300. The CPU automático and "Salto al vuelo" stand still meanwhile; a movie or the menu end it at once.
static void ForjaFramePushEnd(const char* why)
{
	if (!s_push_active)
		return;
	s_push_active = false;
	if (s_video_active)
	{
		// The movie began during the push and took the raised rate as the game's own.
		ProbeLog("Frame push: ends (%s); the movie goes back to EE rate %d afterwards, not %d\n", why, s_push_saved_rate, s_video_saved_rate);
		s_video_saved_rate = s_push_saved_rate;
		return;
	}
	const int before = EmuConfig.Speedhacks.EECycleRate;
	if (before != s_push_saved_rate)
	{
		EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(s_push_saved_rate);
		ForjaEERateChanged(before);
	}
	ProbeLog("Frame push: EE rate %d -> %d (%s)\n", before, s_push_saved_rate, why);
}

static void UpdateFramePush(double elapsed)
{
	static double s_last_check = 0.0, s_settle_until = 0.0, s_until = 0.0, s_judge_at = -1.0, s_next = 0.0, s_wait = 30.0;
	static double s_before_fps = 0.0, s_best = 0.0, s_best_at = -1000.0;
	static double s_recent[6];
	static unsigned s_recent_count = 0, s_stuck = 0;
	static u32 s_crc = 0;
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	const bool can = s_frame_push && g_forja_cpu_seamless_active && !s_video_active;
#else
	const bool can = false;
#endif
	const u32 crc = VMManager::GetDiscCRC();
	if (!can || crc != s_crc)
	{
		ForjaFramePushEnd(s_video_active ? "a movie" : "off or another game");
		s_crc = crc;
		s_judge_at = -1.0;
		s_recent_count = s_stuck = 0;
		s_best = 0.0;
		s_next = 0.0;
		s_wait = 30.0;
		return;
	}
	if (s_push_active)
	{
		if (elapsed >= s_until)
		{
			ForjaFramePushEnd("the two seconds are over");
			s_judge_at = elapsed + 4.0;
			s_recent_count = 0;
			s_last_check = elapsed;
		}
		return;
	}
	if (elapsed - s_last_check < 0.5)
		return;
	// The menu paused the game: the first numbers after it count that pause, and what was being judged is forgotten.
	if (elapsed - s_last_check > 1.0)
	{
		s_settle_until = elapsed + 2.0;
		s_recent_count = s_stuck = 0;
		s_judge_at = -1.0;
	}
	s_last_check = elapsed;
	if (elapsed < s_settle_until)
		return;
	const double speed = PerformanceMetrics::GetSpeed();
	const double fps = PerformanceMetrics::GetInternalFPS();
	const double busiest = std::max({PerformanceMetrics::GetCPUThreadUsage(), static_cast<double>(PerformanceMetrics::GetVUThreadUsage()),
		static_cast<double>(PerformanceMetrics::GetGSThreadUsage())});
	const bool in_game_3d = PerformanceMetrics::GetVUThreadUsage() >= 8.0f && fps >= 5.0;
	for (unsigned i = 5; i > 0; i--)
		s_recent[i] = s_recent[i - 1];
	s_recent[0] = fps;
	s_recent_count = std::min(s_recent_count + 1, 6u);
	if (s_recent_count < 6)
		return;
	double mean = 0.0;
	for (const double value : s_recent)
		mean += value / 6.0;
	if (s_judge_at >= 0.0)
	{
		if (elapsed < s_judge_at)
			return;
		s_judge_at = -1.0;
		if (mean >= s_before_fps * 1.15)
		{
			ProbeLog("Frame push: it worked, %.1f -> %.1f FPS\n", s_before_fps, mean);
			s_wait = 30.0;
			s_next = elapsed + 15.0;
		}
		else
		{
			ProbeLog("Frame push: no change, %.1f -> %.1f FPS; no other for %.0f s\n", s_before_fps, mean, s_wait);
			s_next = elapsed + s_wait;
			s_wait = std::min(s_wait * 2.0, 300.0);
		}
		return;
	}
	// The best three seconds the game gave lately at full speed.
	if (speed >= 97.0 && in_game_3d && (mean > s_best || elapsed - s_best_at > 120.0))
	{
		s_best = mean;
		s_best_at = elapsed;
	}
	const int rate = EmuConfig.Speedhacks.EECycleRate;
	const bool stuck = in_game_3d && speed >= 98.0 && busiest < 80.0 && rate < 0 && s_best > 0.0 && mean <= s_best * 0.8;
	s_stuck = stuck ? s_stuck + 1 : 0;
	if (s_stuck < 6 || elapsed < s_next)
		return;
	s_stuck = 0;
	int target = rate;
	while (target < 0 && target - rate < 2 && busiest * EERatePercent(target + 1) / EERatePercent(rate) <= 110.0)
		target++;
	if (target == rate)
	{
		s_next = elapsed + 10.0;
		return;
	}
	s_push_saved_rate = rate;
	s_push_active = true;
	s_until = elapsed + 2.0;
	s_before_fps = mean;
	EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(target);
	ForjaEERateChanged(rate);
	ProbeLog("Frame push: %.1f FPS at %.0f %% speed with room (busiest thread %.0f %%), %.1f lately: EE rate %d -> %d for 2 s\n", mean, speed,
		busiest, s_best, rate, target);
}

// Compilation 103: "Salto de ciclo: Auto". The least that holds the game's speed: each frame repeated is a frame less, and the best
// value is different in each game and scene (Guitar Hero III 1/2, God of War II less or none).
// Compilation 104 (Joseph, 2026-10-31: "cuando pongo en automático, la verdad que es peor"): it started from the last value, 3 after going
// through the row, and in God of War II it never came down: 14-17 FPS against 31 with 1/2. It asked for 10 s in a row at 99 % or more, and
// the speed jumps around when a game repeats frames. Now by the means of a window, along the row's steps, down only when the game is really
// repeating frames (a menu at 50 FPS stays where it is), and up only with a part at its limit: this lightens no hitch.
// Compilation 105 (2026-11-01): it starts at no skip in every game, and it judges each step up. Its top of 1 left Guitar Hero III's heaviest stage at
// 83-88 %. A step that doesn't raise the speed, or that costs more than 30 % of the frames, goes back, with no more steps up for 5 minutes.
// Measured on the street in GTA San Andreas (2026-11-02, the 105): no skip 91 % at 13.5 FPS, skip 1 99.9 % at 11.8 FPS. An earlier note here blamed the skip for
// "27.7 to 12.8 FPS": those 27.7 were the game's opening scenes.
// Compilation 107 (Joseph, 2026-11-02, the menu of Need for Speed Most Wanted): skip 0 gives 28 % at 14.2 FPS, skip 2 gives 98 % at 13.6 FPS, and Auto stayed at 0.
// It judged the frames as a share of the refreshes, and a faster game has more refreshes: 14.2 -> 13.9 FPS read as "100 -> 69 %" and the step went back. It now
// judges the game's own FPS. Its 5 minutes without steps up also held in the next scene, so a new scene (another share of frames) frees them,
// after 30 s; each failed step in a row doubles that wait, up to 4 minutes, so a game whose scenes keep changing doesn't pay for a try every 30 s.
// A window that goes from one scene to another (30 points of speed or more between its readings) starts no step: its mean is neither scene, and the step would be judged against it.
// Compilation 108 (Joseph, 2026-11-02: "reacciona lento"): the menu took 18 s to reach skip 2, 6 s a step, and a race kept the menu's skip for 100 s, at 12-14 FPS instead of 18.
// A step that holds decides the next one from the same window, far below full speed it takes 3 steps, a failed step waits 10 s for a new scene, downs take 5 s, a video or a load frees
// the waits, and a step that held isn't taken back for 60 s.
// Compilation 109 (the 108 on Joseph's PS4 Pro, 2026-11-02): the menu of Most Wanted reached 8 in ~5 s, but a race still came down a step at a time, 52 s at ~12 FPS
// instead of ~17. Guitar Hero III stayed at 1 (94 %): Auto started at 0 though its factory value is 2 (97 %), and 2 failed on one window of a game whose speed jumps
// 82-110 %. Dragon Ball Z BT4 went from 4 to 8 at full speed and kept 20 FPS instead of 25-30. Now it starts at the game's factory value and never goes below it,
// the first scene with frames after a video or a load goes straight back to it, and a step taken near full speed (90 % or more) that costs 15 % of the FPS goes back.
// Compilation 111 (the 110 on Joseph's PS4 Pro, 2026-11-03, its "holds:" lines): Dragon Ball Z BT4 at 2 ran at 99.4 % and 25.8 FPS, and dips of ~3 s took it to 3, 4
// and 8, fixed at 20 FPS. Each step was judged against the dip that set it off (4 -> 8: 23.7 -> 21.2 FPS, -11 %), so each passed, while the step's normal FPS were 26.2
// (-24 %). Now a step's FPS are judged against the value's normal ones, the median of its last windows in the same scene, and a mild dip (85-96 %) has to last three
// windows (~9 s): a dip of 3 s that falls across two windows leaves both slow. Under 85 % it still reacts at once (the menu of Most Wanted, at 28 %).
// herramientas/simulaciones/simular_automaticos.py, with dips of 3 s, saw the 110's climb to 8 and sees none with these rules.
// Compilation 112 (the 111 on Joseph's PS4 Pro, 2026-11-03): Dragon Ball went from 23 to 28 FPS, but the rule for steps down (the busiest part under 91 %) took it
// from 2 to 1 (100 -> 95.5 %) and from 1 to 0 (28.2 -> 20.1 FPS), unjudged; at 95.5 % its dips never made three slow windows, so it stayed. In God of War II the
// same scene's share of frames moves more than 0.15 (30-44 FPS), so the normal was again the dip. Now a step down is judged against the value's normal from
// before (back up after 3 points of speed without 5 % more FPS, or 15 % of the FPS); a scene's windows are those within 0.25, the margin the Auto already uses
// for "another scene"; a strong dip takes two windows (70-85 %; under 70 % at once, the menu of Most Wanted), and a step is undone after two bad windows in a
// row, as one window sometimes fell in a dip (Dragon Ball's 1 -> 2 judged at 89.8 % was refused, then 5 minutes at 1).
// Compilation 113 (the 112 on Joseph's PS4 Pro, 2026-11-03): God of War II went from 0 to 2 in a heavy moment (79 -> 95 % with the same FPS), and then
// a step that held wasn't taken back for 60 s, while 2 costs it FPS in the normal scene (going down: 25.2 -> 30.6 and 34 -> 49.7 FPS). Guitar Hero III,
// at 2x, went down from 8 to 4 for 17 % more FPS and the music ran at 86-92 %. Now after five windows in a row at 97 % or more (~15 s) a step down is
// tried whatever the busiest part says (the step down is judged and comes back if it costs), a step that held isn't taken back for 20 s, and a step
// down that leaves the game under 95 % comes back even with more FPS.
// Compilation 114 (the 113 on Joseph's PS4 Pro, 2026-11-03): in the menu of Most Wanted the step to 8 took the speed from 60 to 100 %, and 20 s later the early
// step down tried 4: the menu ran at ~61-72 % for ~11 s ("va lento"). Guitar Hero III at 2x stayed 71 s at 4 and 87 %, untried: no part reached 85 %, its
// GS waiting on the GPU. Now a step that gained 25 points or more isn't taken back for 120 s, and under 90 % a step is tried whatever the busiest part says.
// Compilation 115 (the 114 on Joseph's PS4 Pro, 2026-11-03): a failed step blocked steps up for 5 minutes, a rule from the 105, when one window judged a step.
// Guitar Hero III went from 1x to 2x in the menu, the step after it was judged against the 1x normal, and it stayed 5 minutes at 2 and 84 %; Shadow of the
// Colossus tried 4 in a dip to 49 %, judged against the normal FPS, and later stayed 111 s at 1 and 76 %. Now the block starts at 60 s and doubles with each
// failed step in a row, up to 300; a pause or the menu (its settings may have changed) gives back the step being judged and forgets the normal and the
// blocks; a step set off under 70 % is judged against its own window; a step up is judged on the mean of two windows (~6 s), as with shorter blocks noise
// kept steps that cost FPS (GTA at 88 %: 0 -> 1, 24 % fewer FPS, and a game at 88 % never steps down); after a bad window, a step down is judged on the mean
// of the two; and a step up that gains too little without costing FPS, with the game under 90 %, goes one step more instead of back (Guitar Hero III at 2x:
// 2 at 85 %, 3 at 85 %, 4 at 87 %, 8 at 98 %).
// Compilation 140: the old "Salto de ciclo Auto" (102-122: every half second, on 2 s averages and windows of "normal" speed) removed.
// "Al vuelo" does its work at every vsync and better (God of War II with the 139, 2026-11-18: 36.7 FPS at 99.7 % against the 136's 30.4),
// so a profile's Auto (EECycleSkipAuto 1) runs as Al vuelo (2), and the row has Al vuelo only. "Salto Auto afinado" (122), its tuning,
// has nothing left to tune. In the lab: contexto/MEJORAS_CATALOGO.md.
static void UpdateCycleSkipAuto(double elapsed)
{
	if (EmuConfig.Speedhacks.EECycleSkipAuto)
	{
		EmuConfig.Speedhacks.EECycleSkipAuto = 2;
		UpdateCycleSkipFast(elapsed);
		return;
	}
	s_skip_fast_running = false;
	g_forja_gs_skip_director.store(true, std::memory_order_relaxed); // no Al vuelo: Saltar cuadros is the player's choice alone
	if (s_skip_instant_on) // "Al vuelo" left; the row's quarters rule again
	{
		s_skip_instant_on = false;
		g_forja_skip16.store(-1, std::memory_order_relaxed);
	}
}

// Forja SX2: the profile was read before boot from the library's scan of the image. If PCSX2 ends up with another serial or
// CRC (a disc whose scan failed, a changed disc), the profile is read again for the game that is really running.
static void UpdateGameProfile(double elapsed)
{
	if (elapsed - s_profile_last_check < 1.0)
		return;
	s_profile_last_check = elapsed;
	const u32 crc = VMManager::GetDiscCRC();
	// Forja SX2 (compilation 42): the community's patches for this disc, fetched in the background; once they arrive, the
	// patches are read again (they stay off until the player turns them on in L3+R3 > Parches).
	if (crc != 0 && crc != s_patch_fetch_crc)
	{
		s_patch_fetch_crc = crc;
		ForjaPatches::FetchCommunity(VMManager::GetDiscSerial(), crc);
	}
	if (ForjaPatches::TakeArrived())
	{
		VMManager::ReloadPatches(true, true, false, true);
		ProbeLog("Community patches: reloaded\n");
	}
	// Compilation 122: the game's settings once PCSX2 runs it, the GameDB's fixes in. The first "Game settings" line comes from
	// ApplyGameProfile, before the GameDB (Bully's said fpu_full=0 while it ran with it, 2026-11-07).
	static u32 s_settings_logged_crc = 0;
	if (crc != 0 && crc != s_settings_logged_crc)
	{
		s_settings_logged_crc = crc;
		LogGameSettings("running, after the GameDB");
	}
	if (crc == 0 || crc == s_profile.crc)
		return;
	const std::string serial = VMManager::GetDiscSerial();
	ProbeLog("Game profile: PCSX2 runs %s %08X, the library's scan gave %s %08X; reading its profile\n", serial.c_str(), crc,
		s_profile.serial.c_str(), s_profile.crc);
	ForjaGuardStart(serial, crc); // compilation 131: the mark goes to the game that really runs
	ApplyGameProfile(ForjaGameProfile::Load(serial, crc));
	RepinWorkersForProfile();
}

// Forja SX2: Left/Right (delta -1/+1), or X (+1) on the switch-like rows, on a row of the L3+R3 menu. Everything but the
// performance overlay is saved in the game's profile (the player's file), like Dolphin's per-game properties.
static void ChangeGameMenuRow(unsigned row, int delta)
{
	switch (row)
	{
		case PS4GameMenu::ROW_VU1_FILTER:
			if (!s_vu1_candidates.empty())
			{
				const int count = static_cast<int>(s_vu1_candidates.size());
				s_vu1_index = static_cast<unsigned>((static_cast<int>(s_vu1_index) + delta % count + count) % count);
				PublishVu1FilterText();
			}
			break;
		case PS4GameMenu::ROW_GS_FILTER:
			if (!s_gs_candidates.empty())
			{
				const int count = static_cast<int>(s_gs_candidates.size());
				s_gs_index = static_cast<unsigned>((static_cast<int>(s_gs_index) + delta % count + count) % count);
				PublishGsFilterText();
			}
			break;
		case PS4GameMenu::ROW_TEXTURES:
			ApplyGameTextures((s_game_textures + static_cast<unsigned>(delta + 3)) % 3);
			break;
		case PS4GameMenu::ROW_CHANGE_DISC:
			if (s_disc_paths.size() >= 2)
			{
				const int count = static_cast<int>(s_disc_paths.size());
				s_disc_index = static_cast<unsigned>((static_cast<int>(s_disc_index) + delta % count + count) % count);
				PublishDiscText();
			}
			break;
		case PS4GameMenu::ROW_PATCH:
		{
			if (!s_patch_infos.empty())
			{
				const int count = static_cast<int>(s_patch_infos.size());
				s_patch_index = static_cast<unsigned>((static_cast<int>(s_patch_index) + delta % count + count) % count);
				PublishPatchMenuText();
			}
			break;
		}
		case PS4GameMenu::ROW_CHEATS: // compilation 58
		{
			if (!s_cheat_infos.empty())
			{
				const int count = static_cast<int>(s_cheat_infos.size());
				s_cheat_index = static_cast<unsigned>((static_cast<int>(s_cheat_index) + delta % count + count) % count);
				PublishCheatText();
			}
			break;
		}
		case PS4GameMenu::ROW_RESOLUTION:
		{
			const unsigned requested = static_cast<unsigned>(std::clamp(static_cast<int>(s_game_upscale) + delta,
				1, static_cast<int>(s_max_game_upscale)));
			if (requested == s_game_upscale)
				break;
			EmuConfig.GS.UpscaleMultiplier = static_cast<float>(requested);
			MTGS::ApplySettings();
			ReadGameResolution();
			EmuConfig.GS.UpscaleMultiplier = static_cast<float>(s_game_upscale);
			// Applied above already; the file is for the next time the game starts, so PCSX2's layers need no reload.
			SavePlayerProfile("Internal resolution", [](INISettingsInterface& ini) {
				ini.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(s_game_upscale));
			}, false);
			ProbeLog("Internal resolution: requested=%ux applied=%ux menu_limit=%ux\n", requested, s_game_upscale, s_max_game_upscale);
			break;
		}
		case PS4GameMenu::ROW_ASPECT:
			s_game_aspect = static_cast<unsigned>((static_cast<int>(s_game_aspect) + delta + 5) % 5);
			ApplyGameAspect(s_game_aspect);
			SavePlayerProfile("Aspect ratio", [](INISettingsInterface& ini) {
				ini.SetStringValue("EmuCore/GS", "AspectRatio", Pcsx2Config::GSOptions::AspectRatioNames[std::min(s_game_aspect, 4u)]);
			}, false);
			break;
		case PS4GameMenu::ROW_SHARPNESS:
			s_game_sharpness = static_cast<unsigned>((static_cast<int>(s_game_sharpness) + delta + 5) % 5);
			ApplyGameSharpness(s_game_sharpness, true);
			break;
		case PS4GameMenu::ROW_DEINTERLACE: // compilation 104
		{
			// Compilation 137: Auto, in a game with a factory mode, is that mode (the file keeps Auto: VMManager.cpp turns it again).
			constexpr int steps = static_cast<int>(std::size(ForjaDeinterlaceSteps));
			const int now = ForjaDeinterlaceAutoFactory() ? 0 : static_cast<int>(ForjaImageStep(ForjaDeinterlaceSteps, EmuConfig.GS.InterlaceMode));
			const int step = (now + delta + steps) % steps;
			const int factory = g_forja_deinterlace_factory.load(std::memory_order_relaxed);
			const bool auto_factory = ForjaDeinterlaceSteps[step].first == GSInterlaceMode::Automatic && factory > 0;
			EmuConfig.GS.InterlaceMode = auto_factory ? static_cast<GSInterlaceMode>(factory) : ForjaDeinterlaceSteps[step].first;
			g_forja_deinterlace_auto.store(auto_factory, std::memory_order_relaxed);
			MTGS::ApplySettings();
			MTGS::WaitGS(false, false, false);
			const int saved = static_cast<int>(ForjaDeinterlaceSteps[step].first);
			SavePlayerProfile("Deinterlace", [saved](INISettingsInterface& ini) {
				ini.SetIntValue("EmuCore/GS", "deinterlace_mode", saved);
			}, false);
			ProbeLog("Deinterlace applied: %d%s\n", static_cast<int>(EmuConfig.GS.InterlaceMode),
				auto_factory ? " (Auto: the game's factory mode)" : "");
			break;
		}
		case PS4GameMenu::ROW_TEXTURE_FILTER: // compilation 104
		{
			constexpr int steps = static_cast<int>(std::size(ForjaTextureFilterSteps));
			const int step = (static_cast<int>(ForjaImageStep(ForjaTextureFilterSteps, EmuConfig.GS.TextureFiltering)) + delta + steps) % steps;
			EmuConfig.GS.TextureFiltering = ForjaTextureFilterSteps[step].first;
			MTGS::ApplySettings();
			MTGS::WaitGS(false, false, false);
			SavePlayerProfile("Texture filter", [](INISettingsInterface& ini) {
				ini.SetIntValue("EmuCore/GS", "filter", static_cast<int>(EmuConfig.GS.TextureFiltering));
			}, false);
			ProbeLog("Texture filter applied: %d\n", static_cast<int>(EmuConfig.GS.TextureFiltering));
			break;
		}
		case PS4GameMenu::ROW_OVERLAY:
			s_game_show_performance_overlay = !s_game_show_performance_overlay;
			ApplyPerformanceOverlay();
			break;
		case PS4GameMenu::ROW_EE_RATE:
		{
			const int requested = std::clamp(s_game_ee_rate + delta, -6, 0);
			if (requested != s_game_ee_rate)
				ApplyGameEECycleRate(requested);
			break;
		}
		case PS4GameMenu::ROW_VIDEO_MODE:
		{
			// Off, auto, 100, 75, 60, 50 and 36 % (VideoMenuValue's 0, 6, 1, 2, 3, 4 and 5; 100 and 75 % since compilation 51).
			// A fixed rate is also where Auto starts.
			static constexpr unsigned steps[] = {0, 6, 1, 2, 3, 4, 5};
			constexpr int count = static_cast<int>(std::size(steps));
			const unsigned current = VideoMenuValue();
			int position = 0;
			for (int i = 0; i < count; i++)
			{
				if (steps[i] == current)
					position = i;
			}
			const unsigned next = steps[(position + delta + count) % count];
			// A movie playing now keeps its rate until it ends; turned off, UpdateVideoMode gives the game's rate back.
			SavePlayerProfile("Video mode", [next](INISettingsInterface& ini) {
				ini.SetIntValue("Forja", "VideoMode", next == 0 ? 0 : next == 6 ? 2 : 1);
				if (next != 0 && next != 6)
					ini.SetIntValue("Forja", "VideoEECycleRate", 1 - static_cast<int>(next));
			}, false);
			break;
		}
		case PS4GameMenu::ROW_FRAME_GEN:
		{
			// No, yes, fluid ([Forja] FrameGen 0, 1, 2; compilation 40).
			const int current = g_orbis_fg_wanted ? (g_orbis_fg_fluid ? 2 : 1) : 0;
			const int next = (current + delta + 3) % 3;
			if (next != 0)
				s_frame_gen_blocked = false; // turned back on by hand after a hang guard
			g_orbis_fg_restart.fetch_add(1, std::memory_order_relaxed); // Forja 1.06: a change by hand undoes "off for this game"
			SavePlayerProfile("Frame generation", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "FrameGen", next); },
				false);
			break;
		}
		case PS4GameMenu::ROW_CODE_AHEAD: // compilation 132: No / Sí ([Forja] CodigoListo); ApplyGameProfile takes it
		{
			const int next = s_code_ahead_game ? 0 : 1;
			SavePlayerProfile("Code ahead", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "CodigoListo", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_VU_JUMPS: // compilation 132: No / Sí ([Forja] SaltosVU1); the VU1's next blocks take it
		{
			const int next = s_vu_jumps_game ? 0 : 1;
			SavePlayerProfile("VU1 inline jumps", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "SaltosVU1", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_FG_MARGIN: // compilation 136: No / Sí ([Forja] FGConMargen); ApplyGameProfile takes it
		{
			const int next = s_fg_margin ? 0 : 1;
			SavePlayerProfile("Frame generation with room", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "FGConMargen", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_FRAME_PUSH: // compilation 132: No / Sí ([Forja] EmpujonCuadros); UpdateFramePush takes it
		{
			const int next = s_frame_push ? 0 : 1;
			SavePlayerProfile("Frame push", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "EmpujonCuadros", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_SAFE_MODE: // compilation 131: No / Parcial / Total ([Forja] ModoSeguro); ApplyGameProfile takes it
		{
			const int next = (s_safe_mode + delta + 3) % 3;
			// Total also changes two PCSX2 keys (VMManager.cpp), so the game's layer is read again going into it or out of it.
			SavePlayerProfile("Safe mode", [next](INISettingsInterface& ini) {
				ini.SetIntValue("Forja", "ModoSeguro", next);
				ini.DeleteValue("Forja", "GuardianApagadas"); // compilation 140: the player decides again
			}, next == 2 || s_safe_mode == 2);
			break;
		}
		case PS4GameMenu::ROW_CYCLE_SKIP: // compilation 100
		{
			// Compilation 103: no reload: only MTVU.cpp reads these, at every program. Reloading the game's settings reset the recompilers
			// for nothing: 5-15 s at ~10 % speed with each press (God of War II, 2026-10-31).
			constexpr int steps = static_cast<int>(std::size(ForjaCycleSkipSteps)) + 1; // compilation 140: and "Al vuelo" (no Auto)
			const int step = (static_cast<int>(ForjaCycleSkipStep()) + delta + steps) % steps;
			const int automatic = step < static_cast<int>(ForjaCycleSkipAutoStep) ? 0 : 2;
			// Compilation 105: Auto starts at no skip, what's best where the game can do without it. Compilation 109: UpdateCycleSkipAuto starts
			// it at the game's factory value instead (Guitar Hero III: 2).
			const int next = automatic ? 0 : ForjaCycleSkipSteps[step];
			SavePlayerProfile("EE cycle skip", [next, automatic](INISettingsInterface& ini) {
				ini.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", next / 4);
				ini.SetIntValue("EmuCore/Speedhacks", "EECycleSkipQuarters", next % 4);
				ini.SetIntValue("EmuCore/Speedhacks", "EECycleSkipAuto", automatic);
			}, false);
			EmuConfig.Speedhacks.EECycleSkip = static_cast<u8>(next / 4);
			EmuConfig.Speedhacks.EECycleSkipQuarters = static_cast<u8>(next % 4);
			EmuConfig.Speedhacks.EECycleSkipAuto = static_cast<u8>(automatic);
			ProbeLog("EE cycle skip: requested=%d quarters, auto=%d\n", next, automatic);
			PublishCycleSkipText();
			break;
		}
		case PS4GameMenu::ROW_SPEED_LIMIT: // compilation 100
		{
			static constexpr float scalars[3] = {1.0f, 1.3f, 10.0f};
			const float now = EmuConfig.EmulationSpeed.NominalScalar;
			const int current = now > 2.0f ? 2 : now > 1.1f ? 1 : 0;
			const float next = scalars[(current + delta + 3) % 3];
			// Compilation 104: no reload, as Salto de ciclo in the 103: each press cost ~5 s at 8-15 % speed (God of War II, 2026-10-31).
			SavePlayerProfile("Speed limit", [next](INISettingsInterface& ini) { ini.SetFloatValue("Framerate", "NominalScalar", next); }, false);
			EmuConfig.EmulationSpeed.NominalScalar = next;
			VMManager::UpdateTargetSpeed();
			ProbeLog("Speed limit: requested=%.1f active=%.1f\n", next, EmuConfig.EmulationSpeed.NominalScalar);
			PublishSpeedLimitText();
			break;
		}
		case PS4GameMenu::ROW_CPU_AUTO:
		{
			// No, down to 75 %, 60 %, 50 %, 36 %, 31 % or 27 % ([Forja] CpuAuto 0, -1 ... -6). UpdateCpuAuto takes the change from here.
			const int next = (-s_cpu_auto_floor + delta + 7) % 7;
			SavePlayerProfile("CPU auto", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "CpuAuto", -next); }, false);
			break;
		}
		case PS4GameMenu::ROW_REPORT: // compilation 77: the player's word for the report, not saved
			s_report_rating = (s_report_rating + REPORT_RATING_COUNT + delta) % REPORT_RATING_COUNT;
			s_report_rating_chosen = true;
			break;
		case PS4GameMenu::ROW_REPORT_ISSUE: // compilation 80: what went wrong, not saved either
			s_report_issue = (s_report_issue + REPORT_ISSUE_COUNT + delta) % REPORT_ISSUE_COUNT;
			break;
		case PS4GameMenu::ROW_REPORT_WHERE: // compilation 88: where it shows
			s_report_where = s_report_where < 0 ? (delta > 0 ? 0 : REPORT_WHERE_COUNT - 1) :
			                                      (s_report_where + REPORT_WHERE_COUNT + delta) % REPORT_WHERE_COUNT;
			break;
		case PS4GameMenu::ROW_REPORT_COMMENT: // compilation 87: a ready phrase as the comment
			s_report_phrase = s_report_phrase < 0 ? (delta > 0 ? 0 : REPORT_PHRASE_COUNT - 1) :
			                                        (s_report_phrase + REPORT_PHRASE_COUNT + delta) % REPORT_PHRASE_COUNT;
			s_report_comment = ForjaLang::T(REPORT_PHRASES[s_report_phrase]);
			break;
		case PS4GameMenu::ROW_NET_MODE: // compilation 144: No / Internet / Sala LAN
		{
			const int next = (NetMode() + delta + 3) % 3;
			SavePlayerProfile("Network mode", [next](INISettingsInterface& ini) {
				ini.SetBoolValue("DEV9/Eth", "EthEnable", next != 0);
				if (next != 0)
					ini.SetStringValue("DEV9/Eth", "EthApi", next == 1 ? "Sockets" : "Forja LAN");
			}, true);
			ProbeLog("Network: mode %d (0 off, 1 internet, 2 room)\n", next);
			break;
		}
		case PS4GameMenu::ROW_NET_UPNP: // compilation 144: the router's port, only with the player's Sí
		{
			const bool next = !EmuConfig.DEV9.LanUPnP;
			SavePlayerProfile("Network UPnP", [next](INISettingsInterface& ini) { ini.SetBoolValue("DEV9/ForjaLAN", "UPnP", next); }, true);
			break;
		}
		case PS4GameMenu::ROW_CROWD: // compilation 82: the crowd in software every 1, 2, 4 or 8 passes ([Forja] PublicoCada); 137: Al vuelo
		{
			if (!GameDrawsCrowdInSoftware())
				break;
			constexpr int count = static_cast<int>(std::size(CROWD_EVERY_STEPS));
			int position = 0;
			for (int i = 0; i < count; i++)
			{
				if (CROWD_EVERY_STEPS[i] == s_crowd_every)
					position = i;
			}
			const int next = static_cast<int>(CROWD_EVERY_STEPS[(position + delta + count) % count]);
			SavePlayerProfile("Crowd", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "PublicoCada", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_FPU_EXACT: // compilation 120: PCSX2's key, so the game layer is reloaded (the recompilers start over)
		{
			// Turning it on needs the two lighter modes too (Pcsx2Config::RecompilerOptions::ApplySanityCheck resets all three
			// otherwise); turning it off keeps them as the GameDB left them (Bully: 3 -> 2, the extra overflow stays).
			const bool next = !EmuConfig.Cpu.Recompiler.fpuFullMode;
			SavePlayerProfile("FPU exact", [next](INISettingsInterface& ini) {
				if (next)
				{
					ini.SetBoolValue("EmuCore/CPU/Recompiler", "fpuOverflow", true);
					ini.SetBoolValue("EmuCore/CPU/Recompiler", "fpuExtraOverflow", true);
				}
				ini.SetBoolValue("EmuCore/CPU/Recompiler", "fpuFullMode", next);
			}, true);
			ProbeLog("FPU exact: requested=%d active=%d overflow=%d extra=%d\n", next ? 1 : 0, EmuConfig.Cpu.Recompiler.fpuFullMode ? 1 : 0,
				EmuConfig.Cpu.Recompiler.fpuOverflow ? 1 : 0, EmuConfig.Cpu.Recompiler.fpuExtraOverflow ? 1 : 0);
			break;
		}
		case PS4GameMenu::ROW_IOP_FAST: // compilation 120: No / Sí ([Forja] IopRapido); ApplyGameProfile takes it
		{
			const int next = s_iop_fast ? 0 : 1;
			SavePlayerProfile("IOP fast writes", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "IopRapido", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_IOP_SLEEP: // compilation 122: No / Sí ([Forja] IopDormido); ApplyGameProfile takes it
		{
			const int next = s_iop_sleep_game ? 0 : 1;
			SavePlayerProfile("IOP asleep in the game", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "IopDormido", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_VU1_FAST: // compilation 126: PCSX2's keys, so the game layer is reloaded (the VU recompilers start over)
		{
			// On: the flag hack and no clamping. Off (compilation 127, when the global is already without clamping): the clamp back, and the
			// flag hack and the extra clamps from the GameDB or the global values.
			const bool next = !Vu1FastOn();
			SavePlayerProfile("VU1 fast", [next](INISettingsInterface& ini) {
				if (next)
				{
					ini.SetBoolValue("EmuCore/Speedhacks", "vuFlagHack", true);
					ini.SetBoolValue("EmuCore/CPU/Recompiler", "vu1Overflow", false);
					ini.SetBoolValue("EmuCore/CPU/Recompiler", "vu1ExtraOverflow", false);
					ini.SetBoolValue("EmuCore/CPU/Recompiler", "vu1SignOverflow", false);
				}
				else
				{
					ini.DeleteValue("EmuCore/Speedhacks", "vuFlagHack");
					ini.SetBoolValue("EmuCore/CPU/Recompiler", "vu1Overflow", true);
					ini.DeleteValue("EmuCore/CPU/Recompiler", "vu1ExtraOverflow");
					ini.DeleteValue("EmuCore/CPU/Recompiler", "vu1SignOverflow");
				}
			}, true);
			ProbeLog("VU1 fast: requested=%d active=%d flag_hack=%d vu1_clamp=%d\n", next ? 1 : 0, Vu1FastOn() ? 1 : 0,
				EmuConfig.Speedhacks.vuFlagHack ? 1 : 0, EmuConfig.Cpu.Recompiler.vu1Overflow ? 1 : 0);
			break;
		}
		case PS4GameMenu::ROW_SKIP_REPEATS: // compilation 128: PCSX2's [EmuCore/GS] SkipDuplicateFrames for this game (the settings reload)
		{
			const bool next = !EmuConfig.GS.SkipDuplicateFrames;
			SavePlayerProfile("Skip repeats", [next](INISettingsInterface& ini) { ini.SetBoolValue("EmuCore/GS", "SkipDuplicateFrames", next); }, true);
			ProbeLog("Skip repeated presents: requested=%d active=%d\n", next ? 1 : 0, EmuConfig.GS.SkipDuplicateFrames ? 1 : 0);
			break;
		}
		case PS4GameMenu::ROW_DISC_NO_WAIT: // compilation 130: No / Sí ([Forja] DiscoSinEsperas); CDVD.cpp reads it at the next read
		{
			const int next = s_disc_no_wait_game ? 0 : 1;
			SavePlayerProfile("Disc without waits", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "DiscoSinEsperas", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_CPU_SEAMLESS: // compilation 128: No / Sí ([Forja] CpuSinTirones); the profile's apply resets the EE recompiler
		{
			const int next = s_cpu_seamless_game ? 0 : 1;
			SavePlayerProfile("CPU seamless", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "CpuSinTirones", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_EFFECTS_AHEAD: // compilation 127: No / Sí ([Forja] EfectosListos); the GS thread makes the list's next start
		{
			const int next = s_effects_ahead_game ? 0 : 1;
			SavePlayerProfile("Effects ahead", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "EfectosListos", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_FAST_CODE_CHECK: // compilation 124: No / Sí ([Forja] RevisionRapida); the recompiler starts over in the new mode
		{
			const int next = s_fast_code_game ? 0 : 1;
			SavePlayerProfile("Fast code check", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "RevisionRapida", next); }, false);
#if defined(PCSX2_PS4_VM_BOOT_JIT)
			const bool skip = s_fast_code_marker || s_fast_code_game;
			if (skip != g_ps4_skip_smc_checks && VMManager::GetDiscCRC() != 0)
			{
				g_ps4_skip_smc_checks = skip;
				g_ps4_bytecopy_fusion = skip && access("/data/ForjaSX2/vm-no-bytecopy-fusion", F_OK) != 0;
				Cpu->Reset();
				ProbeLog("EE JIT manual SMC validation: %s (the game's row; recompiler reset)\n", skip ? "relaxed" : "exact");
			}
#endif
			break;
		}
		case PS4GameMenu::ROW_CPU_AUTO_UP: // compilation 122: No / Sí ([Forja] CpuAutoSube)
		{
			const int next = s_cpu_auto_up ? 0 : 1;
			SavePlayerProfile("CPU auto back up", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "CpuAutoSube", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_SW_JIT: // compilation 117: No / Sí ([Forja] TraductorSoftware); ApplyGameProfile takes it
		{
			const int next = s_sw_jit ? 0 : 1;
			SavePlayerProfile("Software draws JIT", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "TraductorSoftware", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_DRAW_THREAD: // compilation 116: No / Sí ([Forja] GrabarEnOtroHilo); ApplyGameProfile takes it
		{
			const int next = s_draw_thread ? 0 : 1;
			SavePlayerProfile("Draw thread", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "GrabarEnOtroHilo", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_GS_SKIP:
		{
			// No, auto, one frame in two ([Forja] SaltarCuadros 0, 1, 2; compilation 47), and without the guard (3, compilation 82).
			// ApplyGameProfile takes it from the file.
			int next = (s_gs_skip_mode + delta + 6) % 6; // compilation 88: "2 de 3" too; 90: "Al vuelo"
			if (next == 1) // compilation 140: no old Auto any more
				next = delta > 0 ? 2 : 0;
			SavePlayerProfile("GS skip", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "SaltarCuadros", next); }, false);
			break;
		}
		case PS4GameMenu::ROW_CORES:
		{
			constexpr int count = static_cast<int>(std::size(CORE_PLANS));
			const int current = static_cast<int>(CoresMenuValue());
			const int next = current < count ? (current + delta + count) % count : 0;
			const char* plan = CORE_PLANS[next];
			SavePlayerProfile("Cores", [plan](INISettingsInterface& ini) { ini.SetStringValue("Forja", "Nucleos", plan); }, false);
			RepinWorkersForProfile();
			break;
		}
		default:
			break;
	}
}

static void RefreshStateSlot()
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	s_state_slot_used = VMManager::HasSaveStateInSlot(serial.c_str(), crc, static_cast<s32>(s_state_slot + 1));
	// Compilation 58: when the slot was saved, for the Guardar and Cargar rows (as M33's slot card; its picture comes later).
	std::string date;
	struct stat info{};
	if (s_state_slot_used &&
		stat(VMManager::GetSaveStateFileName(serial.c_str(), crc, static_cast<s32>(s_state_slot + 1)).c_str(), &info) == 0)
	{
		char text[40] = "";
		const std::time_t when = info.st_mtime;
		if (const std::tm* tm = std::localtime(&when))
			std::strftime(text, sizeof(text), ForjaLang::T("%d/%m a las %H:%M"), tm); // strftime: any translation is safe
		date = text;
	}
	PS4GameMenu::SetStateSlotDate(date);
}

// A movie's lowered EE rate is not part of a save state: put the game's own rate back before a load or a restart, which
// both clear the recompilers, so the new blocks get it.
static void EndVideoModeForStateChange()
{
	if (!s_video_active)
		return;
	EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(s_video_saved_rate);
	s_video_active = false;
	s_video_busy_checks = 0;
	ForjaEndMovieSettings(); // compilation 67
}

// Forja SX2: "Guardar estado". The menu has the VM paused, outside VMManager::Execute, so the state is consistent; it is
// zipped on this thread (about a second) so the row can say at once how it went.
static void SaveGameState()
{
	const s32 slot = static_cast<s32>(s_state_slot + 1);
	bool failed = false;
	std::string failure;
	const auto t0 = Common::Timer::GetCurrentValue();
	ForjaAnimHalfRestore(); // 174 in the app
	VMManager::SaveStateToSlot(slot, false, [&failed, &failure](const std::string& error) {
		failed = true;
		failure = error;
	});
	s_state_action = failed ? PS4GameMenu::STATE_SAVE_FAILED : PS4GameMenu::STATE_SAVED;
	RefreshStateSlot();
	ProbeLog("Save state: slot %d %s in %.0f ms %s\n", slot, failed ? "FAILED" : "saved",
		Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - t0), failure.c_str());
}

// Compilation 61: a save state that does not load well (Crash Nitro Kart's slot 1, saved with build 46, 2026-10-20: the VU1
// sent the GS broken packets and the game hung, every try). Before a load the moment the player is in goes to
// sstates/<serial> (<crc>).deshacer.p2s; CheckLoadedState watches the first 20 s and RevertLoadedState goes back there.
extern std::atomic<u32> g_forja_gif_exceeded;
static std::string s_state_undo_path;
static double s_state_check_since = 0.0; // 0: no load being watched
static u32 s_state_check_gif = 0;
static s32 s_state_check_slot = 0;
static bool s_state_revert_pending = false;

// Forja SX2: "Cargar estado". True when the game goes on from the loaded moment (the menu then closes).
static bool LoadGameState()
{
	const s32 slot = static_cast<s32>(s_state_slot + 1);
	if (!s_state_slot_used)
	{
		s_state_action = PS4GameMenu::STATE_EMPTY;
		return false;
	}
	EndVideoModeForStateChange();
	const auto t0 = Common::Timer::GetCurrentValue();
	// Compilation 61: the moment the player is in, kept in case the slot does not load well (CheckLoadedState).
	s_state_undo_path = Path::Combine(EmuFolders::Savestates,
		fmt::format("{} ({:08X}).deshacer.p2s", VMManager::GetDiscSerial(), VMManager::GetDiscCRC()));
	bool undo = true;
	ForjaAnimHalfRestore(); // 174 in the app
	VMManager::SaveState(s_state_undo_path.c_str(), false, false, [&undo](const std::string& why) {
		undo = false;
		ProbeLog("Save state: the moment before the load could not be kept (%s)\n", why.c_str());
	});
	const double undo_ms = Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - t0);
	Error error;
	const bool loaded = VMManager::LoadStateFromSlot(slot, false, &error);
	// Compilation 146 had a notice here for a state older than the memory card; Joseph (2026-11-18): "no quiero que esté ahí molestando
	// a los jugadores". Gone.
	if (!loaded)
		s_state_action = PS4GameMenu::STATE_LOAD_FAILED;
	if (loaded)
	{
		ForjaSession::NoteStateLoad(); // compilation 130: the summary's memory growth leaves the load's own jump out
		s_forja_techo_load_grace.store(true, std::memory_order_relaxed); // 164 in the app: the load's stall doesn't lift the ceiling
	}
	ProbeLog("Save state: slot %d %s in %.0f ms (the moment before kept in %.0f ms: %d) %s\n", slot, loaded ? "loaded" : "LOAD FAILED",
		Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - t0), undo_ms, undo ? 1 : 0,
		loaded ? "" : error.GetDescription().c_str());
	if (loaded && undo)
	{
		s_state_check_since = s_run_timer.GetTimeSeconds();
		s_state_check_gif = g_forja_gif_exceeded.load(std::memory_order_relaxed);
		s_state_check_slot = slot;
	}
	else
	{
		s_state_check_since = 0.0;
		unlink(s_state_undo_path.c_str());
	}
	return loaded;
}

// Compilation 61: with the game running, after a load. Over 3000 broken PATH1 packets in its first 20 s mean the state does not
// load well (Crash Nitro Kart's slot 1 said it 25,000 times in ten seconds, then hung); the VM pauses and VMHostPoll goes
// back to the moment before (RevertLoadedState). After 20 s the kept moment is deleted. A game that plays well can say it
// too: God of War II, 911 times in two minutes of play (2026-10-18), ~150 in 20 s, far under 3000.
static void CheckLoadedState(double elapsed)
{
	if (s_state_check_since <= 0.0)
		return;
	const u32 broken = g_forja_gif_exceeded.load(std::memory_order_relaxed) - s_state_check_gif;
	if (broken > 3000)
	{
		ProbeLog("Save state: slot %d does not load well (%u broken GS packets in %.1f s); going back to the moment before\n",
			s_state_check_slot, broken, elapsed - s_state_check_since);
		s_state_check_since = 0.0;
		s_state_revert_pending = true;
		VMManager::SetPaused(true);
	}
	else if (elapsed - s_state_check_since > 20.0)
	{
		ProbeLog("Save state: slot %d loaded well (%u broken GS packets in 20 s)\n", s_state_check_slot, broken);
		s_state_check_since = 0.0;
		unlink(s_state_undo_path.c_str());
	}
}

// Compilation 61: with the VM paused by CheckLoadedState. The game goes on from where the player was before the load, and the
// Cargar estado row says the slot is damaged (the file stays: nothing of the player's is deleted).
static void RevertLoadedState()
{
	s_state_revert_pending = false;
	EndVideoModeForStateChange();
	Error error;
	const bool back = VMManager::LoadState(s_state_undo_path.c_str(), &error);
	s_forja_techo_load_grace.store(true, std::memory_order_relaxed); // 164 in the app
	unlink(s_state_undo_path.c_str());
	s_state_action = PS4GameMenu::STATE_LOAD_DAMAGED;
	ProbeLog("Save state: back to the moment before slot %d: %s %s\n", s_state_check_slot, back ? "done" : "FAILED",
		back ? "" : error.GetDescription().c_str());
	if (!s_game_menu_open)
		VMManager::SetPaused(false);
}

// Forja SX2: "Reiniciar juego". Paused, VMManager::Reset is immediate (outside a running VM it never has to wait for a
// safe point), and the game boots again without leaving Forja.
static void RestartGame()
{
	EndVideoModeForStateChange();
	VMManager::Reset();
	ProbeLog("Emulator menu: game restarted\n");
}

// Compilation 137: frame generation's resources, made on the GS thread before the game goes on (GSDeviceVK.cpp).
extern void ForjaFrameGenPrepare();
static void ForjaFrameGenReadyBeforeResume()
{
	if (!g_orbis_fg_wanted)
		return;
	MTGS::RunOnGSThread([]() { ForjaFrameGenPrepare(); });
	MTGS::WaitGS(false, false, false);
}

static void ResumeFromGameMenu(u32 raw)
{
	ForjaFrameGenReadyBeforeResume(); // compilation 137
	s_game_menu_open = false;
	s_blocked_pad_buttons |= raw;
	PS4GameMenu::SetInGameMenu({});
	ProbeLog("Emulator menu: resume\n");
	// Compilation 61: recommended settings still downloading are not used with the game running (VMHostPoll uses them only
	// with the menu open); the downloaded copy stays for X next time.
	if (s_recommended_wait_since > 0.0)
	{
		s_recommended_wait_since = 0.0;
		ProbeLog("Recommended settings: the menu closed before the download ended, not used\n");
	}
	// Forja SX2: the GS's hitch detector (GSRendererPS4.cpp) does not count the time the menu was open as a hitch.
	g_forja_resumes.fetch_add(1, std::memory_order_relaxed);
	VMManager::SetPaused(false);
}

// Forja 1.06: the session's summary, logs/resumen.txt (SessionStats.h): who, what console, what settings, and the figures.
// Every minute of play (VMHostPoll's heartbeat) and when the game ends (RunVM).
static unsigned s_session_beats = 0;
// Compilation 76: PCSX2's title is "<serial> [?]" for a game the PS4's GameDB reading leaves without a name (every one of the
// five new games on 2026-10-23): the library's file name says which game it is.
static std::string ForjaGameTitle()
{
	std::string title = VMManager::GetTitle(false);
	if ((title.empty() || title.ends_with(" [?]")) && !s_selected_game.empty())
		title = std::string(Path::GetFileTitle(s_selected_game));
	return title;
}

// The summary's first lines: who, what console, what settings (compilation 77: the player's report starts with them too).
static void ForjaSessionLines(std::string& header_text, std::string& settings_text)
{
	const std::string title = ForjaGameTitle();
	char header[640];
	std::snprintf(header, sizeof(header),
		"Forja SX2 %d.%02d (compilación %d): resumen de la partida\n"
		"Juego: %s (%s, CRC %08X)\n"
		"Consola: %s (CPU ~%.2f GHz, modo Pro %d, aparato Pro %d/%d), sistema %s\n",
		FORJA_VERSION_MAJOR, FORJA_VERSION_MINOR, FORJA_BUILD_NUMBER, title.c_str(),
		VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC(), ConsoleName(), s_console_cpu_ghz, s_console_neo,
		s_console_has_neo, s_console_authentic_neo, s_console_system.empty() ? "?" : s_console_system.c_str());
	const int rate = s_video_active ? s_video_saved_rate : static_cast<int>(EmuConfig.Speedhacks.EECycleRate);
	char cpu_auto[256] = "apagado";
	if (s_cpu_auto_floor != 0)
		std::snprintf(cpu_auto, sizeof(cpu_auto), "hasta %d %%%s", EERatePercent(s_cpu_auto_floor),
			CpuAutoIdle() ? ", no actúa: la CPU del juego ya está igual o más baja" : ""); // compilation 76
	const char* frame_gen = !g_orbis_fg_wanted ? "No" : s_fg_fluid_profile ? "Fluida" : "Sí";
	const char* gs_skip = s_gs_skip_mode == 5 ? "al vuelo" : s_gs_skip_mode == 4 ? "2 de 3" : s_gs_skip_mode == 3 ? "1 de 2 rápido" : s_gs_skip_mode == 2 ? "1 de 2" :
	                      s_gs_skip_mode == 1 ? (s_gs_skip_useless ? "Auto (se apagó: el juego dibuja fuera de la pantalla y no hay cuadros que saltar)" :
	                                                                 "Auto") :
	                                            "No";
	// Compilation 87: where the CPU started when the automatic one moved it (F-3420: "31 %" was the end; it began at 300 %).
	if (s_cpu_auto_steps > 0 && s_cpu_auto_base != rate)
	{
		const size_t used = std::strlen(cpu_auto);
		std::snprintf(cpu_auto + used, sizeof(cpu_auto) - used, "; empezó en %d %%%s", EERatePercent(s_cpu_auto_base),
			s_cpu_auto_base > 0 ? ", lo que pide la base de datos de PCSX2 para este juego: si se ve raro, súbela" : "");
	}
	char settings[768];
	std::snprintf(settings, sizeof(settings),
		"Ajustes: CPU de la PS2 %d %% (CPU automático %s), resolución %ux, generación de cuadros %s%s, Saltar cuadros %s, "
		"huellas [%s], núcleos %s, Modo video %s\n",
		EERatePercent(rate), cpu_auto, s_game_upscale, frame_gen, g_orbis_fg_wanted && g_orbis_fg_gave_up ? " (se apagó sola: costaba velocidad)" : "",
		gs_skip, RulesOnList().c_str(), s_profile.cores.empty() ? "global" : s_profile.cores.c_str(),
		!s_video_mode_enabled ? "No" : s_video_auto ? "Auto" : "fijo");
	header_text = header;
	settings_text = settings;
	// Compilation 82: the crowd of the Guitar Hero games, drawn in software once every N passes.
	if (GameDrawsCrowdInSoftware())
	{
		char crowd[96];
		if (s_crowd_every == CROWD_AUTO) // compilation 137
			std::snprintf(crowd, sizeof(crowd), "Público por software: al vuelo (1 de cada 1, 2 o 4 cuadros)\n");
		else
			std::snprintf(crowd, sizeof(crowd), "Público por software: 1 de cada %u cuadros\n", s_crowd_every);
		settings_text += crowd;
	}
}

static void ForjaSessionWrite(bool final)
{
	std::string header, settings;
	ForjaSessionLines(header, settings);
	ForjaSession::Write(header, settings, final);
}

// Forja SX2 (compilation 77): "Reportar este juego" (L3+R3 > Sistema; ForjaReport.cpp). Joseph (2026-10-23), after M33's reports:
// the players send how a game runs, with the logs and the meters, so the emulator can get better from them. The report is the
// session's summary, the player's word for it, the game's profile and settings lines and the last ~2 minutes of the meters, plus
// a crash or hang log if one is there; nothing of the PSN account. Left/Right picks the word, X sends; one a minute at most.
// The lines of a log worth sending, the last `max_bytes` of them.
static std::string ReportLogLines(const char* path, size_t read_bytes, size_t max_bytes)
{
	static constexpr const char* kept[] = {"Performance:", "GS groups:", "Frame pacing:", "Video mode:", "Movie settings:", "GS skip:",
		"CPU auto:", "[fg]", "VM heartbeat:", "Game settings", "Game profile:", "Render configuration:", "Hitch:", "Audio SPU2 split:",
		"PS4 audio queue:", "EE JIT cumulative:", "Patches:", "VU1 programs:", "Recommended settings:", "Emulator menu:",
		"EE guest PC:", "Vulkan compute GS:", "Cycle skip auto:", "GS skip on the fly:", "Speed limit:", "Frame limiter:", "EE cycle skip:", // compilation 106
		"GS thread split:", "EE-VU1 waits:", "Compilation 122 rows:", "Threads:"}; // compilation 110; 122; 123
	std::string text;
	std::FILE* file = std::fopen(path, "rb");
	if (!file)
		return text;
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, size > static_cast<long>(read_bytes) ? size - static_cast<long>(read_bytes) : 0, SEEK_SET);
	std::string raw(read_bytes, '\0');
	raw.resize(std::fread(raw.data(), 1, raw.size(), file));
	std::fclose(file);
	size_t start = size > static_cast<long>(read_bytes) ? raw.find('\n') + 1 : 0; // a cut first line is dropped
	while (start < raw.size())
	{
		size_t end = raw.find('\n', start);
		if (end == std::string::npos)
			end = raw.size();
		const std::string_view line(raw.data() + start, end - start);
		for (const char* prefix : kept)
		{
			if (line.starts_with(prefix))
			{
				text.append(line);
				text += '\n';
				break;
			}
		}
		start = end + 1;
	}
	if (text.size() > max_bytes)
		text.erase(0, text.find('\n', text.size() - max_bytes) + 1);
	return text;
}

// A whole small file (a crash or hang log), with its date. Compilation 79: only one from the last two hours (this session, or the one
// that just closed): the 77's first report carried a crash.log from three days before, of another game.
static std::string ReportSmallFile(const char* path, size_t max_bytes)
{
	struct stat st{};
	if (stat(path, &st) != 0 || st.st_size <= 0 || std::time(nullptr) - st.st_mtime > 2 * 3600)
		return {};
	std::FILE* file = std::fopen(path, "rb");
	if (!file)
		return {};
	std::string text(std::min<size_t>(static_cast<size_t>(st.st_size), max_bytes), '\0');
	text.resize(std::fread(text.data(), 1, text.size(), file));
	std::fclose(file);
	char date[64];
	std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", std::localtime(&st.st_mtime));
	return std::string("--- ") + path + " (" + date + ")\n" + text + "\n";
}

// Compilation 146 (app): the console's list of effects for this game (/data/ForjaSX2/cache/efectos/<serial>.bin, made by
// GSDeviceVK.cpp: 'FJEF', 24 bytes of header and 32 per effect, at most 2048), in hexadecimal at the end of the report. In the lab,
// herramientas/efectos_de_reportes.py joins the reports' lists with the project's (ps4/vm/assets/efectos), and the settings center
// brings them to every player: a game nobody on the project has gets its effects made at the start from its players' sessions.
static std::string ReportEffectsList(const std::string& serial)
{
	if (serial.empty())
		return {};
	std::string name;
	for (const char c : serial)
		name += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
	const std::string path = "/data/ForjaSX2/cache/efectos/" + name + ".bin";
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (!file)
		return {};
	std::string raw(24 + 32 * 2048, '\0');
	raw.resize(std::fread(raw.data(), 1, raw.size(), file));
	std::fclose(file);
	u32 magic = 0, count = 0;
	if (raw.size() < 24)
		return {};
	std::memcpy(&magic, raw.data(), 4);
	std::memcpy(&count, raw.data() + 16, 4);
	if (magic != 0x46454A46u || count > 2048 || raw.size() != 24 + 32 * static_cast<size_t>(count))
		return {};
	static constexpr char digits[] = "0123456789abcdef";
	std::string out = "\n--- Efectos " + name + " (" + std::to_string(count) + ", FJEF en hexadecimal)\n";
	for (size_t i = 0; i < raw.size(); i++)
	{
		const unsigned char b = static_cast<unsigned char>(raw[i]);
		out += digits[b >> 4];
		out += digits[b & 15];
		if (i % 64 == 63)
			out += '\n';
	}
	return out + "\n--- Fin de los efectos\n";
}

static void SendReport(double elapsed)
{
	s_report_too_soon = elapsed - s_report_last_sent < 60.0;
	if (s_report_too_soon || ForjaReport::GetStatus() == ForjaReport::Status::Sending)
		return;
	const std::string serial = VMManager::GetDiscSerial();
	const std::time_t now = std::time(nullptr);
	char stamp[32];
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
	char name[128];
	std::snprintf(name, sizeof(name), "%s_%s_c%d", serial.empty() ? "sin-serial" : serial.c_str(), stamp, FORJA_BUILD_NUMBER);
	// A short number the player can quote: four hexadecimal digits from the report's name.
	unsigned hash = 2166136261u;
	for (const char* c = name; *c; c++)
		hash = (hash ^ static_cast<unsigned char>(*c)) * 16777619u;
	char id[16];
	std::snprintf(id, sizeof(id), "F-%04X", hash & 0xffffu);
	const std::string rating_text = std::string(REPORT_RATINGS[std::clamp(s_report_rating, 0, REPORT_RATING_COUNT - 1)]) +
		(s_report_rating_chosen ? "" : " (sin elegir)"); // compilation 110
	const char* rating = rating_text.c_str();
	const char* issue = REPORT_ISSUES[std::clamp(s_report_issue, 0, REPORT_ISSUE_COUNT - 1)]; // compilation 80
	const std::string measured = ForjaSession::Verdict();

	std::string header, settings;
	ForjaSessionLines(header, settings);
	std::string text = "Reporte " + std::string(id) + " de un jugador: " + rating + "; qué falló: " + issue + "\n";
	if (!s_report_comment.empty()) // compilation 86
		text += "Comentario del jugador: " + s_report_comment + "\n";
	if (s_report_where >= 0) // compilation 88
		text += std::string("Dónde se ve: ") + REPORT_WHERE[s_report_where] + "\n";
	text += "Medido: " + measured + "\n";
	text += ForjaSession::Text(header, settings, false);
	text += "\n--- Ahora\n";
	{
		char line[512];
		std::snprintf(line, sizeof(line), "Velocidad %.0f %%, %.1f cuadros/s, EE %.0f %%, GS %.0f %%, VU1 %.0f %%, gráfica %.0f %%; video %s\n",
			PerformanceMetrics::GetSpeed(), PerformanceMetrics::GetInternalFPS(), PerformanceMetrics::GetCPUThreadUsage(),
			PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetVUThreadUsage(), PerformanceMetrics::GetGPUUsage(),
			s_video_active ? "sí" : "no");
		text += line;
	}
	text += "\n--- El registro (líneas de medidores, los últimos minutos)\n";
	text += ReportLogLines("/data/ForjaSX2/logs/PCSX2-vm-boot.log", 384 * 1024, 48 * 1024);
	text += ReportSmallFile("/data/ForjaSX2/crash.log", 4096);
	text += ReportSmallFile("/data/ForjaSX2/logs/crash.log", 4096);
	text += ReportSmallFile("/data/ForjaSX2/logs/hang.log", 2048);
	text += ReportEffectsList(serial); // compilation 146 (app)

	// Compilation 80: with what went wrong and what the meters measured, so the channel's line says it without the file.
	char summary[768];
	std::snprintf(summary, sizeof(summary), "Reporte %s · %s · %s · %s · falló: %s · Forja %d.%02d (%d) · %s · velocidad %.0f %% · "
		"medido: %s", id, ForjaGameTitle().c_str(), serial.empty() ? "?" : serial.c_str(), rating, issue, FORJA_VERSION_MAJOR,
		FORJA_VERSION_MINOR, FORJA_BUILD_NUMBER, ConsoleName(),
		PerformanceMetrics::GetSpeed(), measured.c_str());
	// Compilation 86: and the player's comment, its first 300 bytes in the line (Discord's message takes 2,000).
	std::string line = summary;
	if (!s_report_comment.empty())
		line += " · comentario: " + ForjaLang::Cut(s_report_comment, 300);
	if (s_report_where >= 0) // compilation 88
		line += std::string(" · dónde: ") + REPORT_WHERE[s_report_where];
	if (ForjaReport::Send(std::string(id) + "_" + name, line, text))
	{
		s_report_last_sent = elapsed;
		ProbeLog("Report: %s (%s, %s, %zu bytes, comment %zu bytes) %s\n", id, rating, issue, text.size(), s_report_comment.size(),
			ForjaReport::HasChannel() ? "sending to the reports channel" : "kept in reportes/ (no channel in this build)");
		s_report_comment.clear(); // it went with this one
		s_report_phrase = -1; // compilation 87
		s_report_where = -1; // compilation 88
	}
}

static void PublishReportText()
{
	using ForjaLang::T;
	using ForjaLang::TF;
	char label[256];
	std::snprintf(label, sizeof(label), TF("Reportar este juego:  < %s >"),
		T(REPORT_RATINGS[std::clamp(s_report_rating, 0, REPORT_RATING_COUNT - 1)]));
	const std::string last = ForjaReport::LastName();
	const std::string id = last.substr(0, last.find('_'));
	const bool channel = ForjaReport::HasChannel();
	char description[512];
	switch (ForjaReport::GetStatus())
	{
		case ForjaReport::Status::Sending:
			// Compilation 86: with its seconds. Joseph (2026-10-24) didn't know whether it had gone: one took ~7 s, another
			// hadn't answered after 35 s when he closed the game.
			std::snprintf(description, sizeof(description),
				TF("Mandando el reporte... %d s. Con internet lento puede tardar hasta un minuto; puedes seguir jugando"),
				ForjaReport::SendingSeconds());
			break;
		case ForjaReport::Status::Sent:
			std::snprintf(description, sizeof(description), TF("Mandado: reporte %s. Gracias. Si quieres, cuéntanos más en el Discord de Forja"),
				id.c_str());
			break;
		case ForjaReport::Status::Kept:
			std::snprintf(description, sizeof(description),
				channel ? TF("No se pudo mandar (¿sin internet?): el reporte %s quedó guardado y se vuelve a mandar solo al abrir otro juego") :
						  TF("Esta versión no tiene el canal de reportes: el reporte %s quedó en /data/ForjaSX2/reportes"),
				id.c_str());
			break;
		default:
			std::snprintf(description, sizeof(description), "%s",
				channel ? T("X: manda al Discord de Forja cómo va este juego, con el resumen de la partida y sus medidores (nada de tu cuenta)") :
						  T("X: guarda en /data/ForjaSX2/reportes cómo va este juego (esta versión no tiene el canal de reportes)"));
			break;
	}
	if (s_report_too_soon && s_run_timer.GetTimeSeconds() - s_report_last_sent < 60.0)
		std::snprintf(description, sizeof(description), "%s", T("Ya mandaste uno hace poco: espera un minuto para mandar otro"));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_REPORT, label, description);
	// Compilation 80: what went wrong, sent with the report.
	std::snprintf(label, sizeof(label), TF("Qué falló:  < %s >"),
		T(REPORT_ISSUES[std::clamp(s_report_issue, 0, REPORT_ISSUE_COUNT - 1)]));
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_REPORT_ISSUE, label,
		T("Elige lo que notaste y manda el reporte. La velocidad la miden los medidores; la imagen, el sonido y los controles solo los ves tú"));
	// Compilation 86: the comment, written with the PS4's keyboard.
	if (s_report_comment.empty())
		std::snprintf(label, sizeof(label), "%s", T("Comentario:  ninguno"));
	else
		std::snprintf(label, sizeof(label), TF("Comentario:  %s"), ForjaLang::Cut(s_report_comment, 70).c_str());
	{
		char where[256];
		if (s_report_where < 0)
			std::snprintf(where, sizeof(where), "%s", T("Dónde se ve:  < sin elegir >"));
		else
			std::snprintf(where, sizeof(where), TF("Dónde se ve:  < %s >"), T(REPORT_WHERE[s_report_where]));
		PS4GameMenu::SetRowText(PS4GameMenu::ROW_REPORT_WHERE, where,
			T("Izquierda/Derecha: en qué parte de la pantalla o en qué momento se ve lo que falla. Es lo que solo tú ves: va con el reporte"));
	}
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_REPORT_COMMENT, label,
		ForjaKeyboard::Failed() ? T("El teclado de la PS4 no abrió: elige una frase con Izquierda/Derecha. Cuadrado la borra") :
		s_report_comment.empty() ? T("X: escribe con el teclado de la PS4 qué pasó (R2 lo guarda). Izquierda/Derecha: una frase lista. Se manda con el reporte") :
		                           T("X: cambiarlo  ·  Izquierda/Derecha: otra frase  ·  Cuadrado: borrarlo. Se manda con el próximo reporte"));
}

// Compilation 141: what "Código al día" did, every 5 s when something happened.
static void LogFreshCode(double elapsed)
{
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	static double last = 0.0;
	static u32 seen_loads = 0, seen_flushes = 0;
	if (elapsed - last < 5.0)
		return;
	last = elapsed;
	const u32 loads = g_forja_fresh_loads.load(std::memory_order_relaxed), flushes = g_forja_fresh_flushes.load(std::memory_order_relaxed);
	if (loads == seen_loads && flushes == seen_flushes)
		return;
	seen_loads = loads;
	seen_flushes = flushes;
	ProbeLog("Fresh code (141): fast code check %s; disc loads over changed code %u (%u blocks made again); instruction cache flushes %u, "
			 "passes %u (%u blocks changed, %.2f ms in all)\n", g_ps4_skip_smc_checks ? "on" : "off", loads,
		g_forja_fresh_load_blocks.load(std::memory_order_relaxed), flushes, g_forja_fresh_scans.load(std::memory_order_relaxed),
		g_forja_fresh_flush_blocks.load(std::memory_order_relaxed), g_forja_fresh_scan_us.load(std::memory_order_relaxed) / 1000.0);
#else
	(void)elapsed;
#endif
}

void VMHostPoll()
{
	++s_host_poll_calls;
	// (AI-assisted) Paused/menu input must not be gated by EE cycles - otherwise
	// the 16 ms paused loop (62 Hz) is throttled to one pad poll per 1024 calls
	// ~16 seconds, appearing as a hang on L3+R3. Needs proper testing.
	const VMState vm_state_early = VMManager::GetState();
	const bool is_paused_menu = (vm_state_early == VMState::Paused) || s_game_menu_open;
	if (!is_paused_menu)
	{
		// Fast path: gate clock query and CPU queue draining first to avoid
		// atomic memory fences and state queries on ~600k events/s during FMV/DMA.
		// Needs proper testing across games.
		if (!s_host_poll_gate.ShouldCheckClock(cpuRegs.cycle))
			return;
	}
	VMHostDrainCPUQueue();
	const VMState vm_state = VMManager::GetState();
	if (vm_state != VMState::Running && vm_state != VMState::Paused)
		return;
	++s_host_clock_checks;
	const double elapsed = s_run_timer.GetTimeSeconds();
	// (AI-assisted) Avoid a Pad syscall on every EE event; poll at most 100 times/s.
	// While paused/menu we still respect this 10 ms throttle for 100 Hz pad polling.
	if (elapsed - s_last_input < 0.01)
		return;
	s_last_input = elapsed;
	PS4ThreadCPUMetrics::Sample(PS4ThreadCPUMetrics::Role::EE);
	// Compilation 61: a load that broke the game goes back once the VM has paused (the paused loop, outside the EE's run).
	if (s_state_revert_pending && vm_state == VMState::Paused)
		RevertLoadedState();
	if (!is_paused_menu)
	{
		UpdateGameProfile(elapsed);
		UpdateVideoMode(elapsed);
		UpdateRuleShares(); // compilation 55
		UpdateCpuAuto(elapsed);
		UpdateCycleSkipAuto(elapsed);
		UpdateFramePush(elapsed); // compilation 132
		UpdateGsSkip(elapsed);
		UpdateOutputRate(elapsed);
		LogFreshCode(elapsed); // compilation 141
		CheckLoadedState(elapsed); // compilation 61
	}
	// (AI-assisted) Verbose 1s heartbeat when paused/menu open, 5s otherwise. Needs proper testing.
	const bool should_log_heartbeat = s_verbose_diag ?
		(s_game_menu_open || vm_state == VMState::Paused ? (elapsed - s_last_log >= 1.0) : (elapsed - s_last_log >= 2.0)) :
		(elapsed - s_last_log >= 5.0);
	if (should_log_heartbeat)
	{
		// Late loader logs still need migration, but never at EE event frequency.
		// Filesystem calls here are bounded by the heartbeat (normally 5 seconds).
		MigrateLegacyLogs();
		if (s_log_direct_boot) // compilation 67: the game is running; the log goes back to its queue
		{
			s_log_direct_boot = false;
			PS4ProbeLogger::SetDirect(false);
			ProbeLog("PS4 log: back to its queue (the game started)\n");
		}
		const double interval = elapsed - s_last_log;
		const double ee_mcycles = static_cast<u32>(cpuRegs.cycle - s_last_ee_cycle) / (interval * 1000000.0);
		const double host_polls_per_second = (s_host_poll_calls - s_last_host_poll_calls) / interval;
		const double host_clock_checks_per_second = (s_host_clock_checks - s_last_host_clock_checks) / interval;
		s_last_host_clock_checks = s_host_clock_checks;
		s_last_host_poll_calls = s_host_poll_calls;
		s_last_log = elapsed;
		s_last_ee_cycle = cpuRegs.cycle;
			// Compilation 59: the flexible memory left (threads' stacks, system modules and shared memory come from it; malloc
			// comes from the system pool, heap_high_water_mb): threads that could not start on 2026-10-19 point to it running out.
			{
				char heap_line[256];
				while (ps4rt_heap_log_next(heap_line, sizeof(heap_line)))
					ProbeLog("PS4 runtime: %s", heap_line);
			}
			const u64 flexible_free = GetAvailablePhysicalMemory();
			{
				// Forja 1.06: the session's summary (SessionStats.h).
				unsigned underruns = 0;
				unsigned long long limited = 0;
				PS4Audio::Totals(underruns, limited);
				ForjaSession::AddHeartbeat(flexible_free, ps4rt_heap_high_water(), underruns, limited, s_video_active);
				// Compilation 130: how long the emulator stood waiting for the console's disc in these 5 s, and the reads "Disco sin
				// esperas" let come back later.
				{
					static u64 s_disc_wait_seen = 0;
					static u32 s_disc_defer_seen = 0;
					const u64 waited = g_forja_disc_wait_us.load(std::memory_order_relaxed);
					const u32 deferred = g_forja_disc_deferrals.load(std::memory_order_relaxed);
					if (waited - s_disc_wait_seen >= 1000 || deferred != s_disc_defer_seen)
						ProbeLog("Disc waits: stood_still_ms=%.0f reads_deferred=%u (5 s; Disco sin esperas %s)\n",
							(waited - s_disc_wait_seen) / 1000.0, deferred - s_disc_defer_seen, s_disc_no_wait_game ? "on" : "off");
					s_disc_wait_seen = waited;
					s_disc_defer_seen = deferred;
				}
#if defined(PCSX2_PS4_VM_BOOT_JIT)
				if (s_code_ahead_game) // compilation 132
				{
					// Compilation 133: also every 30 s with nothing made, with whether the game was seen started and how many calls looked at
					// the list: in the 132 this line never came out, and nothing said why (the entry point's block, gone after a load).
					static u32 s_ahead_seen = 0, s_ahead_beats = 0;
					const u32 ahead = g_forja_code_ahead_done.load(std::memory_order_relaxed);
					if (ahead != s_ahead_seen || ++s_ahead_beats % 6 == 0)
						ProbeLog("Code ahead: known=%u made_ahead=%u (%u in 5 s) left=%u started=%u passes=%u caught_up=%u\n",
							g_forja_code_ahead_known.load(std::memory_order_relaxed), ahead, ahead - s_ahead_seen,
							g_forja_code_ahead_left.load(std::memory_order_relaxed), g_forja_code_ahead_started.load(std::memory_order_relaxed),
							g_forja_code_ahead_passes.load(std::memory_order_relaxed), g_forja_code_ahead_caught.load(std::memory_order_relaxed));
					s_ahead_seen = ahead;
				}
#endif
				if (vm_state == VMState::Running && ++s_session_beats % 12 == 0)
					ForjaSessionWrite(false);
			}
			ProbeLog("VM heartbeat: seconds=%.1f EE_PC=%08x IOP_PC=%08x EE_cycles=%u EE_Mcycles_per_second=%.2f "
				 "Host_polls_per_second=%.0f Host_clock_checks_per_second=%.0f CPU_queue_drain_locks=%llu verbose=%d paused=%d menu=%d vsyncq=%d "
				 "flexible_free_mb=%llu flexible_free_kb=%llu heap_high_water_mb=%zu\n",
			elapsed, cpuRegs.pc, psxRegs.pc,
			cpuRegs.cycle, ee_mcycles, host_polls_per_second, host_clock_checks_per_second,
			static_cast<unsigned long long>(VMHostGetCPUQueueLockCount()), s_verbose_diag ? 1 : 0,
			vm_state == VMState::Paused ? 1 : 0, s_game_menu_open ? 1 : 0, MTGS::GetCurrentVsyncQueueSize(),
			static_cast<unsigned long long>(flexible_free / (1024 * 1024)), static_cast<unsigned long long>(flexible_free / 1024),
			ps4rt_heap_high_water() >> 20);
		// Live telemetry heartbeat (PC debug server). Needs proper testing.
		{
			const double boot_s = PS4Telemetry::GetBootSeconds();
			const char* game = s_selected_game.empty() ? "" : s_selected_game.c_str();
			PS4Telemetry::Heartbeat(boot_s, elapsed, cpuRegs.pc, psxRegs.pc,
				ee_mcycles, host_polls_per_second, host_clock_checks_per_second,
				static_cast<int>(vm_state), s_game_menu_open, MTGS::GetCurrentVsyncQueueSize(), game);
		}
		if (s_verbose_diag && g_gs_device)
		{
			if (auto* vkdev = dynamic_cast<GSDeviceVK*>(g_gs_device.get()))
				ProbeLog("GS verbose: completed_fence=%llu current_fence=%llu deferred_flip=%d\n",
					(unsigned long long)vkdev->GetCompletedFenceCounter(), (unsigned long long)vkdev->GetCurrentFenceCounter(),
					vkdev->PS4HasDeferredFlip() ? 1 : 0);
		}
		PS4EEHotspots::LogInterval(interval);
		{
			// Forja SX2 (compilation 122): how long the EE waits for the VU1, writes into its ring, and how long the VU1 has no work, as a share
			// of the interval's time. MTVU.cpp counts the three in TSC ticks (eerec-281) and nothing wrote them. God of War II (2026-11-07,
			// the 121's sampler): the EE and the VU1 each waiting ~35 % and the GS waiting for the VU1 20 %: is the brake that they wait for
			// each other? This says how much of the EE's wait is for the VU1 (the VU1's idle time is counted when it wakes up, so an idle
			// spell across two intervals lands in the second).
			extern unsigned long long g_orbis_vu_idle_ticks, g_orbis_ee_waitvu_ticks, g_orbis_ee_vuring_ticks;
			static unsigned long long s_last_tsc = 0, s_last_waitvu = 0, s_last_vu_idle = 0, s_last_vuring = 0;
			const unsigned long long tsc = __builtin_ia32_rdtsc();
			const unsigned long long waitvu = g_orbis_ee_waitvu_ticks, vu_idle = g_orbis_vu_idle_ticks, vuring = g_orbis_ee_vuring_ticks;
			if (s_last_tsc != 0 && tsc > s_last_tsc && vm_state == VMState::Running)
			{
				const double span = static_cast<double>(tsc - s_last_tsc);
				ProbeLog("EE-VU1 waits: EE_waits_VU1_pct=%.1f EE_writes_ring_pct=%.1f VU1_idle_pct=%.1f vu_thread=%d\n",
					100.0 * static_cast<double>(waitvu - s_last_waitvu) / span, 100.0 * static_cast<double>(vuring - s_last_vuring) / span,
					100.0 * static_cast<double>(vu_idle - s_last_vu_idle) / span, EmuConfig.Speedhacks.vuThread ? 1 : 0);
			}
			s_last_tsc = tsc;
			s_last_waitvu = waitvu;
			s_last_vu_idle = vu_idle;
			s_last_vuring = vuring;
		}
		{
			// Forja SX2 (compilation 123, Joseph: "ataca los hilos, los núcleos"): the threads besides the EE, the VU1 and the GS. The audio
			// mix thread's CPU time (ThreadCPUMetricsPS4, sampled by the thread itself every 100 ms) and the core plan; the GS recorder writes
			// its own line ("GS recorder: ... worker_busy_ms").
			static u64 s_last_audio_us = 0;
			const u64 audio_us = PS4ThreadCPUMetrics::Read(PS4ThreadCPUMetrics::Role::Audio);
			if (s_last_audio_us != 0 && audio_us >= s_last_audio_us && vm_state == VMState::Running)
				ProbeLog("Threads: audio_mix_pct=%.1f plan=\"%s\"\n", 100.0 * static_cast<double>(audio_us - s_last_audio_us) / (interval * 1000000.0),
					s_profile.cores.empty() ? "global" : s_profile.cores.c_str());
			s_last_audio_us = audio_us;
		}
		{
			// 151 in the app, "VIF1 en menos turnos": the VIF1 steps joined into the scan (EE thread, like this heartbeat).
			static u64 s_last_vif1_fused = 0, s_last_not[5] = {};
			const s32 limit = g_forja_vif1_fuse.load(std::memory_order_relaxed);
			if (limit > 4 && interval > 0.0 && vm_state == VMState::Running)
			{
				double why[5];
				for (int i = 0; i < 5; i++)
					why[i] = static_cast<double>(g_forja_vif1_not_joined[i] - s_last_not[i]) / interval;
				ProbeLog("VIF1 in fewer turns: steps_joined_per_second=%.0f (under %d cycles); not joined per second: over_limit=%.0f "
						 "counter_due=%.0f iop_owed=%.0f run_cap=%.0f waits=%.0f\n",
					static_cast<double>(g_forja_vif1_fused - s_last_vif1_fused) / interval, limit, why[0], why[1], why[2], why[3], why[4]);
			}
			s_last_vif1_fused = g_forja_vif1_fused;
			std::copy(std::begin(g_forja_vif1_not_joined), std::end(g_forja_vif1_not_joined), s_last_not);
			// 152 in the app, the VU1's map: a dump at the 12th and the 24th heartbeat of play (~60 and ~120 s).
			if (s_vu1_map_wanted && vm_state == VMState::Running && PerformanceMetrics::GetInternalFPS() >= 5.0f)
			{
				s_vu1_map_beats++;
				if (s_vu1_map_beats == 12 || s_vu1_map_beats == 24)
				{
					g_orbis_vu1_dump_request.store(1, std::memory_order_relaxed);
					ProbeLog("VU1 map: dump requested after %u heartbeats of play\n", s_vu1_map_beats);
				}
			}
			// 152 in the app, "Ahorro del VU1": the microcode uploads and the ones kept (their bytes already there).
			static u64 s_last_mpg = 0, s_last_mpg_same = 0;
			const u64 mpg = g_forja_mpg_total, mpg_same = g_forja_mpg_same; // VU thread's counters (read whole, 64-bit)
			if (g_forja_vu1_ahorro.load(std::memory_order_relaxed) && interval > 0.0 && vm_state == VMState::Running)
				ProbeLog("VU1 savings: microcode_uploads_per_second=%.0f kept_same=%.0f\n", static_cast<double>(mpg - s_last_mpg) / interval,
					static_cast<double>(mpg_same - s_last_mpg_same) / interval);
			s_last_mpg = mpg;
			s_last_mpg_same = mpg_same;
			// 157 in the app, "Registros entre bloques": blocks compiled for a handed-on map and branches that handed registers on, so far.
			// 160 in the app, "Banderas muertas seguras": programs that keep a flag at their jumps, and readers the scan missed, so far.
			if (g_forja_vu1_dead_flags.load(std::memory_order_relaxed) && vm_state == VMState::Running)
				ProbeLog("VU1 dead flags: programs_keeping_flags=%llu missed_readers=%llu\n",
					static_cast<unsigned long long>(g_forja_vu1_flag_risk_progs.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(g_forja_vu1_flag_missed.load(std::memory_order_relaxed)));
			if (g_forja_vu1_keep_regs.load(std::memory_order_relaxed) && vm_state == VMState::Running)
				ProbeLog("VU1 registers between blocks: kept_blocks=%llu handing_branches=%llu\n",
					static_cast<unsigned long long>(g_forja_vu1_kept_blocks.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(g_forja_vu1_kept_links.load(std::memory_order_relaxed)));
			// 158 in the app: the copies "Tres operandos" left out, and the VU memory addresses that loaded the base or reused it, so far.
			if ((g_forja_vu1_three_op.load(std::memory_order_relaxed) || g_forja_vu1_mem_reg.load(std::memory_order_relaxed) ||
				    g_forja_vu1_mac_just.load(std::memory_order_relaxed)) &&
				vm_state == VMState::Running)
				ProbeLog("VU1 compile savings: copies_left_out=%llu mem_base_loads=%llu mem_base_reuses=%llu mac_flags_left_out=%llu\n",
					static_cast<unsigned long long>(g_forja_vu1_three_op_copies.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(g_forja_vu1_mem_reg_loads.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(g_forja_vu1_mem_reg_reuses.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(g_forja_vu1_mac_left_out.load(std::memory_order_relaxed)));
			// 167 in the app, "VIF en tramos": its mode (0 off, 1 checking, 2 on), the runs sent so far and what they carried, and the
			// check (UNPACKs predicted right, wrong; the first wrong one: prediction / what the normal path sent).
			{
				const int tm = g_forja_vif_tramos_mode.load(std::memory_order_relaxed);
				const u64 wrong = g_forja_vif_tramos_wrong.load(std::memory_order_acquire);
				if ((tm != 0 || wrong != 0) && vm_state == VMState::Running)
				{
					char first[200] = "";
					if (wrong)
					{
						const u32(&w)[2][10] = g_forja_vif_tramos_first_wrong;
						snprintf(first, sizeof(first),
							" first_wrong=[same_data=%u addr=%x/%x size=%u/%u cmd=%x/%x usn=%u/%u aligned=%u/%u num=%u/%u cycle=%x/%x mode=%u/%u mask=%x/%x]",
							w[1][0], w[0][1], w[1][1], w[0][2], w[1][2], w[0][3], w[1][3], w[0][4], w[1][4], w[0][5], w[1][5], w[0][6], w[1][6],
							w[0][7], w[1][7], w[0][8], w[1][8], w[0][9], w[1][9]);
					}
					ProbeLog("VIF in runs: mode=%d runs=%llu unpacks=%llu words=%llu checked=%llu wrong=%llu%s\n", tm,
						static_cast<unsigned long long>(g_forja_vif_tramos_runs.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(g_forja_vif_tramos_unpacks.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(g_forja_vif_tramos_words.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(g_forja_vif_tramos_checked.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(wrong), first);
				}
			}
			// 169 in the app, "Cola corta del VU1": the lap that ran these 5 s, then (VU1ColaAlterna) the next one: 16384, 2048, 1024,
			// 512 KB, 20 s each (4 meters), so each size gets turns in the same scenes.
			// 170 in the app: the VIF1 DMA tags read per second, per kind (which ones a game's chains use, and how many steps they make).
			if (g_forja_vif_adelanto.load(std::memory_order_relaxed) && vm_state == VMState::Running && interval > 0.0)
			{
				static u64 s_tags[8] = {};
				double r[8];
				for (int i = 0; i < 8; i++)
				{
					const u64 now = g_orbis_vif1_tag_ids[i];
					r[i] = static_cast<double>(now - s_tags[i]) / interval;
					s_tags[i] = now;
				}
				ProbeLog("VIF1 tags per second: refe=%.0f cnt=%.0f next=%.0f ref=%.0f refs=%.0f call=%.0f ret=%.0f end=%.0f\n", r[0], r[1], r[2],
					r[3], r[4], r[5], r[6], r[7]);
			}
			// 180 in the app, "A/B en un dibujo" (counting only): draws a second, the palette cuts a run of two textures could skip, and
			// the draws there would be with them joined (written whenever something was joinable).
			// 181: draws a second from every flush (the 180 took a counter the 139 meter resets), the scissor-only joins, why the other
			// cuts can't join, runs broken by another flush, and the runs' lengths.
			if (vm_state == VMState::Running && interval > 0.0)
			{
				static u64 s_ab_draws = 0, s_ab_join = 0, s_ab_runs = 0, s_ab_sc = 0, s_ab_broken = 0, s_ab_block[12] = {}, s_ab_len[6] = {};
				const u64 draws = g_forja_draws_total, join = g_forja_ab_joinable, runs = g_forja_ab_runs, sc = g_forja_ab_joinable_sc;
				const double d = static_cast<double>(draws - s_ab_draws) / interval, j = static_cast<double>(join - s_ab_join) / interval;
				const double js = static_cast<double>(sc - s_ab_sc) / interval;
				if (join != s_ab_join || sc != s_ab_sc)
				{
					double bl[12], ln[6];
					for (int k = 0; k < 12; k++)
					{
						const u64 v = g_forja_ab_block[k];
						bl[k] = static_cast<double>(v - s_ab_block[k]) / interval;
						s_ab_block[k] = v;
					}
					for (int k = 0; k < 6; k++)
					{
						const u64 v = g_forja_ab_len[k];
						ln[k] = static_cast<double>(v - s_ab_len[k]) / interval;
						s_ab_len[k] = v;
					}
					ProbeLog("GS A/B joins (182): draws_per_second=%.0f joinable_per_second=%.0f scissor_only_per_second=%.0f draws_after_exact=%.0f "
							 "draws_after_with_scissor=%.0f draws_after_with_tex1=%.0f (%.2fx fewer) runs_per_second=%.0f broken_per_second=%.0f\n",
						d, j, js, d - j, d - j - js, d - j - js - bl[7], (d - j - js - bl[7]) > 0.0 ? d / (d - j - js - bl[7]) : 0.0,
						static_cast<double>(runs - s_ab_runs) / interval, static_cast<double>(g_forja_ab_broken - s_ab_broken) / interval);
					ProbeLog("GS A/B blocks (182): joinable=%.0f not_other_context=%.0f format=%.0f frame_or_zbuf=%.0f test=%.0f alpha_or_fba=%.0f clamp=%.0f "
							 "tex1_only=%.0f xyoffset=%.0f third_texture=%.0f scissor_only=%.0f; runs of 1/2/3-4/5-8/9-16/17+ draws a second: %.0f %.0f %.0f %.0f %.0f %.0f\n",
						bl[0], bl[1], bl[2], bl[3], bl[4], bl[5], bl[6], bl[7], bl[8], bl[9], bl[11], ln[0], ln[1], ln[2], ln[3], ln[4], ln[5]);
				}
				s_ab_draws = draws;
				s_ab_join = join;
				s_ab_runs = runs;
				s_ab_sc = sc;
				s_ab_broken = g_forja_ab_broken;
			}
			// 178 in the app, "Detalle de los cortes": every 30 s (6 meters), the next 12 palette cuts.
			if (s_gs_cut_detail && vm_state == VMState::Running)
			{
				static unsigned s_cut_meters = 0;
				if (++s_cut_meters % 6 == 0)
					g_forja_pal_detail.store(12, std::memory_order_relaxed);
			}
			// 175 in the app, "VIF sin copia": its mode, the unpacks read from the game's RAM, and the check (the first different: bytes, where).
			{
				const int nm = g_forja_vif_nocopy_mode.load(std::memory_order_relaxed);
				const u64 nwrong = g_forja_vif_nocopy_wrong.load(std::memory_order_acquire);
				if ((nm != 0 || nwrong != 0) && vm_state == VMState::Running)
				{
					// 176 in the app: the pages it reads without the copy, the per-frame ones, and the bytes each way so far.
					u32 safe_pages = 0, dynamic_pages = 0;
					for (u32 p = 0; p < (Ps2MemSize::MainRam >> 12); p++)
					{
						const bool dyn = g_forja_vif_page_dynamic[p].load(std::memory_order_relaxed) != 0;
						dynamic_pages += dyn ? 1u : 0u;
						safe_pages += (!dyn && g_forja_vif_page_good[p].load(std::memory_order_relaxed) >= 256) ? 1u : 0u;
					}
					const u64 bram = g_forja_vif_nocopy_bytes_ram.load(std::memory_order_relaxed);
					const u64 bcopy = g_forja_vif_nocopy_bytes_copied.load(std::memory_order_relaxed);
					ProbeLog("VIF without copy: mode=%d from_ram=%llu checked=%llu wrong=%llu late_wrong=%llu safe_pages=%u per_frame_pages=%u "
							 "bytes_from_ram_pct=%.1f first_wrong=[bytes=%u at=%u]\n",
						nm, static_cast<unsigned long long>(g_forja_vif_nocopy_ptr.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(g_forja_vif_nocopy_checked.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(nwrong), static_cast<unsigned long long>(g_forja_vif_nocopy_late.load(std::memory_order_relaxed)),
						safe_pages, dynamic_pages, (bram + bcopy) ? 100.0 * static_cast<double>(bram) / static_cast<double>(bram + bcopy) : 0.0,
						g_forja_vif_nocopy_first_wrong[0], g_forja_vif_nocopy_first_wrong[1]);
				}
			}
			// 174 in the app, "Animación a medias": the vsyncs it skipped the decoding, the word's changes, and the vsyncs the word held
			// something else (not this program, or still loading).
			if (s_anim_half_on && vm_state == VMState::Running)
				ProbeLog("Animation at half rate: skipped_vsyncs=%llu swaps=%llu foreign=%llu\n", static_cast<unsigned long long>(s_anim_half_skipped),
					static_cast<unsigned long long>(s_anim_half_swaps), static_cast<unsigned long long>(s_anim_half_foreign));
			// 183 in the app, "A/B en un dibujo": switches between the slots, joined draws flushed, those sent as one draw (and their
			// pieces), those drawn back piece by piece, why (a pass that couldn't be kept, the configurations differing), and where the
			// first differences were (bytes of GSHWDrawConfig; 0xFFFF000x = LOD both ways, filters, too many vertices, index counts).
			{
				static u64 s_ab[8] = {};
				const u64 now[8] = {g_forja_ab_switches, g_forja_ab_flushes, g_forja_ab_one_draws, g_forja_ab_one_segs, g_forja_ab_replays,
					g_forja_ab_replay_segs, g_forja_ab_capture_fail, g_forja_ab_mismatch};
				const int abm = g_forja_gs_ab.load(std::memory_order_relaxed);
				if (abm != 0 && vm_state == VMState::Running && interval > 0.0)
				{
					double d[8];
					for (int k = 0; k < 8; k++)
						d[k] = static_cast<double>(now[k] - s_ab[k]) / interval;
					ProbeLog("GS A/B draw (183): mode=%d switches_per_second=%.0f joined_per_second=%.0f one_draw_per_second=%.0f (pieces %.0f) "
						"piece_by_piece_per_second=%.0f (pieces %.0f) capture_fail=%.0f mismatch=%.0f first_diffs=%x,%x,%x,%x,%x,%x,%x,%x\n",
						abm, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], g_forja_ab_diff_offset[0], g_forja_ab_diff_offset[1],
						g_forja_ab_diff_offset[2], g_forja_ab_diff_offset[3], g_forja_ab_diff_offset[4], g_forja_ab_diff_offset[5],
						g_forja_ab_diff_offset[6], g_forja_ab_diff_offset[7]);
					// 185: why the switches weren't taken so far (nothing pending, another context, a global register, PRIM, TEX0,
					// palette, shared registers, the slot's registers).
					ProbeLog("GS A/B switches refused (185): pending=%llu context=%llu global=%llu prim=%llu tex0=%llu palette=%llu shared=%llu slot=%llu\n",
						static_cast<unsigned long long>(g_forja_ab_switch_fail[0]), static_cast<unsigned long long>(g_forja_ab_switch_fail[1]),
						static_cast<unsigned long long>(g_forja_ab_switch_fail[2]), static_cast<unsigned long long>(g_forja_ab_switch_fail[3]),
						static_cast<unsigned long long>(g_forja_ab_switch_fail[4]), static_cast<unsigned long long>(g_forja_ab_switch_fail[5]),
						static_cast<unsigned long long>(g_forja_ab_switch_fail[6]), static_cast<unsigned long long>(g_forja_ab_switch_fail[7]));
				}
				for (int k = 0; k < 8; k++)
					s_ab[k] = now[k];
			}
			// 171 in the app, "Vértices una vez": its mode, the draws read once and the vertex reads left out so far, and the check.
			{
				const int vo = g_forja_gs_vt_once.load(std::memory_order_relaxed);
				const u64 vwrong = g_forja_gs_vt_once_wrong.load(std::memory_order_acquire);
				if ((vo != 0 || vwrong != 0) && vm_state == VMState::Running)
					ProbeLog("GS vertices once: mode=%d draws=%llu reads_left_out=%llu checked=%llu wrong=%llu first_wrong=[indices=%u vertices=%u part=%u prim=%u]\n",
						vo, static_cast<unsigned long long>(g_forja_gs_vt_once_draws.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(g_forja_gs_vt_once_saved.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(g_forja_gs_vt_once_checked.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(vwrong), g_forja_gs_vt_once_first_wrong[0], g_forja_gs_vt_once_first_wrong[1],
						g_forja_gs_vt_once_first_wrong[2], g_forja_gs_vt_once_first_wrong[3]);
			}
			if (s_vu1_ring_logged && vm_state == VMState::Running)
			{
				ProbeLog("VU1 ring lap: kb=%u\n", g_orbis_mtvu_ring_kb.load(std::memory_order_relaxed));
				if (s_vu1_ring_alternate)
				{
					static const u32 kLaps[4] = {16384, 2048, 1024, 512};
					static unsigned s_ring_meters = 0;
					s_ring_meters++;
					g_orbis_mtvu_ring_kb.store(kLaps[(s_ring_meters / 4) % 4], std::memory_order_relaxed);
				}
			}
		}
		if (OrbisEEDiag::on.load(std::memory_order_relaxed) && interval > 0.0)
		{
			// 150 in the app (/data/ForjaSX2/vm-eediag): God of War II's heavy scenes (the 148, 2026-11-19) need the EE ~20 ms a frame and
			// a quarter of it is handing work to the VU1 (VIF1 DMA, ~2,100 VU1 programs a frame). Before taking that off the EE: which VIF
			// codes each game sends (the ones that need the EE's own state: MARK, FLUSH*, DIRECT*, the interrupt bit), which interrupts it
			// takes (the GS and VU1 ones handed over by the VU thread) and which system calls it makes.
			static u64 s_vif[128] = {}, s_vif_irq = 0, s_intc[16] = {}, s_dmac[16] = {}, s_sys[256] = {}, s_cpu_int[32] = {};
			static u64 s_mtvu[4] = {}, s_ev[5] = {};
			static bool s_diag_first = true;
			const auto rate = [interval](u64 now, u64 before) { return now >= before ? static_cast<double>(now - before) / interval : 0.0; };
			if (!s_diag_first)
			{
				double v[128];
				for (int i = 0; i < 128; i++)
					v[i] = rate(OrbisEEDiag::vif1_codes[i], s_vif[i]);
				double unpack = 0.0, known = 0.0;
				for (int i = 0x60; i < 0x80; i++)
					unpack += v[i];
				static constexpr int named[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17, 0x20, 0x30,
					0x31, 0x4a, 0x50, 0x51};
				for (const int i : named)
					known += v[i];
				double all = 0.0;
				for (int i = 0; i < 128; i++)
					all += v[i];
				ProbeLog("EE diag (150): vif1_codes_per_second unpack=%.0f mscal=%.0f mscalf=%.0f mscnt=%.0f mpg=%.0f direct=%.0f directhl=%.0f "
						 "flush=%.0f flusha=%.0f flushe=%.0f mark=%.0f mskpath3=%.0f stmask=%.0f strow=%.0f stcol=%.0f stcycl=%.0f stmod=%.0f "
						 "base=%.0f offset=%.0f itop=%.0f nop=%.0f other=%.0f with_irq_bit=%.0f\n",
					unpack, v[0x14], v[0x15], v[0x17], v[0x4a], v[0x50], v[0x51], v[0x11], v[0x13], v[0x10], v[0x07], v[0x06], v[0x20], v[0x30],
					v[0x31], v[0x01], v[0x05], v[0x03], v[0x02], v[0x04], v[0x00], all - unpack - known, rate(OrbisEEDiag::vif1_irq_codes, s_vif_irq));
				double i_[16], d_[16];
				for (int i = 0; i < 16; i++)
				{
					i_[i] = rate(OrbisEEDiag::intc_irq[i], s_intc[i]);
					d_[i] = rate(OrbisEEDiag::dmac_irq[i], s_dmac[i]);
				}
				ProbeLog("EE diag (150): intc_per_second gs=%.0f sbus=%.0f vblank_s=%.0f vblank_e=%.0f vif0=%.0f vif1=%.0f vu0=%.0f vu1=%.0f ipu=%.0f "
						 "tim0=%.0f tim1=%.0f tim2=%.0f tim3=%.0f sfifo=%.0f vu0wd=%.0f; from_vu1_thread finish=%.0f signal=%.0f label=%.0f tbit=%.0f; "
						 "dmac_per_second vif0=%.0f vif1=%.0f gif=%.0f ipu_from=%.0f ipu_to=%.0f sif0=%.0f sif1=%.0f sif2=%.0f spr_from=%.0f spr_to=%.0f\n",
					i_[0], i_[1], i_[2], i_[3], i_[4], i_[5], i_[6], i_[7], i_[8], i_[9], i_[10], i_[11], i_[12], i_[13], i_[14],
					rate(OrbisEEDiag::mtvu_finish, s_mtvu[0]), rate(OrbisEEDiag::mtvu_signal, s_mtvu[1]), rate(OrbisEEDiag::mtvu_label, s_mtvu[2]),
					rate(OrbisEEDiag::mtvu_tbit, s_mtvu[3]), d_[0], d_[1], d_[2], d_[3], d_[4], d_[5], d_[6], d_[7], d_[8], d_[9]);
				// The 10 system calls made most often, and the 8 kinds of event scheduled most often.
				std::pair<double, int> top[256];
				int count = 0;
				for (int call = 0; call < 256; call++)
				{
					const double r = rate(OrbisEEDiag::syscalls[call], s_sys[call]);
					if (r > 0.0)
						top[count++] = {r, call};
				}
				std::sort(top, top + count, [](const std::pair<double, int>& a, const std::pair<double, int>& b) { return a.first > b.first; });
				count = std::min(count, 10);
				char calls[256] = {};
				size_t used = 0;
				for (int i = 0; i < count && used + 24 < sizeof(calls); i++)
					used += std::snprintf(calls + used, sizeof(calls) - used, "%s%02x=%.0f", i ? "," : "", top[i].second, top[i].first);
				char events[256] = {};
				used = 0;
				for (int n = 0; n < 32 && used + 24 < sizeof(events); n++)
				{
					const double r = rate(OrbisEEDiag::cpu_int[n], s_cpu_int[n]);
					if (r >= 1.0)
						used += std::snprintf(events + used, sizeof(events) - used, "%s%d=%.0f", used ? "," : "", n, r);
				}
				ProbeLog("EE diag (150): syscalls_per_second %s; events_per_second %s; event_tests_with_dma=%.0f counters=%.0f iop_rapid=%.0f "
						 "intc_taken=%.0f dmac_taken=%.0f (per second)\n",
					count ? calls : "none", used ? events : "none", rate(OrbisEEDiag::ev_dma, s_ev[0]), rate(OrbisEEDiag::rcnt, s_ev[1]),
					rate(OrbisEEDiag::iop_rapid, s_ev[2]), rate(OrbisEEDiag::exc_intc, s_ev[3]), rate(OrbisEEDiag::exc_dmac, s_ev[4]));
			}
			s_diag_first = false;
			std::copy(std::begin(OrbisEEDiag::vif1_codes), std::end(OrbisEEDiag::vif1_codes), s_vif);
			std::copy(std::begin(OrbisEEDiag::intc_irq), std::end(OrbisEEDiag::intc_irq), s_intc);
			std::copy(std::begin(OrbisEEDiag::dmac_irq), std::end(OrbisEEDiag::dmac_irq), s_dmac);
			std::copy(std::begin(OrbisEEDiag::syscalls), std::end(OrbisEEDiag::syscalls), s_sys);
			std::copy(std::begin(OrbisEEDiag::cpu_int), std::end(OrbisEEDiag::cpu_int), s_cpu_int);
			s_vif_irq = OrbisEEDiag::vif1_irq_codes;
			s_mtvu[0] = OrbisEEDiag::mtvu_finish;
			s_mtvu[1] = OrbisEEDiag::mtvu_signal;
			s_mtvu[2] = OrbisEEDiag::mtvu_label;
			s_mtvu[3] = OrbisEEDiag::mtvu_tbit;
			s_ev[0] = OrbisEEDiag::ev_dma;
			s_ev[1] = OrbisEEDiag::rcnt;
			s_ev[2] = OrbisEEDiag::iop_rapid;
			s_ev[3] = OrbisEEDiag::exc_intc;
			s_ev[4] = OrbisEEDiag::exc_dmac;
		}
		{
			// Forja SX2: the SPU2 mixing cost measured in IopCounters.cpp (EE thread, like this heartbeat).
			extern u64 g_forja_spu2_calls, g_forja_spu2_sampled_ticks;
			const double spu2_ms = Common::Timer::ConvertValueToMilliseconds(g_forja_spu2_sampled_ticks);
			ProbeLog("Audio SPU2: ms_per_second=%.1f share_of_EE_thread_pct=%.1f calls_per_second=%.0f\n",
				spu2_ms / interval, spu2_ms / interval / 10.0, g_forja_spu2_calls / interval);
			g_forja_spu2_calls = 0;
			g_forja_spu2_sampled_ticks = 0;
			// The split of one sample in 16 (pcsx2/SPU2/Mixer.cpp): voices, the cores' mix and reverb, the output and its stretching.
			extern u64 g_forja_spu2_split_voices, g_forja_spu2_split_mix, g_forja_spu2_split_output;
			extern u64 g_forja_spu2_split_samples, g_forja_spu2_split_active, g_forja_spu2_split_irq;
			if (g_forja_spu2_split_samples > 0)
			{
				const double n = static_cast<double>(g_forja_spu2_split_samples);
				extern u64 g_forja_spu2_deferred_samples, g_forja_spu2_catch_ups; // Mixer.cpp: stopped voices in batches
				extern u64 g_forja_spu2_split_fx_on, g_forja_spu2_split_fx_heard, g_forja_spu2_split_fx_area; // the reverb in use?
				ProbeLog("Audio SPU2 split: voices_ms=%.1f mix_reverb_ms=%.1f output_stretch_ms=%.1f (per second) active_voices=%.1f of 48 irq_cores=%.2f "
						 "deferred_voice_samples_per_second=%.0f catch_ups_per_second=%.0f reverb_cores: area=%.2f on=%.2f heard=%.2f\n",
					Common::Timer::ConvertValueToMilliseconds(g_forja_spu2_split_voices) * 16.0 / interval,
					Common::Timer::ConvertValueToMilliseconds(g_forja_spu2_split_mix) * 16.0 / interval,
					Common::Timer::ConvertValueToMilliseconds(g_forja_spu2_split_output) * 16.0 / interval,
					g_forja_spu2_split_active / n, g_forja_spu2_split_irq / n, g_forja_spu2_deferred_samples / interval,
					g_forja_spu2_catch_ups / interval, g_forja_spu2_split_fx_area / n, g_forja_spu2_split_fx_on / n,
					g_forja_spu2_split_fx_heard / n);
				g_forja_spu2_deferred_samples = g_forja_spu2_catch_ups = 0;
				g_forja_spu2_split_fx_on = g_forja_spu2_split_fx_heard = g_forja_spu2_split_fx_area = 0;
				// pcsx2/SPU2/spu2.cpp: the EE thread waiting for the audio write thread (a full ring).
				extern u64 g_forja_audio_wait_ticks, g_forja_audio_waits, g_forja_audio_chunks;
				ProbeLog("Audio write thread: chunks_per_second=%.0f waits_per_second=%.1f EE_wait_ms_per_second=%.1f\n",
					g_forja_audio_chunks / interval, g_forja_audio_waits / interval,
					Common::Timer::ConvertValueToMilliseconds(g_forja_audio_wait_ticks) / interval);
				g_forja_audio_wait_ticks = g_forja_audio_waits = g_forja_audio_chunks = 0;
				// pcsx2/IPU/IPU.cpp: the EE polling the IPU (the shortcut moves the clock on while it is busy; vm-ipu-exacto turns it off).
				extern u64 g_forja_ipu_polls, g_forja_ipu_busy_polls, g_forja_ipu_spin_polls;
				ProbeLog("IPU polls: per_second=%.0f busy_per_second=%.0f spin_per_second=%.0f shortcut=%d\n", g_forja_ipu_polls / interval,
					g_forja_ipu_busy_polls / interval, g_forja_ipu_spin_polls / interval,
					access("/data/ForjaSX2/vm-ipu-exacto", F_OK) != 0 ? 1 : 0);
				g_forja_ipu_polls = g_forja_ipu_busy_polls = g_forja_ipu_spin_polls = 0;
			}
			// Compilation 66: the EE's events a second while a movie plays, the IPU's among them (pcsx2/IPU/IPUdma.cpp), and the
			// FIFO fills moved at once by the burst and the feed instead of an event each.
			{
				static u64 s_last_ipu_events[3] = {}, s_last_ipu_fills[2] = {};
				static u64 s_last_iop_sleeps = 0, s_last_iop_runs = 0; // compilation 67
				const u64 iop_sleeps = g_forja_iop_sleeps - s_last_iop_sleeps, iop_runs = g_forja_iop_runs - s_last_iop_runs;
				s_last_iop_sleeps = g_forja_iop_sleeps;
				s_last_iop_runs = g_forja_iop_runs;
				u64 events[3], fills[2];
				for (unsigned i = 0; i < 3; i++)
				{
					events[i] = g_forja_ipu_events[i] - s_last_ipu_events[i];
					s_last_ipu_events[i] = g_forja_ipu_events[i];
				}
				for (unsigned i = 0; i < 2; i++)
				{
					fills[i] = g_forja_ipu_inline_fills[i] - s_last_ipu_fills[i];
					s_last_ipu_fills[i] = g_forja_ipu_inline_fills[i];
				}
				if (s_video_active)
					ProbeLog("Video meter: EE events %.0f a second, the IPU's %.0f (decoder %.0f, toIPU %.0f, fromIPU %.0f); FIFO "
							 "fills moved at once %.0f out, %.0f in (the feed %s); the IOP asleep at %.0f, run at %.0f (%s)\n",
						host_polls_per_second, (events[0] + events[1] + events[2]) / interval, events[0] / interval,
						events[1] / interval, events[2] / interval, fills[0] / interval, fills[1] / interval,
						g_ps4_ipu_feed ? "on" : "off", iop_sleeps / interval, iop_runs / interval,
						g_forja_iop_sleep ? "sleep on" : "sleep off");
				else if (s_iop_sleep_game) // compilation 122
				{
					// Compilation 123: and the reading loops it knows (R3000A.cpp, ForjaNoteIopPollLoop).
					ProbeLog("IOP asleep in the game: asleep at %.0f, run at %.0f a second (%s), reading loops known %u\n", iop_sleeps / interval,
						iop_runs / interval, g_forja_iop_sleep ? "sleep on" : "sleep off", g_forja_iop_poll_loops);
					// Compilation 125: the 123 copied the IOP's code where it waited awake; at such low addresses that is the BIOS's code,
					// which the house rule keeps out of the analysis. The heartbeat's IOP_PC says where it waits, without its code.
				}
			}
			g_forja_spu2_split_voices = g_forja_spu2_split_mix = g_forja_spu2_split_output = 0;
			g_forja_spu2_split_samples = g_forja_spu2_split_active = g_forja_spu2_split_irq = 0;
		}
		// Read on the owning EE thread, once per heartbeat. No MMIO reads or
		// worker calls here: observing a stalled IPU must not advance it.
		ProbeLog("IPU state: ctrl=%08x cmd=%08x index=%d pos=%d,%d,%d,%d,%d,%d "
			"IFC=%u OFC=%u BP=%u FP=%u wait_to=%d wait_from=%d requested=%d "
			"dma_to_qwc=%u dma_to_str=%u dma_from_qwc=%u dma_from_str=%u\n",
			ipuRegs.ctrl._u32, ipu_cmd.current, ipu_cmd.index,
			ipu_cmd.pos[0], ipu_cmd.pos[1], ipu_cmd.pos[2], ipu_cmd.pos[3], ipu_cmd.pos[4], ipu_cmd.pos[5],
			g_BP.IFC, static_cast<u32>(ipuRegs.ctrl.OFC), g_BP.BP, g_BP.FP,
			IPUCoreStatus.WaitingOnIPUTo, IPUCoreStatus.WaitingOnIPUFrom, IPUCoreStatus.DataRequested,
			ipu1ch.qwc, static_cast<u32>(ipu1ch.chcr.STR), ipu0ch.qwc, static_cast<u32>(ipu0ch.chcr.STR));
		PS4Audio::LogStatus();
		PS4ProbeLogger::LogStatus();
		// Do not attribute host logging/control work to the following guest gap.
		PS4EEEventProfile::s_sampling.gap_pending = false;
		PS4EEEventProfile::LogGaps(interval);
#if defined(PCSX2_PS4_VM_BOOT_JIT)
		ProbeLog("EE JIT cumulative: compiles=%u discards=%u page_resets=%u full_resets=%u\n",
			g_orbis_rec_compiles.load(std::memory_order_relaxed), g_orbis_rec_discards.load(std::memory_order_relaxed),
			g_orbis_rec_page_resets.load(std::memory_order_relaxed), g_orbis_rec_full_resets.load(std::memory_order_relaxed));
		ProbeLog("VTLB cached access sites compiled: quad_reads=%llu writes=%llu (not runtime calls)\n",
			static_cast<unsigned long long>(g_ps4_vtlb_cached_quad_reads),
			static_cast<unsigned long long>(g_ps4_vtlb_cached_writes));
		ProbeLog("EE multi-block wait cumulative: blocks=%llu fast_forwards=%llu skipped_iterations=%llu\n",
			static_cast<unsigned long long>(g_ps4_multiblock_wait_blocks),
			static_cast<unsigned long long>(g_ps4_multiblock_wait_runs),
			static_cast<unsigned long long>(g_ps4_multiblock_wait_iterations));
		ProbeLog("EE byte-copy fusion cumulative: blocks=%llu runs=%llu bytes=%llu\n",
			static_cast<unsigned long long>(g_ps4_bytecopy_blocks),
			static_cast<unsigned long long>(g_ps4_bytecopy_runs),
			static_cast<unsigned long long>(g_ps4_bytecopy_bytes));
		ProbeLog("EE byte-copy native fallback: short=%llu deadline=%llu mapping=%llu state=%llu scratch_batches=%llu last_declined_source=%08x last_declined_destination=%08x\n",
			static_cast<unsigned long long>(g_ps4_bytecopy_short),
			static_cast<unsigned long long>(g_ps4_bytecopy_deadline),
			static_cast<unsigned long long>(g_ps4_bytecopy_mapping),
			static_cast<unsigned long long>(g_ps4_bytecopy_state),
			static_cast<unsigned long long>(g_ps4_bytecopy_scratch),
			g_ps4_bytecopy_last_source, g_ps4_bytecopy_last_destination);
#endif
	}
	// (AI-assisted) Even when heartbeat throttled, keep a 1s paused tick visible. Needs proper testing.
	if (s_verbose_diag && (s_game_menu_open || vm_state == VMState::Paused) && elapsed - s_last_pause_log >= 1.0)
	{
		s_last_pause_log = elapsed;
		ProbeLog("VM paused heartbeat: seconds=%.1f EE_PC=%08x IOP_PC=%08x vsyncq=%d\n",
			elapsed, cpuRegs.pc, psxRegs.pc, MTGS::GetCurrentVsyncQueueSize());
		PS4ProbeLogger::LogStatus();
	}
	OrbisPadData buttons{};
	const bool pad_read = s_pad >= 0 && scePadReadState(s_pad, &buttons) == 0 && buttons.connected;
	if (!pad_read)
		buttons = {};
	const u32 raw = buttons.buttons;
	const u32 pressed = raw & ~s_previous_host_buttons;
	s_previous_host_buttons = raw;
	s_blocked_pad_buttons &= raw;
	constexpr u32 menu_chord = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
	// Compilation 53: the touchpad's right half (x from 960 of its 1920) as the menu's button.
	bool touch_edge = false;
	if (s_menu_button != 0 && (pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) != 0)
	{
		s_menu_touch_held = buttons.touch.fingers > 0 && buttons.touch.touch[0].x >= 960;
		touch_edge = s_menu_touch_held;
	}
	else if ((raw & ORBIS_PAD_BUTTON_TOUCH_PAD) == 0)
		s_menu_touch_held = false;
	const bool chord_edge = (s_menu_button != 1 && (raw & menu_chord) == menu_chord && !s_menu_chord_latched) || touch_edge;
	const bool analog_left = pad_read && buttons.leftStick.x < 72;
	const bool analog_right = pad_read && buttons.leftStick.x > 184;
	const bool analog_up = pad_read && buttons.leftStick.y < 72;
	const bool analog_down = pad_read && buttons.leftStick.y > 184;
	if ((raw & menu_chord) == 0)
		s_menu_chord_latched = false;
	else if (chord_edge)
		s_menu_chord_latched = true;

	// (AI-assisted) Keep raw edges separate from guest buttons. Release all guest
	// controls on pause; consume held menu buttons until released on resume.
	const bool menu_was_open = s_game_menu_open;
	if (chord_edge && !s_game_menu_open && vm_state == VMState::Running)
	{
		s_game_menu_open = true;
		s_game_menu_selection = 0;
		s_state_action = PS4GameMenu::STATE_NONE;
		RefreshStateSlot();
		ReloadPatchMenu(); // Forja SX2 (compilation 42): the Parches tab
		ReloadCheats(); // compilation 58
		// Compilation 44: the Extras tab's other rows and Partida's discs.
		ReloadVu1Filter();
		ReloadGsFilter(); // compilation 51
		PublishTexturesText();
		s_recommended_applied = false; // compilation 61
		PublishRecommendedText();
		ReloadDiscList();
		ForjaFramePushEnd("the menu"); // compilation 132: before the game's rate is read
		ForjaCodeAheadStore(false); // compilation 132: the game is paused, a good moment for the disk
		ForjaSkipRemember(); // compilation 139
		ForjaSkipTraceDump(); // compilation 139
		// The game's own rate, not the one a movie (video mode) or the automatic speed lowered it to.
		s_game_ee_rate = s_cpu_auto_steps > 0 ? s_cpu_auto_base : s_video_active ? s_video_saved_rate : EmuConfig.Speedhacks.EECycleRate;
		s_game_menu_direction = 0;
		s_game_menu_next_repeat = elapsed;
	}
	if (!pad_read || s_game_menu_open || vm_state == VMState::Paused)
	{
		buttons = {};
		buttons.leftStick.x = buttons.leftStick.y = 128;
		buttons.rightStick.x = buttons.rightStick.y = 128;
	}
	else
		buttons.buttons &= ~(s_blocked_pad_buttons | (s_menu_touch_held ? static_cast<u32>(ORBIS_PAD_BUTTON_TOUCH_PAD) : 0u));
	VMHostUpdateController(0, buttons, s_previous_pad_buttons);
	// Forja SX2 (compilation 43): the second player, on the PS2's second port (released while the menu is open).
	UpdateSecondPad(elapsed);
	OrbisPadData buttons2{};
	const bool pad2_read = s_pad2 >= 0 && scePadReadState(s_pad2, &buttons2) == 0 && buttons2.connected;
	if (!pad2_read || s_game_menu_open || vm_state == VMState::Paused)
	{
		buttons2 = {};
		buttons2.leftStick.x = buttons2.leftStick.y = 128;
		buttons2.rightStick.x = buttons2.rightStick.y = 128;
	}
	VMHostUpdateController(1, buttons2, s_previous_pad2_buttons);
	if (!menu_was_open && s_game_menu_open)
	{
		// (AI-assisted) Verbose bracket to isolate which pause step hangs. Needs proper testing.
		const auto pause_t0 = Common::Timer::GetCurrentValue();
		ProbeLog("pause chord: raw=%08x pressed=%08x vm_state=%d EE_PC=%08x IOP_PC=%08x vsyncq=%d\n",
			raw, pressed, static_cast<int>(vm_state), cpuRegs.pc, psxRegs.pc, MTGS::GetCurrentVsyncQueueSize());
		ProbeLog("pause step: SetPaused(true) begin\n");
		const auto setpause_t0 = Common::Timer::GetCurrentValue();
		if (s_verbose_diag)
		{
			// Diagnostic: try timed VU+GS drain, log on timeout instead of infinite hang.
			const bool vu_ok = true; // VMManager::SetPaused does vu1Thread.WaitVU internally; we bracket around full SetPaused
			if (!MTGS::TimedWaitGS(0, false, false, false))
			{
				// Probe: GS not idle before pause - this is informational, real drain is inside SetPaused
			}
			VMManager::SetPaused(true);
			(void)vu_ok;
		}
		else
		{
			VMManager::SetPaused(true);
		}
		ProbeLog("pause step: SetPaused done dur_ms=%.2f state=%d\n",
			Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - setpause_t0),
			static_cast<int>(VMManager::GetState()));
		ProbeLog("pause step: FlushPipelineCache queue begin vsyncq=%d\n", MTGS::GetCurrentVsyncQueueSize());
		MTGS::RunOnGSThread([]() {
			ProbeLog("GS: FlushPipelineCache begin\n");
			if (g_vulkan_shader_cache)
				g_vulkan_shader_cache->FlushPipelineCache();
			ProbeLog("GS: FlushPipelineCache end\n");
		});
		ProbeLog("pause step: ReadGameResolution begin\n");
		const auto readres_t0 = Common::Timer::GetCurrentValue();
		ReadGameResolution();
		ProbeLog("pause step: ReadGameResolution done dur_ms=%.2f vsyncq=%d\n",
			Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - readres_t0),
			MTGS::GetCurrentVsyncQueueSize());
		ProbeLog("pause step: PublishGameMenu begin\n");
		PublishGameMenu();
		ProbeLog("Emulator menu: opened with L3+R3 total_ms=%.2f verbose=%d\n",
			Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - pause_t0), s_verbose_diag ? 1 : 0);
		return;
	}
	if (!s_game_menu_open)
		return;

	// Compilation 61: the recommended settings X asked for, used as soon as their download ends (or after 20 s, offline or a
	// slow line: then with the copy already on the console). Here, with the VM paused, as Restablecer.
	if (s_recommended_wait_since > 0.0 && (!ForjaPatches::Busy() || elapsed - s_recommended_wait_since > 20.0))
	{
		ProbeLog("Recommended settings: download %s after %.1f s\n", ForjaPatches::Busy() ? "still running" : "done",
			elapsed - s_recommended_wait_since);
		s_recommended_wait_since = 0.0;
		s_recommended_applied = ApplyRecommendedProfile();
		ReloadPatchMenu();
		ReloadCheats();
		ReloadVu1Filter();
		ReloadGsFilter();
		PublishTexturesText();
		PublishRecommendedText();
		PublishGameMenu();
	}
	// Compilation 77: the report's row follows its sending (ForjaReport.cpp's thread); compilation 86: every second of it.
	{
		static int s_report_shown_seconds = -1;
		const ForjaReport::Status status = ForjaReport::GetStatus();
		const int seconds = status == ForjaReport::Status::Sending ? ForjaReport::SendingSeconds() : -1;
		if (status != s_report_shown || seconds != s_report_shown_seconds)
		{
			s_report_shown = status;
			s_report_shown_seconds = seconds;
			PublishGameMenu();
		}
	}

	// Compilation 86: the PS4's keyboard open over the menu: the menu reads no buttons until it closes.
	{
		std::string written;
		bool done = false;
		if (ForjaKeyboard::Busy(written, done))
		{
			if (done)
			{
				if (s_keyboard_target == 0)
					s_report_comment = written.substr(0, 600); // 200 characters at most, up to 3 bytes each
				else
					SaveNetText(s_keyboard_target, written); // compilation 144
				s_keyboard_target = 0;
				PublishGameMenu();
			}
			return;
		}
	}

	const bool cancel = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0 || chord_edge;
	const bool confirm = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
	const unsigned row = s_game_menu_selection;
	if (cancel || (confirm && (row == PS4GameMenu::ROW_RESUME || row == PS4GameMenu::ROW_LIBRARY || row == PS4GameMenu::ROW_EXIT)))
	{
		s_game_menu_open = false;
		s_blocked_pad_buttons |= raw;
		PS4GameMenu::SetInGameMenu({});
		if (!cancel && row == PS4GameMenu::ROW_LIBRARY)
		{
			s_return_to_main_menu.store(true, std::memory_order_release);
			ProbeLog("VM stop request: return to main menu\n");
			VMManager::SetState(VMState::Stopping);
		}
		else if (!cancel && row == PS4GameMenu::ROW_EXIT)
		{
			ProbeLog("VM stop request: exit application\n");
			VMManager::SetState(VMState::Stopping);
		}
		else
		{
			ProbeLog("Emulator menu: resume\n");
			ForjaFrameGenReadyBeforeResume(); // compilation 137: frame generation's resources with the menu still open
			// Forja SX2: the GS's hitch detector (GSRendererPS4.cpp) does not count the time the menu was open as a hitch.
			g_forja_resumes.fetch_add(1, std::memory_order_relaxed);
			VMManager::SetPaused(false);
		}
	}
	else
	{
		// X changes the switch-like rows one step, saves, loads or restarts on the save state rows, and resets the game's
		// profile on its row.
		if (confirm && (row == PS4GameMenu::ROW_OVERLAY || row == PS4GameMenu::ROW_VIDEO_MODE || row == PS4GameMenu::ROW_FRAME_GEN ||
						   row == PS4GameMenu::ROW_SHARPNESS || row == PS4GameMenu::ROW_DEINTERLACE || row == PS4GameMenu::ROW_TEXTURE_FILTER ||
						   row == PS4GameMenu::ROW_CORES || row == PS4GameMenu::ROW_CPU_AUTO || row == PS4GameMenu::ROW_CYCLE_SKIP || row == PS4GameMenu::ROW_SPEED_LIMIT || row == PS4GameMenu::ROW_RESET_PROFILE ||
						   row == PS4GameMenu::ROW_GS_SKIP || row == PS4GameMenu::ROW_SAFE_MODE || row == PS4GameMenu::ROW_CROWD ||
						   row == PS4GameMenu::ROW_SAVE_STATE || row == PS4GameMenu::ROW_LOAD_STATE || row == PS4GameMenu::ROW_RESTART ||
						   row == PS4GameMenu::ROW_CHEATS ||
						   row == PS4GameMenu::ROW_PATCH || row == PS4GameMenu::ROW_VU1_FILTER || row == PS4GameMenu::ROW_GS_FILTER ||
						   row == PS4GameMenu::ROW_TEXTURES || row == PS4GameMenu::ROW_DRAW_THREAD || row == PS4GameMenu::ROW_SW_JIT ||
						   row == PS4GameMenu::ROW_FPU_EXACT || row == PS4GameMenu::ROW_IOP_FAST || row == PS4GameMenu::ROW_IOP_SLEEP ||
						   row == PS4GameMenu::ROW_CPU_AUTO_UP || row == PS4GameMenu::ROW_FAST_CODE_CHECK || row == PS4GameMenu::ROW_VU1_FAST || row == PS4GameMenu::ROW_EFFECTS_AHEAD || row == PS4GameMenu::ROW_SKIP_REPEATS || row == PS4GameMenu::ROW_CPU_SEAMLESS || row == PS4GameMenu::ROW_DISC_NO_WAIT ||
						   row == PS4GameMenu::ROW_CODE_AHEAD || row == PS4GameMenu::ROW_VU_JUMPS || row == PS4GameMenu::ROW_FRAME_PUSH ||
						   row == PS4GameMenu::ROW_FG_MARGIN ||
						   row == PS4GameMenu::ROW_RECOMMENDED || row == PS4GameMenu::ROW_CHANGE_DISC || row == PS4GameMenu::ROW_REPORT ||
						   row == PS4GameMenu::ROW_REPORT_ISSUE || row == PS4GameMenu::ROW_REPORT_COMMENT || row == PS4GameMenu::ROW_REPORT_WHERE ||
						   row == PS4GameMenu::ROW_NET_DNS || row == PS4GameMenu::ROW_NET_HOST || row == PS4GameMenu::ROW_NET_KEY))
		{
			if (row == PS4GameMenu::ROW_SAVE_STATE)
				SaveGameState();
			else if (row == PS4GameMenu::ROW_NET_DNS || row == PS4GameMenu::ROW_NET_HOST || row == PS4GameMenu::ROW_NET_KEY) // compilation 144
			{
				OrbisUserServiceUserId user = s_pad1_user;
				if (user == ORBIS_USER_SERVICE_USER_ID_INVALID)
					sceUserServiceGetInitialUser(&user);
				s_keyboard_target = row == PS4GameMenu::ROW_NET_DNS ? 1 : row == PS4GameMenu::ROW_NET_HOST ? 2 : 3;
				const char* title = s_keyboard_target == 1 ? ForjaLang::T("DNS del servidor del juego") :
				                    s_keyboard_target == 2 ? ForjaLang::T("IP o dominio del anfitrión de la sala") :
				                                             ForjaLang::T("Clave de la sala");
				const std::string current = s_keyboard_target == 2 ? EmuConfig.DEV9.LanHost : s_keyboard_target == 3 ? EmuConfig.DEV9.LanKey : "";
				if (!ForjaKeyboard::Open(user, title, current))
				{
					s_keyboard_target = 0;
					ProbeLog("Keyboard: could not open it\n");
				}
			}
			else if (row == PS4GameMenu::ROW_REPORT_COMMENT) // compilation 86: the PS4's keyboard
			{
				OrbisUserServiceUserId user = s_pad1_user;
				if (user == ORBIS_USER_SERVICE_USER_ID_INVALID)
					sceUserServiceGetInitialUser(&user);
				if (!ForjaKeyboard::Open(user, ForjaLang::T("Comentario para el reporte"), s_report_comment))
					ProbeLog("Keyboard: could not open it\n");
			}
			// Compilation 77; 82: from "Qué falló" too. Joseph chose what went wrong in Guitar Hero (2026-10-24) and pressed X there,
			// on the row below the one that sent, and nothing went. Compilation 98: from "Dónde se ve", the last one, too (the same, 2026-10-30).
			else if (row == PS4GameMenu::ROW_REPORT || row == PS4GameMenu::ROW_REPORT_ISSUE || row == PS4GameMenu::ROW_REPORT_WHERE)
				SendReport(elapsed);
			else if (row == PS4GameMenu::ROW_LOAD_STATE)
			{
				if (LoadGameState())
				{
					ResumeFromGameMenu(raw);
					return;
				}
			}
			else if (row == PS4GameMenu::ROW_RESTART)
			{
				RestartGame();
				ResumeFromGameMenu(raw);
				return;
			}
			else if (row == PS4GameMenu::ROW_RESET_PROFILE)
			{
				if (s_profile.has_player)
					ResetGameProfile();
			}
			else if (row == PS4GameMenu::ROW_PATCH)
				TogglePatch();
			else if (row == PS4GameMenu::ROW_CHEATS) // compilation 58
				ToggleCheat();
			else if (row == PS4GameMenu::ROW_VU1_FILTER)
				ToggleVu1Program();
			else if (row == PS4GameMenu::ROW_GS_FILTER)
				ToggleGsGroup();
			else if (row == PS4GameMenu::ROW_RECOMMENDED)
			{
				if (s_recommended_wait_since <= 0.0) // compilation 61: used once the download ends (above)
				{
					s_recommended_applied = false;
					PublishRecommendedText(true);
					ForjaPatches::FetchCommunity(VMManager::GetDiscSerial(), VMManager::GetDiscCRC(), true);
					s_recommended_wait_since = std::max(elapsed, 0.001);
				}
			}
			else if (row == PS4GameMenu::ROW_CHANGE_DISC)
				ChangeToSelectedDisc();
			else
				ChangeGameMenuRow(row, 1);
			PublishGameMenu();
			s_game_menu_direction = 0;
			return;
		}
		// Forja SX2 (compilation 52): Cuadrado on a filter's row brings everything it left out back. Compilation 86: on the
		// comment's, it clears it.
		if ((pressed & ORBIS_PAD_BUTTON_SQUARE) != 0 &&
			(row == PS4GameMenu::ROW_GS_FILTER || row == PS4GameMenu::ROW_VU1_FILTER || row == PS4GameMenu::ROW_REPORT_COMMENT ||
				row == PS4GameMenu::ROW_REPORT_WHERE || row == PS4GameMenu::ROW_NET_DNS || row == PS4GameMenu::ROW_NET_HOST ||
				row == PS4GameMenu::ROW_NET_KEY))
		{
			if (row == PS4GameMenu::ROW_GS_FILTER)
				BringBackGsGroups();
			else if (row == PS4GameMenu::ROW_NET_DNS || row == PS4GameMenu::ROW_NET_HOST || row == PS4GameMenu::ROW_NET_KEY) // compilation 144
				SaveNetText(row == PS4GameMenu::ROW_NET_DNS ? 1 : row == PS4GameMenu::ROW_NET_HOST ? 2 : 3, std::string());
			else if (row == PS4GameMenu::ROW_REPORT_COMMENT)
			{
				s_report_comment.clear();
				s_report_phrase = -1; // compilation 87
			}
			else if (row == PS4GameMenu::ROW_REPORT_WHERE)
				s_report_where = -1; // compilation 88
			else
				BringBackVu1Programs();
			PublishGameMenu();
			s_game_menu_direction = 0;
			return;
		}
		// Forja SX2 (compilation 35): L1 and R1 move between the menu's tabs, onto the first row of the next one.
		const bool tab_left = (pressed & ORBIS_PAD_BUTTON_L1) != 0, tab_right = (pressed & ORBIS_PAD_BUTTON_R1) != 0;
		if (tab_left != tab_right)
		{
			const unsigned tab = PS4GameMenu::TabOfRow(row);
			const unsigned next = (tab + (tab_left ? PS4GameMenu::TAB_COUNT - 1 : 1)) % PS4GameMenu::TAB_COUNT;
			s_game_menu_selection = PS4GameMenu::TAB_FIRST_ROW[next];
			PublishGameMenu();
			s_game_menu_direction = 0;
			return;
		}
		u32 navigation = 0;
		if (raw & ORBIS_PAD_BUTTON_UP || analog_up)
			navigation = ORBIS_PAD_BUTTON_UP;
		else if (raw & ORBIS_PAD_BUTTON_DOWN || analog_down)
			navigation = ORBIS_PAD_BUTTON_DOWN;
		else if (raw & ORBIS_PAD_BUTTON_LEFT || analog_left)
			navigation = ORBIS_PAD_BUTTON_LEFT;
		else if (raw & ORBIS_PAD_BUTTON_RIGHT || analog_right)
			navigation = ORBIS_PAD_BUTTON_RIGHT;
		if (navigation && (navigation != s_game_menu_direction || elapsed >= s_game_menu_next_repeat))
		{
			s_game_menu_next_repeat = elapsed + (navigation == s_game_menu_direction ? 0.12 : 0.35);
			// Compilation 124: Up and Down go along the whole list (one list since this compilation, GameMenu.cpp), wrapping around it;
			// L1/R1 still jump between its sections.
			const unsigned first = 0, count = PS4GameMenu::TAB_FIRST_ROW[PS4GameMenu::TAB_COUNT];
			if (navigation == ORBIS_PAD_BUTTON_UP || navigation == ORBIS_PAD_BUTTON_DOWN)
			{
				// Compilation 131: over the hidden rows (PS4GameMenu::RowHidden).
				const unsigned move = navigation == ORBIS_PAD_BUTTON_UP ? count - 1 : 1;
				unsigned next = first + (row - first + move) % count;
				for (unsigned tries = 0; tries < count && PS4GameMenu::RowHidden(next); tries++)
					next = first + (next - first + move) % count;
				s_game_menu_selection = next;
			}
			// Compilation 79: and the report's word, the one row of Sistema with values (in the 77 Left/Right did nothing there, so
			// every report said "Perfecto").
			else if (row == PS4GameMenu::ROW_CHANGE_DISC || row == PS4GameMenu::ROW_REPORT ||
					 row == PS4GameMenu::ROW_REPORT_ISSUE || row == PS4GameMenu::ROW_REPORT_COMMENT ||
					 row == PS4GameMenu::ROW_REPORT_WHERE) // 44; 80: what went wrong; 87; 88
				ChangeGameMenuRow(row, navigation == ORBIS_PAD_BUTTON_LEFT ? -1 : 1);
			else if (row == PS4GameMenu::ROW_SAVE_STATE || row == PS4GameMenu::ROW_LOAD_STATE)
			{
				s_state_slot = (s_state_slot + (navigation == ORBIS_PAD_BUTTON_LEFT ? 4u : 1u)) % 5u;
				s_state_action = PS4GameMenu::STATE_NONE;
				RefreshStateSlot();
			}
			else if (PS4GameMenu::TabOfRow(row) == 1 || PS4GameMenu::TabOfRow(row) == 2 ||
					 PS4GameMenu::TabOfRow(row) == PS4GameMenu::TAB_PATCHES || PS4GameMenu::TabOfRow(row) == PS4GameMenu::TAB_ONLINE ||
					 PS4GameMenu::TabOfRow(row) == PS4GameMenu::TAB_LAB) // Imagen, Rendimiento, Extras, En línea and No tocar
				ChangeGameMenuRow(row, navigation == ORBIS_PAD_BUTTON_LEFT ? -1 : 1);
			PublishGameMenu();
		}
		s_game_menu_direction = navigation;
	}
}

static void PrepareEmulatorFolders(SettingsInterface& settings)
{
	// (AI-assisted) Initialize the same paths before the launcher and the VM.
	// Settings must be absolute before LoadConfig derives its debugger paths.
	EmuFolders::AppRoot = "/app0";
	EmuFolders::DataRoot = "/data/ForjaSX2";
	ForjaCenterFreshForBuild(); // compilation 132
	EmuFolders::Resources = "/app0/resources";
	EmuFolders::Settings = "/data/ForjaSX2/inis";
	EmuFolders::LoadConfig(settings);
	EmuFolders::Bios = "/data/ForjaSX2/bios";
	EmuFolders::Logs = "/data/ForjaSX2/logs";
	EmuFolders::Cache = "/data/ForjaSX2/cache/vm-boot";
	const char* paths[] = {
		EmuFolders::DataRoot.c_str(), EmuFolders::Settings.c_str(),
		EmuFolders::Bios.c_str(), "/data/ForjaSX2/roms",
		EmuFolders::MemoryCards.c_str(), EmuFolders::Savestates.c_str(),
		EmuFolders::Snapshots.c_str(), EmuFolders::Logs.c_str(),
		EmuFolders::Cheats.c_str(), EmuFolders::Patches.c_str(),
		EmuFolders::Covers.c_str(), EmuFolders::GameSettings.c_str(),
		EmuFolders::UserResources.c_str(), EmuFolders::Cache.c_str(),
		EmuFolders::Textures.c_str(), EmuFolders::InputProfiles.c_str(),
		EmuFolders::Videos.c_str(), EmuFolders::DebuggerLayouts.c_str(),
		EmuFolders::DebuggerSettings.c_str()};
	for (const char* path : paths)
	{
		Error error;
		if (FileSystem::CreateDirectoryPath(path, true, &error))
			ProbeLog("Emulator directory ready: %s\n", path);
		else
			ProbeLog("Emulator directory FAILED: %s: %s\n", path, error.GetDescription().c_str());
	}
}

static void* RunVM(void*)
{
	PS4ThreadAffinity::PinCurrentThread("EE", 2);
	MeasureConsoleCpu();
	VMHostSetCPUThread();
	PS4ThreadCPUMetrics::Sample(PS4ThreadCPUMetrics::Role::EE);
	PS4IPUProfile::s_sampling.enabled = access("/data/ForjaSX2/vm-profile-ipu", F_OK) == 0 &&
	                                    access("/data/ForjaSX2/vm-no-ipu-profile", F_OK) != 0;
	PS4IPUProfile::s_access_sampling.enabled = PS4IPUProfile::s_sampling.enabled;
	s_ipu_profile_marker = PS4IPUProfile::s_sampling.enabled; // compilation 65: ForjaVideoMeter
	s_ipu_profile_off = access("/data/ForjaSX2/vm-no-ipu-profile", F_OK) == 0;
	ProbeLog("IPU profiling: %s (sampled duration, nominal ratio 1/%u)\n",
		PS4IPUProfile::s_sampling.enabled ? "enabled by opt-in marker with inclusive access sampling 1/1024" :
		s_ipu_profile_off ? "off (vm-no-ipu-profile)" : "on while a movie plays (the video meter)",
		PS4IPUProfile::SAMPLE_WEIGHT);
	static MemorySettingsInterface settings_storage;
	static MemorySettingsInterface secrets_storage;
	auto* settings = &settings_storage;
	Host::Internal::SetBaseSettingsLayer(settings);
	Host::Internal::SetSecretsSettingsLayer(&secrets_storage);
	VMManager::SetDefaultSettings(*settings, true, true, false, false, false);
	// (AI-assisted) Fase 1: EE is 30-38ms bottleneck on Jaguar (Speed 48-55% in GTA/GoW FMV, GPU 1-2ms).
	// Exact cycle accuracy costs ~25% EE time on this CPU. Compilation 45: with no ajustes.ini yet, every game starts at
	// ForjaSettings' default (27 %), the lowest; the markers below and L3+R3 raise it.
	int ee_cycle_rate = ForjaSettings::Values{}.ee_rate;
	if (access("/data/ForjaSX2/vm-ee-cycle-normal", F_OK) == 0)
		ee_cycle_rate = 0;
	else if (access("/data/ForjaSX2/vm-ee-cycle-underclock", F_OK) == 0)
		ee_cycle_rate = -2;
	else if (access("/data/ForjaSX2/vm-ee-cycle-mild", F_OK) == 0)
		ee_cycle_rate = -1;
	// Forja SX2: the launcher's Settings page (ajustes.ini) sets the default for every game once it has
	// been saved; a game's own gamesettings ini still overrides it.
	const ForjaSettings::Values forja = ForjaSettings::Load();
	if (forja.from_file)
		ee_cycle_rate = forja.ee_rate;
	s_game_show_performance_overlay = forja.overlay && access("/data/ForjaSX2/vm-hide-stats", F_OK) != 0;
	ProbeLog("Forja settings: file=%d ee_rate=%d upscale=%dx aspect=%d overlay=%d stick=%d fast_cdvd=%d fast_boot=%d readback=%d\n",
		forja.from_file ? 1 : 0, forja.ee_rate, forja.upscale, forja.aspect, forja.overlay ? 1 : 0, forja.stick,
		forja.fast_cdvd ? 1 : 0, forja.fast_boot ? 1 : 0, forja.readback);
	settings->SetIntValue("EmuCore/Speedhacks", "EECycleRate", ee_cycle_rate);
	// (AI-assisted) Fase 3-A reverted: EECycleSkip=1 gave -1.2pp on GoW FMV (32.8ms vs 31.4ms) and no gain on GTA.
	// Reverted to 0 (opt-in via /data/ForjaSX2/vm-ee-cycle-skip only). Needs proper testing.
	int ee_cycle_skip = 0;
	if (access("/data/ForjaSX2/vm-ee-cycle-skip", F_OK) == 0)
		ee_cycle_skip = 1;
	settings->SetIntValue("EmuCore/Speedhacks", "EECycleSkip", ee_cycle_skip);
	// Compilation 146: the base for every game, the cycle skip "Al vuelo" (God of War II 30 -> 36 FPS at full speed, the 139); a
	// game's own profile still wins.
	settings->SetIntValue("EmuCore/Speedhacks", "EECycleSkipAuto", 2);
	ProbeLog("EE cycle profile requested: rate=%d skip=%d (underclock marker=%s, mild marker=%s, normal marker=%s, skip marker=%s)\n",
		ee_cycle_rate, ee_cycle_skip,
		access("/data/ForjaSX2/vm-ee-cycle-underclock", F_OK) == 0 ? "present" : "absent",
		access("/data/ForjaSX2/vm-ee-cycle-mild", F_OK) == 0 ? "present" : "absent",
		access("/data/ForjaSX2/vm-ee-cycle-normal", F_OK) == 0 ? "present" : "absent",
		access("/data/ForjaSX2/vm-ee-cycle-skip", F_OK) == 0 ? "present" : "absent");

	// (AI-assisted) Recommended speedhacks: vuFlagHack eliminates VU1 status flag overhead,
	// IntcStat and WaitLoop bypass guest idle loops. Keep normal disc timing by default.
	// Needs proper testing per game.
	settings->SetBoolValue("EmuCore/Speedhacks", "vuFlagHack", true);
	// Compilation 127 (Joseph, 2026-11-08: "VU1 rápido" on for every game, polished for all): the VU1 without clamping. With the
	// flag hack it gave God of War II 33.4 -> 37.1 FPS at full speed (126). The GameDB's vuClampMode still wins (110 games that need
	// a clamp), and its mvuFlag: 0 (77); a game's profile wins over both (the row's No writes the clamp back for that game).
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "vu1Overflow", false);
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "vu1ExtraOverflow", false);
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "vu1SignOverflow", false);
	settings->SetBoolValue("EmuCore/Speedhacks", "IntcStat", true);
	settings->SetBoolValue("EmuCore/Speedhacks", "WaitLoop", true);
	// Compilation 131 (Joseph, 2026-11-13: the improvements on from the factory): "Refrescos sin repetir" for every game, the row of the
	// 128 (hidden now). A game's profile still wins, and "Modo seguro: Total" turns it off (VMManager.cpp).
	settings->SetBoolValue("EmuCore/GS", "SkipDuplicateFrames", true);
	const bool fast_cdvd = (access("/data/ForjaSX2/vm-fast-cdvd", F_OK) == 0 || forja.fast_cdvd) &&
	                       access("/data/ForjaSX2/vm-no-fast-cdvd", F_OK) != 0;
	settings->SetBoolValue("EmuCore/Speedhacks", "fastCDVD", fast_cdvd);
	ProbeLog("CDVD timing: %s\n", fast_cdvd ? "accelerated by opt-in marker" : "normal (compatibility default)");
	// (AI-assisted) Let the backend choose the compiled renderer; on this PS4 build that is Vulkan.
	settings->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
	settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(forja.upscale));
	settings->SetStringValue("EmuCore/GS", "AspectRatio", Pcsx2Config::GSOptions::AspectRatioNames[forja.aspect]);
	// Forja SX2: GPU readbacks. "Don't wait" keeps the GS thread from stalling until the GPU has finished,
	// the stall that showed on the console as slow scenes with no thread near 100%.
	settings->SetIntValue("EmuCore/GS", "HWDownloadMode", ForjaSettings::ReadbackModes[forja.readback]);
	const bool disable_shader_cache = access("/data/ForjaSX2/vm-no-shader-cache", F_OK) == 0;
	settings->SetBoolValue("EmuCore/GS", "DisableShaderCache", disable_shader_cache);
	settings->SetBoolValue("EmuCore/GS", "HWSpinGPUForReadbacks", false);
	settings->SetBoolValue("EmuCore/GS", "HWSpinCPUForReadbacks", false);
	// (AI-assisted) Hardware palette conversion, frame preloading, and partial invalidation bypass
	// reduce CPU stalls in 1x/2x upscale; needs proper testing.
	settings->SetBoolValue("EmuCore/GS", "paltex", true);
	settings->SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", true);
	// (AI-assisted) Let per-game GameDB HW fixes apply (autoFlush, halfPixelOffset, etc) to fix
	// shadows/post-processing without forcing manual overrides. GTA SA (SLUS-20946) needs
	// autoFlush=1 + halfPixelOffset=5. DisablePartialInvalidation=true stays as PS4 perf
	// default when GameDB has no entry. Needs proper testing across titles.
	settings->SetBoolValue("EmuCore/GS", "UserHacks", false);
	settings->SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", true);
	// (AI-assisted) Triple VideoOut (ImageCount=3) with VsyncQueueSize=3 queues 2 extra frames
	// and can show stale buffers as ghosting. Use 2 (upstream default) to reduce latency
	// and present stalls; vsyncq was 0..1 in baseline. Needs proper testing.
	settings->SetIntValue("EmuCore/GS", "VsyncQueueSize", 2);
	// Forja SX2 (compilation 43): a DualShock 2 in the PS2's second port too, for the second player (UpdateSecondPad).
	settings->SetStringValue("Pad2", "Type", "DualShock2");
	// (AI-assisted) Skip the PS2 BIOS animation and fast-forward only the
	// emulator's boot phase. The game's own logos and loading still run normally.
	bool fast_boot = access("/data/ForjaSX2/vm-no-fast-boot", F_OK) != 0 && forja.fast_boot;
	settings->SetBoolValue("EmuCore", "EnableFastBoot", fast_boot);
	settings->SetBoolValue("EmuCore", "EnableFastBootFastForward", fast_boot);
	settings->SetBoolValue("EmuCore", "EnablePINE", false);
	settings->SetBoolValue("EmuCore", "EnableThreadPinning", false);
	// (AI-assisted) Respect the PS4 build option; release scripts enable MTVU.
#if defined(PCSX2_PS4_VM_BOOT_MTVU)
	settings->SetBoolValue("EmuCore/Speedhacks", "vuThread", true);
#else
	settings->SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
#endif
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
#if defined(PCSX2_PS4_VM_BOOT_FASTMEM)
	// (AI-assisted) Fastmem is compiled in for performance, but keep a
	// console-side escape hatch for page-fault diagnosis. The marker must be
	// checked before CPUThreadInitialize creates the VTLB mappings.
	const bool fastmem_disabled_by_marker = access("/data/ForjaSX2/vm-no-fastmem", F_OK) == 0;
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", !fastmem_disabled_by_marker);
	ProbeLog("Fastmem runtime override: %s\n",
		fastmem_disabled_by_marker ? "disabled by /data/ForjaSX2/vm-no-fastmem" : "enabled");
#else
	settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", false);
#endif
#endif
	settings->SetBoolValue("Achievements", "Enabled", false);
	// (AI-assisted) Both library and VM poll scePad directly on PS4.
	settings->SetBoolValue("InputSources", "SDL", false);
	// (AI-assisted) The Orbis Cubeb slot routes to native PS4 AudioOut.
	extern int ForjaAudioMode(); // compilation 67: ps4/core/AudioStreamPS4.cpp; 2 after two deaths opening the sound
	const bool audio_enabled = access("/data/ForjaSX2/vm-no-audio", F_OK) != 0 && ForjaAudioMode() < 2;
	settings->SetStringValue("SPU2/Output", "Backend", audio_enabled ? "Cubeb" : "Null");
	settings->SetStringValue("SPU2/Output", "SyncMode", "TimeStretch");
	// (AI-assisted) Longer sequences suit slowed audio and need fewer overlap
	// searches. Leave enough queued audio for the resulting larger bursts.
	// Needs proper testing with speech, transitions and pause on the console.
	// (AI-assisted) Optimize audio buffer and time-stretching window for PS4.
	// 70 ms buffer with 35 ms sequences avoids audio buffer underruns (input_short)
	// while keeping speech and sound effects responsive and glitch-free.
	// Needs proper testing across games on the console.
	const bool legacy_audio = access("/data/ForjaSX2/vm-audio-legacy-stretch", F_OK) == 0;
	// Forja SX2 (compilation 26): the stretching for music. Movies run at 85-90 % of full speed, so their music is stretched
	// the whole time, and 35 ms sequences with a 15 ms search and 8 ms overlaps (plus the quick seek below) made it sound
	// choppy, like a damaged song (Tales of the Abyss, God of War II, 2026-10-15), while the queue never ran dry. Longer
	// sequences with wider searches and overlaps join music smoothly; the cost is on the audio write thread (spu2.cpp,
	// the audio core), not the EE thread, since compilation 23. vm-audio-legacy-stretch keeps the old values.
	settings->SetIntValue("SPU2/Output", "BufferMS", legacy_audio ? 50 : 80);
	settings->SetIntValue("SPU2/Output", "StretchSequenceLengthMS", legacy_audio ? 30 : 60);
	settings->SetIntValue("SPU2/Output", "StretchSeekWindowMS", legacy_audio ? 20 : 25);
	settings->SetIntValue("SPU2/Output", "StretchOverlapMS", legacy_audio ? 10 : 12);
	// SoundTouch's full search, for the same reason (it ran on the EE thread until compilation 23, hence the quick seek then).
	// /data/ForjaSX2/vm-audio-quick-seek brings the quick seek back.
	settings->SetBoolValue("SPU2/Output", "StretchUseQuickSeek", access("/data/ForjaSX2/vm-audio-quick-seek", F_OK) == 0);
	settings->SetIntValue("SPU2/Output", "StandardVolume", 100);
	settings->SetBoolValue("SPU2/Output", "OutputMuted", false);
	ProbeLog("PS4 audio configuration: %s (stereo PCM16 48 kHz, main port, explicit 0 dB; disable with /data/ForjaSX2/vm-no-audio)\n",
		audio_enabled ? "native AudioOut with TimeStretch" : "Null by marker");
	PrepareEmulatorFolders(*settings);
	PS4GSProfile::enabled = access("/data/ForjaSX2/vm-profile-gs", F_OK) == 0 &&
	                        access("/data/ForjaSX2/vm-no-gs-profile", F_OK) != 0;
	ProbeLog("GS profiling: %s (hot calls sampled 1/64; inclusive wall time)\n",
		PS4GSProfile::enabled ? "enabled by opt-in marker" : "disabled for performance");
	ProbeLog("VM configuration: observation=unlimited renderer=vulkan mtvu=%s fastmem-build=%s shader_cache=%s directory=%s\n",
#if defined(PCSX2_PS4_VM_BOOT_MTVU)
		"on",
#else
		"off",
#endif
#if defined(PCSX2_PS4_VM_BOOT_FASTMEM)
		"on",
#else
		"off",
#endif
		disable_shader_cache ? "disabled by marker" : "enabled", EmuFolders::Cache.c_str());
	ProbeLog("STAGE: CPUThreadInitialize (real VM, JIT, optional user disc)\n");
	PS4Telemetry::Stage("cpu-init");
	if (!VMManager::Internal::CPUThreadInitialize())
	{
		ProbeLog("CPUThreadInitialize FAILED\n");
		PS4Telemetry::Stage("cpu-init-failed");
		return nullptr;
	}
	ProbeLog("VM settings loaded: MTVU=%s Fastmem=%s; awaiting initial VTLB mappings\n",
		EmuConfig.Speedhacks.vuThread ? "on" : "off",
		EmuConfig.Cpu.Recompiler.EnableFastmem ? "on" : "off");
	ProbeLog("VM runtime EE cycle profile: rate=%d skip=%u\n",
		static_cast<int>(EmuConfig.Speedhacks.EECycleRate),
		static_cast<unsigned>(EmuConfig.Speedhacks.EECycleSkip));
	VMBootParameters boot{};
	GamePlatform platform = GamePlatform::Unknown;
	g_ps4_ipu_csc_reuse = access("/data/ForjaSX2/vm-no-ipu-csc-reuse", F_OK) != 0;
	ProbeLog("IPU CSC output reuse: %s (partial FIFO drain; timing unchanged)\n",
		g_ps4_ipu_csc_reuse ? "enabled" : "disabled by marker");
	g_ps4_ipu0_burst = access("/data/ForjaSX2/vm-no-ipu-burst", F_OK) != 0;
	ProbeLog("IPU output burst: %s (up to 64 FIFO fills per DMA event, cycles preserved; disable with /data/ForjaSX2/vm-no-ipu-burst)\n",
		g_ps4_ipu0_burst ? "enabled" : "disabled by marker");
	g_ps4_ipu_feed = access("/data/ForjaSX2/vm-no-ipu-feed", F_OK) != 0; // compilation 66
	s_iop_sleep_allowed = access("/data/ForjaSX2/vm-no-iop-sleep", F_OK) != 0; // compilation 67
	ProbeLog("IOP sleep in movies: %s (the IOP waits in its idle loop between the events it needs; disable with "
			 "/data/ForjaSX2/vm-no-iop-sleep)\n", s_iop_sleep_allowed ? "enabled" : "disabled by marker");
	ProbeLog("IPU input feed: %s (the decoder fed from its toIPU DMA while that is all it waits for; disable with "
			 "/data/ForjaSX2/vm-no-ipu-feed)\n", g_ps4_ipu_feed ? "enabled" : "disabled by marker");
#if defined(PCSX2_PS4_VM_BOOT_JIT)
	g_ps4_vtlb_register_cache = access("/data/ForjaSX2/vm-no-vtlb-register-cache", F_OK) != 0;
	ProbeLog("VTLB scalar read register cache: %s (device handlers retained; Fastmem unchanged)\n",
		g_ps4_vtlb_register_cache ? "enabled" : "disabled by marker");
	g_ps4_vtlb_bulk_register_cache = g_ps4_vtlb_register_cache &&
	                                 access("/data/ForjaSX2/vm-no-vtlb-bulk-cache", F_OK) != 0;
	g_ps4_vtlb_cached_quad_reads = g_ps4_vtlb_cached_writes = 0;
	ProbeLog("VTLB quad read/store register cache: %s (disable with /data/ForjaSX2/vm-no-vtlb-bulk-cache)\n",
		g_ps4_vtlb_bulk_register_cache ? "enabled (default since v0.87)" : "disabled by marker");
	// (AI-assisted) Fase 3-B REVERTED 2026-10-12: SMC skip default ON broke GTA SLUS-20946
	// (Unknown SPECIAL 00000001 at 00eaa1c4, Trap 00eaa1d4, stuck at 8000fe74 100% unavailable).
	// GoW gained +18pp but GTA unbootable -> revert to exact SMC. Opt-in via /data/ForjaSX2/vm-skip-smc. Needs proper testing.
	// Compilation 124: or the game's row ("Revisión rápida del código"), read by ApplyGameProfile before this.
	s_fast_code_marker = access("/data/ForjaSX2/vm-skip-smc", F_OK) == 0;
	g_ps4_skip_smc_checks = s_fast_code_marker || s_fast_code_game;
	ProbeLog("EE JIT manual SMC validation: %s (%s)\n",
		g_ps4_skip_smc_checks ? "relaxed (skip per-block checks, manual protection only)" : "enabled for all games (exact block checks)",
		s_fast_code_marker ? "enabled by /data/ForjaSX2/vm-skip-smc" : s_fast_code_game ? "the game's row, Revisión rápida del código" : "exact (reverted Fase 3-B)");
	// (AI-assisted) Exact SMC mode must inspect code on every loop iteration.
	g_ps4_bytecopy_fusion = g_ps4_skip_smc_checks && access("/data/ForjaSX2/vm-no-bytecopy-fusion", F_OK) != 0;
	g_ps4_bytecopy_blocks = g_ps4_bytecopy_runs = g_ps4_bytecopy_bytes = 0;
	g_ps4_bytecopy_short = g_ps4_bytecopy_deadline = g_ps4_bytecopy_mapping = g_ps4_bytecopy_state = g_ps4_bytecopy_scratch = 0;
	g_ps4_bytecopy_last_source = g_ps4_bytecopy_last_destination = 0;
	ProbeLog("EE byte-copy fusion: %s (disable with /data/ForjaSX2/vm-no-bytecopy-fusion)\n",
		g_ps4_bytecopy_fusion ? "enabled with inline deadline gate, mapped RAM batches and native fallback" : "disabled by marker or exact SMC checks");
#endif
	// (AI-assisted) This process has never initialized Piglet. The recovery
	// selector and the VM share only the Vulkan backend on the GS thread.
	if (s_use_vulkan_selector)
	{
		ProbeLog("STAGE: Vulkan game library\n");
		if (!PS4GameMenu::Select(s_pad, s_selected_game, platform))
		{
			ProbeLog("Vulkan selector ended without a selection\n");
			VMManager::Internal::CPUThreadShutdown();
			return nullptr;
		}
	}
	// Forja SX2: the settings above were read when the app started, before the library opened, so a change
	// made on the Settings page only reached the next launch of the app. Read ajustes.ini again now that a
	// game has been chosen and apply it to the base layer before the VM boots (the PS1 profile below can
	// still override it; a game's own gamesettings ini still wins over all of this).
	{
		ForjaSettings::Values chosen = ForjaSettings::Load();
		// Forja SX2: GSRendererPS4.cpp leaves fg-en-prueba while frame generation's first seconds run; if it is still
		// here, the last try never got through them (the PS4's GPU hung on it once), so it is turned off and saved off.
		if (std::FILE* guard = std::fopen("/data/ForjaSX2/fg-en-prueba", "r"))
		{
			// Since the per-game profiles the file names the game ("<serial> <crc>"): its profile gets FrameGen = 0 too.
			char guard_serial[32] = {};
			unsigned guard_crc = 0;
			const bool named = std::fscanf(guard, "%31s %x", guard_serial, &guard_crc) == 2 && std::strcmp(guard_serial, "-") != 0;
			std::fclose(guard);
			unlink("/data/ForjaSX2/fg-en-prueba");
			s_frame_gen_blocked = true;
			if (named)
				ForjaGameProfile::EditPlayer(guard_serial, guard_crc, [](INISettingsInterface& ini) { ini.SetIntValue("Forja", "FrameGen", 0); });
			if (chosen.frame_gen)
			{
				chosen.frame_gen = false;
				ForjaSettings::Save(chosen);
			}
			ProbeLog("Frame generation: turned off, the last try did not finish its first seconds (GPU hang?) in %s %08X\n",
				named ? guard_serial : "(unnamed)", guard_crc);
		}
		if (chosen.from_file)
			settings->SetIntValue("EmuCore/Speedhacks", "EECycleRate", chosen.ee_rate);
		settings->SetIntValue("EmuCore/Speedhacks", "EECycleSkipAuto", 2); // compilation 146: the base
		// Compilation 45: every console starts at the native 1x (before, a PS4 Pro with Boost Mode started at 2x until the
		// Settings page was saved); the player raises it per game in L3+R3, or for all games on the Settings page.
		const int upscale = chosen.upscale;
		settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(upscale));
		settings->SetStringValue("EmuCore/GS", "AspectRatio", Pcsx2Config::GSOptions::AspectRatioNames[chosen.aspect]);
		settings->SetIntValue("EmuCore/GS", "HWDownloadMode", ForjaSettings::ReadbackModes[chosen.readback]);
		const bool chosen_fast_cdvd = (access("/data/ForjaSX2/vm-fast-cdvd", F_OK) == 0 || chosen.fast_cdvd) &&
		                              access("/data/ForjaSX2/vm-no-fast-cdvd", F_OK) != 0;
		settings->SetBoolValue("EmuCore/Speedhacks", "fastCDVD", chosen_fast_cdvd);
		fast_boot = access("/data/ForjaSX2/vm-no-fast-boot", F_OK) != 0 && chosen.fast_boot;
		settings->SetBoolValue("EmuCore", "EnableFastBoot", fast_boot);
		settings->SetBoolValue("EmuCore", "EnableFastBootFastForward", fast_boot);
		s_game_show_performance_overlay = chosen.overlay && access("/data/ForjaSX2/vm-hide-stats", F_OK) != 0;
		ApplyPerformanceOverlay();
		// Video mode and frame generation (pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp, from PS5SX2; read by the GS at every
		// new frame): ajustes.ini gives the default, the game's profile can change it.
		s_global_video_mode = chosen.video_mode;
		s_global_frame_gen = chosen.frame_gen;
		s_global_cpu_auto = chosen.cpu_auto;
		s_menu_button = chosen.menu_button; // compilation 53
		// Forja SX2: the game's profile, before the VM boots, so its cores and video settings are in place from the first frame.
		// The library already scanned the image for its serial; GameList reads its boot ELF's CRC too (the ini's name).
		{
			const auto scan_start = Common::Timer::GetCurrentValue();
			GameList::Entry entry;
			std::string serial;
			u32 crc = 0;
			if (!s_selected_game.empty() && GameList::PopulateEntryFromPath(s_selected_game, &entry))
			{
				serial = entry.serial;
				std::transform(serial.begin(), serial.end(), serial.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				crc = entry.crc;
			}
			ProbeLog("Game profile: scanned %s -> %s %08X in %.0f ms\n", s_selected_game.c_str(), serial.empty() ? "(no serial)" : serial.c_str(),
				crc, Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - scan_start));
			s_cpu_auto_steps = 0;
			s_gs_skip_mode = -1; // compilation 47: a new game starts drawing every frame (ApplyGameProfile sets it again)
			s_video_auto_ceiling = VIDEO_AUTO_HIGHEST; // compilation 65: a new game's movies find their own ceiling
			s_movie_settings = false; // compilation 65
			g_orbis_fg_movie = false; // compilation 66
			g_orbis_fg_held = false; // Forja 1.06
			g_orbis_fg_gave_up = false; // Forja 1.06: a new game gets its frame generation back (OrbisFrameGenPacing.h, NewGame)
			ForjaSession::Begin(); // Forja 1.06: and its own summary
			ForjaReport::ResendPending(); // compilation 86: a report that didn't reach the channel goes again
			s_session_beats = 0;
			g_orbis_fg_restart.fetch_add(1, std::memory_order_relaxed);
			g_forja_iop_sleep = false; // compilation 67
			ResetGsFilterList(); // compilation 54: a new game's list starts empty
			LoadRules(); // compilation 55: huellas.ini, before the profile turns its rules on
			StartHangWatch();
			s_ps1_game = platform == GamePlatform::PS1; // compilation 131
			ForjaGuardStart(serial, crc); // compilation 131: before the profile is read
			s_push_active = false; // compilation 132
			ApplyGameProfile(ForjaGameProfile::Load(serial, crc));
			ForjaCodeAheadStart(serial, crc, false); // compilation 132: the game's list, or none
			RepinWorkersForProfile();
#if defined(PCSX2_PS4_VM_BOOT_JIT)
			// Compilation 125: "Revisión rápida del código" from the game's start. In the 124 it was read only when the row changed: Bully
			// (2026-11-08) had it on in its profile and ran its whole session with the exact checks (EE at 101 % at 44 %), and a game after one
			// that had it on kept the relaxed mode (GTA San Andreas doesn't start with it). Before the game's first block is compiled.
			g_ps4_skip_smc_checks = s_fast_code_marker || s_fast_code_game;
			g_ps4_bytecopy_fusion = g_ps4_skip_smc_checks && access("/data/ForjaSX2/vm-no-bytecopy-fusion", F_OK) != 0;
			ProbeLog("EE JIT manual SMC validation: %s (at the game's start: %s)\n", g_ps4_skip_smc_checks ? "relaxed" : "exact",
				s_fast_code_marker ? "the vm-skip-smc marker" : s_fast_code_game ? "the game's row, Revisión rápida del código" : "the row is off");
#endif
		}
		VMManager::ApplySettings();
		ProbeLog("Forja settings at game start: file=%d ee_rate=%d upscale=%dx aspect=%d overlay=%d stick=%d fast_cdvd=%d "
				 "fast_boot=%d readback=%d video_mode=%d frame_gen=%d\n",
			chosen.from_file ? 1 : 0, chosen.ee_rate, upscale, chosen.aspect, chosen.overlay ? 1 : 0, chosen.stick,
			chosen_fast_cdvd ? 1 : 0, fast_boot ? 1 : 0, chosen.readback, chosen.video_mode ? 1 : 0, chosen.frame_gen ? 1 : 0);
	}
	if (platform == GamePlatform::PS1)
	{
		// (AI-assisted) PS1DRV takes over the EE after the BIOS. Do not carry the
		// experimental PS2 FMV shortcuts into its boot/code replacement sequence.
		// This compatibility profile needs proper testing on the console.
		fast_boot = false;
		settings->SetBoolValue("EmuCore", "EnableFastBoot", false);
		settings->SetBoolValue("EmuCore", "EnableFastBootFastForward", false);
		settings->SetBoolValue("EmuCore/Speedhacks", "fastCDVD", false);
		// Restore the core's conservative idle-loop detection. Keep the port's
		// wider loop rule disabled for PS1, and retain exact code validation.
		settings->SetBoolValue("EmuCore/Speedhacks", "WaitLoop", true);
		settings->SetBoolValue("EmuCore/Speedhacks", "IntcStat", false);
		settings->SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
		settings->SetIntValue("EmuCore/Speedhacks", "EECycleRate", 0);
		settings->SetIntValue("EmuCore/Speedhacks", "EECycleSkip", 0);
#if defined(PCSX2_PS4_VM_BOOT_JIT)
		g_ps4_skip_smc_checks = false;
		g_ps4_bytecopy_fusion = false;
		g_ps4_vtlb_register_cache = false;
		g_ps4_vtlb_bulk_register_cache = false;
		g_orbis_waitloop_ext.store(0, std::memory_order_relaxed);
#endif
		// Still before VM reset and the first guest instruction. Updating the base
		// layer also preserves the profile when menu settings are reapplied later.
		VMManager::ApplySettings();
		ProbeLog("PS1 compatibility profile: full BIOS boot; exact SMC checks; byte-copy fusion and VTLB register caches OFF; WaitLoop=upstream; IntcStat/fastCDVD/MTVU OFF; EE rate=0 skip=0\n");
	}
	boot.filename = s_selected_game;
	ProbeLog("Session archive: platform marker saved=%d platform=%s\n",
		PS4SessionLogs::MarkGame(platform, s_profile.serial) ? 1 : 0,
		platform == GamePlatform::PS1 ? "PS1" : platform == GamePlatform::PS2 ? "PS2" : "Other");
	PS4Telemetry::GameSelect(boot.filename.c_str(),
		platform == GamePlatform::PS1 ? "PS1" : platform == GamePlatform::PS2 ? "PS2" : "Other");
	PS4Telemetry::Stage(boot.filename.empty() ? "game-bios" : "game-selected");
	if (s_use_vulkan_selector)
		PS4GameMenu::ShowLoading(24, boot.filename.empty() ? ForjaLang::T("Preparando la BIOS de la PS2...") :
			boot.filename == PS4Disc::DevicePath ? ForjaLang::T("Abriendo el DVD...") : ForjaLang::T("Abriendo la imagen del juego..."));
	ProbeLog("Disc selection: library returned %s\n", boot.filename.empty() ? "BIOS (no disc)" : boot.filename.c_str());
	if (boot.filename.empty())
	{
		boot.source_type = CDVD_SourceType::NoDisc;
		ProbeLog("Disc selection: BIOS without a disc selected in menu.\n");
	}
	else
	{
		boot.source_type = boot.filename == PS4Disc::DevicePath ? CDVD_SourceType::Disc : CDVD_SourceType::Iso;
		ProbeLog("Disc selection: mounting %s\n", boot.filename.c_str());
	}
	boot.fast_boot = fast_boot;
	ProbeLog("Fast boot: %s (PS1 always uses full BIOS boot; PS2 override: /data/ForjaSX2/vm-no-fast-boot)\n", fast_boot ? "enabled" : "disabled");
	Error error;
	if (s_use_vulkan_selector)
		PS4GameMenu::ShowLoading(45, ForjaLang::T("Iniciando la máquina virtual..."));
	// Compilation 67: line by line until the game's first heartbeat, so a start that dies keeps its last step (a friend's PS4:
	// the lines of its audio's opening stayed in the queue).
	PS4ProbeLogger::SetDirect(true);
	s_log_direct_boot = true;
	ProbeLog("PS4 log: line by line while the game starts\n");
	ProbeLog("STAGE: VMManager::Initialize\n");
	PS4Telemetry::Stage("vm-init");
	const auto result = VMManager::Initialize(boot, &error);
	if (result == VMBootResult::StartupSuccess)
	{
		if (s_use_vulkan_selector)
			PS4GameMenu::ShowLoading(92, ForjaLang::T("Encendiendo la PlayStation 2..."));
		ProbeLog("VM runtime Fastmem=%s (after initial VTLB mappings)\n",
			EmuConfig.Cpu.Recompiler.EnableFastmem ? "on" : "off");
		ProbeLog("VM initialized: executing BIOS; no probe MMIO shadows\n");
		PS4Telemetry::Stage("vm-running");
		// (AI-assisted) A DS4 has no dedicated analog-mode switch; start the emulated DS2 in analog mode.
		Pad::SetControllerState(0, PadDualshock2::Inputs::PAD_ANALOG, 1.0f);
		Pad::SetControllerState(0, PadDualshock2::Inputs::PAD_ANALOG, 0.0f);
		Pad::SetControllerState(1, PadDualshock2::Inputs::PAD_ANALOG, 1.0f); // the second player's too (compilation 43)
		Pad::SetControllerState(1, PadDualshock2::Inputs::PAD_ANALOG, 0.0f);
		ProbeLog("Controller: DualShock 2 analog mode enabled; both sticks and analog triggers forwarded.\n");
		LoadStickSensitivity();
		s_verbose_diag = access("/data/ForjaSX2/vm-verbose-log", F_OK) == 0;
		// 150 in the app: the EE counters (OrbisEEDiag.h) while /data/ForjaSX2/vm-eediag is there, read at each game's start.
		OrbisEEDiag::on.store(access("/data/ForjaSX2/vm-eediag", F_OK) == 0 ? 1 : 0, std::memory_order_relaxed);
		ProbeLog("EE diag (150): %s\n", OrbisEEDiag::on.load(std::memory_order_relaxed) ? "on (vm-eediag): the EE diag lines every 5 s" : "off");
		ProbeLog("Verbose diagnostics: %s (single marker /data/ForjaSX2/vm-verbose-log forces 1s heartbeat, TimedWaitGS, and GS/EE hotspot sampling)\n",
			s_verbose_diag ? "ENABLED" : "disabled");
		s_run_timer.Reset();
		// Forja SX2: the video mode's and the profile's clocks count from this game's start too. Kept from the previous
		// game, a movie in the next one went unnoticed for as long as the previous one had run.
		s_video_active = false;
		s_video_busy_checks = 0;
		s_video_last_check = 0.0;
		s_video_last_busy = -100.0;
		s_video_since = 0.0;
		s_video_last_off = -100.0; // compilation 87
		s_video_flapping = false;
		s_video_last_decodes = g_forja_ipu_decodes;
		s_profile_last_check = 0.0;
		// Forja SX2 (compilation 37): the 50 Hz output's guard still here means a game at 50 Hz never closed normally in its
		// first 90 s (a TV that showed nothing, the app closed from the PS button): this console keeps the system's rate.
		if (access("/data/ForjaSX2/salida-50hz-en-prueba", F_OK) == 0)
		{
			unlink("/data/ForjaSX2/salida-50hz-en-prueba");
			if (std::FILE* marker = std::fopen("/data/ForjaSX2/vm-sin-50hz", "w"))
				std::fclose(marker);
			ProbeLog("Output rate: the last game at 50 Hz did not close normally; vm-sin-50hz written\n");
		}
		s_output_50hz_enabled = access("/data/ForjaSX2/vm-sin-50hz", F_OK) != 0;
		s_output_last_check = 0.0;
		s_output_pal_since = -1.0;
		PS4Port::g_output_hz_request.store(0, std::memory_order_relaxed);
		ProbeLog("Output rate: 50 Hz for PAL games %s (/data/ForjaSX2/vm-sin-50hz turns it off)\n", s_output_50hz_enabled ? "on" : "off");
		const bool hotspot_marker = access("/data/ForjaSX2/vm-ee-hotspots", F_OK) == 0 && access("/data/ForjaSX2/vm-no-ee-hotspots", F_OK) != 0;
		PS4EEHotspots::Reset(hotspot_marker || s_verbose_diag);
		ProbeLog("EE hotspots: %s; event and IPU-call frequencies, not CPU-time shares (disable with /data/ForjaSX2/vm-no-ee-hotspots)\n",
			PS4EEHotspots::enabled ? "enabled" : "disabled");
#if defined(PCSX2_PS4_VM_EE_BLOCK_PROFILE)
		PS4EEHotspots::ResetBlockSampling(true);
		ProbeLog("EE JIT block sampling: enabled by diagnostic build; randomized interval 2048-4095 block entries\n");
#else
		PS4EEHotspots::ResetBlockSampling(hotspot_marker || s_verbose_diag);
		ProbeLog("EE JIT block sampling: %s\n", (hotspot_marker || s_verbose_diag) ? "enabled (verbose forces on)" : "disabled in performance build");
#endif
		PS4EEEventProfile::s_sampling = {};
		// (AI-assisted) Keep deep profiling opt-in: PS1 can reach over a million
		// event polls per second. Normal FPS/CPU metrics and heartbeat stay active.
		const bool performance_profile = access("/data/ForjaSX2/vm-profile-perf", F_OK) == 0 &&
		                                 access("/data/ForjaSX2/vm-no-perf-profile", F_OK) != 0;
		PS4WorkerProfile::enabled.store(performance_profile, std::memory_order_relaxed);
		PS4EEEventProfile::s_sampling.enabled = ((performance_profile || access("/data/ForjaSX2/vm-profile-ee", F_OK) == 0) && access("/data/ForjaSX2/vm-no-ee-profile", F_OK) != 0) || s_verbose_diag;
		s_ee_profile_marker = PS4EEEventProfile::s_sampling.enabled; // compilation 66: otherwise on only in movies (ForjaVideoMeter)
		PS4EEEventProfile::s_compact.store(false, std::memory_order_relaxed);
		PS4EEEventProfile::s_sampling.gaps_only = false;
		PS4EEEventProfile::s_sampling.gap_observer = PS4EEEventProfile::RecordGap;
		ProbeLog("Performance diagnosis: workers=%d EE_phases=%d sample_ratio=1/1024; opt-in with /data/ForjaSX2/vm-profile-perf\n",
			performance_profile ? 1 : 0, PS4EEEventProfile::s_sampling.enabled ? 1 : 0);
		// (AI-assisted) Verbose also forces GS profiling on. Needs proper testing on console overhead.
		PS4GSProfile::enabled = (access("/data/ForjaSX2/vm-profile-gs", F_OK) == 0 && access("/data/ForjaSX2/vm-no-gs-profile", F_OK) != 0) || s_verbose_diag;
		ProbeLog("GS profiling: %s (verbose forces 1/64 sampling when vm-verbose-log present)\n",
			PS4GSProfile::enabled ? "enabled" : "disabled");
		s_host_poll_gate.Reset();
		ProbeLog("Host clock gate runtime: max_calls=%u cycle_interval=%u input_interval_ms=10\n",
			s_host_poll_gate.GetMaxCalls(), s_host_poll_gate.GetCycleInterval());
		s_host_poll_calls = s_last_host_poll_calls = 0;
		s_host_clock_checks = s_last_host_clock_checks = 0;
		s_last_log = 0.0;
		s_last_pause_log = 0.0;
		s_last_input = -1.0;
		s_last_ee_cycle = cpuRegs.cycle;
		s_previous_pad_buttons = s_previous_host_buttons = s_previous_pad2_buttons = 0;
		s_blocked_pad_buttons = ~u32{0}; // Consume any launcher button still held.
		s_menu_chord_latched = s_game_menu_open = false;
		s_game_menu_selection = 0;
		s_menu_touch_held = false;
		ProbeLog("Controller: OPTIONS=PS2 Start, touchpad=PS2 Select%s, emulator menu=%s\n",
			s_menu_button != 0 ? " (left half)" : "", s_menu_button == 0 ? "L3+R3" : s_menu_button == 1 ? "touchpad right half" :
			"L3+R3 or touchpad right half");
		if (s_use_vulkan_selector)
		{
			PS4GameMenu::ShowLoading(100, ForjaLang::T("Listo"));
			Threading::Sleep(50);
			PS4GameMenu::HideLoading();
		}
		VMManager::SetPaused(false);
		while (VMManager::GetState() != VMState::Stopping && VMManager::GetState() != VMState::Shutdown)
		{
			if (VMManager::GetState() == VMState::Paused)
			{
				// Keep reading menu controls while the EE is paused.
				VMHostPoll();
				Threading::Sleep(16);
			}
			else
			{
				VMManager::Execute();
			}
		}
		PS4EEEventProfile::s_sampling.enabled = false;
		PS4EEHotspots::Reset(false);
		PS4EEHotspots::ResetBlockSampling(false);
		PS4EEEventProfile::s_sampling.gap_pending = false;
		ForjaSessionWrite(true); // Forja 1.06: the summary's last version, before the VM goes
		ProbeLog("STAGE: VMManager::Shutdown\n");
		PS4Telemetry::Stage("vm-shutdown");
		if (s_log_direct_boot) // compilation 67: a game closed before its first heartbeat
		{
			s_log_direct_boot = false;
			PS4ProbeLogger::SetDirect(false);
		}
		ForjaCodeAheadStore(true); // compilation 132
		ForjaSkipRemember(); // compilation 139
		g_forja_guard_mark[0] = '\0'; // compilation 131: the game is over; what happens from here on is not its fault
		VMManager::Shutdown(false);
		// Forja SX2 (compilation 37): a normal end: the 50 Hz output worked or was never asked for.
		PS4Port::g_output_hz_request.store(0, std::memory_order_relaxed);
		unlink("/data/ForjaSX2/salida-50hz-en-prueba");
	}
	else
	{
		if (s_use_vulkan_selector)
			PS4GameMenu::HideLoading();
		ProbeLog("VM initialization failed: %s\n", error.GetDescription().c_str());
		s_log_direct_boot = false; // compilation 67
		PS4ProbeLogger::SetDirect(false);
		PS4Telemetry::Stage("vm-init-failed");
		// Compilation 59: the library that opens again (a new process) says why, from this file (GameMenu.cpp). Until now a game
		// that could not start, most often for want of a PS2 BIOS, just went back to the library without a word.
		if (std::FILE* reason = std::fopen("/data/ForjaSX2/ultimo-error.txt", "w"))
		{
			std::fprintf(reason, "%s\n", error.GetDescription().c_str());
			std::fclose(reason);
		}
		// A failed mount is not an exit request. Reuse the clean-process return
		// path, since Initialize may already have shut down the shared GS device.
		s_return_to_main_menu.store(true, std::memory_order_release);
	}
	VMManager::Internal::CPUThreadShutdown();
	ProbeLog("VM lifecycle finished\n");
	return nullptr;
}

// Forja SX2: watchdog. Joseph once got a black screen on the way back to the library and the log just
// stopped, with nothing to say what was stuck. This low-priority helper looks every 2 s at two heartbeats:
// the library loop (GameMenu.cpp) while it is open, and GS vsyncs while a game runs. If one stands still
// for 8 s it logs once where the EE and IOP are and how the GS queue looks, so the next hang names itself.
std::atomic<u64> g_forja_menu_ticks{0};
std::atomic<bool> g_forja_menu_active{false};
// Forja SX2: GitHub Actions' run number (CMakeLists.txt); 0 for a local build.
#ifndef FORJA_BUILD_NUMBER
#define FORJA_BUILD_NUMBER 0
#endif
#ifndef FORJA_VERSION_MAJOR
#define FORJA_VERSION_MAJOR 1
#define FORJA_VERSION_MINOR 0
#endif
extern std::atomic<u64> g_forja_gs_vsyncs; // ps4/core/GSRendererPS4.cpp

// Forja SX2 (compilation 69): the library is up (GameMenu.cpp): the log goes back to its queue, unless a game is starting.
void ForjaStartupLogDone()
{
	static bool s_done = false;
	if (s_done || s_log_direct_boot)
		return;
	s_done = true;
	PS4ProbeLogger::SetDirect(false);
	ProbeLog("PS4 log: back to its queue (the library is up)\n");
}

// Forja SX2 (compilation 67): the last game's start died while its sound opened (ps4/core/AudioStreamPS4.cpp,
// FORJA_AUDIO_TRIAL): the next mode, and the library says so (GameMenu.cpp reads ultimo-error.txt).
static void ForjaCheckAudioTrial()
{
	std::FILE* trial = std::fopen("/data/ForjaSX2/audio-en-prueba", "r");
	if (!trial)
		return;
	int mode = 0;
	if (std::fscanf(trial, "%d", &mode) != 1)
		mode = 0;
	std::fclose(trial);
	unlink("/data/ForjaSX2/audio-en-prueba");
	const int next = std::clamp(mode + 1, 1, 2);
	if (std::FILE* file = std::fopen("/data/ForjaSX2/audio-modo", "w"))
	{
		std::fprintf(file, "%d\n", next);
		std::fclose(file);
	}
	if (std::FILE* reason = std::fopen("/data/ForjaSX2/ultimo-error.txt", "w"))
	{
		std::fprintf(reason, "forja-audio-%d\n", next);
		std::fclose(reason);
	}
	ProbeLog("Audio trial: the last start died opening the sound in mode %d; audio-modo is now %d (%s)\n", mode, next,
		next == 1 ? "the simple opening" : "games without sound");
}

static void* ForjaWatchdog(void*)
{
	PS4ThreadAffinity::PinCurrentThreadToHelpers("watchdog");
	u64 last_menu = 0, last_vsyncs = 0;
	unsigned menu_still = 0, game_still = 0;
	bool menu_reported = false, game_reported = false;
	for (;;)
	{
		Threading::Sleep(2000);
		const u64 menu = g_forja_menu_ticks.load(std::memory_order_relaxed);
		if (g_forja_menu_active.load(std::memory_order_relaxed) && menu == last_menu)
		{
			if (++menu_still >= 4 && !menu_reported)
			{
				menu_reported = true;
				ProbeLog("Watchdog: the library loop has not turned for %u s (ticks=%llu, vsync queue=%d)\n",
					menu_still * 2, static_cast<unsigned long long>(menu), MTGS::GetCurrentVsyncQueueSize());
			}
		}
		else
		{
			menu_still = 0;
			menu_reported = false;
		}
		last_menu = menu;

		const u64 vsyncs = g_forja_gs_vsyncs.load(std::memory_order_relaxed);
		if (VMManager::GetState() == VMState::Running && !s_game_menu_open && vsyncs == last_vsyncs)
		{
			if (++game_still >= 4 && !game_reported)
			{
				game_reported = true;
				ProbeLog("Watchdog: no GS vsync for %u s while running: EE_PC=%08x IOP_PC=%08x EE_cycle=%u vsync queue=%d\n",
					game_still * 2, cpuRegs.pc, psxRegs.pc, static_cast<unsigned>(cpuRegs.cycle),
					MTGS::GetCurrentVsyncQueueSize());
			}
		}
		else
		{
			game_still = 0;
			game_reported = false;
		}
		last_vsyncs = vsyncs;
	}
	return nullptr;
}

static void StartForjaWatchdog()
{
	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0)
		return;
	pthread_attr_setstacksize(&attr, 128 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	const int result = pthread_create(&thread, &attr, ForjaWatchdog, nullptr);
	pthread_attr_destroy(&attr);
	ProbeLog("Watchdog: %s\n", result == 0 ? "started" : "could not start");
}

int main()
{
	mkdir("/data/ForjaSX2", 0777);
	// (AI-assisted) Keep all runtime diagnostics together for FTP collection.
	mkdir("/data/ForjaSX2/logs", 0777);
	const int migrated_logs = MigrateLegacyLogs();
	const auto archived_logs = PS4SessionLogs::ArchivePrevious();
	// If archiving fails, append rather than destroying the remaining evidence.
	const int log_flags = O_WRONLY | O_CREAT | (archived_logs.error ? O_APPEND : O_TRUNC);
	// (AI-assisted) Redirect descriptors without reopening or altering libc FILE
	// objects. The GS diagnostics use ProbeLog directly; needs proper testing.
	const char* stream_paths[] = {"/data/ForjaSX2/logs/PCSX2-vm-stdout.log", "/data/ForjaSX2/logs/PCSX2-vm-stderr.log"};
	for (int stream = 0; stream < 2; stream++)
	{
		const int fd = open(stream_paths[stream], log_flags, 0666);
		if (fd >= 0)
		{
			if (fd != stream + 1)
			{
				dup2(fd, stream + 1);
				close(fd);
			}
		}
	}
	s_log = open("/data/ForjaSX2/logs/PCSX2-vm-boot.log", log_flags, 0666);
	s_verbose_diag = access("/data/ForjaSX2/vm-verbose-log", F_OK) == 0;
	const bool synchronous_log = access("/data/ForjaSX2/vm-sync-log", F_OK) == 0 || s_verbose_diag;
	const bool kernel_log = access("/data/ForjaSX2/vm-kernel-log", F_OK) == 0 || s_verbose_diag;
	const int log_thread_result = PS4ProbeLogger::Start(s_log, synchronous_log, kernel_log);
	// Compilation 69: line by line until the library is up (GameMenu.cpp, ForjaStartupLogDone): a start that dies before it
	// keeps its last step (the PS4s with firmware 9.00 that close at the loading picture, 2026-10-20).
	PS4ProbeLogger::SetDirect(true);
	// Live telemetry to PC debug server (reads /data/ForjaSX2/telemetry.conf). Needs proper testing.
	const bool telemetry_started = PS4Telemetry::Start();
	// Record entry before module loading, so a stalled re-launch still has a startup marker.
	// Compilation 121: and whether the build went through ThinLTO (ps4/vm/CMakeLists.txt, FORJA_LTO_ON).
#ifndef FORJA_LTO_ON
#define FORJA_LTO_ON 0
#endif
	ProbeLog("Forja SX2 %d.%02d (base PS4SX2 1.00) BUILD: FORJASX2-%d.%02d compilacion=%d lto=%d\n", FORJA_VERSION_MAJOR,
		FORJA_VERSION_MINOR, FORJA_VERSION_MAJOR, FORJA_VERSION_MINOR, FORJA_BUILD_NUMBER, FORJA_LTO_ON);
	ProbeLog("Telemetry: start=%d\n", telemetry_started ? 1 : 0);
	ForjaApplyBase(); // compilation 146: after the log is up
	ForjaApplyNative147(); // 147 in the app: the PS2 CPU native for every game
	ForjaApplyDeinterlace149(); // 149 in the app: a game's measured deinterlace back
	ForjaApplyRespaldo150(); // 150 in the app: the CPU "native with a fallback" in every game
	// Compilation 78: whether this build sends the players' reports (ForjaReport.cpp), so a log says it without trying one.
	ProbeLog("Reports: %s\n", ForjaReport::HasChannel() ? "the reports channel is set (Reportar este juego sends to Forja's Discord)" :
															  "no reports channel in this build (reports are kept in /data/ForjaSX2/reportes)");
	PS4Telemetry::Stage("boot");
	ProbeLog("Telemetry: PC debug server target from /data/ForjaSX2/telemetry.conf (create with 'host=PC_IP port=19423')\n");
	ProbeLog("Log directory: /data/ForjaSX2/logs migrated=%d\n", migrated_logs);
	ProbeLog("Session archive: directory=%s moved=%d errno=%d history_files=%d (logs/sesiones, last %d sessions)\n",
		archived_logs.directory, archived_logs.moved, archived_logs.error, archived_logs.history, PS4SessionLogs::HISTORY_KEEP);
	ProbeLog("PS4 log startup: fd=%d synchronous_marker=%d kernel_mirror=%d thread_result=%d verbose=%d\n", s_log, synchronous_log, kernel_log, log_thread_result, s_verbose_diag ? 1 : 0);
	PS4ProbeLogger::LogStatus();
	StartForjaWatchdog();
	ForjaCheckAudioTrial(); // compilation 67
	PS4Telemetry::Stage("loading-modules");
	ProbeLog("STAGE: loading system modules\n");
	const int system_service_result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
	// (AI-assisted) Load VideoOut before the shared Vulkan menu/game device.
	const int video_out_result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
	Log::SetHostOutputLevel(LOGLEVEL_DEV, ConsoleMessage);
	// (AI-assisted) v0.72 still queried the host clock 17–19k times/s in GoW FMV.
	// At the measured ~240M EE cycles/s, 131072 cycles span ~0.55 ms. The
	// call limit also allows progress without guest cycles; input latency on
	// much slower titles needs proper console testing. Paused VMs bypass this gate.
	// v0.86: 512/262144 halves that again and reduces per-event overhead in FMV.
	// v0.87: 1024/524288 halves Host_clock_checks again (FMV ~1.2k -> ~0.6k/s)
	// and benefits all games during busy loops. Needs proper testing on input latency and slow titles.
	// (AI-assisted) Fase 2: HostPollGate 1024/524288 -> 2048/1M. FMV loops hit VMHostPoll ~600k/s;
	// doubling the gate halves Host_clock_checks and CPU queue fences; input stays 100 Hz via 10ms throttle. Needs proper testing.
	const bool legacy_host_poll = access("/data/ForjaSX2/vm-legacy-host-poll", F_OK) == 0;
	const u32 host_poll_calls = legacy_host_poll ? 64 : 2048;
	const u32 host_poll_cycles = legacy_host_poll ? 32768 : 1048576;
	s_host_poll_gate = HostPollGate(host_poll_calls, host_poll_cycles);
	ProbeLog("Host clock gate: max_calls=%u cycle_interval=%u input_interval_ms=10 legacy_marker=%d\n",
		host_poll_calls, host_poll_cycles, legacy_host_poll);
	ProbeLog("Launch profile: PAID=0x3800000000000011 APP_TYPE=1 ATTRIBUTE=0 CATEGORY=gd\n");
	{
		// Compilation 69: the console's system software and Pro mode, for the reports from other firmwares (both calls exist in
		// 9.00 and earlier). The CPU mode is the package's: the six cores of every app since this compilation (ATTRIBUTE 0).
		OrbisKernelSwVersion sw{};
		sw.Size = sizeof(sw);
		const int sw_result = sceKernelGetSystemSwVersion(&sw);
		char version[sizeof(sw.VersionString) + 1] = {};
		if (sw_result == 0)
			std::memcpy(version, sw.VersionString, sizeof(sw.VersionString));
		s_console_system = version; // Forja 1.06
		s_console_neo = sceKernelIsNeoMode();
		s_console_has_neo = sceKernelHasNeoMode(); // compilation 87
		s_console_authentic_neo = sceKernelIsAuthenticNeo();
		ProbeLog("Console: system software '%s' (0x%08x, result %#x), Pro mode %d, Pro console %d/%d, CPU mode 6 (ATTRIBUTE 0)\n",
			version, sw_result == 0 ? sw.Version : 0u, static_cast<unsigned>(sw_result), sceKernelIsNeoMode(), s_console_has_neo,
			s_console_authentic_neo);
	}
	ProbeLog("Vulkan preflight: SystemService=%#x VideoOut=%#x\n", system_service_result, video_out_result);
	{
		MemorySettingsInterface folder_settings;
		ProbeLog("STAGE: emulator directories before launcher\n");
		PrepareEmulatorFolders(folder_settings);
	}
	// (AI-assisted) Never initialize RADV under the mini-app memory profile
	// observed in v0.49. This guard runs before CPUThreadInitialize or any GPU map.
	const size_t direct_memory = sceKernelGetDirectMemorySize();
	ProbeLog("Vulkan preflight: direct memory pool=%zu MiB\n", direct_memory / (1024 * 1024));
	if (direct_memory < 1024ULL * 1024 * 1024)
	{
		ProbeLog("Vulkan startup BLOCKED: application memory profile is too small; no GPU initialized\n");
		Log::SetHostOutputLevel(LOGLEVEL_NONE, nullptr);
		PS4ProbeLogger::Stop();
		if (s_log >= 0)
			fsync(s_log);
		sceSystemServiceLoadExec("exit", nullptr);
		return 1;
	}
	// Old one-shot files must not bypass the library after this package update.
	unlink("/data/ForjaSX2/vm-launch-request");
	unlink("/data/ForjaSX2/vm-launch-request.tmp");
	s_use_vulkan_selector = true;
	ProbeLog("STAGE: unified Vulkan library and VM; SDL/Piglet disabled\n");
	{
		// A new process must reopen both services and its own controller handle.
		sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
		sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
		OrbisUserServiceUserId user = 0;
		int user_result = sceUserServiceGetInitialUser(&user);
		if (user_result != 0)
		{
			OrbisUserServiceInitializeParams params{};
			params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
			const int init_result = sceUserServiceInitialize(&params);
			ProbeLog("Vulkan input: UserService init=%#x\n", init_result);
			user_result = sceUserServiceGetInitialUser(&user);
		}
		if (s_pad < 0)
		{
			const int pad_result = scePadInit();
			if (user_result == 0)
			{
				s_pad = scePadOpen(user, 0, 0, nullptr);
				s_pad1_user = user;
			}
			ProbeLog("Vulkan input: pad init=%#x user=%#x handle=%d\n", pad_result, user_result, s_pad);
		}
	}
	int rc = 0;
	s_return_to_main_menu.store(false, std::memory_order_release);
	pthread_attr_t attr;
	pthread_t cpu_thread;
	rc = pthread_attr_init(&attr);
	if (rc == 0)
	{
		// Forja SX2: 8 MiB (was 16), Linux's default and 4x upstream PCSX2's CPU thread (EMU_THREAD_STACK_SIZE). The
		// PS4 takes the whole stack out of its regular flexible memory when the thread starts.
		orbis_stage("before the CPU thread (8 MiB stack)");
		rc = pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
		if (rc == 0)
			rc = pthread_create(&cpu_thread, &attr, RunVM, nullptr);
		pthread_attr_destroy(&attr);
		if (rc == 0)
			pthread_join(cpu_thread, nullptr);
	}
	if (rc != 0)
		ProbeLog("CPU thread failed: %d\n", rc);
	if (s_pad >= 0)
		scePadClose(s_pad);
	if (s_pad2 >= 0)
		scePadClose(s_pad2);
	const bool relaunch_main_menu = rc == 0 && s_return_to_main_menu.load(std::memory_order_acquire);
	Log::SetHostOutputLevel(LOGLEVEL_NONE, nullptr);
	PS4Telemetry::Stage(relaunch_main_menu ? "exit-relaunch" : "exit");
	PS4Telemetry::Stop();
	PS4ProbeLogger::Stop();
	if (s_log >= 0)
	{
		fsync(s_log);
		close(s_log);
	}
	if (relaunch_main_menu)
	{
		// VM teardown closes Vulkan/VideoOut resources. Reinitializing them in the
		// same process was the source of the return-to-menu crash; ask the shell to
		// start a clean selector process instead.
		// Forja SX2 (2026-10-19): the library that starts knows it is the same session coming back from a game, so it does
		// not ask for the language again (GameMenu.cpp reads and deletes this).
		if (std::FILE* marker = std::fopen("/data/ForjaSX2/volver-biblioteca", "w"))
		{
			std::fprintf(marker, "%lld\n", static_cast<long long>(std::time(nullptr)));
			std::fclose(marker);
		}
		sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
	}
	sceSystemServiceLoadExec("exit", nullptr);
	// Forja SX2 (compilation 59): out without the static destructors. Returning from main ran them while the detached threads
	// (audio, SPU2 mix, the watchdogs) could still go on, and a lock on a mutex already destroyed throws std::system_error. The
	// logs are closed above; stdout and stderr (their own logs) are flushed.
	std::fflush(stdout);
	std::fflush(stderr);
	_exit(rc == 0 ? 0 : 1);
}
