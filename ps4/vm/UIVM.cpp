// (AI-assisted) Minimal frontend for Vulkan BIOS boot. Fullscreen menus are disabled.
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ImGuiOverlays.h"
#include "Input/InputManager.h"
#include "common/SmallString.h"
#include "common/Timer.h"
#include "Host.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"
#include "imgui_freetype.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "GameMenu.h"
#include "ForjaLang.h"
#include "ForjaSettings.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#include <vector>
#include "SessionStats.h"

extern void ProbeLog(const char* format, ...);
// Forja 1.06: HostVM.cpp, what held the last 5 s back (SessionStats.h's ForjaSession::Limit), for the HUD's brackets.
extern std::atomic<int> g_forja_limit;
// PlatformVM.cpp: a "STAGE:" line with the PS4's free flexible memory.
extern "C" void orbis_stage(const char* stage);

namespace InputRecordingUI
{
} // namespace InputRecordingUI

InputRecordingUI::InputRecordingData g_InputRecordingData{};

namespace FullscreenUI
{
	void CheckForConfigChanges(const Pcsx2Config& old_config)
	{
		(void)old_config;
	}
	void OnVMStarted()
	{
	}
	void OnVMDestroyed()
	{
	}
	void OnVMResumed()
	{
	}
	void GameChanged(std::string title, std::string path, std::string serial, u32 disc_crc, u32 crc)
	{
		(void)title;
		(void)path;
		(void)serial;
		(void)disc_crc;
		(void)crc;
	}
	void OpenPauseMenu()
	{
	}
	bool OpenAchievementsWindow()
	{
		return false;
	}
	bool OpenLeaderboardsWindow()
	{
		return false;
	}
	void ReportStateLoadError(const std::string& message, std::optional<s32> slot, bool backup)
	{
		(void)message;
		(void)slot;
		(void)backup;
	}
	void ReportStateSaveError(const std::string& message, std::optional<s32> slot)
	{
		(void)message;
		(void)slot;
	}
	bool IsAchievementsWindowOpen()
	{
		return false;
	}
	bool IsLeaderboardsWindowOpen()
	{
		return false;
	}
	void ReturnToPreviousWindow()
	{
	}
	void ReturnToMainWindow()
	{
	}
	void SetStandardSelectionFooterText(bool back_instead_of_cancel)
	{
		(void)back_instead_of_cancel;
	}
	void Render()
	{
	}
	TinyString TimeToPrintableString(time_t t)
	{
		(void)t;
		return TinyString();
	}
} // namespace FullscreenUI

// Forja SX2: the FPS box's frame generation figures (pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp, ps4/core/GSRendererPS4.cpp). Declared
// here, outside the namespace, so they name the globals.
extern std::atomic<bool> g_orbis_fg_active;
extern std::atomic<u32> g_forja_presents;

// Forja SX2 (2026-10-19, languages): the letters Gontserrat lacks, merged into it: Cyrillic (Montserrat, its own family),
// Arabic and Chinese, Japanese and Korean (Noto), subsets made by assets/fonts/fuentes.py. ImGui 1.92 rasterizes a glyph the
// first time it is drawn, so they cost only the files' memory. Two fonts: Chinese first, and Japanese first (the same
// characters are drawn the Japanese way); GameMenu.cpp draws with the second one in Japanese (g_forja_language_fonts).
ImFont* g_forja_language_fonts[2] = {};

static void AddLanguageFonts(ImFont* ui_font, const char* base_path)
{
	// stb_truetype sizes a font by its ascent-to-descent height, which is 1.219 em in Gontserrat, 1.448 in Noto CJK and 2.112
	// in Noto Arabic: each one is scaled by that ratio so its letters have the same em as Gontserrat's.
	struct Source
	{
		const char* path;
		float scale;
		std::vector<unsigned char> data;
	};
	static Source sources[] = {{"/app0/resources/fonts/Forja-Cirilico.ttf", 1.0f, {}},
		{"/app0/resources/fonts/NotoSansArabic-Forja.ttf", 2.112f / 1.219f, {}},
		{"/app0/resources/fonts/NotoSansSC-Forja.ttf", 1.448f / 1.219f, {}},
		{"/app0/resources/fonts/NotoSansJP-Forja.ttf", 1.448f / 1.219f, {}},
		{"/app0/resources/fonts/NotoSansKR-Forja.ttf", 1.448f / 1.219f, {}}};
	for (Source& source : sources)
	{
		if (std::FILE* file = std::fopen(source.path, "rb"))
		{
			std::fseek(file, 0, SEEK_END);
			const long size = std::ftell(file);
			std::rewind(file);
			source.data.resize(size > 0 ? static_cast<size_t>(size) : 0);
			if (size <= 0 || std::fread(source.data.data(), 1, source.data.size(), file) != source.data.size())
				source.data.clear();
			std::fclose(file);
		}
		ProbeLog("Language font: %s %zu bytes\n", source.path, source.data.size());
	}
	auto& io = ImGui::GetIO();
	const auto merge = [&](ImFont* into, std::initializer_list<int> order) {
		for (const int index : order)
		{
			Source& source = sources[index];
			if (source.data.empty())
				continue;
			ImFontConfig config;
			config.MergeMode = true;
			config.DstFont = into;
			config.FontDataOwnedByAtlas = false; // the same file serves both fonts
			io.Fonts->AddFontFromMemoryTTF(source.data.data(), static_cast<int>(source.data.size()), 20.0f * source.scale, &config);
		}
	};
	merge(ui_font, {0, 1, 2, 3, 4});
	ImFont* japanese = io.Fonts->AddFontFromFileTTF(base_path, 20.0f);
	if (japanese)
		merge(japanese, {0, 1, 3, 2, 4});
	g_forja_language_fonts[0] = ui_font;
	g_forja_language_fonts[1] = japanese ? japanese : ui_font;
}

namespace ImGuiManager
{
	static bool s_show_performance_hud = true;
	// This is changed on the GS thread by the pause menu, so RenderOSD does not
	// need to read a cross-thread GSConfig field for this presentation-only flag.
	void SetPerformanceHUDVisible(bool visible)
	{
		s_show_performance_hud = visible;
		ProbeLog("Performance HUD: %s (in-game menu)\n", visible ? "enabled" : "hidden");
	}
	bool InitializeFullscreenUI()
	{
		return false;
	}
	bool Initialize()
	{
		// (AI-assisted) The BIOS owns the display; ImGui supplies only the renderer context.
		ProbeLog("STAGE: ImGui context creation\n");
		ImGui::CreateContext();
		// Forja SX2: the Settings page's "FPS counter" choice, unless the old hide marker is present.
		s_show_performance_hud = access("/data/ForjaSX2/vm-hide-stats", F_OK) != 0 && ForjaSettings::Load().overlay;
		ProbeLog("Performance HUD: %s\n", s_show_performance_hud ? "enabled" : "hidden (marker or Settings page)");
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1920, 1080);
		io.DeltaTime = 1.0f / 60.0f;
		// (AI-assisted) A null loader crashes atlas initialization even without menus.
		// This font/texture path still needs proper testing on the console.
		io.Fonts->SetFontLoader(ImFontAtlasGetFontLoaderForStbTruetype());
		// (AI-assisted) Keep the approved launcher's proportional typeface on Vulkan.
		const char* font_path = "/app0/resources/fonts/Gontserrat-Regular.ttf";
		ImFont* ui_font = access(font_path, F_OK) == 0 ? io.Fonts->AddFontFromFileTTF(font_path, 20.0f) : nullptr;
		io.FontDefault = ui_font ? ui_font : io.Fonts->AddFontDefault();
		ProbeLog("Library font: %s\n", ui_font ? "Gontserrat" : "default fallback");
		if (ui_font)
			AddLanguageFonts(ui_font, font_path);
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
		ProbeLog("STAGE: ImGui first frame (stb font loader)\n");
		ImGui::NewFrame();
		ProbeLog("STAGE: ImGui initialization complete\n");
		orbis_stage("ImGui ready (fonts loaded)");
		return true;
	}
	bool InitializeFullscreenUIOnce()
	{
		return false;
	} // Headless: never intercept input, no software cursor.
	void UpdateMousePosition(float x, float y)
	{
		(void)x;
		(void)y;
	}
	bool ProcessPointerButtonEvent(InputBindingKey key, float value)
	{
		(void)key;
		(void)value;
		return false;
	}
	bool ProcessPointerAxisEvent(InputBindingKey key, float value)
	{
		(void)key;
		(void)value;
		return false;
	}
	bool ProcessHostKeyEvent(InputBindingKey key, float value)
	{
		(void)key;
		(void)value;
		return false;
	}
	bool ProcessGenericInputEvent(GenericInputBinding key, InputLayout layout, float value, u32 controller_id)
	{
		(void)key;
		(void)layout;
		(void)value;
		(void)controller_id;
		return false;
	}
	void ProcessGenericAxisEvent(GenericInputBinding negative_key, GenericInputBinding positive_key, InputLayout layout, float value, u32 controller_id)
	{
		(void)negative_key;
		(void)positive_key;
		(void)layout;
		(void)value;
		(void)controller_id;
	}
	bool HasSoftwareCursor(u32 index)
	{
		(void)index;
		return false;
	}
	void SkipFrame()
	{
		ImGui::EndFrame();
		ImGui::NewFrame();
	}
	void Shutdown(bool clear_state)
	{
		(void)clear_state;
		ImGui::DestroyContext();
	}
	void NewFrame()
	{
		ImGui::NewFrame();
	}
	void RenderOSD()
	{
		if (PS4GameMenu::Render())
			return;
		// (AI-assisted) Use the GS thread's existing samples; avoid CPU/GS sharing.
		if (!s_show_performance_hud || VMManager::GetState() != VMState::Running || PerformanceMetrics::GetFPS() <= 0.0f)
			return;
		char fields[6][192];
		// Forja SX2: with frame generation at work the box shows both the game's frames and the frames the TV gets (the game's
		// and the generated ones, counted in GSRendererPS4.cpp), measured here over half a second.
		static Common::Timer::Value s_presents_since = 0;
		static u32 s_presents_at = 0;
		static float s_shown_fps = 0.0f;
		{
			const Common::Timer::Value now = Common::Timer::GetCurrentValue();
			const u32 presents = g_forja_presents.load(std::memory_order_relaxed);
			const double seconds = Common::Timer::ConvertValueToSeconds(now - s_presents_since);
			if (s_presents_since == 0 || seconds > 2.0)
			{
				s_presents_since = now;
				s_presents_at = presents;
			}
			else if (seconds >= 0.5)
			{
				s_shown_fps = static_cast<float>((presents - s_presents_at) / seconds);
				s_presents_since = now;
				s_presents_at = presents;
			}
		}
		// 2026-10-19: in the library's language (ForjaLang.h), the figures' names; the units stay.
		if (g_orbis_fg_active.load(std::memory_order_relaxed) && PerformanceMetrics::IsInternalFPSValid() && s_shown_fps > 0.0f)
			std::snprintf(fields[0], sizeof(fields[0]), ForjaLang::TF("Juego %.0f / Pantalla %.0f FPS (FG)"), PerformanceMetrics::GetInternalFPS(),
				s_shown_fps);
		else if (PerformanceMetrics::IsInternalFPSValid())
			std::snprintf(fields[0], sizeof(fields[0]), ForjaLang::TF("Juego ~%.1f FPS"), PerformanceMetrics::GetInternalFPS());
		else
			std::snprintf(fields[0], sizeof(fields[0]), "%s", ForjaLang::T("FPS del juego: sin datos"));
		std::snprintf(fields[1], sizeof(fields[1]), ForjaLang::TF("Velocidad %.0f%%"), PerformanceMetrics::GetSpeed());
		std::snprintf(fields[2], sizeof(fields[2]), "PS2 %.1f Hz", PerformanceMetrics::GetFPS());
		const double ee_ms = PerformanceMetrics::GetCPUThreadAverageTime();
		const float gs_ms = PerformanceMetrics::GetGSThreadAverageTime();
		const float vu_ms = PerformanceMetrics::GetVUThreadAverageTime();
		if (ee_ms + gs_ms + vu_ms > 0.0)
		{
			// Forja 1.06: the one that held the last 5 s back goes in brackets (HostVM.cpp, g_forja_limit); the GPU takes the
			// PS2's rate's place when it is the one. Language-neutral, so nothing to translate (LEEME_FORJA_SX2.md explains it).
			const int limit = g_forja_limit.load(std::memory_order_relaxed);
			std::snprintf(fields[3], sizeof(fields[3]), limit == ForjaSession::LimitEE ? "[EE %.1f ms]" : "EE %.1f ms", ee_ms);
			std::snprintf(fields[4], sizeof(fields[4]), limit == ForjaSession::LimitGS ? "[GS %.1f ms]" : "GS %.1f ms", gs_ms);
			std::snprintf(fields[5], sizeof(fields[5]), limit == ForjaSession::LimitVU1 ? "[VU1 %.1f ms]" : "VU1 %.1f ms", vu_ms);
			if (limit == ForjaSession::LimitGPU)
				std::snprintf(fields[2], sizeof(fields[2]), "[GPU %.1f ms]", PerformanceMetrics::GetGPUAverageTime());
		}
		else
		{
			std::snprintf(fields[3], sizeof(fields[3]), "EE N/A");
			std::snprintf(fields[4], sizeof(fields[4]), "GS N/A");
			std::snprintf(fields[5], sizeof(fields[5]), "VU1 N/A");
		}
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f)
			return;
		const float scale = std::min(display.x / 1920.0f, display.y / 1080.0f);
		const float padding = 16.0f * scale;
		const float top = 24.0f * scale;
		float font_size = 22.0f * scale;
		ImFont* font = ImGui::GetFont();
		constexpr const char* separator = "  |  ";
		float widths[6];
		float separator_width = 0.0f;
		const auto measure = [&]() {
			separator_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, separator).x;
			float width = separator_width * 5;
			for (unsigned i = 0; i < 6; i++)
			{
				widths[i] = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, ForjaLang::Visual(fields[i])).x;
				width += widths[i];
			}
			return width;
		};
		// (AI-assisted) Measure the actual font so the whole row stays centered,
		// including unavailable counters and different output resolutions.
		float row_width = measure();
		const float available = display.x - 2.0f * (top + padding);
		if (row_width > available)
		{
			font_size *= available / row_width;
			row_width = measure();
		}
		float x = (display.x - row_width) * 0.5f;
		constexpr ImU32 colors[] = {
			IM_COL32(88, 211, 255, 255),  // FPS: cyan
			IM_COL32(113, 232, 153, 255), // Speed: green
			IM_COL32(223, 232, 245, 255), // PS2 Hz: silver
			IM_COL32(255, 209, 102, 255), // EE: amber
			IM_COL32(189, 160, 255, 255), // GS: lilac
			IM_COL32(255, 155, 190, 255), // VU1: pink
		};
		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		draw->AddRectFilled(ImVec2(x - padding, top),
			ImVec2(x + row_width + padding, top + font_size + 20.0f * scale), IM_COL32(4, 13, 24, 200), 8.0f * scale);
		for (unsigned i = 0; i < 6; i++)
		{
			draw->AddText(font, font_size, ImVec2(x, top + 10.0f * scale), colors[i], ForjaLang::Visual(fields[i]));
			x += widths[i];
			if (i < 5)
			{
				draw->AddText(font, font_size, ImVec2(x, top + 10.0f * scale), IM_COL32(111, 137, 162, 255), separator);
				x += separator_width;
			}
		}
	}
	void WindowResized()
	{
	}
	void RequestScaleUpdate()
	{
	}
	void ReloadFonts()
	{
	}
	float GetWindowWidth()
	{
		return 1920.0f;
	}
	float GetWindowHeight()
	{
		return 1080.0f;
	}
	void SetSoftwareCursor(u32 index, std::string image_path, float image_scale, u32 multiply_color)
	{
		(void)index;
		(void)image_path;
		(void)image_scale;
		(void)multiply_color;
	}
	void ClearSoftwareCursor(u32 index)
	{
		(void)index;
	}
	void SetSoftwareCursorPosition(u32 index, float pos_x, float pos_y)
	{
		(void)index;
		(void)pos_x;
		(void)pos_y;
	}
} // namespace ImGuiManager

namespace ImGuiFullscreen
{
	void SetNotificationPosition(float horizontal_position, float vertical_position, float direction)
	{
		(void)horizontal_position;
		(void)vertical_position;
		(void)direction;
	}
	void OpenProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value)
	{
		(void)str_id;
		(void)message;
		(void)min;
		(void)max;
		(void)value;
	}
	void CloseProgressDialog(const char* str_id)
	{
		(void)str_id;
	}
	void AddNotification(std::string title, float duration, std::string message, std::string key, std::string text)
	{
		(void)title;
		(void)duration;
		(void)message;
		(void)key;
		(void)text;
	}
	bool InvalidateCachedTexture(const std::string& name)
	{
		(void)name;
		return false;
	}
} // namespace ImGuiFullscreen

// ImGuiFullscreen helpers used by GSDevice debug overlay. Headless: null.
namespace ImGuiFullscreen
{
	std::pair<ImFont*, float> g_medium_font{nullptr, 0.0f};
	std::pair<ImFont*, float> g_large_font{nullptr, 0.0f};
	float g_layout_scale = 1.0f;
	GSTexture* GetCachedTextureAsync(std::string_view name)
	{
		(void)name;
		return nullptr;
	}
	bool WantsToCloseMenu()
	{
		return true;
	}
	bool BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
		const ImVec4& background, float rounding, const ImVec2& padding, ImGuiWindowFlags flags)
	{
		(void)left;
		(void)top;
		(void)width;
		(void)height;
		(void)name;
		(void)background;
		(void)rounding;
		(void)padding;
		(void)flags;
		return false;
	}
	bool BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
		const ImVec4& background, float rounding, const ImVec2& padding, ImGuiWindowFlags flags)
	{
		(void)position;
		(void)size;
		(void)name;
		(void)background;
		(void)rounding;
		(void)padding;
		(void)flags;
		return false;
	}
	bool MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered, ImVec2* min, ImVec2* max,
		ImGuiButtonFlags flags, float hover_alpha)
	{
		(void)str_id;
		(void)enabled;
		(void)height;
		(void)visible;
		(void)hovered;
		(void)min;
		(void)max;
		(void)flags;
		(void)hover_alpha;
		return false;
	}
	void EndFullscreenWindow()
	{
	}
	void BeginMenuButtons(u32 num_items, float y_align, float x_padding, float y_padding, float item_height)
	{
		(void)num_items;
		(void)y_align;
		(void)x_padding;
		(void)y_padding;
		(void)item_height;
	}
	void EndMenuButtons()
	{
	}
	bool MenuHeadingButton(const char* title, const char* value, bool enabled, bool draw_line)
	{
		(void)title;
		(void)value;
		(void)enabled;
		(void)draw_line;
		return false;
	}
	bool IsGamepadInputSource()
	{
		return false;
	}
	ImGuiFullscreen::GamepadGlyphs GetGamepadGlyphs()
	{
		return {};
	}
	void SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items)
	{
		(void)items;
	}
	void RenderTextClippedWithShadow(const ImVec2& pos_min, const ImVec2& pos_max, const char* text, const char* text_end,
		const ImVec2* text_size_if_known, const ImVec2& align, const ImRect* clip_rect)
	{
		(void)pos_min;
		(void)pos_max;
		(void)text;
		(void)text_end;
		(void)text_size_if_known;
		(void)align;
		(void)clip_rect;
	}
	bool FloatingButton(const char* text, float x, float y, float width, float height,
		float anchor_x, float anchor_y, bool enabled, std::pair<ImFont*, float> font, ImVec2* out_position,
		bool repeat_button)
	{
		(void)text;
		(void)x;
		(void)y;
		(void)width;
		(void)height;
		(void)anchor_x;
		(void)anchor_y;
		(void)enabled;
		(void)font;
		(void)out_position;
		(void)repeat_button;
		return false;
	}
	void BeginNavBar(float x_padding, float y_padding)
	{
		(void)x_padding;
		(void)y_padding;
	}
	void EndNavBar()
	{
	}
	bool NavTab(const char* title, bool is_active, bool enabled, float width, float height, const ImVec4& background,
		std::pair<ImFont*, float> font)
	{
		(void)title;
		(void)is_active;
		(void)enabled;
		(void)width;
		(void)height;
		(void)background;
		(void)font;
		return false;
	}

	// ImGuiFullscreen globals (normally owned by the UI).
	ImVec4 UIPrimaryColor{};
	ImVec4 UIPrimaryDarkColor{};
	ImVec4 UIPrimaryTextColor{};
	ImVec4 UISecondaryColor{};
	float g_rcp_layout_scale = 1.0f;
} // namespace ImGuiFullscreen

namespace ImGuiFreeType
{
	const ImFontLoader* GetFontLoader()
	{
		// (AI-assisted) Compatibility entry for the upstream default font selection.
		return ImFontAtlasGetFontLoaderForStbTruetype();
	}
} // namespace ImGuiFreeType

namespace InputRec
{
	void log(const std::string& log, const float duration)
	{
		(void)log;
		(void)duration;
	}
	void consoleLog(const std::string& log)
	{
		(void)log;
	}
	void consoleMultiLog(const std::vector<std::string>& logs)
	{
		(void)logs;
	}
} // namespace InputRec
