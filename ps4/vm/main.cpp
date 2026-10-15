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
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
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
static u32 s_blocked_pad_buttons = 0;
static bool s_menu_chord_latched = false;
static bool s_game_menu_open = false;
static unsigned s_game_menu_selection = 0;
static unsigned s_game_upscale = 1;
static unsigned s_max_game_upscale = 1;
static unsigned s_game_aspect = 1;
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
static constexpr double VIDEO_DECODES_PER_SECOND = 2000.0; // a movie: tens of thousands; gameplay: ~0
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

// (AI-assisted) Forward the PS4 controller to the first emulated DualShock 2.
static void VMHostUpdateController(const OrbisPadData& data)
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
		const bool was_pressed = (s_previous_pad_buttons & mapping.host_mask) != 0;
		const bool is_pressed = (data.buttons & mapping.host_mask) != 0;
		if (was_pressed != is_pressed)
			Pad::SetControllerState(0, mapping.ps2_button, is_pressed ? 1.0f : 0.0f);
	}
	s_previous_pad_buttons = data.buttons;

	if (PadBase* pad = Pad::GetPad(0))
	{
		const auto left = MapStick(data.leftStick.x, data.leftStick.y);
		const auto right = MapStick(data.rightStick.x, data.rightStick.y);
		pad->SetRawAnalogs({left.first, left.second}, {right.first, right.second});
		Pad::SetControllerState(0, PadDualshock2::Inputs::PAD_L2, data.analogButtons.l2 / 255.0f);
		Pad::SetControllerState(0, PadDualshock2::Inputs::PAD_R2, data.analogButtons.r2 / 255.0f);
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
	});
	if (s_verbose_diag)
	{
		if (!MTGS::TimedWaitGS(2000, false, false, false))
			ProbeLog("pause step: ReadGameResolution TIMEOUT vsyncq=%d\n", MTGS::GetCurrentVsyncQueueSize());
	}
	else
		MTGS::WaitGS(false, false, false);
}

static void PublishGameMenu()
{
	PS4GameMenu::SetInGameMenu(s_game_menu_open, s_game_menu_selection, s_game_upscale, s_max_game_upscale,
		s_game_aspect, s_game_show_performance_overlay, s_game_ee_rate);
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

// Forja SX2: the pause menu's "PS2 CPU speed" row. The value goes to this game's settings ini
// (gamesettings/<serial>_<crc>.ini, PCSX2's own per-game file), then the game layer is reloaded so the
// recompilers pick up the new EECycleRate. ReloadGameSettings rebuilds EmuConfig from the settings
// layers, which drops the session-only resolution and aspect choices, so those are put back afterwards.
static void ApplyGameEECycleRate(int rate)
{
	rate = std::clamp(rate, -3, 3);
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	if (crc == 0)
	{
		ProbeLog("EE cycle rate: no disc CRC yet, not saved\n");
		return;
	}
	const std::string path = VMManager::GetGameSettingsPath(serial, crc);
	INISettingsInterface ini(path);
	ini.Load();
	ini.SetIntValue("EmuCore/Speedhacks", "EECycleRate", rate);
	const bool saved = ini.Save();
	const bool reloaded = VMManager::ReloadGameSettings();
	EmuConfig.GS.UpscaleMultiplier = static_cast<float>(s_game_upscale);
	EmuConfig.GS.AspectRatio = static_cast<AspectRatioType>(std::clamp(s_game_aspect, 0u, 4u));
	EmuConfig.CurrentAspectRatio = EmuConfig.GS.AspectRatio;
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	s_video_active = false; // ReloadGameSettings rebuilt EmuConfig; video mode re-arms if a movie is still on
	s_game_ee_rate = EmuConfig.Speedhacks.EECycleRate;
	ProbeLog("EE cycle rate: requested=%d active=%d saved=%d reloaded=%d file=%s\n", rate, s_game_ee_rate,
		saved ? 1 : 0, reloaded ? 1 : 0, path.c_str());
}

// Forja SX2: automatic video mode. Measured on a PS4 Pro with Mega Man X Command Mission: during a movie
// the EE thread was the limit (game code polling the IPU ~130k times a second, plus IOP/SPU2, DMA and IPU
// work), and the intro ran at 55 % speed with EECycleRate 0, 66-72 % with -1 and ~83 % with -3, while
// gameplay ran fine at any of them. So while the game decodes pictures at a movie's pace the EE runs at
// 50 %, and the game's own rate comes back when the movie ends. The rate is baked into recompiled EE
// blocks, so each switch resets the EE recompiler (recResetEE defers that to a safe point).

static void UpdateVideoMode(double elapsed)
{
	if (!s_video_mode_enabled || elapsed - s_video_last_check < 0.5)
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
		s_video_saved_rate = EmuConfig.Speedhacks.EECycleRate;
		s_video_since = elapsed;
		s_video_active = true;
		if (s_video_saved_rate <= VIDEO_EE_RATE)
		{
			ProbeLog("Video mode: movie detected (%.0f decodes/s), EE rate already %d\n", per_second, s_video_saved_rate);
			return;
		}
		EmuConfig.Speedhacks.EECycleRate = static_cast<decltype(EmuConfig.Speedhacks.EECycleRate)>(VIDEO_EE_RATE);
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
		UpdateVideoMode(elapsed);
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
	const bool chord_edge = (raw & menu_chord) == menu_chord && !s_menu_chord_latched;
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
		s_game_ee_rate = s_video_active ? s_video_saved_rate : EmuConfig.Speedhacks.EECycleRate;
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
		buttons.buttons &= ~s_blocked_pad_buttons;
	VMHostUpdateController(buttons);
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
	if (cancel || (confirm && (s_game_menu_selection < 1 || s_game_menu_selection > 4)))
	{
		s_game_menu_open = false;
		s_blocked_pad_buttons |= raw;
		PS4GameMenu::SetInGameMenu(false, 0);
		if (!cancel && s_game_menu_selection == 5)
		{
			s_return_to_main_menu.store(true, std::memory_order_release);
			ProbeLog("VM stop request: return to main menu\n");
			VMManager::SetState(VMState::Stopping);
		}
		else if (!cancel && s_game_menu_selection == 6)
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
		if (confirm && s_game_menu_selection == 3)
		{
			s_game_show_performance_overlay = !s_game_show_performance_overlay;
			ApplyPerformanceOverlay();
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
			if (navigation == ORBIS_PAD_BUTTON_UP)
				s_game_menu_selection = s_game_menu_selection == 0 ? 6 : s_game_menu_selection - 1;
			else if (navigation == ORBIS_PAD_BUTTON_DOWN)
				s_game_menu_selection = (s_game_menu_selection + 1) % 7;
			else if (s_game_menu_selection >= 1 && s_game_menu_selection <= 4)
			{
				const int delta = navigation == ORBIS_PAD_BUTTON_LEFT ? -1 : 1;
				if (s_game_menu_selection == 1)
				{
					const unsigned requested = static_cast<unsigned>(std::clamp(static_cast<int>(s_game_upscale) + delta,
						1, static_cast<int>(s_max_game_upscale)));
					if (requested != s_game_upscale)
					{
						EmuConfig.GS.UpscaleMultiplier = static_cast<float>(requested);
						MTGS::ApplySettings();
						ReadGameResolution();
						EmuConfig.GS.UpscaleMultiplier = static_cast<float>(s_game_upscale);
						ProbeLog("Internal resolution: requested=%ux applied=%ux menu_limit=%ux (session only)\n",
							requested, s_game_upscale, s_max_game_upscale);
					}
				}
				else if (s_game_menu_selection == 2)
				{
					s_game_aspect = static_cast<unsigned>((static_cast<int>(s_game_aspect) + delta + 5) % 5);
					ApplyGameAspect(s_game_aspect);
					ProbeLog("Aspect ratio: index=%u (session only)\n", s_game_aspect);
				}
				else if (s_game_menu_selection == 4)
				{
					const int requested = std::clamp(s_game_ee_rate + delta, -3, 3);
					if (requested != s_game_ee_rate)
						ApplyGameEECycleRate(requested);
				}
				else if (delta != 0)
				{
					s_game_show_performance_overlay = !s_game_show_performance_overlay;
					ApplyPerformanceOverlay();
				}
			}
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
	// Default to mild underclock (-1 = 75% cycles) for headroom; exact cycle accuracy costs ~25%
	// EE time on this CPU. Original 100% stays via marker for sensitive titles. Needs proper testing.
	int ee_cycle_rate = -1;
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
	settings->SetIntValue("SPU2/Output", "BufferMS", legacy_audio ? 50 : 70);
	settings->SetIntValue("SPU2/Output", "StretchSequenceLengthMS", legacy_audio ? 30 : 35);
	settings->SetIntValue("SPU2/Output", "StretchSeekWindowMS", legacy_audio ? 20 : 15);
	settings->SetIntValue("SPU2/Output", "StretchOverlapMS", legacy_audio ? 10 : 8);
	// Forja SX2: SoundTouch's quick seek. The stretching runs on the EE thread (AudioStream::WriteChunk) and only works while the
	// game is below full speed, which is when the EE thread has no time to spare (MMXCM's intro: 80-88 %, the SPU2 then at 15-16 %
	// of the thread against 12-13 % at full speed). /data/ForjaSX2/vm-audio-full-seek brings back the full search.
	settings->SetBoolValue("SPU2/Output", "StretchUseQuickSeek", access("/data/ForjaSX2/vm-audio-full-seek", F_OK) != 0);
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
		if (access("/data/ForjaSX2/fg-en-prueba", F_OK) == 0)
		{
			unlink("/data/ForjaSX2/fg-en-prueba");
			if (chosen.frame_gen)
			{
				chosen.frame_gen = false;
				ForjaSettings::Save(chosen);
				ProbeLog("Frame generation: turned off, the last try did not finish its first seconds (GPU hang?)\n");
			}
		}
		if (chosen.from_file)
			settings->SetIntValue("EmuCore/Speedhacks", "EECycleRate", chosen.ee_rate);
		// Console profile: until the player saves the Settings page, a PS4 Pro with Boost Mode (CPU ~2.1 GHz)
		// starts at 2x, which it holds in light games; a base PS4/Slim (1.6 GHz) keeps the native 1x.
		const int upscale = (!chosen.from_file && s_console_fast_cpu) ? 2 : chosen.upscale;
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
		s_video_mode_enabled = chosen.video_mode;
		// Frame generation (pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp, from PS5SX2): read by the GS at every new frame.
		extern bool g_orbis_fg_wanted;
		g_orbis_fg_wanted = chosen.frame_gen;
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
		PS4SessionLogs::MarkGame(platform) ? 1 : 0,
		platform == GamePlatform::PS1 ? "PS1" : platform == GamePlatform::PS2 ? "PS2" : "Other");
	PS4Telemetry::GameSelect(boot.filename.c_str(),
		platform == GamePlatform::PS1 ? "PS1" : platform == GamePlatform::PS2 ? "PS2" : "Other");
	PS4Telemetry::Stage(boot.filename.empty() ? "game-bios" : "game-selected");
	if (s_use_vulkan_selector)
		PS4GameMenu::ShowLoading(24, boot.filename.empty() ? "Preparing PS2 BIOS..." :
			boot.filename == PS4Disc::DevicePath ? "Opening physical DVD..." : "Mounting game image...");
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
		PS4GameMenu::ShowLoading(45, "Initializing virtual machine...");
	ProbeLog("STAGE: VMManager::Initialize\n");
	PS4Telemetry::Stage("vm-init");
	const auto result = VMManager::Initialize(boot, &error);
	if (result == VMBootResult::StartupSuccess)
	{
		if (s_use_vulkan_selector)
			PS4GameMenu::ShowLoading(92, "Starting PlayStation 2...");
		ProbeLog("VM runtime Fastmem=%s (after initial VTLB mappings)\n",
			EmuConfig.Cpu.Recompiler.EnableFastmem ? "on" : "off");
		ProbeLog("VM initialized: executing BIOS; no probe MMIO shadows\n");
		PS4Telemetry::Stage("vm-running");
		// (AI-assisted) A DS4 has no dedicated analog-mode switch; start the emulated DS2 in analog mode.
		Pad::SetControllerState(0, PadDualshock2::Inputs::PAD_ANALOG, 1.0f);
		Pad::SetControllerState(0, PadDualshock2::Inputs::PAD_ANALOG, 0.0f);
		ProbeLog("Controller: DualShock 2 analog mode enabled; both sticks and analog triggers forwarded.\n");
		LoadStickSensitivity();
		s_verbose_diag = access("/data/ForjaSX2/vm-verbose-log", F_OK) == 0;
		ProbeLog("Verbose diagnostics: %s (single marker /data/ForjaSX2/vm-verbose-log forces 1s heartbeat, TimedWaitGS, and GS/EE hotspot sampling)\n",
			s_verbose_diag ? "ENABLED" : "disabled");
		s_run_timer.Reset();
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
		s_previous_pad_buttons = s_previous_host_buttons = 0;
		s_blocked_pad_buttons = ~u32{0}; // Consume any launcher button still held.
		s_menu_chord_latched = s_game_menu_open = false;
		s_game_menu_selection = 0;
		ProbeLog("Controller: OPTIONS=PS2 Start, touchpad=PS2 Select, L3+R3=emulator menu\n");
		if (s_use_vulkan_selector)
		{
			PS4GameMenu::ShowLoading(100, "Listo");
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
	ProbeLog("Forja SX2 1.00 (base PS4SX2 1.00) BUILD: FORJASX2-1.00 compilacion=%d\n", FORJA_BUILD_NUMBER);
	ProbeLog("Telemetry: start=%d\n", telemetry_started ? 1 : 0);
	PS4Telemetry::Stage("boot");
	ProbeLog("Telemetry: PC debug server target from /data/ForjaSX2/telemetry.conf (create with 'host=PC_IP port=19423')\n");
	ProbeLog("Log directory: /data/ForjaSX2/logs migrated=%d\n", migrated_logs);
	ProbeLog("Session archive: directory=%s moved=%d errno=%d\n",
		archived_logs.directory, archived_logs.moved, archived_logs.error);
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
				s_pad = scePadOpen(user, 0, 0, nullptr);
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
		sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
	}
	sceSystemServiceLoadExec("exit", nullptr);
	return rc == 0 ? 0 : 1;
}
