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
		// Compilation 77: per limit, the windows by how much cheaper the limiting part had to be (percent, 0-99).
		uint32_t need[ForjaSession::LimitCount][100] = {};
		uint32_t need_windows[ForjaSession::LimitCount] = {};
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

	void AddWindow(double speed, double fps, int limit, bool movie, bool fg_generating, double limit_ms, double budget_ms)
	{
		std::lock_guard lock(s_mutex);
		Stats& s = s_stats;
		// Compilation 77: Shadow of the Colossus's GS took 28.6 ms of its 16.7: it had to be 42 % cheaper (2026-10-23).
		if (limit >= LimitEE && limit <= LimitGPU && budget_ms > 0.0 && limit_ms > budget_ms)
		{
			s.need[limit][std::clamp(static_cast<int>((1.0 - budget_ms / limit_ms) * 100.0), 0, 99)]++;
			s.need_windows[limit]++;
		}
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
		s.fg_windows += fg_generating ? 1u : 0u;
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
				Append(text, "Cuánto más barato tenía que ser lo que frenó para ir al 100 %% (mediana): %s\n", need.c_str());
		}
		const double minutes = seconds / 60.0;
		Append(text, "Tirones (más de 50 ms y del doble de los de alrededor): %u (%.1f por minuto), %u de más de 200 ms, el peor de %.0f ms. Causas: efectos nuevos "
			"(sombreadores) %u, recompilador reiniciado %u, código nuevo del juego %u, disco %u, otras %u\n",
			s.hitches, minutes > 0.0 ? s.hitches / minutes : 0.0, s.hitches_200, s.hitch_worst_ms, s.hitch_pipelines,
			s.hitch_resets, s.hitch_compile, s.hitch_disc, s.hitch_other);
		Append(text, "Sonido: %llu cortes de la salida (%.1f por minuto), %llu de ellos en videos; el limitador bajó el volumen %llu veces\n",
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
