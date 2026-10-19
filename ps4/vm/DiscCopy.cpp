// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 41): a PS2 DVD in the PS4's drive copied to /data/ForjaSX2/roms as an .iso, so a player
// with the disc keeps a copy of it on the console, and the disc in the drive then starts that copy (GameMenu.cpp).
//
// As PS5SX2 does it on the PS5 (swordpdf's ps5/coreorbis/orbis-shims/ProsperoDiscDump.cpp, GPL-3.0-or-later): the copy goes to
// a .part file renamed to .iso at the end; the disc is read many sectors at a time, a range that won't read is read again
// sector by sector, and a sector that still won't is written as zeros and counted; the log gives the speed every 5 %, as a
// DVD multiple (1x = 1.385 MB/s). Nothing is ever written to the disc or the drive. The reads go through Forja's own CAM
// transport (PhysicalDisc.cpp, after Al-Azif's ps-disc-detect), not PS5SX2's device nodes. The CRC-32 of the whole image
// is logged, to compare with redump.org. The PS4's drive reads DVDs only: PS2 CD games can't be copied.
#include "DiscCopy.h"
#include "ForjaLang.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern void ProbeLog(const char* format, ...);

namespace
{
	constexpr const char* RomsDirectory = "/data/ForjaSX2/roms";
	constexpr uint32_t SectorBytes = 2048;
	constexpr uint32_t ChunkSectors = 32;   // 64 KiB a read, while the drive takes it (PhysicalDisc.cpp falls back to one)
	constexpr uint32_t BufferSectors = 512; // 1 MiB a write
	constexpr double DvdSpeed = 1.385;      // MB/s at 1x

	using Clock = std::chrono::steady_clock;

	std::atomic<PS4DiscCopy::State> s_state{PS4DiscCopy::State::Idle};
	std::atomic<bool> s_cancel{false};
	std::atomic<bool> s_finished{false};
	std::atomic<uint32_t> s_done{0};
	std::atomic<uint32_t> s_total{0};
	std::atomic<uint32_t> s_bad{0};
	std::mutex s_mutex; // guards the ones below
	std::string s_title, s_path, s_message;
	std::unique_ptr<PS4Disc::Reader> s_reader;
	Clock::time_point s_started{};
	pthread_t s_thread{};
	bool s_thread_live = false;

	// A file name from the game's title: no path or reserved characters (": " becomes " - ", as PS5SX2 does).
	std::string SafeName(const std::string& title)
	{
		std::string name;
		for (size_t i = 0; i < title.size(); i++)
		{
			const char c = title[i];
			if (c == ':' && i + 1 < title.size() && title[i + 1] == ' ')
				name += " -";
			else if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
				name += ' ';
			else if (static_cast<unsigned char>(c) >= 32)
				name += c;
		}
		while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
			name.pop_back();
		while (!name.empty() && name.front() == ' ')
			name.erase(0, 1);
		return name.empty() ? "Disco PS2" : name;
	}

	void Finish(PS4DiscCopy::State state, const std::string& message)
	{
		{
			std::lock_guard lock(s_mutex);
			s_message = message;
		}
		s_state.store(state, std::memory_order_release);
		s_finished.store(true, std::memory_order_release);
		ProbeLog("Disc copy: %s\n", message.c_str());
	}

	bool WriteAll(int fd, const uint8_t* data, size_t bytes, int& error)
	{
		while (bytes > 0)
		{
			const ssize_t written = write(fd, data, bytes);
			if (written < 0)
			{
				if (errno == EINTR)
					continue;
				error = errno;
				return false;
			}
			data += written;
			bytes -= static_cast<size_t>(written);
		}
		return true;
	}

	void* Copy(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 disc copy");
		std::unique_ptr<PS4Disc::Reader> reader;
		std::string title, path;
		{
			std::lock_guard lock(s_mutex);
			reader = std::move(s_reader);
			title = s_title;
			path = s_path;
		}
		const uint32_t total = reader->GetInfo().sectors;
		const std::string part = path + ".part";
		const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
		{
			const int error = errno;
			Finish(PS4DiscCopy::State::Failed, ForjaLang::F(ForjaLang::TF("No se pudo crear la copia en /data/ForjaSX2/roms (error %d)."), error));
			return nullptr;
		}

		std::vector<uint8_t> buffer(static_cast<size_t>(BufferSectors) * SectorBytes);
		uLong crc = crc32(0L, Z_NULL, 0);
		const Clock::time_point start = Clock::now();
		Clock::time_point mark = start;
		uint32_t mark_sector = 0;
		uint32_t next_percent = 5;
		uint32_t sector = 0;
		enum class Stop { None, Cancelled, Media, Write } stop = Stop::None;
		int write_error = 0;
		while (sector < total && stop == Stop::None)
		{
			const uint32_t fill = std::min(BufferSectors, total - sector);
			for (uint32_t i = 0; i < fill && stop == Stop::None;)
			{
				if (s_cancel.load(std::memory_order_relaxed))
				{
					stop = Stop::Cancelled;
					break;
				}
				const uint32_t count = std::min(ChunkSectors, fill - i);
				uint8_t* out = buffer.data() + static_cast<size_t>(i) * SectorBytes;
				if (!reader->ReadSectors(sector + i, count, out))
				{
					// Again one sector at a time: one that still won't read is written as zeros, unless the disc is gone.
					for (uint32_t k = 0; k < count; k++)
					{
						if (reader->ReadSectors(sector + i + k, 1, out + static_cast<size_t>(k) * SectorBytes))
							continue;
						if (!reader->CheckMedia())
						{
							stop = Stop::Media;
							break;
						}
						std::memset(out + static_cast<size_t>(k) * SectorBytes, 0, SectorBytes);
						s_bad.fetch_add(1, std::memory_order_relaxed);
						ProbeLog("Disc copy: LBA %u unreadable, written as zeros\n", sector + i + k);
					}
				}
				i += count;
				s_done.store(sector + i, std::memory_order_relaxed);
			}
			if (stop != Stop::None)
				break;
			crc = crc32(crc, buffer.data(), fill * SectorBytes);
			if (!WriteAll(fd, buffer.data(), static_cast<size_t>(fill) * SectorBytes, write_error))
			{
				stop = Stop::Write;
				break;
			}
			sector += fill;
			const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(sector) * 100 / total);
			if (percent >= next_percent || sector == total)
			{
				const Clock::time_point now = Clock::now();
				const double span = std::chrono::duration<double>(now - mark).count();
				const double all = std::chrono::duration<double>(now - start).count();
				const double now_mbs = span > 0.0 ? (sector - mark_sector) * double(SectorBytes) / 1e6 / span : 0.0;
				const double avg_mbs = all > 0.0 ? sector * double(SectorBytes) / 1e6 / all : 0.0;
				ProbeLog("Disc copy: %u %% (%u of %u sectors): %.2f MB/s (%.1fx DVD) now, %.2f MB/s (%.1fx) on average, %u unreadable\n",
					percent, sector, total, now_mbs, now_mbs / DvdSpeed, avg_mbs, avg_mbs / DvdSpeed, s_bad.load(std::memory_order_relaxed));
				mark = now;
				mark_sector = sector;
				next_percent = percent / 5 * 5 + 5;
			}
		}
		close(fd);

		if (stop != Stop::None)
		{
			unlink(part.c_str());
			switch (stop)
			{
				case Stop::Cancelled:
					Finish(PS4DiscCopy::State::Cancelled, ForjaLang::T("Copia cancelada. No quedó nada a medias."));
					break;
				case Stop::Media:
					Finish(PS4DiscCopy::State::Failed, ForjaLang::T("Se sacó el disco o la unidad dejó de leerlo: la copia a medias se borró."));
					break;
				default:
					if (write_error == ENOSPC)
						Finish(PS4DiscCopy::State::Failed,
							ForjaLang::F(ForjaLang::TF("No hay espacio en la PS4: hacen falta %llu GB libres. La copia a medias se borró."),
								static_cast<unsigned long long>((static_cast<uint64_t>(total) * SectorBytes) >> 30)));
					else
						Finish(PS4DiscCopy::State::Failed,
							ForjaLang::F(ForjaLang::TF("No se pudo escribir la copia (error %d). La copia a medias se borró."), write_error));
					break;
			}
			return nullptr;
		}
		if (std::rename(part.c_str(), path.c_str()) != 0)
		{
			const int error = errno;
			unlink(part.c_str());
			Finish(PS4DiscCopy::State::Failed, ForjaLang::F(ForjaLang::TF("No se pudo terminar la copia (error %d)."), error));
			return nullptr;
		}
		const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
		const uint32_t bad = s_bad.load(std::memory_order_relaxed);
		ProbeLog("Disc copy: done %s: %u sectors, %u unreadable, crc32 %08lx, %.0f s\n", path.c_str(), total, bad,
			static_cast<unsigned long>(crc), seconds);
		Finish(PS4DiscCopy::State::Done, bad > 0 ?
			ForjaLang::F(ForjaLang::TF("Copia lista: %s. %u sectores no se pudieron leer (van en ceros). X para jugar."), title.c_str(), bad) :
			ForjaLang::F(ForjaLang::TF("Copia lista: %s. Desde ahora, con el disco puesto, X juega la copia."), title.c_str()));
		return nullptr;
	}
} // namespace

namespace PS4DiscCopy
{
	bool Start(std::unique_ptr<PS4Disc::Reader> reader, const std::string& title, std::string& error)
	{
		if (Busy())
		{
			error = ForjaLang::T("Ya hay una copia en curso.");
			return false;
		}
		if (!reader)
		{
			error = ForjaLang::T("Pulsa Triángulo para leer el disco primero.");
			return false;
		}
		if (s_thread_live)
		{
			pthread_join(s_thread, nullptr); // the last copy's thread, finished
			s_thread_live = false;
		}
		const PS4Disc::Info& info = reader->GetInfo();
		const std::string path = std::string(RomsDirectory) + "/" + SafeName(title) + " (" + info.serial + ").iso";
		{
			std::lock_guard lock(s_mutex);
			s_title = title;
			s_path = path;
		}
		s_done.store(0, std::memory_order_relaxed);
		s_total.store(info.sectors, std::memory_order_relaxed);
		s_bad.store(0, std::memory_order_relaxed);
		s_cancel.store(false, std::memory_order_relaxed);
		s_finished.store(false, std::memory_order_relaxed);
		struct stat existing{};
		if (stat(path.c_str(), &existing) == 0 && static_cast<uint64_t>(existing.st_size) == static_cast<uint64_t>(info.sectors) * SectorBytes)
		{
			Finish(State::Done, ForjaLang::F(ForjaLang::TF("Ya estaba copiado: %s."), title.c_str()));
			return true;
		}
		mkdir(RomsDirectory, 0777);
		{
			std::lock_guard lock(s_mutex);
			s_reader = std::move(reader);
			s_started = Clock::now();
		}
		s_state.store(State::Copying, std::memory_order_release);
		if (pthread_create(&s_thread, nullptr, Copy, nullptr) != 0)
		{
			{
				std::lock_guard lock(s_mutex);
				s_reader.reset();
			}
			s_state.store(State::Failed, std::memory_order_release);
			error = ForjaLang::T("No se pudo iniciar la copia.");
			return false;
		}
		s_thread_live = true;
		ProbeLog("Disc copy: started %s (%s) -> %s, %u sectors (%.2f GB)\n", title.c_str(), info.serial.c_str(), path.c_str(), info.sectors,
			info.sectors * double(SectorBytes) / 1e9);
		return true;
	}

	bool Busy()
	{
		return s_state.load(std::memory_order_acquire) == State::Copying;
	}

	void Cancel()
	{
		s_cancel.store(true, std::memory_order_relaxed);
	}

	State GetState()
	{
		return s_state.load(std::memory_order_acquire);
	}

	bool TakeFinished()
	{
		return s_finished.exchange(false, std::memory_order_acq_rel);
	}

	std::string ResultPath()
	{
		std::lock_guard lock(s_mutex);
		return GetState() == State::Done ? s_path : std::string();
	}

	std::string StatusText()
	{
		if (!Busy())
		{
			std::lock_guard lock(s_mutex);
			return s_message;
		}
		const uint32_t done = s_done.load(std::memory_order_relaxed);
		const uint32_t total = std::max(s_total.load(std::memory_order_relaxed), 1u);
		std::string title;
		double seconds = 0.0;
		{
			std::lock_guard lock(s_mutex);
			title = s_title;
			seconds = std::chrono::duration<double>(Clock::now() - s_started).count();
		}
		const double mbs = seconds > 1.0 ? done * double(SectorBytes) / 1e6 / seconds : 0.0;
		char text[512];
		if (mbs > 0.05)
		{
			const double left_min = (total - done) * double(SectorBytes) / 1e6 / mbs / 60.0;
			std::snprintf(text, sizeof(text), ForjaLang::TF("Copiando %s: %u %% · %.1f MB/s · faltan %.0f min · □ para cancelar"), title.c_str(),
				static_cast<unsigned>(static_cast<uint64_t>(done) * 100 / total), mbs, std::max(left_min, 1.0));
		}
		else
			std::snprintf(text, sizeof(text), ForjaLang::TF("Copiando %s: empezando... · □ para cancelar"), title.c_str());
		return text;
	}
} // namespace PS4DiscCopy
