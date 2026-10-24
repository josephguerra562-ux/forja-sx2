// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja 1.06: the session's summary (SessionStats.h). One mutex for all of it: the GS thread adds a window every
// 5 s and the odd hitch, the CPU thread a heartbeat every 5 s and the file every minute.
#include "SessionStats.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#ifndef FORJA_SESSION_SUMMARY
#define FORJA_SESSION_SUMMARY "/data/ForjaSX2/logs/resumen.txt"
#endif

namespace
{
	constexpr int kSpeedBins = 201; // percent, 0 to 200
	constexpr int kFpsBins = 121;   // frames a second, 0 to 120

	struct Stats
	{
		uint32_t windows = 0, full_speed = 0;
		uint32_t speed[kSpeedBins] = {};
		uint32_t fps[kFpsBins] = {};
		uint32_t limits[ForjaSession::LimitCount] = {};
		uint32_t fg_windows = 0;
		uint32_t movie_windows = 0, movies = 0;
		uint32_t movie_speed[kSpeedBins] = {};
		uint32_t movie_fps[kFpsBins] = {};
		bool in_movie = false;
		// Hitches over 50 ms and over twice the usual gap (the ones GSRendererPS4.cpp logs; compilation 76), by what happened in them.
		uint32_t hitches = 0, hitches_200 = 0, hitch_pipelines = 0, hitch_resets = 0, hitch_compile = 0, hitch_disc = 0,
				 hitch_other = 0;
		double hitch_worst_ms = 0.0;
		uint64_t flexible_min = 0, heap_max = 0;
		bool memory_seen = false;
		uint32_t audio_last = 0;
		uint64_t audio_cuts = 0, limited_last = 0, limited = 0;
		uint64_t audio_cuts_movie = 0; // compilation 79: of audio_cuts, the ones while a movie played
		// Compilation 79: the heap after the first minute (the game's load is in it), and the heartbeats since, for its growth.
		uint64_t heap_at_minute = 0;
		uint32_t heartbeats = 0;
		bool audio_seen = false;
		uint64_t skipped = 0, guard_pauses = 0;
		// Compilation 77: per limit, the windows by how much cheaper the limiting part had to be (percent, 0-99). Compilation 80: of
		// the game's own windows only (a menu's or a movie's limit says nothing of the game).
		uint32_t need[ForjaSession::LimitCount][100] = {};
		uint32_t need_windows[ForjaSession::LimitCount] = {};
		// Compilation 80: the scenes. Each one's windows and speeds; the game's own, its frames a second and its limits; when the
		// first frame and the game itself came (the window's number, -1 if never); the loads in a row (each one's length in
		// windows); the controllers; the windows where two parts were at their limit together; the hangs.
		uint32_t scene_windows[ForjaSession::SceneCount] = {};
		uint32_t scene_speed[ForjaSession::SceneCount][kSpeedBins] = {};
		uint32_t game_fps[kFpsBins] = {};
		uint32_t game_limits[ForjaSession::LimitCount] = {};
		int first_frame_window = -1, first_game_window = -1;
		uint32_t game_run = 0; // the game's windows in a row, to call it reached after two (10 s)
		uint32_t loads = 0, load_run = 0, load_longest = 0;
		int pads_most = 1;
		uint32_t pads2_windows = 0, pads2_speed[kSpeedBins] = {}, pads1_game_windows = 0, pads1_speed[kSpeedBins] = {};
		uint32_t two_at_limit = 0;
		uint32_t hangs = 0;
	};

	std::mutex s_mutex;
	Stats s_stats;

	int Bin(double value, int bins)
	{
		return std::clamp(static_cast<int>(value + 0.5), 0, bins - 1);
	}

	// The value under which `share` of the windows fall (0.5: the median).
	int Percentile(const uint32_t* histogram, int bins, uint32_t count, double share)
	{
		if (count == 0)
			return 0;
		const uint32_t target = static_cast<uint32_t>(count * share);
		uint32_t seen = 0;
		for (int i = 0; i < bins; i++)
		{
			seen += histogram[i];
			if (seen > target)
				return i;
		}
		return bins - 1;
	}

	unsigned Share(uint32_t part, uint32_t whole)
	{
		return whole ? static_cast<unsigned>((part * 100u + whole / 2) / whole) : 0u;
	}

	void Append(std::string& text, const char* format, ...) __attribute__((format(printf, 2, 3)));
	void Append(std::string& text, const char* format, ...)
	{
		char line[512];
		va_list args;
		va_start(args, format);
		std::vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		text += line;
	}

	// Compilation 80: the windows the verdict judges, the game's own, or the simple screens' when no window was the game's (a
	// game that never left its menus, or a 2D one that draws little).
	struct Judged
	{
		const uint32_t* speed;
		uint32_t windows;
		bool game;
	};

	Judged JudgedWindows(const Stats& s)
	{
		if (s.scene_windows[ForjaSession::SceneGame] > 0)
			return {s.scene_speed[ForjaSession::SceneGame], s.scene_windows[ForjaSession::SceneGame], true};
		return {s.scene_speed[ForjaSession::SceneSimple], s.scene_windows[ForjaSession::SceneSimple], false};
	}

	// The words for some windows' speeds: "velocidad completa (mediana 100 %, al 95 % o más el 96 %)".
	std::string SpeedWords(const uint32_t* speed, uint32_t windows)
	{
		uint32_t at_full = 0;
		for (int i = 95; i < kSpeedBins; i++)
			at_full += speed[i];
		const int median = Percentile(speed, kSpeedBins, windows, 0.5);
		const unsigned full = Share(at_full, windows);
		const char* words = full >= 90 ? "velocidad completa" :
		                    median >= 95 && full >= 60 ? "casi completa (bajones cortos)" :
		                    median >= 80 ? "jugable con bajones" :
		                    median >= 60 ? "lento" : "muy lento";
		char text[128];
		std::snprintf(text, sizeof(text), "%s (mediana %d %%, al 95 %% o más el %u %%)", words, median, full);
		return text;
	}

	// What the speed says of the game's compatibility, in a few words: the summary's first line and the report's.
	std::string VerdictText(const Stats& s)
	{
		if (s.windows == 0)
			return "sin datos todavía";
		if (s.first_frame_window < 0)
			return "no mostró ninguna imagen del juego";
		const Judged judged = JudgedWindows(s);
		// Dragon Ball's fights went at 100 % for one player and at 51-59 % for two (2026-10-23): with both, each one apart.
		const bool both = judged.game && s.pads1_game_windows >= 6 && s.pads2_windows >= 6;
		std::string text;
		if (judged.windows < 6) // under 30 s
			text = "muy poca partida para juzgar";
		else if (both)
			text = "a un jugador " + SpeedWords(s.pads1_speed, s.pads1_game_windows) + "; a dos " +
			       SpeedWords(s.pads2_speed, s.pads2_windows);
		else
			text = SpeedWords(judged.speed, judged.windows);
		if (!judged.game)
			text += ", solo pantallas simples: no llegó a una escena con muchos dibujos";
		if (s.hangs > 0)
		{
			char part[48];
			std::snprintf(part, sizeof(part), ", se colgó %u %s", s.hangs, s.hangs == 1 ? "vez" : "veces");
			text += part;
		}
		if (s.pads_most >= 2 && !both)
			text += ", con 2 mandos";
		return text;
	}

	void AppendClock(std::string& text, uint32_t seconds)
	{
		Append(text, "%u:%02u", seconds / 60u, seconds % 60u);
	}
} // namespace

namespace ForjaSession
{
	const char* LimitName(int limit)
	{
		static constexpr const char* names[LimitCount] = {"none", "EE", "VU1", "GS", "GPU", "wait"};
		return limit >= 0 && limit < LimitCount ? names[limit] : "?";
	}

	int ClassifyLimit(double speed, double ee_pct, double vu1_pct, double gs_pct, double gpu_pct)
	{
		if (speed >= 97.0)
			return LimitNone;
		int limit = LimitWait;
		double busiest = 65.0;
		const double usages[] = {ee_pct, vu1_pct, gs_pct, gpu_pct};
		for (int i = 0; i < 4; i++)
		{
			if (usages[i] >= busiest)
			{
				busiest = usages[i];
				limit = LimitEE + i;
			}
		}
		return limit;
	}

	const char* SceneName(int scene)
	{
		static constexpr const char* names[SceneCount] = {"game", "simple", "movie", "load"};
		return scene >= 0 && scene < SceneCount ? names[scene] : "?";
	}

	int ClassifyScene(bool movie, double ps2_hz, double fps, double draws_per_s)
	{
		if (movie)
			return SceneMovie;
		if (ps2_hz < 1.0 || fps < 1.0)
			return SceneLoad;
		return draws_per_s / fps < 100.0 ? SceneSimple : SceneGame;
	}

	void AddHang()
	{
		std::lock_guard lock(s_mutex);
		s_stats.hangs++;
	}

	void AddWindow(const Window& w)
	{
		const double speed = w.speed, fps = w.fps;
		const int limit = w.limit;
		const bool movie = w.movie;
		std::lock_guard lock(s_mutex);
		Stats& s = s_stats;
		const int scene = std::clamp(w.scene, 0, SceneCount - 1);
		const bool game = scene == SceneGame;
		// Compilation 77: Shadow of the Colossus's GS took 28.6 ms of its 16.7: it had to be 42 % cheaper (2026-10-23).
		if (game && limit >= LimitEE && limit <= LimitGPU && w.budget_ms > 0.0 && w.limit_ms > w.budget_ms)
		{
			s.need[limit][std::clamp(static_cast<int>((1.0 - w.budget_ms / w.limit_ms) * 100.0), 0, 99)]++;
			s.need_windows[limit]++;
		}
		// Compilation 80: the scenes, the game's own figures, the loads, the controllers.
		const int index = static_cast<int>(s.windows);
		s.scene_windows[scene]++;
		s.scene_speed[scene][Bin(speed, kSpeedBins)]++;
		if (s.first_frame_window < 0 && fps >= 1.0)
			s.first_frame_window = index;
		s.game_run = game ? s.game_run + 1 : 0;
		if (s.first_game_window < 0 && s.game_run == 2)
			s.first_game_window = index - 1;
		if (game)
		{
			s.game_fps[Bin(fps, kFpsBins)]++;
			s.game_limits[std::clamp(limit, 0, LimitCount - 1)]++;
			s.two_at_limit += w.two_at_limit ? 1u : 0u;
			if (w.pads < 2)
			{
				s.pads1_game_windows++;
				s.pads1_speed[Bin(speed, kSpeedBins)]++;
			}
		}
		if (scene == SceneLoad)
		{
			if (s.load_run++ == 0)
				s.loads++;
			s.load_longest = std::max(s.load_longest, s.load_run);
		}
		else
			s.load_run = 0;
		s.pads_most = std::max(s.pads_most, w.pads);
		if (w.pads >= 2 && game)
		{
			s.pads2_windows++;
			s.pads2_speed[Bin(speed, kSpeedBins)]++;
		}
		s.fg_windows += w.fg_generating ? 1u : 0u;
		if (movie)
		{
			if (!s.in_movie)
				s.movies++;
			s.movie_windows++;
			s.movie_speed[Bin(speed, kSpeedBins)]++;
			s.movie_fps[Bin(fps, kFpsBins)]++;
		}
		s.in_movie = movie;
		s.windows++;
		s.full_speed += speed >= 95.0 ? 1u : 0u;
		s.speed[Bin(speed, kSpeedBins)]++;
		s.fps[Bin(fps, kFpsBins)]++;
		s.limits[std::clamp(limit, 0, LimitCount - 1)]++;
	}

	void AddHitch(double gap_ms, unsigned ee_blocks_compiled, unsigned ee_rec_resets, unsigned pipelines_created, unsigned disc_kb)
	{
		std::lock_guard lock(s_mutex);
		Stats& s = s_stats;
		s.hitches++;
		s.hitches_200 += gap_ms >= 200.0 ? 1u : 0u;
		s.hitch_worst_ms = std::max(s.hitch_worst_ms, gap_ms);
		// The first cause that fits, in this order: a new effect (its pipeline compiled), the EE recompiler started over (a
		// CPU speed change, a movie's start or end), new game code compiled, the disc read, something else.
		if (pipelines_created > 0)
			s.hitch_pipelines++;
		else if (ee_rec_resets > 0)
			s.hitch_resets++;
		else if (ee_blocks_compiled >= 300)
			s.hitch_compile++;
		else if (disc_kb > 0)
			s.hitch_disc++;
		else
			s.hitch_other++;
	}

	void AddHeartbeat(uint64_t flexible_free, uint64_t heap_high, uint32_t audio_underruns, uint64_t audio_limited, bool movie)
	{
		std::lock_guard lock(s_mutex);
		Stats& s = s_stats;
		s.flexible_min = s.memory_seen ? std::min(s.flexible_min, flexible_free) : flexible_free;
		s.heap_max = std::max(s.heap_max, heap_high);
		s.memory_seen = true;
		if (++s.heartbeats == 12) // a heartbeat every 5 s
			s.heap_at_minute = heap_high;
		// The sound's counters start over when its stream opens again: what they grew by, or all of it after a restart.
		if (s.audio_seen)
		{
			const uint64_t cuts = audio_underruns >= s.audio_last ? audio_underruns - s.audio_last : audio_underruns;
			s.audio_cuts += cuts;
			if (movie)
				s.audio_cuts_movie += cuts;
			s.limited += audio_limited >= s.limited_last ? audio_limited - s.limited_last : audio_limited;
		}
		s.audio_last = audio_underruns;
		s.limited_last = audio_limited;
		s.audio_seen = true;
	}

	void AddSkip(unsigned skipped, unsigned guard_pauses)
	{
		std::lock_guard lock(s_mutex);
		s_stats.skipped += skipped;
		s_stats.guard_pauses += guard_pauses;
	}

	void Begin()
	{
		std::lock_guard lock(s_mutex);
		s_stats = Stats{};
	}

	std::string Text(const std::string& header, const std::string& settings, bool final)
	{
		Stats s;
		{
			std::lock_guard lock(s_mutex);
			s = s_stats;
		}
		std::string text = header;
		text += settings;
		const unsigned seconds = s.windows * 5u;
		Append(text, "Tiempo jugado: %u min %02u s (%u tramos de 5 s)%s\n", seconds / 60u, seconds % 60u, s.windows,
			final ? "" : ", la partida sigue");
		if (s.windows == 0)
			return text;
		// Compilation 80: the verdict, and how long it took to show a picture and to reach the game itself.
		Append(text, "Compatibilidad medida (velocidad; la imagen y el sonido los dice el jugador en el reporte): %s\n",
			VerdictText(s).c_str());
		if (s.first_frame_window >= 0)
		{
			text += "Primer cuadro a los ";
			AppendClock(text, static_cast<uint32_t>(s.first_frame_window) * 5u);
			if (s.first_game_window >= 0)
			{
				text += "; llegó al juego (escenas con muchos dibujos) a los ";
				AppendClock(text, static_cast<uint32_t>(s.first_game_window) * 5u);
			}
			else
				text += "; no llegó a escenas con muchos dibujos";
			text += "\n";
		}
		// The scenes: their share of the time and their median speed; the loads, how many and the longest.
		{
			static constexpr const char* names[ForjaSession::SceneCount] = {"juego", "pantallas simples (menús, mapas)", "videos",
				"cargas"};
			std::string scenes;
			for (int scene = 0; scene < ForjaSession::SceneCount; scene++)
			{
				const uint32_t count = s.scene_windows[scene];
				if (count == 0)
					continue;
				char part[128];
				if (scene == ForjaSession::SceneLoad)
					std::snprintf(part, sizeof(part), "%s%s %u %% (%u, la más larga %u s)", scenes.empty() ? "" : ", ", names[scene],
						Share(count, s.windows), s.loads, s.load_longest * 5u);
				else
					std::snprintf(part, sizeof(part), "%s%s %u %% (velocidad mediana %d %%)", scenes.empty() ? "" : ", ", names[scene],
						Share(count, s.windows), Percentile(s.scene_speed[scene], kSpeedBins, count, 0.5));
				scenes += part;
			}
			Append(text, "Escenas, del tiempo total: %s\n", scenes.c_str());
		}
		// The game alone, without its menus, movies and loads: what the player plays.
		if (const uint32_t game = s.scene_windows[ForjaSession::SceneGame]; game > 0)
		{
			Append(text, "En el juego mismo: velocidad mediana %d %%, el peor 10 %% %d %% o menos; cuadros por segundo mediana %d, el "
				"peor 10 %% %d o menos\n",
				Percentile(s.scene_speed[ForjaSession::SceneGame], kSpeedBins, game, 0.5),
				Percentile(s.scene_speed[ForjaSession::SceneGame], kSpeedBins, game, 0.1), Percentile(s.game_fps, kFpsBins, game, 0.5),
				Percentile(s.game_fps, kFpsBins, game, 0.1));
			Append(text, "Qué frenó en el juego mismo: GS %u %%, VU1 %u %%, EE %u %%, gráfica %u %%, esperas %u %%; a velocidad completa "
				"el %u %%; dos partes al tope a la vez el %u %%\n",
				Share(s.game_limits[LimitGS], game), Share(s.game_limits[LimitVU1], game), Share(s.game_limits[LimitEE], game),
				Share(s.game_limits[LimitGPU], game), Share(s.game_limits[LimitWait], game), Share(s.game_limits[LimitNone], game),
				Share(s.two_at_limit, game));
		}
		// The controllers: Dragon Ball's split screen goes at 51-59 %, its fights for one at 100 % (2026-10-23).
		if (s.pads_most >= 2)
		{
			Append(text, "Mandos: hasta 2 conectados");
			if (s.pads2_windows > 0)
				Append(text, "; en el juego con dos: %u s, velocidad mediana %d %%", s.pads2_windows * 5u,
					Percentile(s.pads2_speed, kSpeedBins, s.pads2_windows, 0.5));
			if (s.pads1_game_windows > 0)
				Append(text, "; con uno: %u s, velocidad mediana %d %%", s.pads1_game_windows * 5u,
					Percentile(s.pads1_speed, kSpeedBins, s.pads1_game_windows, 0.5));
			text += "\n";
		}
		else
			text += "Mandos: 1\n";
		Append(text, "Cuelgues (10 s sin cuadros con el juego andando): %u\n", s.hangs);
		Append(text, "Velocidad: mediana %d %%, el peor 10 %% del tiempo %d %% o menos; al 95 %% o más el %u %% del tiempo\n",
			Percentile(s.speed, kSpeedBins, s.windows, 0.5), Percentile(s.speed, kSpeedBins, s.windows, 0.1),
			Share(s.full_speed, s.windows));
		Append(text, "Cuadros del juego por segundo: mediana %d, el peor 10 %% %d o menos\n",
			Percentile(s.fps, kFpsBins, s.windows, 0.5), Percentile(s.fps, kFpsBins, s.windows, 0.1));
		Append(text, "Qué frenó el juego, del tiempo total: GS %u %%, VU1 %u %%, EE %u %%, gráfica %u %%, esperas %u %%; a velocidad "
			"completa el %u %%\n",
			Share(s.limits[LimitGS], s.windows), Share(s.limits[LimitVU1], s.windows), Share(s.limits[LimitEE], s.windows),
			Share(s.limits[LimitGPU], s.windows), Share(s.limits[LimitWait], s.windows), Share(s.limits[LimitNone], s.windows));
		// Compilation 77: how far each part that held the game back was from its time, the median of the windows it held back.
		{
			std::string need;
			static constexpr int order[] = {LimitGS, LimitVU1, LimitEE, LimitGPU};
			static constexpr const char* names[] = {"GS", "VU1", "EE", "gráfica"};
			for (int i = 0; i < 4; i++)
			{
				const int limit = order[i];
				if (s.need_windows[limit] == 0)
					continue;
				char part[96];
				std::snprintf(part, sizeof(part), "%s%s %d %% (%u tramos)", need.empty() ? "" : ", ", names[i],
					Percentile(s.need[limit], 100, s.need_windows[limit], 0.5), s.need_windows[limit]);
				need += part;
			}
			if (!need.empty())
				Append(text, "Cuánto más barato tenía que ser lo que frenó en el juego mismo para ir al 100 %% (mediana): %s\n",
					need.c_str());
		}
		const double minutes = seconds / 60.0;
		Append(text, "Tirones (más de 50 ms y del doble de los de alrededor): %u (%.1f por minuto), %u de más de 200 ms, el peor de %.0f ms. Causas: efectos nuevos "
			"(sombreadores) %u, recompilador reiniciado %u, código nuevo del juego %u, disco %u, otras %u\n",
			s.hitches, minutes > 0.0 ? s.hitches / minutes : 0.0, s.hitches_200, s.hitch_worst_ms, s.hitch_pipelines,
			s.hitch_resets, s.hitch_compile, s.hitch_disc, s.hitch_other);
		// Compilation 80: the limiter's count isn't a fault, it says the game sounds loud (Guitar Hero's 2,247 on 2026-10-23).
		Append(text, "Sonido: %llu cortes de la salida (%.1f por minuto), %llu de ellos en videos; el limitador suavizó %llu picos de "
			"volumen (no es un error)\n",
			static_cast<unsigned long long>(s.audio_cuts), minutes > 0.0 ? s.audio_cuts / minutes : 0.0,
			static_cast<unsigned long long>(s.audio_cuts_movie), static_cast<unsigned long long>(s.limited));
		if (s.memory_seen)
		{
			Append(text, "Memoria: mínimo %llu MB libres de la flexible normal; el heap llegó a %llu MB",
				static_cast<unsigned long long>(s.flexible_min >> 20), static_cast<unsigned long long>(s.heap_max >> 20));
			// Compilation 79: how fast it grew after the first minute. Crash Nitro Kart's went from 283 to 500 MB in 5.5 minutes
			// (~40 MB a minute) and filled the main heap; the other games stay within a few MB.
			if (s.heartbeats > 24 && s.heap_at_minute > 0)
			{
				const double grown_mb = static_cast<double>(s.heap_max - std::min(s.heap_max, s.heap_at_minute)) / (1024.0 * 1024.0);
				const double after_minutes = (s.heartbeats - 12) * 5.0 / 60.0;
				Append(text, " (después del primer minuto creció %.1f MB por minuto%s)", grown_mb / after_minutes,
					grown_mb / after_minutes > 10.0 ? ": demasiado, puede llenarse" : "");
			}
			text += "\n";
		}
		Append(text, "Generación de cuadros: generó en el %u %% del tiempo\n", Share(s.fg_windows, s.windows));
		Append(text, "Saltar cuadros: %llu cuadros saltados; el guardián se pausó %llu veces\n",
			static_cast<unsigned long long>(s.skipped), static_cast<unsigned long long>(s.guard_pauses));
		if (s.movies > 0)
			Append(text, "Videos: %u (%u tramos de 5 s): velocidad mediana %d %%, mediana de %d cuadros por segundo\n", s.movies,
				s.movie_windows, Percentile(s.movie_speed, kSpeedBins, s.movie_windows, 0.5),
				Percentile(s.movie_fps, kFpsBins, s.movie_windows, 0.5));
		else
			text += "Videos: ninguno\n";
		return text;
	}

	std::string Verdict()
	{
		std::lock_guard lock(s_mutex);
		return VerdictText(s_stats);
	}

	void Write(const std::string& header, const std::string& settings, bool final)
	{
		const std::string text = Text(header, settings, final);
		const std::string part = std::string(FORJA_SESSION_SUMMARY) + ".tmp";
		bool written = false;
		if (std::FILE* file = std::fopen(part.c_str(), "wb"))
		{
			written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
			written = std::fclose(file) == 0 && written;
		}
		if (!written || std::rename(part.c_str(), FORJA_SESSION_SUMMARY) != 0)
			std::remove(part.c_str());
	}
} // namespace ForjaSession

// GSRendererPS4.cpp's hitches (weak there: the probes link without this file).
extern "C" void ForjaSessionHitch(double gap_ms, unsigned compiled, unsigned resets, unsigned pipelines, unsigned disc_kb)
{
	ForjaSession::AddHitch(gap_ms, compiled, resets, pipelines, disc_kb);
}
