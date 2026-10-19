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
#include "GameList.h"
#include "PerformanceMetrics.h"
#include "common/Timer.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "R3000A.h"
#include "IPU/IPU.h"
#include "IPU/IPU_MultiISA.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Pad/PadDualshock2.h"
#include <sys/stat.h>
#include "CDVD/CDVD.h"
#include "ps4/core/VulkanVideoOut.h"
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
static bool s_console_fast_cpu = false;

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
static unsigned s_video_busy_checks = 0;
static constexpr int VIDEO_EE_RATE = -3;
// Compilation 49: Modo video "Auto" ([Forja] VideoMode = 2, and the global "Sí" for a game whose profile does not set it).
// Compilation 51: it looks for the EE rate that shows the most movie pictures a second, from 31 to 100 %. The pictures come
// from the game's decoder, which needs EE cycles (too low a rate: fewer pictures, at full speed), while the PS4 runs the EE
// more slowly the higher the rate (too high: the whole movie slows down). Tales of the Abyss (2026-10-18): 17-20 pictures a
// second at 36 % (99 % speed), 21 at 50 % (94 %), 23 at 60 % (85 %) and 14 at 100 % (38 %); God of War II: 17-21 at 27 %,
// 23 at 60 % (92 %). The best rate differs by game and by movie, so it measures: the rate in place for 3 s (after 1.5 s to
// settle), then a step up, kept while it shows over 4 % more pictures; when the first step up does not, a step down. Then
// it stays, and measures again every 40 s. The best rate found is where the game's next movie starts. Builds 49-50 stepped
// on the speed alone, between 31 and 60 %: never above 60 %, and down for movies whose decoder was short of EE cycles.
enum class VideoProbe
{
	Measure, // the rate in place, before trying its neighbours
	Up,
	Down,
	Hold,
};
static bool s_video_auto = false;
static int s_video_auto_rate = VIDEO_EE_RATE;
static VideoProbe s_video_probe = VideoProbe::Hold;
static int s_video_probe_best = VIDEO_EE_RATE;
static double s_video_probe_best_fps = 0.0;
static double s_video_probe_since = 0.0; // when the rate being measured was put in place (or the hold began)
static double s_video_probe_fps_sum = 0.0;
static unsigned s_video_probe_samples = 0;
static bool s_video_probe_went_up = false;
static constexpr int VIDEO_AUTO_LOWEST = -5; // 31 %
static constexpr int VIDEO_AUTO_HIGHEST = 0; // 100 %
static constexpr double VIDEO_PROBE_SETTLE = 1.5;
static constexpr double VIDEO_PROBE_WINDOW = 3.0;
static constexpr double VIDEO_PROBE_GAIN = 1.04;
static constexpr double VIDEO_PROBE_HOLD = 40.0;
static constexpr double VIDEO_DECODES_PER_SECOND = 2000.0; // a movie: tens of thousands; gameplay: ~0
// Forja SX2: the running game's profile (pcsx2/ForjaGameProfile.h): its factory profile under the player's own. Read before
// the VM boots, from the library's scan of the image, and again if PCSX2 identifies another disc.
extern bool g_orbis_fg_wanted; // pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp: frame generation, read by the GS at every frame
extern bool g_orbis_fg_fluid;  // ...and its fluid mode (compilation 40: [Forja] FrameGen = 2)
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
// Forja SX2 (compilation 47): "Saltar cuadros" (UpdateGsSkip). mode: 0 off, 1 auto, 2 always; the GS reads the atomic.
extern std::atomic<bool> g_forja_gs_skip_active;     // ps4/core/GSRendererPS4.cpp
extern std::atomic<unsigned> g_forja_gs_skipped_frames;
static int s_gs_skip_mode = 0;
static double s_gs_skip_last_check = 0.0;
static double s_gs_skip_last_change = -100.0;
static double s_gs_skip_last_report = 0.0;
static unsigned s_gs_skip_slow_checks = 0;
static unsigned s_gs_skip_room_checks = 0;
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
}

// (AI-assisted) Called only with the VM paused. Device limits and GSConfig belong
// to the GS thread; wait for that snapshot before showing the applied value.
static void ReadGameResolution()
{
	MTGS::RunOnGSThread([]() {
		s_max_game_upscale = (g_gs_device && GSIsHardwareRenderer()) ?
			std::clamp(GSGetMaxUpscaleMultiplier(g_gs_device->GetMaxTextureSize()), 1u, 4u) : 1u;
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

// Forja SX2 (compilation 58): "Avance rápido" (L3+R3 > Partida), an idea from M33: the frame limiter off while it's on, for
// loading screens and dialogues, as fast as the PS4 can run the game. Not saved; a game starts with it off.
static bool s_fast_forward = false;

static void PublishFastForwardText()
{
	PS4GameMenu::SetRowText(PS4GameMenu::ROW_FAST_FORWARD,
		s_fast_forward ? ForjaLang::T("Avance rápido:  < Sí >") : ForjaLang::T("Avance rápido:  < No >"),
		ForjaLang::T("X: el juego corre lo más rápido que aguante la PS4 (cargas, diálogos). No se guarda"));
}

static void ToggleFastForward()
{
	s_fast_forward = !s_fast_forward;
	VMManager::SetLimiterMode(s_fast_forward ? LimiterModeType::Unlimited : LimiterModeType::Nominal);
	ProbeLog("Fast forward: %s\n", s_fast_forward ? "on" : "off");
	PublishFastForwardText();
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
				ProbeLog("Hang watch: frames again after %u s without one\n", still);
			last_frame = frame;
			still = 0;
			reported = false;
			continue;
		}
		if (++still < 10 || reported)
			continue;
		reported = true;
		char when[32] = "?";
		const std::time_t now = std::time(nullptr);
		if (const std::tm* tm = std::localtime(&now))
			std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
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
	pthread_t thread{};
	if (pthread_create(&thread, nullptr, HangWatch, nullptr) == 0)
		pthread_detach(thread);
	else
		ProbeLog("Hang watch: the thread could not start\n");
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
	EmuConfig.GS.LoadTextureReplacements = s_game_textures != 0;
	EmuConfig.GS.PrecacheTextureReplacements = s_game_textures == 2;
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
		T("X: buscarlos otra vez en GitHub (con internet). Valen desde el próximo arranque. Sistema > Restablecer vuelve a ellos"));
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
static void LogGameSettings(const char* why)
{
	ProbeLog("Game settings (%s): ee_rate=%d frame_gen=%d upscale=%u sharpness=%u hd_textures=%u vu1_left_out=[%s] gs_skip=%d "
			 "gs_left_out=[%s] huellas=[%s]\n", why,
		static_cast<int>(EmuConfig.Speedhacks.EECycleRate), g_orbis_fg_wanted ? (g_orbis_fg_fluid ? 2 : 1) : 0, s_game_upscale,
		s_game_sharpness, s_game_textures, Vu1SkipList().c_str(), s_gs_skip_mode, GsFilterList().c_str(), RulesOnList().c_str());
}

static void PublishGameMenu()
{
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
	return origin == ForjaGameProfile::Origin::Player ? "player" : origin == ForjaGameProfile::Origin::Factory ? "factory" : "global";
}

// Forja SX2: makes the game's profile current: video mode, frame generation and cores. Its PCSX2 keys are not here: PCSX2
// reads them itself, as the game settings layer (VMManager::UpdateGameSettingsLayer).
static void ApplyGameProfile(ForjaGameProfile::Values profile)
{
	s_profile = std::move(profile);
	s_video_mode_enabled = s_profile.video_mode >= 0 ? s_profile.video_mode != 0 : s_global_video_mode;
	s_video_ee_rate = s_profile.video_ee_rate <= 0 ? s_profile.video_ee_rate : VIDEO_EE_RATE;
	// Compilation 49: auto when the profile says 2, or when it leaves the video mode to the Settings page's "Sí".
	{
		const bool video_auto = s_video_mode_enabled && (s_profile.video_mode == 2 || s_profile.video_mode < 0);
		// Compilation 51: it starts from the fixed rate last chosen for the game (its VideoEECycleRate), 31 to 100 %.
		if (video_auto && !s_video_auto)
			s_video_auto_rate = std::clamp(s_video_ee_rate, VIDEO_AUTO_LOWEST, VIDEO_AUTO_HIGHEST);
		s_video_auto = video_auto;
	}
	const int frame_gen = s_profile.frame_gen >= 0 ? s_profile.frame_gen : (s_global_frame_gen ? 1 : 0);
	g_orbis_fg_wanted = !s_frame_gen_blocked && frame_gen != 0;
	g_orbis_fg_fluid = frame_gen == 2;
	// Compilation 45: a game without its own CpuAuto (player or factory) takes the Settings page's.
	s_cpu_auto_floor = s_profile.cpu_auto_origin == ForjaGameProfile::Origin::Global ? s_global_cpu_auto : s_profile.cpu_auto;
	// Compilation 47: "Saltar cuadros". Always on at 2; auto (1) starts off and UpdateGsSkip turns it on when the GS holds the game back.
	{
		const int gs_skip = s_profile.gs_skip >= 0 ? s_profile.gs_skip : 0;
		if (gs_skip != s_gs_skip_mode)
		{
			s_gs_skip_mode = gs_skip;
			s_gs_skip_slow_checks = s_gs_skip_room_checks = 0;
			g_forja_gs_skip_active.store(gs_skip == 2, std::memory_order_relaxed);
		}
	}
	PS4ThreadAffinity::SetGamePlan(s_profile.cores.c_str());
	ApplyVu1SkipList(s_profile.vu1_skip); // compilation 44: the VU1 filter
	ApplyGsFilterList(s_profile.gs_filter); // compilation 51: the GS filter
	ApplyRulesOn(s_profile.rules_on); // compilation 55: the fingerprint rules the player turned on
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
	rate = std::clamp(rate, -6, 3);
	SavePlayerProfile("EE cycle rate", [rate](INISettingsInterface& ini) { ini.SetIntValue("EmuCore/Speedhacks", "EECycleRate", rate); },
		true);
	ProbeLog("EE cycle rate: requested=%d active=%d\n", rate, s_game_ee_rate);
}

// Forja SX2: "Restablecer este juego". The player's profile becomes a .bak; the factory profile and the global settings stay.
static void ResetGameProfile()
{
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	const bool reset = ForjaGameProfile::ResetPlayer(serial, crc);
	const bool reloaded = VMManager::ReloadGameSettings();
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	ReadGameResolution();
	s_video_active = false;
	s_cpu_auto_steps = 0;
	s_game_ee_rate = EmuConfig.Speedhacks.EECycleRate;
	ApplyGameProfile(ForjaGameProfile::Load(serial, crc));
	RepinWorkersForProfile();
	ProbeLog("Game profile: reset=%d reloaded=%d upscale=%ux aspect=%u ee_rate=%d\n", reset ? 1 : 0, reloaded ? 1 : 0,
		s_game_upscale, s_game_aspect, s_game_ee_rate);
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
				EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(s_video_saved_rate);
				Cpu->Reset();
			}
			ProbeLog("Video mode: turned off for this game, EE rate back to %d\n", s_video_saved_rate);
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

	// Compilation 51: the auto mode's measuring (VideoProbe), while a movie plays. The pictures a second are the game's own
	// (PerformanceMetrics, over the last half second of real time), the same "Game" FPS the overlay shows.
	if (s_video_active && s_video_auto)
	{
		if (s_video_probe == VideoProbe::Hold)
		{
			if (elapsed - s_video_probe_since >= VIDEO_PROBE_HOLD)
			{
				s_video_probe = VideoProbe::Measure;
				s_video_probe_since = elapsed - VIDEO_PROBE_SETTLE; // same rate: nothing to settle
				s_video_probe_fps_sum = 0.0;
				s_video_probe_samples = 0;
			}
		}
		else if (elapsed - s_video_probe_since >= VIDEO_PROBE_SETTLE)
		{
			s_video_probe_fps_sum += PerformanceMetrics::GetInternalFPS();
			s_video_probe_samples++;
			if (elapsed - s_video_probe_since >= VIDEO_PROBE_SETTLE + VIDEO_PROBE_WINDOW)
			{
				const int rate = EmuConfig.Speedhacks.EECycleRate;
				const double fps = s_video_probe_fps_sum / s_video_probe_samples;
				const bool better = fps > s_video_probe_best_fps * VIDEO_PROBE_GAIN;
				const VideoProbe phase = s_video_probe;
				int next = rate;
				switch (phase)
				{
					case VideoProbe::Measure:
						s_video_probe_best = rate;
						s_video_probe_best_fps = fps;
						s_video_probe_went_up = false;
						if (rate < VIDEO_AUTO_HIGHEST)
						{
							next = rate + 1;
							s_video_probe = VideoProbe::Up;
						}
						else if (rate > VIDEO_AUTO_LOWEST)
						{
							next = rate - 1;
							s_video_probe = VideoProbe::Down;
						}
						else
						{
							s_video_probe = VideoProbe::Hold;
						}
						break;
					case VideoProbe::Up:
						if (better)
						{
							s_video_probe_best = rate;
							s_video_probe_best_fps = fps;
							s_video_probe_went_up = true;
							if (rate < VIDEO_AUTO_HIGHEST)
								next = rate + 1;
							else
								s_video_probe = VideoProbe::Hold;
						}
						else if (!s_video_probe_went_up && s_video_probe_best > VIDEO_AUTO_LOWEST)
						{
							next = s_video_probe_best - 1;
							s_video_probe = VideoProbe::Down;
						}
						else
						{
							next = s_video_probe_best;
							s_video_probe = VideoProbe::Hold;
						}
						break;
					case VideoProbe::Down:
						if (better)
						{
							s_video_probe_best = rate;
							s_video_probe_best_fps = fps;
							if (rate > VIDEO_AUTO_LOWEST)
								next = rate - 1;
							else
								s_video_probe = VideoProbe::Hold;
						}
						else
						{
							next = s_video_probe_best;
							s_video_probe = VideoProbe::Hold;
						}
						break;
					case VideoProbe::Hold:
						break;
				}
				static constexpr const char* phase_names[] = {"measure", "up", "down", "hold"};
				ProbeLog("Video mode: auto %s at %d: %.1f pictures/s (speed %.0f %%), best %d (%.1f) -> %d\n",
					phase_names[static_cast<int>(phase)], rate, fps, PerformanceMetrics::GetSpeed(), s_video_probe_best,
					s_video_probe_best_fps, next);
				s_video_auto_rate = s_video_probe_best;
				s_video_probe_since = elapsed;
				s_video_probe_fps_sum = 0.0;
				s_video_probe_samples = 0;
				if (next != rate)
				{
					EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(next);
					Cpu->Reset();
				}
			}
		}
	}

	// On after two busy checks in a row (~1 s of movie); off 2 s after the last busy check, and never
	// sooner than 3 s after switching on, so short decode bursts (loading screens) don't flip it back and forth.
	bool want = s_video_active;
	if (!s_video_active && s_video_busy_checks >= 2)
		want = true;
	else if (s_video_active && elapsed - s_video_last_busy >= 2.0 && elapsed - s_video_since >= 3.0)
		want = false;
	if (want == s_video_active)
		return;

	if (want)
	{
		const int video_rate = s_video_auto ? s_video_auto_rate : s_video_ee_rate;
		s_video_saved_rate = EmuConfig.Speedhacks.EECycleRate;
		s_video_since = elapsed;
		s_video_probe = VideoProbe::Measure;
		s_video_probe_since = elapsed;
		s_video_probe_fps_sum = 0.0;
		s_video_probe_samples = 0;
		s_video_probe_best = video_rate;
		s_video_probe_best_fps = 0.0;
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
	Cpu->Reset();
	ProbeLog("Video mode: %s (%.0f decodes/s), EE rate now %d (game's own %d)\n", want ? "ON" : "OFF", per_second,
		static_cast<int>(EmuConfig.Speedhacks.EECycleRate), s_video_saved_rate);
}

extern double GetVerticalFrequency(); // pcsx2/GS.h (Counters.cpp): the PS2's refresh now

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
	Cpu->Reset();
	ProbeLog("CPU auto: %s at %.0f %% speed (busiest thread %.0f %%): EE rate %d -> %d (game's own %d, floor %d)\n", why, speed,
		busiest, before, rate, s_cpu_auto_base, s_cpu_auto_floor);
}

// Forja SX2 (compilation 47): "Saltar cuadros" on auto. When the GS thread is what holds the game back (the busiest one, at 90 %
// or more, with the speed under 92 % for ~3 s), the GS draws every other frame of the game (GSRendererPS4.cpp, VSync): the
// game keeps its speed, its music and its moves, with half the GS work. It draws them all again after ~8 s at full speed
// with the GS under 45 % (drawing every frame it would stay under 90 %). Dragon Ball Z Budokai Tenkaichi 4 (2026-10-18):
// ~2,100 draws a frame in its fights, 25 us each for the PS4's CPU, 31 ms a vsync for the GS, 48 % speed.
static void UpdateGsSkip(double elapsed)
{
	if (s_gs_skip_mode == 0)
		return;
	if (elapsed - s_gs_skip_last_report >= 5.0)
	{
		if (const unsigned skipped = g_forja_gs_skipped_frames.exchange(0, std::memory_order_relaxed))
			ProbeLog("GS skip: %u frames of the game not drawn in the last %.0f s\n", skipped, elapsed - s_gs_skip_last_report);
		s_gs_skip_last_report = elapsed;
	}
	if (s_gs_skip_mode != 1 || s_video_active || elapsed - s_gs_skip_last_check < 0.5)
		return;
	s_gs_skip_last_check = elapsed;
	const double speed = PerformanceMetrics::GetSpeed();
	const double gs = PerformanceMetrics::GetGSThreadUsage();
	const double others = std::max(PerformanceMetrics::GetCPUThreadUsage(), static_cast<double>(PerformanceMetrics::GetVUThreadUsage()));
	const bool active = g_forja_gs_skip_active.load(std::memory_order_relaxed);
	if (!active)
		s_gs_skip_slow_checks = (speed < 92.0 && gs >= 90.0 && gs >= others) ? s_gs_skip_slow_checks + 1 : 0;
	else
		s_gs_skip_room_checks = (speed >= 98.0 && gs < 45.0) ? s_gs_skip_room_checks + 1 : 0;
	if (elapsed - s_gs_skip_last_change < 6.0)
		return;
	if (!active && s_gs_skip_slow_checks >= 6)
	{
		g_forja_gs_skip_active.store(true, std::memory_order_relaxed);
		ProbeLog("GS skip: on at %.0f %% speed (GS %.0f %%, EE/VU1 %.0f %%): one frame in two\n", speed, gs, others);
	}
	else if (active && s_gs_skip_room_checks >= 16)
	{
		g_forja_gs_skip_active.store(false, std::memory_order_relaxed);
		ProbeLog("GS skip: off at %.0f %% speed (GS %.0f %% drawing half): every frame again\n", speed, gs);
	}
	else
		return;
	s_gs_skip_last_change = elapsed;
	s_gs_skip_slow_checks = s_gs_skip_room_checks = 0;
	PublishGameMenu();
}

static void UpdateCpuAuto(double elapsed)
{
	if (s_video_active)
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
	const double speed = PerformanceMetrics::GetSpeed();
	const double busiest = std::max({PerformanceMetrics::GetCPUThreadUsage(), static_cast<double>(PerformanceMetrics::GetVUThreadUsage()),
		static_cast<double>(PerformanceMetrics::GetGSThreadUsage())});
	s_cpu_auto_slow_checks = speed < 92.0 ? s_cpu_auto_slow_checks + 1 : 0;
	const bool room = s_cpu_auto_steps > 0 && speed >= 98.0 &&
					  busiest * EERatePercent(rate + 1) / EERatePercent(rate) < 90.0;
	s_cpu_auto_room_checks = room ? s_cpu_auto_room_checks + 1 : 0;
	if (elapsed - s_cpu_auto_last_change < 6.0)
		return;
	if (s_cpu_auto_slow_checks >= 6 && rate > s_cpu_auto_floor)
	{
		s_cpu_auto_steps++;
		SetCpuAutoRate(rate - 1, "slow", speed, busiest);
	}
	else if (s_cpu_auto_room_checks >= 16)
	{
		s_cpu_auto_steps--;
		SetCpuAutoRate(rate + 1, "room", speed, busiest);
	}
	else
		return;
	s_cpu_auto_last_change = elapsed;
	s_cpu_auto_slow_checks = s_cpu_auto_room_checks = 0;
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
	if (crc == 0 || crc == s_profile.crc)
		return;
	const std::string serial = VMManager::GetDiscSerial();
	ProbeLog("Game profile: PCSX2 runs %s %08X, the library's scan gave %s %08X; reading its profile\n", serial.c_str(), crc,
		s_profile.serial.c_str(), s_profile.crc);
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
		case PS4GameMenu::ROW_OVERLAY:
			s_game_show_performance_overlay = !s_game_show_performance_overlay;
			ApplyPerformanceOverlay();
			break;
		case PS4GameMenu::ROW_EE_RATE:
		{
			const int requested = std::clamp(s_game_ee_rate + delta, -6, 3);
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
			SavePlayerProfile("Frame generation", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "FrameGen", next); },
				false);
			break;
		}
		case PS4GameMenu::ROW_CPU_AUTO:
		{
			// No, down to 75 %, 60 %, 50 %, 36 %, 31 % or 27 % ([Forja] CpuAuto 0, -1 ... -6). UpdateCpuAuto takes the change from here.
			const int next = (-s_cpu_auto_floor + delta + 7) % 7;
			SavePlayerProfile("CPU auto", [next](INISettingsInterface& ini) { ini.SetIntValue("Forja", "CpuAuto", -next); }, false);
			break;
		}
		case PS4GameMenu::ROW_GS_SKIP:
		{
			// No, auto, one frame in two ([Forja] SaltarCuadros 0, 1, 2; compilation 47). ApplyGameProfile takes it from the file.
			const int next = (s_gs_skip_mode + delta + 3) % 3;
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
}

// Forja SX2: "Guardar estado". The menu has the VM paused, outside VMManager::Execute, so the state is consistent; it is
// zipped on this thread (about a second) so the row can say at once how it went.
static void SaveGameState()
{
	const s32 slot = static_cast<s32>(s_state_slot + 1);
	bool failed = false;
	std::string failure;
	const auto t0 = Common::Timer::GetCurrentValue();
	VMManager::SaveStateToSlot(slot, false, [&failed, &failure](const std::string& error) {
		failed = true;
		failure = error;
	});
	s_state_action = failed ? PS4GameMenu::STATE_SAVE_FAILED : PS4GameMenu::STATE_SAVED;
	RefreshStateSlot();
	ProbeLog("Save state: slot %d %s in %.0f ms %s\n", slot, failed ? "FAILED" : "saved",
		Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - t0), failure.c_str());
}

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
	Error error;
	const auto t0 = Common::Timer::GetCurrentValue();
	const bool loaded = VMManager::LoadStateFromSlot(slot, false, &error);
	if (!loaded)
		s_state_action = PS4GameMenu::STATE_LOAD_FAILED;
	ProbeLog("Save state: slot %d %s in %.0f ms %s\n", slot, loaded ? "loaded" : "LOAD FAILED",
		Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - t0), loaded ? "" : error.GetDescription().c_str());
	return loaded;
}

// Forja SX2: "Reiniciar juego". Paused, VMManager::Reset is immediate (outside a running VM it never has to wait for a
// safe point), and the game boots again without leaving Forja.
static void RestartGame()
{
	EndVideoModeForStateChange();
	VMManager::Reset();
	ProbeLog("Emulator menu: game restarted\n");
}

static void ResumeFromGameMenu(u32 raw)
{
	s_game_menu_open = false;
	s_blocked_pad_buttons |= raw;
	PS4GameMenu::SetInGameMenu({});
	ProbeLog("Emulator menu: resume\n");
	// Forja SX2: the GS's hitch detector (GSRendererPS4.cpp) does not count the time the menu was open as a hitch.
	g_forja_resumes.fetch_add(1, std::memory_order_relaxed);
	VMManager::SetPaused(false);
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
	if (!is_paused_menu)
	{
		UpdateGameProfile(elapsed);
		UpdateVideoMode(elapsed);
		UpdateRuleShares(); // compilation 55
		UpdateCpuAuto(elapsed);
		UpdateGsSkip(elapsed);
		UpdateOutputRate(elapsed);
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
		const double interval = elapsed - s_last_log;
		const double ee_mcycles = static_cast<u32>(cpuRegs.cycle - s_last_ee_cycle) / (interval * 1000000.0);
		const double host_polls_per_second = (s_host_poll_calls - s_last_host_poll_calls) / interval;
		const double host_clock_checks_per_second = (s_host_clock_checks - s_last_host_clock_checks) / interval;
		s_last_host_clock_checks = s_host_clock_checks;
		s_last_host_poll_calls = s_host_poll_calls;
		s_last_log = elapsed;
		s_last_ee_cycle = cpuRegs.cycle;
			ProbeLog("VM heartbeat: seconds=%.1f EE_PC=%08x IOP_PC=%08x EE_cycles=%u EE_Mcycles_per_second=%.2f "
				 "Host_polls_per_second=%.0f Host_clock_checks_per_second=%.0f CPU_queue_drain_locks=%llu verbose=%d paused=%d menu=%d vsyncq=%d\n",
			elapsed, cpuRegs.pc, psxRegs.pc,
			cpuRegs.cycle, ee_mcycles, host_polls_per_second, host_clock_checks_per_second,
			static_cast<unsigned long long>(VMHostGetCPUQueueLockCount()), s_verbose_diag ? 1 : 0,
			vm_state == VMState::Paused ? 1 : 0, s_game_menu_open ? 1 : 0, MTGS::GetCurrentVsyncQueueSize());
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
		PublishFastForwardText();
		// Compilation 44: the Extras tab's other rows and Partida's discs.
		ReloadVu1Filter();
		ReloadGsFilter(); // compilation 51
		PublishTexturesText();
		PublishRecommendedText();
		ReloadDiscList();
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
						   row == PS4GameMenu::ROW_SHARPNESS ||
						   row == PS4GameMenu::ROW_CORES || row == PS4GameMenu::ROW_CPU_AUTO || row == PS4GameMenu::ROW_RESET_PROFILE ||
						   row == PS4GameMenu::ROW_GS_SKIP ||
						   row == PS4GameMenu::ROW_SAVE_STATE || row == PS4GameMenu::ROW_LOAD_STATE || row == PS4GameMenu::ROW_RESTART ||
						   row == PS4GameMenu::ROW_FAST_FORWARD || row == PS4GameMenu::ROW_CHEATS ||
						   row == PS4GameMenu::ROW_PATCH || row == PS4GameMenu::ROW_VU1_FILTER || row == PS4GameMenu::ROW_GS_FILTER ||
						   row == PS4GameMenu::ROW_TEXTURES ||
						   row == PS4GameMenu::ROW_RECOMMENDED || row == PS4GameMenu::ROW_CHANGE_DISC))
		{
			if (row == PS4GameMenu::ROW_SAVE_STATE)
				SaveGameState();
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
			else if (row == PS4GameMenu::ROW_FAST_FORWARD)
				ToggleFastForward();
			else if (row == PS4GameMenu::ROW_VU1_FILTER)
				ToggleVu1Program();
			else if (row == PS4GameMenu::ROW_GS_FILTER)
				ToggleGsGroup();
			else if (row == PS4GameMenu::ROW_RECOMMENDED)
			{
				PublishRecommendedText(true);
				ForjaPatches::FetchCommunity(VMManager::GetDiscSerial(), VMManager::GetDiscCRC(), true);
			}
			else if (row == PS4GameMenu::ROW_CHANGE_DISC)
				ChangeToSelectedDisc();
			else
				ChangeGameMenuRow(row, 1);
			PublishGameMenu();
			s_game_menu_direction = 0;
			return;
		}
		// Forja SX2 (compilation 52): Cuadrado on a filter's row brings everything it left out back.
		if ((pressed & ORBIS_PAD_BUTTON_SQUARE) != 0 && (row == PS4GameMenu::ROW_GS_FILTER || row == PS4GameMenu::ROW_VU1_FILTER))
		{
			if (row == PS4GameMenu::ROW_GS_FILTER)
				BringBackGsGroups();
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
			// Up and Down stay in the tab, wrapping around inside it.
			const unsigned tab = PS4GameMenu::TabOfRow(row);
			const unsigned first = PS4GameMenu::TAB_FIRST_ROW[tab], count = PS4GameMenu::TAB_FIRST_ROW[tab + 1] - first;
			if (navigation == ORBIS_PAD_BUTTON_UP)
				s_game_menu_selection = first + (row - first + count - 1) % count;
			else if (navigation == ORBIS_PAD_BUTTON_DOWN)
				s_game_menu_selection = first + (row - first + 1) % count;
			else if (row == PS4GameMenu::ROW_CHANGE_DISC) // compilation 44
				ChangeGameMenuRow(row, navigation == ORBIS_PAD_BUTTON_LEFT ? -1 : 1);
			else if (row == PS4GameMenu::ROW_SAVE_STATE || row == PS4GameMenu::ROW_LOAD_STATE)
			{
				s_state_slot = (s_state_slot + (navigation == ORBIS_PAD_BUTTON_LEFT ? 4u : 1u)) % 5u;
				s_state_action = PS4GameMenu::STATE_NONE;
				RefreshStateSlot();
			}
			else if (PS4GameMenu::TabOfRow(row) == 1 || PS4GameMenu::TabOfRow(row) == 2 ||
					 PS4GameMenu::TabOfRow(row) == PS4GameMenu::TAB_PATCHES ||
					 PS4GameMenu::TabOfRow(row) == PS4GameMenu::TAB_LAB) // Imagen, Rendimiento, Extras and No tocar
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
	ProbeLog("IPU profiling: %s (sampled duration, nominal ratio 1/%u)\n",
		PS4IPUProfile::s_sampling.enabled ? "enabled by opt-in marker with inclusive access sampling 1/1024" : "disabled for performance",
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
	settings->SetBoolValue("EmuCore/Speedhacks", "IntcStat", true);
	settings->SetBoolValue("EmuCore/Speedhacks", "WaitLoop", true);
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
	const bool audio_enabled = access("/data/ForjaSX2/vm-no-audio", F_OK) != 0;
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
	ProbeLog("IPU output burst: %s (up to 32 FIFO fills per DMA event, cycles preserved; disable with /data/ForjaSX2/vm-no-ipu-burst)\n",
		g_ps4_ipu0_burst ? "enabled" : "disabled by marker");
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
	g_ps4_skip_smc_checks = access("/data/ForjaSX2/vm-skip-smc", F_OK) == 0;
	ProbeLog("EE JIT manual SMC validation: %s (%s)\n",
		g_ps4_skip_smc_checks ? "relaxed (skip per-block checks, manual protection only)" : "enabled for all games (exact block checks)",
		g_ps4_skip_smc_checks ? "enabled by /data/ForjaSX2/vm-skip-smc" : "exact (reverted Fase 3-B)");
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
			ResetGsFilterList(); // compilation 54: a new game's list starts empty
			LoadRules(); // compilation 55: huellas.ini, before the profile turns its rules on
			s_fast_forward = false; // compilation 58: a game starts at its own speed
			StartHangWatch();
			ApplyGameProfile(ForjaGameProfile::Load(serial, crc));
			RepinWorkersForProfile();
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
		ProbeLog("STAGE: VMManager::Shutdown\n");
		PS4Telemetry::Stage("vm-shutdown");
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
		PS4Telemetry::Stage("vm-init-failed");
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
	// Live telemetry to PC debug server (reads /data/ForjaSX2/telemetry.conf). Needs proper testing.
	const bool telemetry_started = PS4Telemetry::Start();
	// Record entry before module loading, so a stalled re-launch still has a startup marker.
	ProbeLog("Forja SX2 %d.%02d (base PS4SX2 1.00) BUILD: FORJASX2-%d.%02d compilacion=%d\n", FORJA_VERSION_MAJOR,
		FORJA_VERSION_MINOR, FORJA_VERSION_MAJOR, FORJA_VERSION_MINOR, FORJA_BUILD_NUMBER);
	ProbeLog("Telemetry: start=%d\n", telemetry_started ? 1 : 0);
	PS4Telemetry::Stage("boot");
	ProbeLog("Telemetry: PC debug server target from /data/ForjaSX2/telemetry.conf (create with 'host=PC_IP port=19423')\n");
	ProbeLog("Log directory: /data/ForjaSX2/logs migrated=%d\n", migrated_logs);
	ProbeLog("Session archive: directory=%s moved=%d errno=%d history_files=%d (logs/sesiones, last %d sessions)\n",
		archived_logs.directory, archived_logs.moved, archived_logs.error, archived_logs.history, PS4SessionLogs::HISTORY_KEEP);
	ProbeLog("PS4 log startup: fd=%d synchronous_marker=%d kernel_mirror=%d thread_result=%d verbose=%d\n", s_log, synchronous_log, kernel_log, log_thread_result, s_verbose_diag ? 1 : 0);
	PS4ProbeLogger::LogStatus();
	StartForjaWatchdog();
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
		rc = pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
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
	return rc == 0 ? 0 : 1;
}
