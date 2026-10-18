// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 42): the PCSX2 community's patches for the game that starts
// (github.com/PCSX2/pcsx2_patches: widescreen, no-interlacing, skip-the-movies and the like, one file per <SERIAL>_<CRC>).
// That repository declares no license, so Forja doesn't ship its files in the package: the console fetches the one for the
// game being played from its official source, as the player's own download, into /data/ForjaSX2/patches/
// <SERIAL>_<CRC>_comunidad.pnach. PCSX2's patch loader finds it there next to the player's own file and the package's
// (Patch.cpp, FindPatchFilesOnDisk) and only for that exact disc: another region or version has another CRC. None of them is
// on until the player turns it on (L3+R3 > Parches). A file younger than 7 days isn't fetched again.
#include "PatchDownload.h"
#include "CoverDownload.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern void ProbeLog(const char* format, ...);

namespace
{
	constexpr const char* PatchesDirectory = "/data/ForjaSX2/patches";
	constexpr const char* Source = "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/";
	constexpr const char* OffMarker = "/data/ForjaSX2/vm-sin-parches-comunidad";
	constexpr time_t RefreshSeconds = 7 * 24 * 3600;
	constexpr size_t MaxBytes = 1024 * 1024;

	std::atomic<bool> s_busy{false};
	std::atomic<bool> s_arrived{false};
	std::mutex s_mutex;
	std::string s_asked; // the game this launch already asked for ("SERIAL_CRC")

	std::string LocalPath(const std::string& name)
	{
		return std::string(PatchesDirectory) + "/" + name + "_comunidad.pnach";
	}

	void* Fetch(void* argument)
	{
		pthread_setname_np(pthread_self(), "Forja patches");
		const std::unique_ptr<std::string> name(static_cast<std::string*>(argument));
		const std::string url = std::string(Source) + *name + ".pnach";
		std::vector<unsigned char> bytes;
		const bool got = PS4CoverHttp::Get(url, "ForjaSX2 community patches", bytes, MaxBytes);
		const std::string text(bytes.begin(), bytes.end());
		if (!got || text.find("patch=") == std::string::npos)
		{
			ProbeLog("Community patches: none for %s (%s)\n", name->c_str(), got ? "no patch lines" : "not in the repository, or offline");
			s_busy.store(false, std::memory_order_release);
			return nullptr;
		}
		const std::string path = LocalPath(*name);
		const std::string part = path + ".tmp";
		bool written = false;
		if (std::FILE* file = std::fopen(part.c_str(), "wb"))
		{
			char header[256];
			const time_t now = time(nullptr);
			char date[32] = "?";
			if (const std::tm* utc = gmtime(&now))
				std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
			std::snprintf(header, sizeof(header),
				"// The PCSX2 community's patches (github.com/PCSX2/pcsx2_patches), fetched by Forja SX2 on %s.\n"
				"// Turn them on in L3+R3 > Parches. Forja fetches this file again after 7 days.\n\n", date);
			written = std::fputs(header, file) >= 0 && std::fwrite(text.data(), 1, text.size(), file) == text.size();
			written = std::fclose(file) == 0 && written;
		}
		if (!written || std::rename(part.c_str(), path.c_str()) != 0)
		{
			unlink(part.c_str());
			ProbeLog("Community patches: could not save %s\n", path.c_str());
			s_busy.store(false, std::memory_order_release);
			return nullptr;
		}
		ProbeLog("Community patches: %s saved (%zu bytes) from %s\n", path.c_str(), text.size(), url.c_str());
		s_arrived.store(true, std::memory_order_release);
		s_busy.store(false, std::memory_order_release);
		return nullptr;
	}
} // namespace

namespace ForjaPatches
{
	void FetchCommunity(const std::string& serial, uint32_t crc)
	{
		if (serial.empty() || crc == 0 || access(OffMarker, F_OK) == 0)
			return;
		char name[96];
		std::snprintf(name, sizeof(name), "%s_%08X", serial.c_str(), crc);
		{
			std::lock_guard lock(s_mutex);
			if (s_asked == name)
				return;
			s_asked = name;
		}
		struct stat info{};
		if (stat(LocalPath(name).c_str(), &info) == 0 && time(nullptr) - info.st_mtime < RefreshSeconds)
		{
			ProbeLog("Community patches: %s fetched less than 7 days ago\n", name);
			return;
		}
		if (s_busy.exchange(true, std::memory_order_acq_rel))
			return;
		mkdir(PatchesDirectory, 0777);
		auto* argument = new std::string(name);
		pthread_t thread{};
		if (pthread_create(&thread, nullptr, Fetch, argument) != 0)
		{
			delete argument;
			s_busy.store(false, std::memory_order_release);
			ProbeLog("Community patches: the download thread could not start\n");
			return;
		}
		pthread_detach(thread);
	}

	bool TakeArrived()
	{
		return s_arrived.exchange(false, std::memory_order_acq_rel);
	}
} // namespace ForjaPatches
