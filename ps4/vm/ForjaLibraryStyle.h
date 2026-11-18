// SPDX-License-Identifier: GPL-3.0+
// Forja SX2: the library's two designs (Ajustes > Estilos), drawn the same on the PS4 and in the PC preview
// (herramientas/revision-local/vista-previa): header-only, ImGui alone, every text passed already translated.
// - "Original de Forja" (compilation 143, then called "Nuevo"; Joseph, 2026-11-18: "ponle como diseño original ... buscamos nuestra
//   propia identidad"): the sections on a bar on top, five covers a row with each game's name under it, graphite, the selected cover on
//   a plate of the player's color. The library's old design, taken from another program's, is gone (compilation 145).
// - "Carrusel" (compilation 145; Joseph: "un juego en el medio, uno a la derecha y a la izquierda ... como la ruleta de un revólver,
//   que gira"): the selected game big in the middle and the others on a drum around it, smaller and darker the farther they are, each
//   with its reflection; it turns (GameMenu.cpp eases `spin` towards the selected game) and goes round: left of the first is the last.
#pragma once

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace ForjaStyle
{
	constexpr int Original = 0, Carousel = 1;
	constexpr int ColorCount = 5; // Azul, Morado, Rojo, Verde, Naranja
	constexpr size_t GridColumns = 5;
	constexpr size_t GridVisibleRows = 2;
	constexpr int CarouselReach = 3; // covers on each side of the selected one

	// Where `index` sits on the drum when `spin` is in the middle: -count/2 .. count/2 (it goes round).
	inline float CarouselOffset(size_t index, float spin, size_t count)
	{
		const float n = static_cast<float>(count);
		float k = std::fmod(static_cast<float>(index) - spin, n);
		if (k < -n * 0.5f)
			k += n;
		else if (k >= n * 0.5f)
			k -= n;
		return k;
	}

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
		bool carousel = false; // the Carrusel design
		float spin = 0.0f;     // its turn: the index in the middle now (between two while it moves)
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
		if (button == 'l' || button == 'p') // L1/R1, or OPTIONS (compilation 146)
		{
			const float half = button == 'p' ? 44.0f : 30.0f;
			c.Fill(cx - half, cy - 15, half * 2, 30, IM_COL32(58, 62, 70, 255), 15);
			c.Centered(cx, cy + 6, 14, ring, button == 'p' ? "OPTIONS" : "L1 R1");
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

	// The placeholder of a game without a cover: a gradient of its own and its first letter.
	inline void PlaceholderCover(const Canvas& c, size_t index, const char* name, float x, float y, float w, float h, int alpha = 255)
	{
		static constexpr unsigned char shades[5][3] = {{58, 74, 98}, {92, 62, 70}, {96, 74, 52}, {52, 86, 80}, {76, 66, 108}};
		const auto& s = shades[index % 5];
		c.draw->AddRectFilledMultiColor(c.P(x, y), c.P(x + w, y + h), IM_COL32(s[0], s[1], s[2], alpha), IM_COL32(s[0], s[1], s[2], alpha),
			IM_COL32(24, 26, 31, alpha), IM_COL32(24, 26, 31, alpha));
		char initial[2] = {name && (name[0] & 0x80) == 0 ? name[0] : '?', 0};
		c.Centered(x + w * 0.5f, y + h * 0.5f + h * 0.1f, h * 0.29f, IM_COL32(255, 255, 255, alpha * 70 / 255), initial);
	}

	// A star on a dark round badge, for a favorite.
	inline void FavoriteBadge(const Canvas& c, float cx, float cy, float size, ImU32 color)
	{
		const ImVec2 center = c.P(cx, cy);
		c.draw->AddCircleFilled(center, size * c.scale, IM_COL32(20, 22, 26, 225), 32);
		ImVec2 star[10];
		for (int k = 0; k < 10; k++)
		{
			const float radius = (k % 2 == 0 ? size * 0.61f : size * 0.26f) * c.scale;
			const float angle = -1.5708f + static_cast<float>(k) * 0.6283f;
			star[k] = ImVec2(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
		}
		c.draw->AddConcavePolyFilled(star, 10, color);
	}

	// The Carrusel: the covers on a drum, the farthest drawn first. k is a cover's place from the middle (fractional while it turns).
	inline void DrawCarousel(const Canvas& c, const Page& p, ImU32 accent, ImU32 accent_soft, ImU32 white, ImU32 muted)
	{
		constexpr float center_x = 960.0f, center_y = 430.0f, cover_w = 340.0f, cover_h = 470.0f;
		const size_t count = p.card_count;
		const size_t selected = std::min(p.selected, count - 1);
		struct Place
		{
			size_t index;
			float k;
		};
		Place places[2 * CarouselReach + 3];
		int used = 0;
		for (size_t i = 0; i < count && used < static_cast<int>(sizeof(places) / sizeof(places[0])); i++)
		{
			const float k = CarouselOffset(i, p.spin, count);
			if (std::fabs(k) <= static_cast<float>(CarouselReach) + 0.5f)
				places[used++] = {i, k};
		}
		std::sort(places, places + used, [](const Place& a, const Place& b) { return std::fabs(a.k) > std::fabs(b.k); });
		// A soft light of the player's color behind the middle.
		c.draw->AddCircleFilled(c.P(center_x, center_y + 40), 330.0f * c.scale, AccentColor(p.color, 22), 64);
		for (int n = 0; n < used; n++)
		{
			const Place& place = places[n];
			const float a = std::fabs(place.k);
			const float sign = place.k < 0 ? -1.0f : 1.0f;
			// Each step out: 26 % smaller, a little lower, darker; the gaps shrink as on a drum seen from the front.
			const float size = std::pow(0.74f, a);
			const float x_mid = center_x + sign * (360.0f * a - 35.0f * a * a);
			const float y_mid = center_y + 10.0f * a * a;
			const float w = cover_w * size, h = cover_h * size;
			const float x = x_mid - w * 0.5f, y = y_mid - h * 0.5f;
			const int alpha = static_cast<int>(255.0f * std::max(0.25f, 1.0f - 0.2f * a));
			const Card& card = p.cards[place.index];
			const bool middle = a < 0.5f;
			if (middle && !p.bar_focused)
			{
				c.Fill(x - 16, y - 16, w + 32, h + 32, accent_soft, 18);
				c.Frame(x - 8, y - 8, w + 16, h + 16, accent, 12, 4);
			}
			c.Fill(x, y, w, h, IM_COL32(44, 48, 56, alpha), 8 * size);
			if (card.cover)
			{
				FitImage(c, card.cover, x, y, w, h);
				// Its reflection: the bottom of the cover upside down, fading into the floor.
				const float rh = h * 0.22f;
				c.draw->AddImage(card.cover->GetTexRef(), c.P(x, y + h + 6), c.P(x + w, y + h + 6 + rh), ImVec2(0, 1), ImVec2(1, 0.78f),
					IM_COL32(255, 255, 255, alpha * 60 / 255));
				c.draw->AddRectFilledMultiColor(c.P(x, y + h + 6), c.P(x + w, y + h + 6 + rh), IM_COL32(20, 21, 25, 40), IM_COL32(20, 21, 25, 40),
					IM_COL32(20, 21, 25, 255), IM_COL32(20, 21, 25, 255));
			}
			else
				PlaceholderCover(c, place.index, card.name, x, y, w, h, alpha);
			// The farther ones in shadow.
			if (!middle)
				c.Fill(x, y, w, h, IM_COL32(14, 15, 18, static_cast<int>(std::min(1.0f, a / 3.5f) * 150.0f)), 8 * size);
			if (card.favorite)
				FavoriteBadge(c, x + w - 26 * size, y + 26 * size, 20.0f * size, accent);
		}
		// The selected game's name, and where it is in the list.
		const Card& current = p.cards[selected];
		c.Centered(center_x, 836, 34, white, current.name, 1500);
		char where[32];
		std::snprintf(where, sizeof(where), "%u / %u", static_cast<unsigned>(selected + 1), static_cast<unsigned>(count));
		c.Centered(center_x, 880, 18, muted, where);
		// An arrow each side: it turns with Left and Right.
		if (count > 1)
		{
			const ImU32 arrow = IM_COL32(236, 239, 244, 150);
			c.draw->AddTriangleFilled(c.P(70, 430), c.P(96, 404), c.P(96, 456), arrow);
			c.draw->AddTriangleFilled(c.P(1850, 430), c.P(1824, 456), c.P(1824, 404), arrow);
		}
	}

	// Compilation 146: a game's own settings, over the library (Options on a game). One row a setting: its name and value, its help
	// under them; the last row is an action.
	struct DialogRow
	{
		const char* label = "";
		const char* value = "";
		const char* help = "";
		bool action = false;
	};

	inline void DrawGameDialog(const Canvas& c, int color, const char* title, const char* subtitle, const DialogRow* rows, unsigned count,
		unsigned focus, const Hint* hints, unsigned hint_count)
	{
		const ImU32 accent = AccentColor(color), accent_soft = AccentColor(color, 72);
		const ImU32 white = IM_COL32(236, 239, 244, 255), muted = IM_COL32(152, 158, 168, 255);
		c.draw->AddRectFilled(ImVec2(0.0f, 0.0f), c.size, IM_COL32(0, 0, 0, 170));
		constexpr float x = 330, y = 130, w = 1260, h = 820;
		c.Fill(x, y, w, h, IM_COL32(30, 33, 38, 252), 18);
		c.Frame(x, y, w, h, AccentColor(color, 170), 18, 2);
		c.Fill(x, y, w, 6, accent, 3);
		c.Text(x + 44, y + 70, 32, white, title, w - 88);
		c.Text(x + 44, y + 104, 18, muted, subtitle, w - 88);
		constexpr float top = y + 136, pitch = 92, height = 82;
		for (unsigned i = 0; i < count; i++)
		{
			const DialogRow& r = rows[i];
			const float ry = top + static_cast<float>(i) * pitch;
			const bool focused = i == focus;
			c.Fill(x + 30, ry, w - 60, height, focused ? accent_soft : IM_COL32(40, 44, 50, 255), 10);
			if (focused)
				c.Frame(x + 30, ry, w - 60, height, accent, 10, 2);
			char shown[200];
			std::snprintf(shown, sizeof(shown), focused && !r.action ? "<  %s  >" : "%s", r.value);
			const float value_w = std::min(c.Width(24, shown), 520.0f);
			c.Text(x + w - 60 - value_w, ry + 36, 24, focused ? white : accent, shown, 520);
			c.Text(x + 60, ry + 36, 24, white, r.label, w - 180 - value_w);
			c.Text(x + 60, ry + 66, 16, muted, r.help, w - 120);
		}
		float hint_x = x + 70;
		for (unsigned i = 0; i < hint_count && i < 6; i++)
		{
			Button(c, hint_x, y + h - 42, hints[i].button);
			const float label_x = hint_x + (hints[i].button == 'p' ? 54.0f : hints[i].button == 'l' ? 40.0f : 26.0f);
			c.Text(label_x, y + h - 35, 17, white, hints[i].label, 330);
			hint_x = label_x + std::min(c.Width(17, hints[i].label), 330.0f) + 56;
		}
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
		else if (p.card_count > 0 && p.carousel)
			DrawCarousel(c, p, accent, accent_soft, white, muted);
		else if (p.card_count > 0)
		{
			// Five covers a row, the name under each; the selected one on a plate of the style's color.
			constexpr float cover_w = 240, cover_h = 330, column_step = 352, row_step = 392, grid_y = 196;
			constexpr size_t columns = GridColumns, visible_rows = GridVisibleRows;
			const float grid_x = (1920.0f - (static_cast<float>(columns - 1) * column_step + cover_w)) * 0.5f;
			const size_t selected = std::min(p.selected, p.card_count - 1);
			const size_t first_row = FirstRow(selected, p.card_count, columns, visible_rows);
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
					PlaceholderCover(c, index, card.name, x, y, cover_w, cover_h);
				if (card.favorite)
					FavoriteBadge(c, x + cover_w - 24, y + 24, 18.0f, accent);
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
			const float label_x = hint_x + (h.button == 'p' ? 54.0f : h.button == 'l' ? 40.0f : 26.0f);
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
