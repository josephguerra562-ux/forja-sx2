// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (compilation 143): the library's "Nuevo" style, after Dolphin's game list (Joseph, 2026-11-18: "un diseño tipo Dolphin ...
// el menú cuando ya entras y puedes seleccionar los juegos ... y una etiqueta más donde diga estilos, el clásico y el nuevo, para
// gustos, colores"). The sections are a bar on top instead of the column on the left, five covers a row with each game's name under
// it, graphite instead of navy, and the color the player picks (Ajustes > Estilos). The classic style is GameMenu.cpp's own code, as
// it was. Header-only and drawn only with ImGui, so the PC preview (herramientas/revision-local/vista-previa) draws this same code.
// The caller passes every text already translated.
#pragma once

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace ForjaStyle
{
	constexpr int Classic = 0, Modern = 1;
	constexpr int ColorCount = 5; // Azul, Morado, Rojo, Verde, Naranja
	constexpr size_t ModernColumns = 5;
	constexpr size_t ModernVisibleRows = 2;

	inline ImU32 AccentColor(int color, int alpha = 255)
	{
		static constexpr unsigned char rgb[ColorCount][3] = {{0, 120, 215}, {142, 92, 230}, {226, 72, 72}, {46, 170, 100}, {240, 140, 40}};
		const auto& c = rgb[std::clamp(color, 0, ColorCount - 1)];
		return IM_COL32(c[0], c[1], c[2], alpha);
	}

	// The first row of covers on screen: the selected one's row stays on the lower line once the list scrolls.
	inline size_t FirstRow(size_t selected, size_t count, size_t columns, size_t visible_rows)
	{
		const size_t total_rows = (count + columns - 1) / columns;
		const size_t selected_row = selected / columns;
		const size_t max_first_row = total_rows > visible_rows ? total_rows - visible_rows : 0;
		return std::min(selected_row >= visible_rows ? selected_row - (visible_rows - 1) : 0, max_first_row);
	}

	struct Canvas
	{
		ImDrawList* draw = nullptr;
		ImFont* font = nullptr;
		float scale = 1.0f;
		ImVec2 origin{0.0f, 0.0f};
		ImVec2 size{1920.0f, 1080.0f}; // the whole screen
		void (*draw_text)(ImDrawList*, ImFont*, float, ImVec2, ImU32, const char*, float) = nullptr;
		float (*text_width)(ImFont*, float, const char*) = nullptr;

		ImVec2 P(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
		void Fill(float x, float y, float w, float h, ImU32 color, float radius = 0.0f) const
		{
			draw->AddRectFilled(P(x, y), P(x + w, y + h), color, radius * scale);
		}
		void Frame(float x, float y, float w, float h, ImU32 color, float radius, float thickness) const
		{
			draw->AddRect(P(x, y), P(x + w, y + h), color, radius * scale, thickness * scale);
		}
		float Width(float pixels, const char* text) const { return text && *text ? text_width(font, pixels * scale, text) / scale : 0.0f; }
		void Text(float x, float baseline, float pixels, ImU32 color, const char* text, float max_width = 0.0f) const
		{
			if (text && *text)
				draw_text(draw, font, pixels * scale, P(x, baseline - pixels * 0.82f), color, text, max_width * scale);
		}
		void Centered(float center_x, float baseline, float pixels, ImU32 color, const char* text, float max_width = 0.0f) const
		{
			const float width = max_width > 0.0f ? std::min(Width(pixels, text), max_width) : Width(pixels, text);
			Text(center_x - width * 0.5f, baseline, pixels, color, text, max_width);
		}
		void Right(float right_x, float baseline, float pixels, ImU32 color, const char* text, float max_width = 0.0f) const
		{
			const float width = max_width > 0.0f ? std::min(Width(pixels, text), max_width) : Width(pixels, text);
			Text(right_x - width, baseline, pixels, color, text, max_width);
		}
	};

	struct Card
	{
		const char* name = "";
		bool favorite = false;
		ImTextureData* cover = nullptr; // only the ones on screen
	};

	struct Row
	{
		const char* label = "";
		const char* value = "";
		const char* help = "";
		bool focused = false;
		bool arrows = false; // "<  value  >" when focused
	};

	// A pad button and what it does, for the bar at the bottom. button: 'x', 'o', 's' (square), 't' (triangle), 'l' (L1/R1).
	struct Hint
	{
		char button = 'x';
		const char* label = "";
		bool dim = false;
	};

	struct Page
	{
		int color = 0;
		const char* const* tabs = nullptr;
		unsigned tab_count = 0, active_tab = 0;
		bool bar_focused = false;
		ImTextureData* logo = nullptr;
		const char* title = "";
		const char* subtitle = ""; // the count of games, or the updater's line
		const char* status = "";
		const char* version = "";
		// The covers (Biblioteca, Recientes, Favoritos).
		const Card* cards = nullptr;
		size_t card_count = 0, selected = 0;
		// A settings page: the rows on screen.
		bool settings = false;
		const Row* rows = nullptr;
		unsigned row_count = 0, row_first = 0, row_total = 0, row_focused = 0;
		// The Disc section.
		bool disc = false;
		ImTextureData* disc_cover = nullptr;
		const char* disc_lines[4] = {};
		Hint hints[6] = {};
		unsigned hint_count = 0;
	};

	inline void Button(const Canvas& c, float cx, float cy, char button)
	{
		const ImU32 ring = IM_COL32(210, 214, 222, 255);
		if (button == 'l')
		{
			c.Fill(cx - 30, cy - 15, 60, 30, IM_COL32(58, 62, 70, 255), 15);
			c.Centered(cx, cy + 6, 14, ring, "L1 R1");
			return;
		}
		c.draw->AddCircleFilled(c.P(cx, cy), 16.0f * c.scale, IM_COL32(52, 56, 64, 255), 32);
		c.draw->AddCircle(c.P(cx, cy), 16.0f * c.scale, ring, 32, 1.5f * c.scale);
		const float t = 2.4f * c.scale;
		switch (button)
		{
			case 'x':
				c.draw->AddLine(c.P(cx - 7, cy - 7), c.P(cx + 7, cy + 7), IM_COL32(124, 178, 255, 255), t);
				c.draw->AddLine(c.P(cx + 7, cy - 7), c.P(cx - 7, cy + 7), IM_COL32(124, 178, 255, 255), t);
				break;
			case 'o': c.draw->AddCircle(c.P(cx, cy), 7.5f * c.scale, IM_COL32(244, 104, 116, 255), 24, t); break;
			case 's': c.draw->AddRect(c.P(cx - 7, cy - 7), c.P(cx + 7, cy + 7), IM_COL32(236, 132, 210, 255), 0.0f, t); break;
			case 't':
				c.draw->AddTriangle(c.P(cx, cy - 8), c.P(cx + 8, cy + 6), c.P(cx - 8, cy + 6), IM_COL32(86, 214, 168, 255), t);
				break;
			default: break;
		}
	}

	// A cover fitted in its box, centered, keeping its shape.
	inline void FitImage(const Canvas& c, ImTextureData* image, float x, float y, float w, float h)
	{
		if (!image || image->Width <= 0 || image->Height <= 0)
			return;
		const float image_aspect = static_cast<float>(image->Width) / static_cast<float>(image->Height);
		float image_w = w, image_h = h;
		if (image_aspect > w / h)
			image_h = w / image_aspect;
		else
			image_w = h * image_aspect;
		const float ix = x + (w - image_w) * 0.5f, iy = y + (h - image_h) * 0.5f;
		c.draw->AddImage(image->GetTexRef(), c.P(ix, iy), c.P(ix + image_w, iy + image_h));
	}

	inline void Draw(const Canvas& c, const Page& p)
	{
		const ImU32 accent = AccentColor(p.color), accent_soft = AccentColor(p.color, 72), accent_line = AccentColor(p.color, 160);
		const ImU32 white = IM_COL32(236, 239, 244, 255), muted = IM_COL32(152, 158, 168, 255), faint = IM_COL32(104, 110, 120, 255);
		const ImU32 panel = IM_COL32(36, 39, 45, 255), line = IM_COL32(60, 64, 72, 255);

		// Graphite, a little lighter at the top.
		c.draw->AddRectFilled(ImVec2(0.0f, 0.0f), c.size, IM_COL32(17, 18, 21, 255));
		c.draw->AddRectFilledMultiColor(c.P(0, 0), c.P(1920, 1080), IM_COL32(31, 33, 38, 255), IM_COL32(31, 33, 38, 255),
			IM_COL32(16, 17, 20, 255), IM_COL32(16, 17, 20, 255));

		// The bar on top: the logo and the sections. Focused (Up from the first row), Left/Right change the section.
		c.Fill(0, 0, 1920, 96, IM_COL32(34, 37, 43, 255));
		c.Fill(0, 94, 1920, 2, accent_line);
		if (p.logo)
			FitImage(c, p.logo, 24, 14, 196, 66);
		else
			c.Text(32, 60, 30, white, "FORJA SX2");
		if (p.tab_count > 0)
		{
			constexpr float tabs_x = 248.0f, tabs_right = 1892.0f, gap = 8.0f;
			const float tab_w = (tabs_right - tabs_x - gap * static_cast<float>(p.tab_count - 1)) / static_cast<float>(p.tab_count);
			for (unsigned i = 0; i < p.tab_count; i++)
			{
				const float x = tabs_x + static_cast<float>(i) * (tab_w + gap);
				const bool active = i == p.active_tab;
				if (active)
				{
					c.Fill(x, 22, tab_w, 52, p.bar_focused ? accent : accent_soft, 26);
					if (p.bar_focused)
						c.Frame(x - 3, 19, tab_w + 6, 58, white, 29, 2);
				}
				c.Centered(x + tab_w * 0.5f, 56, 17, active ? white : muted, p.tabs[i], tab_w - 18);
			}
		}

		// The section's name, and on the right how many games (or the updater's line).
		c.Text(80, 150, 30, white, p.title, 1100);
		c.Right(1840, 148, 18, muted, p.subtitle, 680);
		c.Fill(80, 166, 1760, 1, line);

		if (p.settings)
		{
			constexpr float top = 196.0f, pitch = 92.0f, height = 84.0f;
			for (unsigned i = 0; i < p.row_count; i++)
			{
				const Row& r = p.rows[i];
				const float y = top + static_cast<float>(i) * pitch;
				c.Fill(80, y, 1760, height, r.focused && !p.bar_focused ? accent_soft : panel, 10);
				if (r.focused && !p.bar_focused)
					c.Frame(80, y, 1760, height, accent, 10, 2);
				char shown[200];
				std::snprintf(shown, sizeof(shown), r.focused && r.arrows ? "<  %s  >" : "%s", r.value);
				const float value_w = std::min(c.Width(26, shown), 640.0f);
				c.Text(1810 - value_w, y + 38, 26, r.focused ? white : accent, shown, 640);
				c.Text(110, y + 38, 26, white, r.label, 1810 - value_w - 40 - 110);
				c.Text(110, y + 70, 18, muted, r.help, 1680);
			}
			if (p.row_total > p.row_count && p.row_count > 0)
			{
				const float track_h = static_cast<float>(p.row_count) * pitch - (pitch - height);
				const float thumb_h = track_h * static_cast<float>(p.row_count) / static_cast<float>(p.row_total);
				const float thumb_y = top + (track_h - thumb_h) * static_cast<float>(p.row_first) /
					static_cast<float>(p.row_total - p.row_count);
				c.Fill(1856, top, 6, track_h, line, 3);
				c.Fill(1856, thumb_y, 6, thumb_h, accent, 3);
			}
		}
		else if (p.disc)
		{
			constexpr float x = 520, y = 220, w = 320, h = 450;
			c.Fill(x - 12, y - 12, w + 24, h + 24, accent_soft, 14);
			c.Fill(x, y, w, h, panel, 8);
			if (p.disc_cover)
				FitImage(c, p.disc_cover, x + 2, y + 2, w - 4, h - 4);
			else
			{
				// A disc, drawn.
				const ImVec2 center = c.P(x + w * 0.5f, y + h * 0.5f);
				c.draw->AddCircleFilled(center, 120.0f * c.scale, IM_COL32(70, 76, 88, 255), 64);
				c.draw->AddCircle(center, 120.0f * c.scale, accent, 64, 3.0f * c.scale);
				c.draw->AddCircleFilled(center, 22.0f * c.scale, IM_COL32(24, 26, 30, 255), 32);
			}
			for (unsigned i = 0; i < 4; i++)
				c.Text(900, 330 + 46.0f * static_cast<float>(i), i == 0 ? 30.0f : 20.0f, i == 0 ? white : muted, p.disc_lines[i], 900);
		}
		else if (p.card_count > 0)
		{
			// Five covers a row, the name under each; the selected one on a plate of the style's color.
			constexpr float cover_w = 240, cover_h = 330, column_step = 352, row_step = 392, grid_y = 196;
			constexpr size_t columns = ModernColumns, visible_rows = ModernVisibleRows;
			const float grid_x = (1920.0f - (static_cast<float>(columns - 1) * column_step + cover_w)) * 0.5f;
			const size_t selected = std::min(p.selected, p.card_count - 1);
			const size_t first_row = FirstRow(selected, p.card_count, columns, visible_rows);
			static constexpr unsigned char shades[5][3] = {{58, 74, 98}, {92, 62, 70}, {96, 74, 52}, {52, 86, 80}, {76, 66, 108}};
			for (size_t slot = 0; slot < columns * visible_rows; slot++)
			{
				const size_t index = first_row * columns + slot;
				if (index >= p.card_count)
					break;
				const Card& card = p.cards[index];
				const float x = grid_x + static_cast<float>(slot % columns) * column_step;
				const float y = grid_y + static_cast<float>(slot / columns) * row_step;
				const bool is_selected = index == selected;
				if (is_selected)
				{
					c.Fill(x - 14, y - 14, cover_w + 28, cover_h + 66, p.bar_focused ? panel : accent_soft, 14);
					if (!p.bar_focused)
						c.Frame(x - 14, y - 14, cover_w + 28, cover_h + 66, accent, 14, 3);
				}
				c.Fill(x, y, cover_w, cover_h, IM_COL32(44, 48, 56, 255), 8);
				if (card.cover)
					FitImage(c, card.cover, x, y, cover_w, cover_h);
				else
				{
					const auto& s = shades[index % 5];
					c.draw->AddRectFilledMultiColor(c.P(x, y), c.P(x + cover_w, y + cover_h), IM_COL32(s[0], s[1], s[2], 255),
						IM_COL32(s[0], s[1], s[2], 255), IM_COL32(24, 26, 31, 255), IM_COL32(24, 26, 31, 255));
					char initial[2] = {card.name && (card.name[0] & 0x80) == 0 ? card.name[0] : '?', 0};
					c.Centered(x + cover_w * 0.5f, y + cover_h * 0.5f + 34, 96, IM_COL32(255, 255, 255, 70), initial);
				}
				if (card.favorite)
				{
					// A star on a dark round badge.
					const ImVec2 center = c.P(x + cover_w - 24, y + 24);
					c.draw->AddCircleFilled(center, 18.0f * c.scale, IM_COL32(20, 22, 26, 225), 32);
					ImVec2 star[10];
					for (int k = 0; k < 10; k++)
					{
						const float radius = (k % 2 == 0 ? 11.0f : 4.6f) * c.scale;
						const float angle = -1.5708f + static_cast<float>(k) * 0.6283f;
						star[k] = ImVec2(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
					}
					c.draw->AddConcavePolyFilled(star, 10, accent);
				}
				c.Centered(x + cover_w * 0.5f, y + cover_h + 34, 18, is_selected ? white : muted, card.name, cover_w + 20);
			}
			const size_t total_rows = (p.card_count + columns - 1) / columns;
			if (total_rows > visible_rows)
			{
				const float track_h = row_step * static_cast<float>(visible_rows) - (row_step - cover_h - 52);
				const float thumb_h = std::max(80.0f, track_h * static_cast<float>(visible_rows) / static_cast<float>(total_rows));
				const float thumb_y = grid_y + (track_h - thumb_h) * static_cast<float>(first_row) /
					static_cast<float>(total_rows - visible_rows);
				c.Fill(1868, grid_y, 6, track_h, line, 3);
				c.Fill(1868, thumb_y, 6, thumb_h, accent, 3);
			}
		}

		// The bar at the bottom: the buttons, the status line (on the right, clear of the names under the covers) and the version.
		c.Fill(0, 986, 1920, 94, IM_COL32(28, 30, 35, 255));
		c.Fill(0, 986, 1920, 1, line);
		float hint_x = 96;
		for (unsigned i = 0; i < p.hint_count && i < 6; i++)
		{
			const Hint& h = p.hints[i];
			Button(c, hint_x, 1033, h.button);
			const float label_x = hint_x + (h.button == 'l' ? 40.0f : 26.0f);
			const float label_w = std::min(c.Width(17, h.label), 330.0f);
			c.Text(label_x, 1040, 17, h.dim ? faint : white, h.label, 330);
			hint_x = label_x + label_w + 56;
		}
		const float version_w = std::min(c.Width(15, p.version), 360.0f);
		c.Right(1880, 1040, 15, faint, p.version, 360);
		if (p.status && *p.status)
			c.Right(1880 - version_w - 40, 1040, 17, accent, p.status, std::max(200.0f, 1880 - version_w - 40 - hint_x));
	}
} // namespace ForjaStyle
