// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (2026-10-19): the emulator in 13 languages. The texts in the code are the Spanish ones. T() gives the
// translation for the language picked in the library (ajustes.ini: idioma), read from /app0/resources/lang/<code>.lng
// ("es:" and "tr:" line pairs), or the same Spanish when the file has none for that text. TF() is T() for a printf format:
// a translation whose %s/%u/... differ from the Spanish is not used, so a wrong line can't crash the app. N_() only marks a
// text kept in a table, for ps4/vm/assets/lang/idiomas.py; it is translated with T() where it is used.
// Visual() turns Arabic into what ImGui can draw: the letters' joined shapes (Unicode presentation forms) and the
// right-to-left order, with numbers and Latin words kept left to right.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define N_(text) text

namespace ForjaLang
{
	struct Language
	{
		const char* code;
		const char* name; // in its own language, as the picker shows it
	};
	inline constexpr Language Languages[] = {{"es", "Español"}, {"en", "English"}, {"pt", "Português (Brasil)"},
		{"fr", "Français"}, {"de", "Deutsch"}, {"it", "Italiano"}, {"ru", "Русский"}, {"zh", "简体中文"}, {"ja", "日本語"},
		{"ko", "한국어"}, {"ar", "العربية"}, {"tr", "Türkçe"}, {"pl", "Polski"}};
	inline constexpr unsigned LanguageCount = sizeof(Languages) / sizeof(Languages[0]);
	inline constexpr unsigned Japanese = 8, Arabic = 10;
#ifndef FORJA_LANG_DIRECTORY
#define FORJA_LANG_DIRECTORY "/app0/resources/lang" // a test on the PC points it at assets/lang
#endif
	inline constexpr const char* Directory = FORJA_LANG_DIRECTORY;

	struct Table
	{
		std::deque<std::string> storage; // a deque: its strings never move, the map points into them
		std::unordered_map<std::string_view, const char*> texts;
	};

	namespace Detail
	{
		// A table is never freed: another thread may still read the one it got (a few tens of KB, one per language at most).
		inline std::atomic<const Table*> s_tables[LanguageCount] = {};
		inline std::atomic<unsigned> s_current{0};

		inline std::string Unescape(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t i = 0; i < text.size(); i++)
			{
				if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == '\\'))
					out += text[++i] == 'n' ? '\n' : '\\';
				else
					out += text[i];
			}
			return out;
		}

		// The printf conversions of a format, in order ("%u|%zu|%.1f|%%|").
		inline std::string Conversions(std::string_view format)
		{
			std::string out;
			for (size_t i = 0; i < format.size(); i++)
			{
				if (format[i] != '%')
					continue;
				size_t end = i + 1;
				while (end < format.size() && std::strchr("-+ #0123456789.hlzjt", format[end]))
					end++;
				if (end < format.size() && std::strchr("diouxXeEfFgGcsp%", format[end]))
				{
					out.append(format.substr(i, end - i + 1));
					out += '|';
				}
				i = end;
			}
			return out;
		}
	} // namespace Detail

	// Reads <code>.lng once. Spanish (0) has no table: its texts are the code's.
	inline const Table* LoadTable(unsigned index)
	{
		if (index == 0 || index >= LanguageCount)
			return nullptr;
		if (const Table* table = Detail::s_tables[index].load(std::memory_order_acquire))
			return table;
		char path[96];
		std::snprintf(path, sizeof(path), "%s/%s.lng", Directory, Languages[index].code);
		std::FILE* file = std::fopen(path, "rb");
		if (!file)
			return nullptr;
		std::string data;
		char chunk[16384];
		for (size_t read; (read = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
			data.append(chunk, read);
		std::fclose(file);
		auto* table = new Table();
		std::string key;
		bool have_key = false;
		for (size_t start = 0; start < data.size();)
		{
			size_t end = data.find('\n', start);
			if (end == std::string::npos)
				end = data.size();
			std::string_view line(data.data() + start, end - start);
			start = end + 1;
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			if (line.size() >= 4 && line.substr(0, 4) == "es: ")
			{
				key = Detail::Unescape(line.substr(4));
				have_key = true;
			}
			else if (have_key && line.size() > 4 && line.substr(0, 4) == "tr: ")
			{
				have_key = false;
				const std::string& stored_key = table->storage.emplace_back(key);
				const std::string& stored_text = table->storage.emplace_back(Detail::Unescape(line.substr(4)));
				table->texts.emplace(std::string_view(stored_key), stored_text.c_str());
			}
		}
		const Table* expected = nullptr;
		if (!Detail::s_tables[index].compare_exchange_strong(expected, table, std::memory_order_acq_rel))
		{
			delete table;
			return expected;
		}
		return table;
	}

	inline unsigned IndexOf(const char* code)
	{
		for (unsigned i = 0; code && i < LanguageCount; i++)
			if (!std::strcmp(code, Languages[i].code))
				return i;
		return 0;
	}

	// The PS4's own language (sceSystemServiceParamGetInt, ORBIS_SYSTEM_PARAM_LANG_*), for the picker's first choice.
	inline unsigned FromSystemLanguage(int system)
	{
		static constexpr unsigned map[] = {8, 1, 3, 0, 4, 5, 1, 2, 6, 9, 7, 7, 1, 1, 1, 1, 12, 2, 1, 11, 0, 10, 3};
		return system >= 0 && system < static_cast<int>(sizeof(map) / sizeof(map[0])) ? map[system] : 1;
	}

	inline unsigned Current() { return Detail::s_current.load(std::memory_order_acquire); }
	inline const char* CurrentCode() { return Languages[Current()].code; }

	// Makes a language the current one. False (and Spanish) when its file can't be read.
	inline bool Use(unsigned index)
	{
		if (index >= LanguageCount)
			index = 0;
		if (index != 0 && !LoadTable(index))
		{
			Detail::s_current.store(0, std::memory_order_release);
			return false;
		}
		Detail::s_current.store(index, std::memory_order_release);
		return true;
	}

	// A text in a given language (the picker shows its title in the language under the cursor).
	inline const char* TIn(unsigned index, const char* spanish)
	{
		const Table* table = index != 0 && index < LanguageCount ? Detail::s_tables[index].load(std::memory_order_acquire) : nullptr;
		if (!table || !spanish)
			return spanish;
		const auto found = table->texts.find(std::string_view(spanish));
		return found == table->texts.end() ? spanish : found->second;
	}

	inline const char* T(const char* spanish) { return TIn(Current(), spanish); }
	inline std::string T(const std::string& spanish) { return T(spanish.c_str()); }

	inline const char* TF(const char* spanish)
	{
		const char* text = T(spanish);
		return text == spanish || Detail::Conversions(spanish) == Detail::Conversions(text) ? text : spanish;
	}

	// printf into a std::string, for the translated formats (TF).
	inline std::string F(const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		return length <= 0 ? std::string() : std::string(buffer, std::min<size_t>(static_cast<size_t>(length), sizeof(buffer) - 1));
	}

	// Cuts a text to at most `bytes` without splitting a UTF-8 character.
	inline std::string Cut(std::string text, size_t bytes, const char* ellipsis = "...")
	{
		if (text.size() <= bytes)
			return text;
		size_t end = bytes > std::strlen(ellipsis) ? bytes - std::strlen(ellipsis) : 0;
		while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
			end--;
		text.resize(end);
		return text + ellipsis;
	}

	namespace Detail
	{
		inline std::u32string Decode(const char* text)
		{
			std::u32string out;
			const auto* p = reinterpret_cast<const unsigned char*>(text);
			while (*p)
			{
				char32_t c = *p;
				int more = 0;
				if (c >= 0xf0) { c &= 0x07; more = 3; }
				else if (c >= 0xe0) { c &= 0x0f; more = 2; }
				else if (c >= 0xc0) { c &= 0x1f; more = 1; }
				p++;
				for (; more > 0 && (*p & 0xc0) == 0x80; more--, p++)
					c = (c << 6) | (*p & 0x3f);
				out += c;
			}
			return out;
		}

		inline void Encode(char32_t c, std::string& out)
		{
			if (c < 0x80)
				out += static_cast<char>(c);
			else if (c < 0x800)
			{
				out += static_cast<char>(0xc0 | (c >> 6));
				out += static_cast<char>(0x80 | (c & 0x3f));
			}
			else if (c < 0x10000)
			{
				out += static_cast<char>(0xe0 | (c >> 12));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (c & 0x3f));
			}
			else
			{
				out += static_cast<char>(0xf0 | (c >> 18));
				out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (c & 0x3f));
			}
		}

		// U+0621..U+064A: isolated, final, initial and medial presentation forms (0: the letter has no such form).
		struct ArabicForms
		{
			char16_t isolated, final, initial, medial;
		};
		inline constexpr ArabicForms ArabicLetters[] = {
			{0xFE80, 0, 0, 0}, {0xFE81, 0xFE82, 0, 0}, {0xFE83, 0xFE84, 0, 0}, {0xFE85, 0xFE86, 0, 0}, {0xFE87, 0xFE88, 0, 0},
			{0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, {0xFE8D, 0xFE8E, 0, 0}, {0xFE8F, 0xFE90, 0xFE91, 0xFE92}, {0xFE93, 0xFE94, 0, 0},
			{0xFE95, 0xFE96, 0xFE97, 0xFE98}, {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
			{0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, {0xFEA9, 0xFEAA, 0, 0}, {0xFEAB, 0xFEAC, 0, 0},
			{0xFEAD, 0xFEAE, 0, 0}, {0xFEAF, 0xFEB0, 0, 0}, {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},
			{0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},
			{0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, {0xFEC9, 0xFECA, 0xFECB, 0xFECC}, {0xFECD, 0xFECE, 0xFECF, 0xFED0},
			{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, // U+063B..U+0640
			{0xFED1, 0xFED2, 0xFED3, 0xFED4}, {0xFED5, 0xFED6, 0xFED7, 0xFED8}, {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},
			{0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},
			{0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, {0xFEED, 0xFEEE, 0, 0}, {0xFEEF, 0xFEF0, 0, 0}, {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}};

		inline const ArabicForms* FormsOf(char32_t c)
		{
			if (c < 0x0621 || c > 0x064A || !ArabicLetters[c - 0x0621].isolated)
				return nullptr;
			return &ArabicLetters[c - 0x0621];
		}
		inline bool Transparent(char32_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }
		inline bool JoinsNext(char32_t c) // joins the letter after it (its left)
		{
			const ArabicForms* forms = FormsOf(c);
			return c == 0x0640 || (forms && forms->initial);
		}
		inline bool JoinsPrevious(char32_t c)
		{
			const ArabicForms* forms = FormsOf(c);
			return c == 0x0640 || (forms && forms->final);
		}

		inline std::u32string Shape(const std::u32string& in)
		{
			std::u32string out;
			for (size_t i = 0; i < in.size(); i++)
			{
				const char32_t c = in[i];
				const ArabicForms* forms = FormsOf(c);
				if (!forms)
				{
					out += c;
					continue;
				}
				char32_t previous = 0, next = 0;
				for (size_t p = i; p > 0;)
					if (!Transparent(in[--p]))
					{
						previous = in[p];
						break;
					}
				size_t n = i + 1;
				for (; n < in.size(); n++)
					if (!Transparent(in[n]))
					{
						next = in[n];
						break;
					}
				const bool joined_before = previous && JoinsNext(previous) && forms->final;
				if (c == 0x0644 && (next == 0x0622 || next == 0x0623 || next == 0x0625 || next == 0x0627))
				{
					// Lam + alef: one ligature, which joins only the letter before it.
					const char32_t isolated = next == 0x0622 ? 0xFEF5 : next == 0x0623 ? 0xFEF7 : next == 0x0625 ? 0xFEF9 : 0xFEFB;
					out += joined_before ? isolated + 1 : isolated;
					out.append(in, i + 1, n - i - 1);
					i = n;
					continue;
				}
				const bool joins_after = next && forms->initial && JoinsPrevious(next);
				out += (joined_before && joins_after) ? forms->medial : joined_before ? forms->final : joins_after ? forms->initial :
				                                                                                      forms->isolated;
			}
			return out;
		}

		enum Bidi : unsigned char { BidiL, BidiR, BidiEN, BidiES, BidiET, BidiCS, BidiNSM, BidiN };
		inline Bidi BidiClass(char32_t c)
		{
			if ((c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669))
				return BidiEN;
			if (c == '+' || c == '-')
				return BidiES;
			if (c == '%' || c == '#' || c == '$' || c == 0xB0)
				return BidiET;
			if (c == '.' || c == ',' || c == ':' || c == '/' || c == 0x060C) // U+060C: the Arabic comma
				return BidiCS;
			if (Transparent(c))
				return BidiNSM;
			if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF))
				return BidiR;
			if (c < 0x80)
				return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ? BidiL : BidiN;
			if (c == 0xAB || c == 0xBB || c == 0xB7 || c == 0xD7 || (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F))
				return BidiN;
			return BidiL;
		}

		// A simplified Unicode bidi algorithm for one line: weak types (W1-W7), neutrals (N1-N2), two levels, reversal (L2).
		inline std::u32string Reorder(const std::u32string& line, bool rtl)
		{
			const size_t size = line.size();
			std::vector<unsigned char> type(size, BidiN);
			for (size_t i = 0; i < size; i++)
				type[i] = BidiClass(line[i]);
			if (!rtl)
			{
				// The paragraph takes the direction of its first strong letter.
				for (size_t i = 0; i < size; i++)
					if (type[i] == BidiL || type[i] == BidiR)
					{
						rtl = type[i] == BidiR;
						break;
					}
				if (!rtl)
					return line;
			}
			for (size_t i = 0; i < size; i++) // W1
				if (type[i] == BidiNSM)
					type[i] = i == 0 ? BidiR : type[i - 1];
			for (size_t i = 1; i + 1 < size; i++) // W4
				if ((type[i] == BidiES || type[i] == BidiCS) && type[i - 1] == BidiEN && type[i + 1] == BidiEN)
					type[i] = BidiEN;
			for (size_t i = 0; i < size; i++) // W5
			{
				if (type[i] != BidiET)
					continue;
				size_t end = i;
				while (end < size && type[end] == BidiET)
					end++;
				const bool number = (i > 0 && type[i - 1] == BidiEN) || (end < size && type[end] == BidiEN);
				for (size_t k = i; k < end; k++)
					type[k] = number ? BidiEN : BidiN;
				i = end - 1;
			}
			unsigned char strong = BidiR; // W6, W7
			for (size_t i = 0; i < size; i++)
			{
				if (type[i] == BidiES || type[i] == BidiET || type[i] == BidiCS)
					type[i] = BidiN;
				else if (type[i] == BidiL || type[i] == BidiR)
					strong = type[i];
				else if (type[i] == BidiEN && strong == BidiL)
					type[i] = BidiL;
			}
			for (size_t i = 0; i < size; i++) // N1, N2: numbers count as right to left here
			{
				if (type[i] != BidiN)
					continue;
				size_t end = i;
				while (end < size && type[end] == BidiN)
					end++;
				const unsigned char before = i == 0 ? BidiR : (type[i - 1] == BidiL ? BidiL : BidiR);
				const unsigned char after = end == size ? BidiR : (type[end] == BidiL ? BidiL : BidiR);
				for (size_t k = i; k < end; k++)
					type[k] = before == after ? before : BidiR;
				i = end - 1;
			}
			std::u32string out = line;
			for (size_t i = 0; i < size; i++) // the left-to-right pieces keep their order...
			{
				if (type[i] == BidiR)
					continue;
				size_t end = i;
				while (end < size && type[end] != BidiR)
					end++;
				std::reverse(out.begin() + i, out.begin() + end);
				i = end - 1;
			}
			std::reverse(out.begin(), out.end()); // ...and the line reads from the right
			for (size_t i = 0; i < size; i++)
			{
				if (type[size - 1 - i] != BidiR)
					continue;
				char32_t& c = out[i];
				c = c == '(' ? ')' : c == ')' ? '(' : c == '[' ? ']' : c == ']' ? '[' : c == '<' ? '>' : c == '>' ? '<' :
					c == 0xAB ? 0xBB : c == 0xBB ? 0xAB : c;
			}
			return out;
		}

		inline std::unordered_map<std::string, std::string> s_visual_cache;
	} // namespace Detail

	// What the menus draw: Arabic shaped and in visual order; anything else as it is. The menus' thread only.
	inline const char* Visual(const char* text)
	{
		if (!text)
			return text;
		bool arabic = false;
		for (const char* p = text; *p && !arabic; p++)
			arabic = static_cast<unsigned char>(*p) >= 0xD8 && static_cast<unsigned char>(*p) <= 0xDB;
		if (!arabic)
			return text;
		auto found = Detail::s_visual_cache.find(text);
		if (found != Detail::s_visual_cache.end())
			return found->second.c_str();
		const std::u32string logical = Detail::Decode(text);
		std::string visual;
		for (size_t start = 0; start <= logical.size();)
		{
			size_t end = logical.find(U'\n', start);
			if (end == std::u32string::npos)
				end = logical.size();
			for (const char32_t c : Detail::Reorder(Detail::Shape(logical.substr(start, end - start)), Current() == Arabic))
				Detail::Encode(c, visual);
			if (end < logical.size())
				visual += '\n';
			start = end + 1;
		}
		return Detail::s_visual_cache.emplace(text, std::move(visual)).first->second.c_str();
	}

	// Once per drawn frame, before any Visual(): the cache stays small and no pointer it gave is still in use.
	inline void NewFrame()
	{
		if (Detail::s_visual_cache.size() > 1024)
			Detail::s_visual_cache.clear();
	}
} // namespace ForjaLang
