// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// (AI-assisted) PS4 build variant based on the upstream shared GS renderer.

#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GSDumpReplayer.h"
#include "MTGS.h"
#include "Host.h"
#include "PerformanceMetrics.h"
#include "pcsx2/Config.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "IconsFontAwesome.h"
#include "ps4/core/ForjaSkipGuard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <thread>
#include <mutex>
#include <cstdio>
#include <unistd.h>

static void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename);

// Forja SX2 (compilation 47): "Saltar cuadros" (ps4/vm/main.cpp, UpdateGsSkip, turns it on and off). While it is on, the GS
// draws every other frame of the game: GSRendererHW::Draw returns at once while g_forja_gs_skip_drawing is set, and the TV
// keeps the last frame drawn (the skipped one is never presented). The game, its music and its speed stay whole.
std::atomic<bool> g_forja_gs_skip_active{false};
std::atomic<unsigned> g_forja_gs_skipped_frames{0};
bool g_forja_gs_skip_drawing = false; // GS thread only
static unsigned s_forja_vsyncs_since_frame = 0;

// Forja SX2 (compilation 62): what the TV is shown follows what each framebuffer holds, not the frames' order. Compilation 47
// hid the picture at the end of every skipped frame; Dragon Ball Z Budokai Tenkaichi 4 shows a buffer a frame after drawing
// it, so the frames hidden were the drawn ones and the TV stayed on a buffer that was never drawn: the frozen screen of
// 2026-10-18. Compilation 48 then left out only the draws elsewhere (its textures and palettes): white pixels. Now
// GSRendererHW::Draw leaves out only the draws into "la pantalla" (the last two framebuffers shown) of every other frame,
// and notes here which framebuffer holds a frame left out (ForjaGsSkipNote); VSync hides those and nothing else.
bool g_forja_gs_skip_tracking = false; // GS thread only: Draw notes the draws into la pantalla
// Compilation 63: one frame in g_forja_gs_skip_every is left out (2: "1 de 2"; Auto in main.cpp, UpdateGsSkip, picks 2 to 8,
// as few as the game needs to run at full speed). The pictures shown drop by as much: one in 6 keeps 25 of 30.
std::atomic<unsigned> g_forja_gs_skip_every{2};
static unsigned s_forja_skip_count = 0;
// Draws left out, pictures hidden, pictures shown, frames drawn whole by force; Forja 1.06: frames left out that showed (the
// guard paused), vsyncs the guard kept Saltar cuadros paused.
std::atomic<unsigned> g_forja_gs_skip_stats[6];
std::atomic<bool> g_forja_gs_skip_paused{false}; // Forja 1.06: the guard has it paused (main.cpp's Auto waits it out)
// Compilation 82: the guard on (main.cpp; off for Saltar cuadros "1 de 2 sin guardián"). Dragon Ball Z Budokai Tenkaichi 4 for
// two players (2026-10-24): the guard kept "1 de 2" paused 165-231 vsyncs of every 250 in the fights, at 52-65 %; in the seconds
// it let it skip, 78-89 %. The warm-up after a load or a light frame stays.
std::atomic<bool> g_forja_gs_skip_guard{true};
// Compilation 84: the controllers playing (ps4/vm/main.cpp); with two, the guard's pauses are short (ForjaSkipGuard.h).
extern std::atomic<int> g_forja_pads;
// Compilation 86: Saltar cuadros "1 de 2 rápido" (SaltarCuadros 3, main.cpp): the guard's short pauses with any controllers.
// In 82-85 the value 3 had no guard, and Dragon Ball's pause menu and the words at a fight's start and end flickered
// (Joseph played two sessions with it, 2026-10-24).
std::atomic<bool> g_forja_gs_skip_quick{false};
// Compilation 88: Saltar cuadros "2 de 3" (SaltarCuadros 4, main.cpp): two frames left out of every three. Dragon Ball Z
// Budokai Tenkaichi 4 for two players (2026-10-25, the 87): at one in two its heavy fights still ran at 48-95 %, the GS at
// 15-31 ms a refresh. The TV is shown one frame in three; a frame is drawn by force after two hidden ones anyway.
std::atomic<bool> g_forja_gs_skip_two{false};
// Compilation 90: Saltar cuadros "Al vuelo" (SaltarCuadros 5, main.cpp): a frame is left out only while the GS is behind,
// at most two in a row. Dragon Ball for two players with "2 de 3" (the 89, 2026-10-25): full speed and more (100-110 %), but
// it left out 73-98 of ~105 frames every 5 s, ~5 new pictures a second on the TV, and Joseph saw it slow ("se ve lento").
// Behind: when this frame starts, the EE has already queued two vsyncs or more for the GS (MTGS::GetCurrentVsyncQueueSize;
// with VsyncQueueSize 2 the EE waits at that point). A frame left out lets the GS catch up, the next one is drawn.
std::atomic<bool> g_forja_gs_skip_lag{false};
static forja_skip::Guard s_forja_guard; // Forja 1.06 (ForjaSkipGuard.h)
static u32 s_forja_hidden_total = 0;    // Forja 1.06: pictures hidden, for the guard's checks
namespace
{
	struct ForjaSkipTarget
	{
		u32 fbp = 0xffffffffu;
		u32 left_out_frame = 0; // the frame (s_forja_frame) its draws were last left out in
		bool stale = false; // it holds a frame whose draws were left out
		bool cut = false; // compilation 67: draws of that frame were really left out (not only hidden while it was drawn)
	};
	ForjaSkipTarget s_forja_targets[4];
	unsigned s_forja_next_target = 0;
	// The framebuffers the TV showed in the last FORJA_DISPLAYED_VSYNCS vsyncs: the ones whose draws may be left out. Up to
	// four, so a triple buffer counts too; one shown longer ago (a menu's, whose memory a game may reuse for its textures)
	// is not, so its draws always run.
	struct ForjaDisplayed
	{
		u32 fbp = 0xffffffffu;
		u32 vsync = 0;
	};
	ForjaDisplayed s_forja_displayed[4];
	u32 s_forja_vsync = 0;
	constexpr u32 FORJA_DISPLAYED_VSYNCS = 12;

	void ForjaNoteDisplayed(u32 fbp, bool new_vsync = true)
	{
		if (new_vsync) // compilation 67: the second circuit's buffer is noted at the same vsync
			s_forja_vsync++;
		ForjaDisplayed* slot = &s_forja_displayed[0];
		for (ForjaDisplayed& entry : s_forja_displayed)
		{
			if (entry.fbp == fbp)
			{
				slot = &entry;
				break;
			}
			if (entry.vsync < slot->vsync)
				slot = &entry;
		}
		slot->fbp = fbp;
		slot->vsync = s_forja_vsync;
	}
	u32 s_forja_frame = 0; // the game's frames, one per write of the privileged registers
	bool s_forja_shown_this_frame = false;
	unsigned s_forja_hidden_frames = 0; // frames in a row whose pictures were all hidden
	// A framebuffer left out longer than this many frames is no longer hidden, whatever happens to it (a game that stops
	// drawing into it mid-skip must not leave the TV frozen).
	constexpr u32 FORJA_STALE_FRAMES = 3;

	ForjaSkipTarget* ForjaFindTarget(u32 fbp)
	{
		for (ForjaSkipTarget& target : s_forja_targets)
			if (target.fbp == fbp)
				return &target;
		return nullptr;
	}

	bool ForjaIsStale(u32 fbp)
	{
		const ForjaSkipTarget* target = ForjaFindTarget(fbp);
		return target && target->stale && s_forja_frame - target->left_out_frame <= FORJA_STALE_FRAMES;
	}

	bool ForjaAnyStale()
	{
		for (const ForjaSkipTarget& target : s_forja_targets)
			if (target.stale && s_forja_frame - target.left_out_frame <= FORJA_STALE_FRAMES)
				return true;
		return false;
	}
} // namespace

// Forja SX2 (compilation 65): the frame pacing meter. A game at 30 on a 60 Hz TV should last 2 vsyncs a frame; a 1-3 alternation
// is the judder the eye sees even at a steady 30, and frame generation needs frames of one length. Saltar cuadros shows in it
// too. Counted in VSync, logged every 5 s by HostVM.cpp (both on the GS thread).
extern void ProbeLog(const char* format, ...);
static u32 s_forja_pace[6] = {}; // frames of the game that lasted 1, 2, 3, 4, 5 and 6+ vsyncs
static u32 s_forja_pace_logged[6] = {};
static u32 s_forja_pace_run = 0;

void ForjaLogFramePacing()
{
	u32 counts[6];
	u32 frames = 0;
	for (unsigned i = 0; i < 6; i++)
	{
		counts[i] = s_forja_pace[i] - s_forja_pace_logged[i];
		s_forja_pace_logged[i] = s_forja_pace[i];
		frames += counts[i];
	}
	if (frames < 10)
		return;
	// The share of frames of the most common length: 100 % is even pacing.
	const u32 top = *std::max_element(counts, counts + 6);
	ProbeLog("Frame pacing: game frames that lasted 1/2/3/4/5/6+ vsyncs: %u %u %u %u %u %u (even: %.0f %%)\n", counts[0], counts[1],
		counts[2], counts[3], counts[4], counts[5], 100.0 * top / frames);
}

// GS thread, GSRendererHW::Draw: whether a draw's framebuffer is one the TV is showing these days (ForjaNoteDisplayed).
bool ForjaGsSkipIsDisplayBuffer(u32 fbp)
{
	for (const ForjaDisplayed& entry : s_forja_displayed)
		if (entry.fbp == fbp && s_forja_vsync - entry.vsync <= FORJA_DISPLAYED_VSYNCS)
			return true;
	return false;
}

// GS thread, GSRendererHW::Draw: a draw into one of those framebuffers, left out or drawn. A framebuffer is stale from its first draw left
// out until a later frame draws into it again; a frame that has both stays stale, so no half-drawn picture is shown.
// Compilation 67: `hidden` is a draw of a frame chosen to be left out, drawn or not: its framebuffer is hidden from the frame's first
// draw. Compilation 66 drew a chosen frame's first 192 draws as ordinary ones, and the first of them showed the buffer again,
// half drawn, until a later draw was left out: Dragon Ball's pause menu and its "¿Salir?" came and went every other frame
// (2026-10-20). A chosen frame drawn whole (a light one) is shown when it ends (ForjaGsSkipFrameEnd).
static void ForjaGsSkipNote(u32 fbp, bool hidden, bool left_out)
{
	ForjaSkipTarget* target = ForjaFindTarget(fbp);
	if (!target)
	{
		target = &s_forja_targets[s_forja_next_target];
		s_forja_next_target = (s_forja_next_target + 1) % std::size(s_forja_targets);
		*target = ForjaSkipTarget{};
		target->fbp = fbp;
	}
	if (hidden)
	{
		if (!target->stale || target->left_out_frame != s_forja_frame)
			target->cut = false; // a new frame in it
		target->stale = true;
		target->left_out_frame = s_forja_frame;
		if (left_out)
		{
			target->cut = true;
			g_forja_gs_skip_stats[0].fetch_add(1, std::memory_order_relaxed);
		}
	}
	else if (target->stale && !(target->cut && target->left_out_frame == s_forja_frame))
	{
		// Compilation 67: a draw outside a chosen frame shows the buffer again; one with draws left out in this same frame stays
		// hidden (no half-drawn picture), until a later frame draws into it or a load shows everything (ForjaGsSkipClearAll).
		target->stale = false;
		target->cut = false;
	}
}

// Compilation 67: the end of a frame chosen to be left out (VSync, at the next frame's start): its framebuffers drawn whole are
// shown; those with draws left out stay hidden, as before.
static void ForjaGsSkipFrameEnd()
{
	for (ForjaSkipTarget& target : s_forja_targets)
		if (target.stale && !target.cut && target.left_out_frame == s_forja_frame)
			target.stale = false;
}

// Compilation 67: no new frame of the game for a while (a load, a pause): what the framebuffers hold is shown. Dragon Ball drew its
// loading screens into a buffer still stale from the last frame left out, and with no new frame it never stopped being one.
static void ForjaGsSkipClearAll()
{
	for (ForjaSkipTarget& target : s_forja_targets)
	{
		target.stale = false;
		target.cut = false;
	}
}

// Forja SX2 (compilation 66): the first FORJA_SKIP_FIRST_DRAWS draws of every frame into la pantalla always run, so a light
// frame (a loading screen, a menu, a result's message) is never left out. Dragon Ball Z Budokai Tenkaichi 4 (2026-10-20, one
// in 2): its fights draw ~2,300 a frame, but its "DERROTADO" and the pause box after it draw once, or a part a frame, and then
// stay; a skipped one was lost for good: a black screen, the last picture, the box in pieces. A heavy frame left out keeps
// its first draws, and stays hidden as before.
static constexpr u32 FORJA_SKIP_FIRST_DRAWS = 192;
static u32 s_forja_frame_draws = 0; // draws into la pantalla in this frame of the game
// Compilation 68: a frame after a light one (under FORJA_SKIP_MIN_PREVIOUS draws into la pantalla: a menu, a loading screen) is
// never chosen to be left out, so the frames a game draws once and keeps are drawn.
static constexpr u32 FORJA_SKIP_MIN_PREVIOUS = 400;
static u32 s_forja_previous_frame_draws = 0;

// Compilation 67: the framebuffers the TV showed at the last vsync (VSync), one a circuit; the second only with both circuits on.
static u32 s_forja_shown_now[2] = {0xffffffffu, 0xffffffffu};

bool ForjaGsSkipDraw(u32 fbp)
{
	// Compilation 68: a chosen frame's draws into a framebuffer the TV shows now (on either circuit) are all left out, so it keeps
	// its last whole picture; into any other one, its first FORJA_SKIP_FIRST_DRAWS run. Dragon Ball draws each frame into the
	// buffer its first circuit shows: compilation 66 drew the first 192 of a chosen frame there and the half-drawn picture showed,
	// merged (the flickering characters and pause menu of 2026-10-20); compilation 67 left out nothing there, and its fights lost
	// Saltar cuadros (0 draws left out, 61-78 % speed). Compilation 65 left them all out, and its fights ran at 98-100 %.
	const bool chosen = g_forja_gs_skip_drawing;
	const bool on_screen = fbp == s_forja_shown_now[0] || fbp == s_forja_shown_now[1];
	const bool left_out = chosen && (on_screen || s_forja_frame_draws >= FORJA_SKIP_FIRST_DRAWS);
	s_forja_frame_draws++;
	ForjaGsSkipNote(fbp, chosen, left_out); // compilation 67: a chosen frame's buffer hidden from its first draw
	return left_out;
}

static constexpr std::array<PresentShader, 8> s_tv_shader_indices = {
	PresentShader::COPY, PresentShader::SCANLINE,
	PresentShader::DIAGONAL_FILTER, PresentShader::TRIANGULAR_FILTER,
	PresentShader::COMPLEX_FILTER, PresentShader::LOTTES_FILTER,
	PresentShader::SUPERSAMPLE_4xRGSS, PresentShader::SUPERSAMPLE_AUTO};

static std::deque<std::thread> s_screenshot_threads;
static std::mutex s_screenshot_threads_mutex;

std::unique_ptr<GSRenderer> g_gs_renderer;

// Since we read this on the EE thread, we can't put it in the renderer, because
// we might be switching while the other thread reads it.
static GSVector4 s_last_draw_rect;

// Last time we reset the renderer due to a GPU crash, if any.
static Common::Timer::Value s_last_gpu_reset_time;

// Screen alignment
static GSDisplayAlignment s_display_alignment = GSDisplayAlignment::Center;

GSRenderer::GSRenderer()
	: m_shader_time_start(Common::Timer::GetCurrentValue())
{
	s_last_draw_rect = GSVector4::zero();
}

GSRenderer::~GSRenderer() = default;

void GSRenderer::Reset(bool hardware_reset)
{
	// Clear the current display texture.
	if (hardware_reset && g_gs_device)
		g_gs_device->ClearCurrent();

	GSState::Reset(hardware_reset);
}

void GSRenderer::Destroy()
{
#if !defined(PCSX2_GS_RENDERER_PROBE)
	GSCapture::EndCapture();
#endif
}

void GSRenderer::UpdateRenderFixes()
{
}

GSTexture* GSRenderer::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return nullptr;
}

bool GSRenderer::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}

bool GSRenderer::Merge(int field)
{
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = {nullptr, nullptr, nullptr};
	float tex_scale[3] = {0.0f, 0.0f, 0.0f};
	int y_offset[3] = {0, 0, 0};
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (PCRTCDisplays.PCRTCDisplays[0].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets.y) !=
	                                (PCRTCDisplays.PCRTCDisplays[1].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets.y);

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		const bool use_rc1 =
			PCRTCDisplays.PCRTCDisplays[0].enabled && // RC1 enabled.
			(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) || // Blend RC1 with non-zero alpha.
				(m_regs->PMODE.AMOD == 0) || // Use alpha of RC1.
				(feedback_merge && m_regs->EXTBUF.FBIN == 0)); // Use RC1 for feedback merge.

		// The following two flags determine if RC1 output completely overwrites RC2 output
		// due to the alpha used for blending and the respective rectangles of the outputs.
		const bool rc1_contains_rc2 =
			PCRTCDisplays.PCRTCDisplays[0].displayRect.rcontains(PCRTCDisplays.PCRTCDisplays[1].displayRect);

		const bool rc1_overwrites_rc2 = use_rc1 && rc1_contains_rc2 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;

		const bool use_rc2 =
			PCRTCDisplays.PCRTCDisplays[1].enabled && // RC2 enabled.
			((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || // Blending RC2 and not overwritten by RC1.
				(m_regs->PMODE.AMOD == 1) || // Use alpha of RC2.
				(feedback_merge && m_regs->EXTBUF.FBIN == 1)); // Use RC2 for feedback merge.

		if (use_rc1)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (use_rc2)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
	{
		// Clear out the MAD buffer as some remnants of the previously shown frame came be left over, causing a flash for one frame.
		if (GSConfig.InterlaceMode == GSInterlaceMode::Automatic || GSConfig.InterlaceMode >= GSInterlaceMode::AdaptiveTFF)
		{
			GSTexture* mad_tex = g_gs_device->GetMAD();

			if (mad_tex)
			{
				g_gs_device->ClearRenderTarget(mad_tex, 0);
				mad_tex = nullptr;
			}
		}

		// Both circuits off still outputs BGCOLOR on real hardware.
		if (PCRTCDisplays.PCRTCDisplays[0].enabled || PCRTCDisplays.PCRTCDisplays[1].enabled)
		{
			m_real_size = GSVector2i(0, 0);
			return false;
		}
	}

	GSVector4 src_gs_read[2] = {};
	GSVector4 dst[3] = {};

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	int field2 = 0;
	int mode = 3; // If the game is manually deinterlacing then we need to bob (if we want to get away with no deinterlacing).
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic || (!game_deinterlacing && !m_regs->SMODE2.FFMD && !scanmask_frame))
	{
		field2 = ((static_cast<int>(GSConfig.InterlaceMode) - 2) & 1);
		mode = ((static_cast<int>(GSConfig.InterlaceMode) - 2) >> 1);
	}

	for (int i = 0; i < 2; i++)
	{
		const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		const GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();

		float interlace_offset = 0.0f;
		if (isReallyInterlaced() && m_regs->SMODE2.FFMD && !is_bob && !GSConfig.DisableInterlaceOffset && GSConfig.InterlaceMode != GSInterlaceMode::Off)
		{
			interlace_offset = (scale.y) * static_cast<float>(field ^ field2);
		}
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;

			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		const GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / ((m_regs->EXTDATA.SMPH - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGH) + 1));
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / ((m_regs->EXTDATA.SMPV - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGV) + 1);

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] || tex[1]) && (tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if ((tex[0] || tex[1]) && isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset);
	}

	if (GSConfig.ShadeBoost)
		g_gs_device->ShadeBoost();

	if (GSConfig.FXAA)
		g_gs_device->FXAA();

	// Sharpens biinear at lower resolutions, almost nearest but with more uniform pixels.
	if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y))
	{
		g_gs_device->Resize(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight());
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

float GSRenderer::GetModXYOffset()
{
	if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal)
	{
		float mod_xy = GetUpscaleMultiplier();
		const int rounded_mod_xy = static_cast<int>(std::round(mod_xy));
		if (rounded_mod_xy > 1)
		{
			if (!(rounded_mod_xy & 1))
				return mod_xy += 0.2f;
			else if (!(rounded_mod_xy & 2))
				return mod_xy += 0.3f;
			else
				return mod_xy += 0.1f;
		}
	}

	return 0.0f;
}

static float GetCurrentAspectRatioFloat(bool is_progressive)
{
	switch (GSConfig.AspectRatio)
	{
		default:
		// We don't know the AR of the display here, nor we care about it
		case AspectRatioType::Stretch:
		case AspectRatioType::RAuto4_3_3_2:
#if !defined(PCSX2_GS_RENDERER_PROBE)
			if (EmuConfig.CurrentCustomAspectRatio > 0.f)
				return EmuConfig.CurrentCustomAspectRatio;
			else
#endif
				if (is_progressive)
				return 3.0f / 2.0f;
			else
				return 4.0f / 3.0f;
		case AspectRatioType::R4_3:
			return 4.0f / 3.0f;
		case AspectRatioType::R16_9:
			return 16.0f / 9.0f;
		case AspectRatioType::R10_7:
			return 10.0f / 7.0f;
	}
}

static GSVector4 CalculateDrawDstRect(s32 window_width, s32 window_height, const GSVector4i& src_rect, const GSVector2i& src_size, GSDisplayAlignment alignment, bool flip_y, bool is_progressive)
{
	const float f_width = static_cast<float>(window_width);
	const float f_height = static_cast<float>(window_height);
	const float clientAr = f_width / f_height;

	float targetAr = clientAr;
#if defined(PCSX2_GS_RENDERER_PROBE)
	// (AI-assisted) There is no VM applying dynamic aspect-ratio patches in the diagnostic.
	const AspectRatioType aspect_ratio = GSConfig.AspectRatio;
	const float custom_aspect_ratio = 0.0f;
#else
	const AspectRatioType aspect_ratio = EmuConfig.CurrentAspectRatio;
	const float custom_aspect_ratio = EmuConfig.CurrentCustomAspectRatio;
#endif
	if (aspect_ratio == AspectRatioType::RAuto4_3_3_2)
	{
		if (is_progressive)
			targetAr = 3.0f / 2.0f;
		else
			targetAr = 4.0f / 3.0f;
		// Fall back on the custom aspect ratio set by patches (e.g. 16:9, 21:9)
		if (custom_aspect_ratio > 0.f)
			targetAr = custom_aspect_ratio;
	}
	else if (aspect_ratio == AspectRatioType::R4_3)
	{
		targetAr = 4.0f / 3.0f;
	}
	else if (aspect_ratio == AspectRatioType::R16_9)
	{
		targetAr = 16.0f / 9.0f;
	}
	else if (aspect_ratio == AspectRatioType::R10_7)
	{
		targetAr = 10.0f / 7.0f;
	}

	const float crop_adjust = (static_cast<float>(src_rect.width()) / static_cast<float>(src_size.x)) /
	                          (static_cast<float>(src_rect.height()) / static_cast<float>(src_size.y));

	const double arr = (targetAr * crop_adjust) / clientAr;
	float target_width = f_width;
	float target_height = f_height;
	if (arr < 1)
		target_width = std::floor(f_width * arr + 0.5f);
	else if (arr > 1)
		target_height = std::floor(f_height / arr + 0.5f);

	target_height *= GSConfig.StretchY / 100.0f;

	if (GSConfig.IntegerScaling)
	{
		// make target width/height an integer multiple of the texture width/height
		float t_width = static_cast<double>(src_rect.width());
		float t_height = static_cast<double>(src_rect.height());

		// If using Bilinear (Shape) the image will be prescaled to larger than the window, so we need to unscale it.
		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && src_rect.width() > 0 && src_rect.height() > 0)
		{
			const GSVector2i resolution = g_gs_renderer->PCRTCDisplays.GetResolution();
			const GSVector2i fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * g_gs_renderer->GetUpscaleMultiplier()),
				static_cast<int>(static_cast<float>(resolution.y) * g_gs_renderer->GetUpscaleMultiplier()));

			if (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y)
			{
				t_width *= static_cast<float>(fs.x) / src_rect.width();
				t_height *= static_cast<float>(fs.y) / src_rect.height();
			}
		}

		float scale;
		if ((t_width / t_height) >= 1.0)
			scale = target_width / t_width;
		else
			scale = target_height / t_height;

		if (scale > 1.0)
		{
			const float adjust = std::floor(scale) / scale;
			target_width = target_width * adjust;
			target_height = target_height * adjust;
		}
	}

	float target_x, target_y;
	if (target_width >= f_width)
	{
		target_x = -((target_width - f_width) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_x = (f_width - target_width) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_x = (f_width - target_width);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_x = 0.0f;
				break;
		}
	}
	if (target_height >= f_height)
	{
		target_y = -((target_height - f_height) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_y = (f_height - target_height) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_y = (f_height - target_height);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_y = 0.0f;
				break;
		}
	}

	GSVector4 ret(target_x, target_y, target_x + target_width, target_y + target_height);

	if (flip_y)
	{
		const float height = ret.w - ret.y;
		ret.y = static_cast<float>(window_height) - ret.w;
		ret.w = ret.y + height;
	}

	return ret;
}

static GSVector4i CalculateDrawSrcRect(const GSTexture* src, const GSVector2i real_size)
{
	const GSVector2i size(src->GetSize());
	const GSVector2 scale = GSVector2(size.x, size.y) / GSVector2(real_size.x, real_size.y).max(GSVector2(0.1f, 0.1f));
	const float upscale = GSIsHardwareRenderer() ? GSConfig.UpscaleMultiplier : 1;
	const int left = static_cast<int>(static_cast<float>(GSConfig.Crop[0] * scale.x) * upscale);
	const int top = static_cast<int>(static_cast<float>(GSConfig.Crop[1] * scale.y) * upscale);
	const int right = size.x - static_cast<int>(static_cast<float>(GSConfig.Crop[2] * scale.x) * upscale);
	const int bottom = size.y - static_cast<int>(static_cast<float>(GSConfig.Crop[3] * scale.y) * upscale);
	return GSVector4i(left, top, right, bottom);
}

#if !defined(PCSX2_GS_RENDERER_PROBE)
static const char* GetScreenshotSuffix()
{
	static constexpr const char* suffixes[static_cast<u8>(GSScreenshotFormat::Count)] = {
		"png", "jpg", "webp"};
	return suffixes[static_cast<u8>(GSConfig.ScreenshotFormat)];
}

static void CompressAndWriteScreenshot(std::string filename, u32 width, u32 height, std::vector<u32> pixels)
{
	RGBA8Image image;
	image.SetPixels(width, height, std::move(pixels));

	std::string key(fmt::format("GSScreenshot_{}", filename));

	if (!GSDumpReplayer::IsRunner())
	{
		Host::AddIconOSDMessage(key, ICON_FA_CAMERA,
			fmt::format(TRANSLATE_FS("GS", "Saving screenshot to '{}'."), Path::GetFileName(filename)), 60.0f);
	}

	// maybe std::async would be better here.. but it's definitely worth threading, large screenshots take a while to compress.
	std::unique_lock lock(s_screenshot_threads_mutex);
	s_screenshot_threads.emplace_back([key = std::move(key), filename = std::move(filename), image = std::move(image),
										  quality = GSConfig.ScreenshotQuality]() {
		if (image.SaveToFile(filename.c_str(), quality))
		{
			if (!GSDumpReplayer::IsRunner())
			{
				Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
					fmt::format(TRANSLATE_FS("GS", "Saved screenshot to '{}'."), Path::GetFileName(filename)),
					Host::OSD_INFO_DURATION);
			}
		}
		else
		{
			Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
				fmt::format(TRANSLATE_FS("GS", "Failed to save screenshot to '{}'."), Path::GetFileName(filename),
					Host::OSD_ERROR_DURATION));
		}

		// remove ourselves from the list, if the GS thread is waiting for us, we won't be in there
		const auto this_id = std::this_thread::get_id();
		std::unique_lock lock(s_screenshot_threads_mutex);
		for (auto it = s_screenshot_threads.begin(); it != s_screenshot_threads.end(); ++it)
		{
			if (it->get_id() == this_id)
			{
				it->detach();
				s_screenshot_threads.erase(it);
				break;
			}
		}
	});
}

void GSJoinSnapshotThreads()
{
	std::unique_lock lock(s_screenshot_threads_mutex);
	while (!s_screenshot_threads.empty())
	{
		std::thread save_thread(std::move(s_screenshot_threads.front()));
		s_screenshot_threads.pop_front();
		lock.unlock();
		save_thread.join();
		lock.lock();
	}
}

#endif

bool GSRenderer::BeginPresentFrame(bool frame_skip)
{
	Host::BeginPresentFrame();

	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
	{
		// If we're skipping a frame, we need to reset imgui's state, since
		// we won't be calling EndPresentFrame().
		ImGuiManager::SkipFrame();
		return false;
	}
	else if (res == GSDevice::PresentResult::OK)
	{
		// All good!
		return true;
	}

#if defined(PCSX2_GS_RENDERER_PROBE)
	// (AI-assisted) Preserve the failure evidence; recovery needs a running VM frontend.
	ImGuiManager::SkipFrame();
	Host::ReportErrorAsync("GS probe", "GS presentation failed; device recovery is unavailable in this diagnostic.");
	return false;
#else
	// If we're constantly crashing on something in particular, we don't want to end up in an
	// endless reset loop.. that'd probably end up leaking memory and/or crashing us for other
	// reasons. So just abort in such case.
	const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
	if (s_last_gpu_reset_time != 0 &&
		Common::Timer::ConvertValueToSeconds(current_time - s_last_gpu_reset_time) < 15.0f)
	{
		pxFailRel("Host GPU lost too many times, device is probably completely wedged.");
	}
	s_last_gpu_reset_time = current_time;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt))
	{
		pxFailRel("Failed to recreate GS device after loss.");
		return false;
	}

	// First frame after reopening is definitely going to be trash, so skip it.
	Host::AddIconOSDMessage("GSDeviceLost", ICON_FA_TRIANGLE_EXCLAMATION,
		TRANSLATE_SV("GS", "Host GPU device encountered an error and was recovered. This may have broken rendering."),
		Host::OSD_CRITICAL_ERROR_DURATION);
	return false;
#endif
}

void GSRenderer::EndPresentFrame()
{
#if !defined(PCSX2_GS_RENDERER_PROBE)
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
#endif
	ImGuiManager::RenderOSD();
	g_gs_device->EndPresent();
	ImGuiManager::NewFrame();
}

// Forja SX2: hitch report. On the console a scene change in Mega Man X Command Mission showed one slow
// frame (50-70 ms) and the 5 s averages could not say why. Every vsync whose gap to the previous one is
// over 50 ms logs what happened meanwhile: EE blocks compiled (new game code), EE recompiler resets
// (video mode, settings), Vulkan pipelines created (new effects) and disc data read. A paused VM sends no
// vsyncs, so the first one after a pause is skipped.
extern void ProbeLog(const char* format, ...);
extern std::atomic<u32> g_orbis_rec_compiles, g_orbis_rec_full_resets; // pcsx2/x86/ix86-32/iR5900.cpp
u32 g_forja_pipelines_created = 0; // GSDeviceVK::CreateTFXPipeline, GS thread only
std::atomic<u64> g_forja_disc_bytes{0}; // ThreadedFileReader, reader thread
std::atomic<u64> g_forja_gs_vsyncs{0}; // every GS vsync; the watchdog in main.cpp checks it moves
std::atomic<u32> g_forja_resumes{0}; // main.cpp, each time the L3+R3 menu resumes the game (the hitch detector skips that gap)
std::atomic<u32> g_forja_presents{0}; // every frame shown in a game, the game's and the generated ones (UIVM.cpp's FPS box)

// Forja SX2: frame generation (GSDeviceVK.cpp's block from PS5SX2). Its clock is the PS2's vsyncs: while generated frames have
// room (g_orbis_fg_engaged), a vsync that only repeats the game's last frame presents nothing, since the generated frame was
// presented in its place; at a new frame the generated one goes first and the game's own on the next refresh.
extern bool g_orbis_fg_wanted;
extern bool g_orbis_fg_engaged;
u32 OrbisVkFrameGenRecord(GSTexture* current, const GSVector4& src_uv, const GSVector4& draw_rect, PresentShader shader, float shader_time,
	Filter filter, u32 vsyncs, u32 already, double ps2_hz, float speed, bool nominal);
void OrbisVkFrameGenDraw();
extern double GetVerticalFrequency();
static u32 s_orbis_fg_vsyncs = 0;
static u32 s_orbis_fg_presents = 0;
static u32 s_orbis_fg_guard = 0; // 0 = not armed; then vsyncs since the first generated frame

// Forja 1.06: ps4/vm/SessionStats.cpp, the session's summary (weak: the probes link without it).
extern "C" void ForjaSessionHitch(double gap_ms, unsigned compiled, unsigned resets, unsigned pipelines, unsigned disc_kb)
	__attribute__((weak));
static void ForjaReportHitch(bool new_frame)
{
	static Common::Timer::Value s_last = 0;
	static u32 s_compiles = 0, s_resets = 0, s_pipelines = 0;
	static u64 s_disc = 0;
	static unsigned s_reported = 0;
	static u32 s_resumes = 0;
	if (const u32 resumes = g_forja_resumes.load(std::memory_order_relaxed); resumes != s_resumes)
	{
		s_resumes = resumes;
		s_last = 0; // the gap since the last vsync is the menu's, not a hitch
	}
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	const u32 compiles = g_orbis_rec_compiles.load(std::memory_order_relaxed);
	const u32 resets = g_orbis_rec_full_resets.load(std::memory_order_relaxed);
	const u64 disc = g_forja_disc_bytes.load(std::memory_order_relaxed);
	if (s_last != 0)
	{
		const double gap_ms = Common::Timer::ConvertValueToMilliseconds(now - s_last);
		// Compilation 76: and over twice the usual gap of the last 16 vsyncs. A game running slowly but evenly has every gap over
		// 50 ms (Guitar Hero III at 40 %: 52 ms each), and each one counted: 1,250 "hitches" in 7 minutes, 3,305 "other" ones in
		// Need for Speed Most Wanted (2026-10-23). A hitch is a gap that stands out.
		// Compilation 79: against the usual length of the game's own frames (the median of the last 16 between two new frames), not
		// of the vsyncs. When the GS is behind, the vsyncs come in bunches, one long and then a few short, and against their median
		// the long one stood out though the picture changed evenly: the 77 still counted 574 "other" hitches in a minute of Shadow
		// of the Colossus (each frame 4-5 vsyncs, ~90 ms) and 594 in Dragon Ball with "1 de 2" (a frame drawn, the next one left
		// out). A still screen has short vsyncs and no new frames: no hitch either.
		static double s_frame_gaps[16] = {};
		static unsigned s_frame_count = 0, s_frame_next = 0;
		static Common::Timer::Value s_frame_last = 0;
		double usual = 0.0;
		if (s_frame_count >= 8)
		{
			double sorted[16];
			std::copy(s_frame_gaps, s_frame_gaps + s_frame_count, sorted);
			std::nth_element(sorted, sorted + s_frame_count / 2, sorted + s_frame_count);
			usual = sorted[s_frame_count / 2];
		}
		if (new_frame)
		{
			const double frame_ms = s_frame_last ? Common::Timer::ConvertValueToMilliseconds(now - s_frame_last) : 0.0;
			if (frame_ms > 0.0 && frame_ms < 2000.0)
			{
				s_frame_gaps[s_frame_next] = frame_ms;
				s_frame_next = (s_frame_next + 1) % 16;
				s_frame_count = std::min(s_frame_count + 1, 16u);
			}
			s_frame_last = now;
		}
		// 50..2000 ms: a hitch. Longer gaps are a pause, a loading screen with no vsyncs, or the menu.
		if (gap_ms > 50.0 && gap_ms < 2000.0 && gap_ms > 2.0 * usual && VMManager::GetState() == VMState::Running)
		{
			if (s_reported < 2000)
			{
				++s_reported;
				ProbeLog("Hitch: frame_gap_ms=%.0f ee_blocks_compiled=%u ee_rec_resets=%u pipelines_created=%u disc_kb=%llu\n",
					gap_ms, compiles - s_compiles, resets - s_resets, g_forja_pipelines_created - s_pipelines,
					static_cast<unsigned long long>((disc - s_disc) / 1024));
			}
			// Forja 1.06: the session's summary counts them all, by cause (ps4/vm/SessionStats.cpp; weak: the probes have none).
			if (ForjaSessionHitch)
				ForjaSessionHitch(gap_ms, compiles - s_compiles, resets - s_resets, g_forja_pipelines_created - s_pipelines,
					static_cast<unsigned>((disc - s_disc) / 1024));
		}
	}
	s_last = now;
	s_compiles = compiles;
	s_resets = resets;
	s_pipelines = g_forja_pipelines_created;
	s_disc = disc;
}

void GSRenderer::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// The PS4 POC starts with the Null renderer and has no presentation device yet.
	if (!g_gs_device)
		return;
	g_forja_gs_vsyncs.fetch_add(1, std::memory_order_relaxed); // the watchdog's (main.cpp)
	// Forja SX2: frame generation's hang guard (main.cpp reads it). Armed at the first generated frame, cleared 10 s later.
	if (s_orbis_fg_guard > 0 && ++s_orbis_fg_guard == 600)
		unlink("/data/ForjaSX2/fg-en-prueba");
	s_orbis_fg_vsyncs++; // frame generation's clock: a PS2 vsync each call
	const bool fg_skips_repeats = g_orbis_fg_wanted && g_orbis_fg_engaged;

#if !defined(PCSX2_GS_RENDERER_PROBE)
	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		if (GSConfig.SaveInfo)
		{
			DumpGSPrivRegs(*m_regs, GetDrawDumpPath("%05lld_f%05lld_vsync_gs_reg.txt", s_n, g_perfmon.GetFrame()));

			DumpDrawInfo(false, false, true);
		}

		if (GSConfig.SaveTransferImages)
			DumpTransferImages();

		if (GSConfig.SaveFrameStats)
		{
			m_perfmon_frame = g_perfmon - m_perfmon_frame;
			m_perfmon_frame.Dump(GetDrawDumpPath("%05lld_f%05lld_frame_stats.txt", s_n, g_perfmon.GetFrame()), GSIsHardwareRenderer());
			m_perfmon_frame = g_perfmon;
		}
	}

	const int fb_sprite_blits = g_perfmon.GetDisplayFramebufferSpriteBlits();
	const bool fb_sprite_frame = (fb_sprite_blits > 0);

	// Forja SX2 (compilation 65): the frame pacing meter (ForjaLogFramePacing): how many PS2 vsyncs each frame of the game lasted.
	{
		bool pace_new_frame = true;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
			case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
				pace_new_frame = registers_written;
				break;
			case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
				pace_new_frame = fb_sprite_frame;
				break;
			default:
				break;
		}
		s_forja_pace_run = std::min(s_forja_pace_run + 1u, 60u);
		if (pace_new_frame)
		{
			s_forja_pace[std::min(s_forja_pace_run, 6u) - 1u]++;
			s_forja_pace_run = 0;
		}
		ForjaReportHitch(pace_new_frame); // compilation 79: it measures against the game's frames
	}

#else
	ForjaReportHitch(true);
#endif
	bool skip_frame = false;
#if !defined(PCSX2_GS_RENDERER_PROBE)
	if ((GSConfig.SkipDuplicateFrames || fg_skips_repeats) && !GSCapture::IsCapturingVideo())
	{
		bool is_unique_frame;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
			case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
				is_unique_frame = registers_written;
				break;
			case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
				is_unique_frame = fb_sprite_frame;
				break;
			default:
				is_unique_frame = true;
				break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
		{
			m_skipped_duplicate_frames = 0;
		}
	}

#endif
	// Forja SX2 (compilation 47): "Saltar cuadros". A new frame of the game is a write of the privileged registers (GS-reg, the
	// way God of War II and Dragon Ball announce theirs); a game that announces them with a blit draws everything. Every
	// other frame is left out (GSRendererHW::Draw). Compilation 62: the TV is not shown a framebuffer that holds a frame left
	// out (ForjaGsSkipNote, above), whichever buffer the game shows when; and when the pictures of two frames in a row were
	// all hidden, the next frame is drawn whole, so the TV can never stay on one picture.
	{
		const bool forja_want = g_forja_gs_skip_active.load(std::memory_order_relaxed) &&
			PerformanceMetrics::GetInternalFPSMethod() == PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister;
		const u32 shown = static_cast<u32>(m_regs->PMODE.EN2 ? m_regs->DISP[1].DISPFB.FBP : m_regs->DISP[0].DISPFB.FBP);
		// Compilation 67: with both circuits on, the TV shows the first one's framebuffer too, merged with the second's (Dragon
		// Ball's pause menu, very likely: its draws left out showed at once, 0 of 290 pictures hidden). It counts as la pantalla.
		const u32 other = (m_regs->PMODE.EN1 && m_regs->PMODE.EN2) ? static_cast<u32>(m_regs->DISP[0].DISPFB.FBP) : 0xffffffffu;
		ForjaNoteDisplayed(shown); // every vsync, so the list is ready when the skip starts
		if (other != 0xffffffffu && other != shown)
			ForjaNoteDisplayed(other, false);
		s_forja_shown_now[0] = shown;
		s_forja_shown_now[1] = other;
		// Turned off, the notes go on until no framebuffer holds a frame left out.
		g_forja_gs_skip_tracking = forja_want || g_forja_gs_skip_drawing || ForjaAnyStale();
		if (g_forja_gs_skip_tracking)
		{
			if (registers_written)
			{
				if (g_forja_gs_skip_drawing)
					ForjaGsSkipFrameEnd(); // compilation 67: the chosen frame just ended; drawn whole, it is shown
				s_forja_frame++;
				s_forja_previous_frame_draws = s_forja_frame_draws; // compilation 68
				s_forja_frame_draws = 0; // compilation 66
				s_forja_vsyncs_since_frame = 0;
				s_forja_hidden_frames = s_forja_shown_this_frame ? 0 : s_forja_hidden_frames + 1;
				s_forja_shown_this_frame = false;
				// Compilation 89: with "2 de 3", after three hidden frames. Dragon Ball shows each picture a frame after drawing it, so
				// two frames left out in a row hide two pictures in a row, the very thing "2 de 3" means: in the 88 the force drew
				// the second frame to leave out about every other time, and it left out ~53 frames in 5 s, as "1 de 2" (Joseph:
				// "no le siento diferencia", 2026-10-25).
				const bool lag = g_forja_gs_skip_lag.load(std::memory_order_relaxed); // compilation 90
				const bool many = lag || g_forja_gs_skip_two.load(std::memory_order_relaxed); // two in a row may be left out
				const bool force = s_forja_hidden_frames >= (many ? 3u : 2u);
				if (force && forja_want)
					g_forja_gs_skip_stats[3].fetch_add(1, std::memory_order_relaxed);
				// Forja 1.06: the guard (ForjaSkipGuard.h): frames left out whose pictures the TV didn't hide pause Saltar cuadros, and
				// after a load or a light frame it waits for a second of heavy frames.
				if (s_forja_guard.FrameStart(s_forja_previous_frame_draws, s_forja_hidden_total, s_forja_vsync,
						g_forja_pads.load(std::memory_order_relaxed) >= 2 || g_forja_gs_skip_quick.load(std::memory_order_relaxed)) &&
					forja_want)
				{
					g_forja_gs_skip_stats[4].fetch_add(1, std::memory_order_relaxed);
					ProbeLog("GS skip: paused for %u vsyncs: the TV hid %u pictures for the last %u frames left out, so they showed "
						"(the game builds this picture another way: a menu, a result, a scene that changes); %u so far\n",
						s_forja_guard.LastPause(), s_forja_guard.LastHidden(), forja_skip::Guard::kGroup, s_forja_guard.Unmasked());
				}
				// Compilation 88: "2 de 3" leaves out the frames at counts 1 and 2 of every three, two in a row (the drawn frame,
				// count 0, comes after them; the force above draws one after two hidden frames all the same).
				const bool two = g_forja_gs_skip_two.load(std::memory_order_relaxed);
				const unsigned every = two ? 3u : std::max(g_forja_gs_skip_every.load(std::memory_order_relaxed), 2u);
				s_forja_skip_count = (s_forja_skip_count + 1) % every;
				const bool behind = lag && MTGS::GetCurrentVsyncQueueSize() >= 2;
				const bool turn = lag ? behind : two ? s_forja_skip_count != 0 : s_forja_skip_count == 0;
				const bool guard = g_forja_gs_skip_guard.load(std::memory_order_relaxed); // compilation 82
				g_forja_gs_skip_drawing = forja_want && (many || !g_forja_gs_skip_drawing) && !force && turn &&
					s_forja_previous_frame_draws >= FORJA_SKIP_MIN_PREVIOUS && // compilation 68: never after a light frame
					(guard ? s_forja_guard.MayChoose(s_forja_vsync) : s_forja_guard.WarmedUp()); // Forja 1.06
				if (g_forja_gs_skip_drawing)
				{
					g_forja_gs_skipped_frames.fetch_add(1, std::memory_order_relaxed);
					if (guard)
						s_forja_guard.Chosen(s_forja_hidden_total);
				}
			}
			else if (++s_forja_vsyncs_since_frame > 8)
			{
				// No new frame for a while (loading, a pause, a movie): draw everything until the next one.
				if (g_forja_gs_skip_drawing)
					ForjaGsSkipFrameEnd(); // compilation 67
				g_forja_gs_skip_drawing = false;
				ForjaGsSkipClearAll(); // compilation 67: and show what the buffers hold
				s_forja_guard.Reset(); // Forja 1.06: the warm-up again
			}
			// Compilation 68: the main circuit only, as until compilation 66. The first circuit's buffer, the one Dragon Ball draws
			// into, is stale every other frame at one in 2: hiding for it (compilation 67) would hide nearly every picture. Its
			// frames left out are left out whole (ForjaGsSkipDraw), so it only ever holds a whole picture.
			if (ForjaIsStale(shown))
			{
				skip_frame = true;
				g_forja_gs_skip_stats[1].fetch_add(1, std::memory_order_relaxed);
				s_forja_hidden_total++; // Forja 1.06
			}
			else
			{
				s_forja_shown_this_frame = true;
				g_forja_gs_skip_stats[2].fetch_add(1, std::memory_order_relaxed);
			}
		}
		else
		{
			s_forja_hidden_frames = 0;
			s_forja_guard.Reset(); // Forja 1.06: off, it warms up again when it comes back
		}
		// Forja 1.06: the guard's pause, for main.cpp's Auto and the 5 s report.
		const bool forja_paused = forja_want && g_forja_gs_skip_guard.load(std::memory_order_relaxed) && s_forja_guard.Paused(s_forja_vsync);
		g_forja_gs_skip_paused.store(forja_paused, std::memory_order_relaxed);
		if (forja_paused)
			g_forja_gs_skip_stats[5].fetch_add(1, std::memory_order_relaxed);
	}

	const bool blank_frame = !Merge(field);

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	// Skip presentation when running uncapped while vsync is on.
	if (skip_frame || g_gs_device->ShouldSkipPresentingFrame())
	{
		if (BeginPresentFrame(true))
			EndPresentFrame();

#if !defined(PCSX2_GS_RENDERER_PROBE)
		PerformanceMetrics::Update(registers_written, fb_sprite_frame, skip_frame);
#endif
	}
	else
	{
		if (!idle_frame)
			g_gs_device->AgePool();

		g_perfmon.EndFrame(idle_frame);

		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();

		// Little bit ugly, but we can't do CAS inside the render pass.
		GSVector4i src_rect;
		GSVector4 src_uv, draw_rect;
		GSTexture* current = g_gs_device->GetCurrent();
		if (current && !blank_frame)
		{
			src_rect = CalculateDrawSrcRect(current, m_real_size);
			src_uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
			draw_rect = CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P);
#if defined(PCSX2_GS_RENDERER_PROBE)
			// (AI-assisted) The diagnostic frontend reserves a viewport beside its status list.
			extern GSVector4 ProbePresentationRect();
			draw_rect = ProbePresentationRect();
#endif
			s_last_draw_rect = draw_rect;

			if (GSConfig.CASMode != GSCASMode::Disabled)
			{
				static bool cas_log_once = false;
				if (g_gs_device->Features().cas_sharpening)
				{
					// sharpen only if the IR is higher than the display resolution
					const bool sharpen_only = (GSConfig.CASMode == GSCASMode::SharpenOnly ||
											   (current->GetWidth() > g_gs_device->GetWindowWidth() &&
												   current->GetHeight() > g_gs_device->GetWindowHeight()));
					g_gs_device->CAS(current, src_rect, src_uv, draw_rect, sharpen_only);
				}
				else if (!cas_log_once)
				{
					Host::AddIconOSDMessage("CASUnsupported", ICON_FA_TRIANGLE_EXCLAMATION,
						TRANSLATE_SV("GS", "CAS is not available, your graphics driver does not support the required functionality."),
						10.0f);
					cas_log_once = true;
				}
			}
		}

#if !defined(PCSX2_GS_RENDERER_PROBE)
		// Forja SX2: frame generation. For a new frame of the game, the frame between it and the last one is made and
		// presented first (the pacing decides how many refreshes it gets); the game's own frame follows below.
		bool fg_new_frame = true;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
			case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
				fg_new_frame = registers_written;
				break;
			case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
				fg_new_frame = fb_sprite_frame;
				break;
			default:
				break;
		}
		if (current && !blank_frame && g_orbis_fg_wanted && fg_new_frame)
		{
			const float fg_time = static_cast<float>(
				Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - m_shader_time_start));
			const u32 fg_vsyncs = s_orbis_fg_vsyncs;
			const u32 fg_already = s_orbis_fg_presents;
			s_orbis_fg_vsyncs = 0;
			s_orbis_fg_presents = 0;
			const float fg_target = VMManager::GetTargetSpeed();
			const bool fg_nominal = VMManager::GetLimiterMode() == LimiterModeType::Nominal && fg_target > 0.5f && fg_target < 1.05f &&
			                        !GSCapture::IsCapturingVideo();
			const float fg_speed = fg_target > 0.0f ? PerformanceMetrics::GetSpeed() / fg_target : PerformanceMetrics::GetSpeed();
			const u32 generated_presents = OrbisVkFrameGenRecord(current, src_uv, draw_rect, s_tv_shader_indices[GSConfig.TVShader],
				fg_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off), fg_vsyncs, fg_already, GetVerticalFrequency(),
				fg_speed, fg_nominal);
			if (generated_presents > 0 && s_orbis_fg_guard == 0)
			{
				// The game goes in the file ("<serial> <crc>"), so main.cpp can turn frame generation off in its profile.
				if (FILE* guard = std::fopen("/data/ForjaSX2/fg-en-prueba", "w"))
				{
					const std::string serial = VMManager::GetDiscSerial();
					std::fprintf(guard, "%s %08X\n", serial.empty() ? "-" : serial.c_str(), VMManager::GetDiscCRC());
					std::fclose(guard);
				}
				s_orbis_fg_guard = 1;
			}
			for (u32 i = 0; i < generated_presents; i++)
			{
				if (!BeginPresentFrame(false))
					break;
				OrbisVkFrameGenDraw();
				EndPresentFrame();
				g_forja_presents.fetch_add(1, std::memory_order_relaxed);
			}
		}
#endif

		if (BeginPresentFrame(false))
		{
			if (current && !blank_frame)
			{
				const u64 current_time = Common::Timer::GetCurrentValue();
				const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

				g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
					s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
			}

			EndPresentFrame();
			s_orbis_fg_presents++; // a refresh of this interval used (frame generation's budget)
			g_forja_presents.fetch_add(1, std::memory_order_relaxed);

			const float gpu_time = g_gs_device->GetAndResetAccumulatedGPUTime();
			GPUPipelineStatistics gpu_stats = g_gs_device->GetAndResetAccumulatedGPUPipelineStatistics();
#if !defined(PCSX2_GS_RENDERER_PROBE)
			PerformanceMetrics::OnGPUPresent(gpu_time, gpu_stats.vs_invocations, gpu_stats.ps_invocations);
#endif
		}

#if !defined(PCSX2_GS_RENDERER_PROBE)
		PerformanceMetrics::Update(registers_written, fb_sprite_frame, false);
#endif
	}

#if !defined(PCSX2_GS_RENDERER_PROBE)
	// snapshot
	if (!m_snapshot.empty())
	{
		u32 screenshot_width, screenshot_height;
		std::vector<u32> screenshot_pixels;

		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			const GSTexture* current = g_gs_device->GetCurrent();
			const GSVector2i internal_res = GetInternalResolution();

			if (current && (current->GetWidth() > internal_res.x || current->GetHeight() > internal_res.y))
				g_gs_device->Resize(internal_res.x, internal_res.y);
		}

		if (!m_dump && m_dump_frames > 0)
		{
			if (GSConfig.UserHacks_ReadTCOnClose)
				ReadbackTextureCache();

			freezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new u8[fd.size];
			Freeze(&fd, false);

			// keep the screenshot relatively small so we don't bloat the dump
			static constexpr u32 DUMP_SCREENSHOT_WIDTH = 640;
			static constexpr u32 DUMP_SCREENSHOT_HEIGHT = 480;
			SaveSnapshotToMemory(DUMP_SCREENSHOT_WIDTH, DUMP_SCREENSHOT_HEIGHT, true, false,
				&screenshot_width, &screenshot_height, &screenshot_pixels);

			std::string_view compression_str;
			if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed)
			{
				m_dump = GSDumpBase::CreateUncompressedDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with no compression");
			}
			else if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA)
			{
				m_dump = GSDumpBase::CreateXzDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with LZMA compression");
			}
			else
			{
				m_dump = GSDumpBase::CreateZstDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with Zstandard compression");
			}

			delete[] fd.data;

			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saving {0} GS dump {1} to '{2}'"),
					(m_dump_frames == 1) ? TRANSLATE_SV("GS", "single frame") : TRANSLATE_SV("GS", "multi-frame"), compression_str,
					Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
		}

		const bool internal_resolution = (GSConfig.ScreenshotSize >= GSScreenshotSize::InternalResolution);
		const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);

		if (g_gs_device->GetCurrent() && SaveSnapshotToMemory(
											 internal_resolution ? 0 : g_gs_device->GetWindowWidth(),
											 internal_resolution ? 0 : g_gs_device->GetWindowHeight(),
											 aspect_correct, true,
											 &screenshot_width, &screenshot_height, &screenshot_pixels))
		{
			CompressAndWriteScreenshot(fmt::format("{}.{}", m_snapshot, GetScreenshotSuffix()),
				screenshot_width, screenshot_height, std::move(screenshot_pixels));
		}
		else
		{
			Host::AddIconOSDMessage("GSScreenshot", ICON_FA_CAMERA,
				TRANSLATE_SV("GS", "Failed to render/download screenshot."), Host::OSD_ERROR_DURATION);
		}

		m_snapshot = {};
	}
	else if (m_dump)
	{
		const bool last = (m_dump_frames == 0);
		if (m_dump->VSync(field, last, m_regs))
		{
			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saved GS dump to '{}'."), Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
			m_dump.reset();
		}
		else if (!last)
		{
			m_dump_frames--;
		}
	}

	// capture
	if (GSCapture::IsCapturingVideo())
	{
		const GSVector2i size = GSCapture::GetSize();
		if (GSTexture* current = g_gs_device->GetCurrent())
		{
			// TODO: Maybe avoid this copy in the future? We can use swscale to fix it up on the dumping thread..
			if (current->GetSize() != size)
			{
				GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, false);
				if (temp)
				{
					g_gs_device->StretchRect(current, temp, GSVector4(0, 0, size.x, size.y), ShaderConvert::COPY, Biln);
					GSCapture::DeliverVideoFrame(temp);
					g_gs_device->Recycle(temp);
				}
			}
			else
			{
				GSCapture::DeliverVideoFrame(current);
			}
		}
		else
		{
			// Bit janky, but unless we want to make variable frame rate files, we need to deliver *a* frame to
			// the video file, so just grab a blank RT.
			GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, true);
			if (temp)
			{
				GSCapture::DeliverVideoFrame(temp);
				g_gs_device->Recycle(temp);
			}
		}
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()) && GSConfig.SaveTransferImages)
		DumpTransferImages();
#endif
}

#if !defined(PCSX2_GS_RENDERER_PROBE)
void GSRenderer::QueueSnapshot(const std::string& path, const u32 gsdump_frames)
{
	if (!m_snapshot.empty())
		return;

	// Allows for providing a complete path
	if (path.size() > 4 && StringUtil::EndsWithNoCase(path, ".png"))
		m_snapshot = path.substr(0, path.size() - 4);
	else
		m_snapshot = GSGetBaseSnapshotFilename();

	// this is really gross, but wx we get the snapshot request after shift...
	m_dump_frames = gsdump_frames;
}

static std::string GSGetBaseFilename()
{
	std::string filename;

	// append the game serial and title
	if (std::string name(VMManager::GetTitle(true)); !name.empty())
	{
		Path::SanitizeFileName(&name);
		if (name.length() > 219)
			name.resize(219);
		filename += name;
	}
	if (std::string serial = VMManager::GetDiscSerial(); !serial.empty())
	{
		Path::SanitizeFileName(&serial);
		filename += '_';
		filename += serial;
	}

	const time_t cur_time = time(nullptr);
	char local_time[16];

	if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
	{
		static time_t prev_snap;
		// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
		// a single second, we'll start using this variable for naming when a second screenshot request is detected
		// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
		// the captured image is the 2nd image captured at this specific time.
		static int n = 2;

		filename += '_';

		if (cur_time == prev_snap)
			filename += fmt::format("{0}_({1})", local_time, n++);
		else
		{
			n = 2;
			filename += fmt::format("{}", local_time);
		}
		prev_snap = cur_time;
	}

	return filename;
}

std::string GSGetBaseSnapshotFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeSnapshotsByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Snapshots, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}

	return Path::Combine(EmuFolders::Snapshots, GSGetBaseFilename());
}

std::string GSGetBaseVideoFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeVideoCaptureByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Videos, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}
	// prepend video directory
	return Path::Combine(EmuFolders::Videos, GSGetBaseFilename());
}

void GSRenderer::StopGSDump()
{
	m_snapshot = {};
	m_dump_frames = 0;
}

void GSRenderer::PresentCurrentFrame()
{
	// Forja SX2: the texture pool's frame counter only moved in VSync(), so in the library and the startup intro (which present
	// through here) a recycled texture was never reused ("used this frame") nor aged out: the intro's 96 frames (1280x720, ~3.7 MB
	// each) and every library view change (~16 MB: covers, icons, the 1920x1080 loading background) piled up in the pool until
	// the GPU ran out and GSDevice::UpdateImGuiTextures() failed ("Failed to create ImGui texture", CE-34878-0) a few view changes
	// after the intro (2026-10-14). Aging here works as in a game: reuse the next frame, free after MAX_TEXTURE_AGE frames.
	if (g_gs_device)
		g_gs_device->AgePool();
	if (BeginPresentFrame(false))
	{
		GSTexture* current = g_gs_device->GetCurrent();
		if (current)
		{
			const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
			const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());
			const GSVector4 draw_rect(CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P));
			s_last_draw_rect = draw_rect;

			const u64 current_time = Common::Timer::GetCurrentValue();
			const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

			g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
				s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		}

		EndPresentFrame();
	}
}

void GSTranslateWindowToDisplayCoordinates(float window_x, float window_y, float* display_x, float* display_y)
{
	const float draw_width = s_last_draw_rect.z - s_last_draw_rect.x;
	const float draw_height = s_last_draw_rect.w - s_last_draw_rect.y;
	const float rel_x = window_x - s_last_draw_rect.x;
	const float rel_y = window_y - s_last_draw_rect.y;
	if (rel_x < 0 || rel_x > draw_width || rel_y < 0 || rel_y > draw_height)
	{
		*display_x = -1.0f;
		*display_y = -1.0f;
		return;
	}

	*display_x = rel_x / draw_width;
	*display_y = rel_y / draw_height;
}

void GSSetDisplayAlignment(GSDisplayAlignment alignment)
{
	s_display_alignment = alignment;
}

bool GSRenderer::BeginCapture(std::string filename, const GSVector2i& size)
{
	const GSVector2i capture_resolution = (size.x != 0 && size.y != 0) ?
	                                          size :
	                                          (GSConfig.VideoCaptureAutoResolution ?
													  GetInternalResolution() :
													  GSVector2i(GSConfig.VideoCaptureWidth, GSConfig.VideoCaptureHeight));

	return GSCapture::BeginCapture(GetTvRefreshRate(), capture_resolution,
		GetCurrentAspectRatioFloat(GetVideoMode() == GSVideoMode::SDTV_480P),
		std::move(filename));
}

void GSRenderer::EndCapture()
{
	GSCapture::EndCapture();
}

bool GSRenderer::SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	GSTexture* const current = g_gs_device->GetCurrent();
	if (!current)
	{
		*width = 0;
		*height = 0;
		pixels->clear();
		return false;
	}

	const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
	const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());

	const bool is_progressive = (GetVideoMode() == GSVideoMode::SDTV_480P);
	GSVector4 draw_rect;
	if (window_width == 0 || window_height == 0)
	{
		if (apply_aspect)
		{
			// use internal resolution of the texture
			const float aspect = GetCurrentAspectRatioFloat(is_progressive);
			const int tex_width = current->GetWidth();
			const int tex_height = current->GetHeight();

			// expand to the larger dimension
			const float tex_aspect = static_cast<float>(tex_width) / static_cast<float>(tex_height);
			if (tex_aspect >= aspect)
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_width), static_cast<float>(tex_width) / aspect);
			else
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_height) * aspect, static_cast<float>(tex_height));
		}
		else
		{
			// uncorrected aspect is only available at internal resolution
			draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(current->GetWidth()), static_cast<float>(current->GetHeight()));
		}
	}
	else
	{
		draw_rect = CalculateDrawDstRect(window_width, window_height, src_rect, current->GetSize(),
			GSDisplayAlignment::LeftOrTop, false, is_progressive);
	}
	const u32 draw_width = static_cast<u32>(draw_rect.z - draw_rect.x);
	const u32 draw_height = static_cast<u32>(draw_rect.w - draw_rect.y);
	const u32 image_width = crop_borders ? draw_width : std::max(draw_width, window_width);
	const u32 image_height = crop_borders ? draw_height : std::max(draw_height, window_height);

	// We're not expecting screenshots to be fast, so just allocate a download texture on demand.
	GSTexture* rt = g_gs_device->CreateRenderTarget(draw_width, draw_height, GSTexture::Format::Color, false);
	if (rt)
	{
		std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(draw_width, draw_height, GSTexture::Format::Color));
		if (dl)
		{
			const GSVector4i rc(0, 0, draw_width, draw_height);
			g_gs_device->StretchRect(current, src_uv, rt, GSVector4(rc), ShaderConvert::TRANSPARENCY_FILTER, Biln);
			dl->CopyFromTexture(rc, rt, rc, 0);
			dl->Flush();

			if (dl->Map(rc))
			{
				const u32 pad_x = (image_width - draw_width) / 2;
				const u32 pad_y = (image_height - draw_height) / 2;
				pixels->clear();
				pixels->resize(image_width * image_height, 0);
				*width = image_width;
				*height = image_height;
				StringUtil::StrideMemCpy(pixels->data() + pad_y * image_width + pad_x, image_width * sizeof(u32), dl->GetMapPointer(),
					dl->GetMapPitch(), draw_width * sizeof(u32), draw_height);

				g_gs_device->Recycle(rt);
				return true;
			}
		}

		g_gs_device->Recycle(rt);
	}

	*width = 0;
	*height = 0;
	pixels->clear();
	return false;
}

void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wt");
	if (!fp)
		return;

	for (int i = 0; i < 2; i++)
	{
		if (i == 0 && !r.PMODE.EN1)
			continue;
		if (i == 1 && !r.PMODE.EN2)
			continue;

		std::fprintf(fp.get(), "DISPFB%d: { BP: 0x%05x, BW: %u, PSM: %u, DBX: %u, DBY: %u }\n",
			i,
			r.DISP[i].DISPFB.Block(),
			r.DISP[i].DISPFB.FBW,
			r.DISP[i].DISPFB.PSM,
			r.DISP[i].DISPFB.DBX,
			r.DISP[i].DISPFB.DBY);

		std::fprintf(fp.get(), "DISPLAY%d: { DX: %u, DY: %u, DW: %u, DH: %u, MAGH: %u, MAGV: %u }\n",
			i,
			r.DISP[i].DISPLAY.DX,
			r.DISP[i].DISPLAY.DY,
			r.DISP[i].DISPLAY.DW,
			r.DISP[i].DISPLAY.DH,
			r.DISP[i].DISPLAY.MAGH,
			r.DISP[i].DISPLAY.MAGV);
	}

	std::fprintf(fp.get(), "PMODE: { EN1: %u, EN2: %u, CRTMD: %u, MMOD: %u, AMOD: %u, SLBG: %u, ALP: %u }\n",
		r.PMODE.EN1,
		r.PMODE.EN2,
		r.PMODE.CRTMD,
		r.PMODE.MMOD,
		r.PMODE.AMOD,
		r.PMODE.SLBG,
		r.PMODE.ALP);

	std::fprintf(fp.get(),
		"SMODE1: { CLKSEL: %u, CMOD: %u, EX: %u, GCONT: %u, LC: %u, NVCK: %u, PCK2: %u, PEHS: %u, PEVS: %u, PHS: %u, PRST: %u, PVS: %u, RC: %u, SINT: %u, SLCK: %u, SLCK2: %u, SPML: %u, T1248: %u, VCKSEL: %u, VHP: %u, XPCK: %u }\n",
		r.SMODE1.CLKSEL,
		r.SMODE1.CMOD,
		r.SMODE1.EX,
		r.SMODE1.GCONT,
		r.SMODE1.LC,
		r.SMODE1.NVCK,
		r.SMODE1.PCK2,
		r.SMODE1.PEHS,
		r.SMODE1.PEVS,
		r.SMODE1.PHS,
		r.SMODE1.PRST,
		r.SMODE1.PVS,
		r.SMODE1.RC,
		r.SMODE1.SINT,
		r.SMODE1.SLCK,
		r.SMODE1.SLCK2,
		r.SMODE1.SPML,
		r.SMODE1.T1248,
		r.SMODE1.VCKSEL,
		r.SMODE1.VHP,
		r.SMODE1.XPCK);

	std::fprintf(fp.get(), "SMODE2: { INT: %u, FFMD: %u, DPMS: %u }\n",
		r.SMODE2.INT,
		r.SMODE2.FFMD,
		r.SMODE2.DPMS);

	std::fprintf(fp.get(), "SRFSH: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SRFSH.U32[0],
		r.SRFSH.U32[1]);

	std::fprintf(fp.get(), "SYNCH1: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH1.U32[0],
		r.SYNCH1.U32[1]);

	std::fprintf(fp.get(), "SYNCH2: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH2.U32[0],
		r.SYNCH2.U32[1]);

	std::fprintf(fp.get(), "SYNCV: { VBP: %u, VBPE: %u, VDP: %u, VFP: %u, VFPE: %u, VS: %u }\n",
		r.SYNCV.VBP,
		r.SYNCV.VBPE,
		r.SYNCV.VDP,
		r.SYNCV.VFP,
		r.SYNCV.VFPE,
		r.SYNCV.VS);

	std::fprintf(fp.get(), "CSR: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.CSR.U32[0],
		r.CSR.U32[1]);

	std::fprintf(fp.get(), "BGCOLOR: { B: %u, G: %u, R: %u }\n",
		r.BGCOLOR.B,
		r.BGCOLOR.G,
		r.BGCOLOR.R);

	std::fprintf(fp.get(), "EXTBUF: { BP: 0x%05x, BW: %u, FBIN: %u, WFFMD: %u, EMODA: %u, EMODC: %u, WDX: %u, WDY: %u }\n",
		r.EXTBUF.EXBP, r.EXTBUF.EXBW, r.EXTBUF.FBIN, r.EXTBUF.WFFMD,
		r.EXTBUF.EMODA, r.EXTBUF.EMODC, r.EXTBUF.WDX, r.EXTBUF.WDY);

	std::fprintf(fp.get(), "EXTDATA: { SX: %u, SY: %u, SMPH: %u, SMPV: %u, WW: %u, WH: %u }\n",
		r.EXTDATA.SX, r.EXTDATA.SY, r.EXTDATA.SMPH, r.EXTDATA.SMPV, r.EXTDATA.WW, r.EXTDATA.WH);

	std::fprintf(fp.get(), "EXTWRITE: { EN: %u }\n", r.EXTWRITE.WRITE);
}
#endif // (AI-assisted) PCSX2_GS_RENDERER_PROBE
