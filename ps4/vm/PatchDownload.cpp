// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2: what the console fetches for the game that starts, in the background, into /data/ForjaSX2.
//
// Compilation 42: the PCSX2 community's patches (github.com/PCSX2/pcsx2_patches: widescreen, no-interlacing, skip-the-movies
// and the like, one file per <SERIAL>_<CRC>). That repository declares no license, so Forja doesn't ship its files in the
// package: the console fetches the one for the game being played from its official source, as the player's own download,
// into patches/<SERIAL>_<CRC>_comunidad.pnach. PCSX2's patch loader finds it there next to the player's own file and the
// package's (Patch.cpp, FindPatchFilesOnDisk) and only for that exact disc: another region or version has another CRC. None
// of them is on until the player turns it on (L3+R3 > Parches). Fetched again after 7 days.
//
// Compilation 44: the settings center, as Dolphin for PS4's (its "Game settings center", v04.06+): the game's factory profile
// and Forja's own patches, from this project's GitHub (ps4/vm/assets/perfiles/<SERIAL>.ini and
// ps4/vm/assets/parches/<SERIAL>_<CRC>_forja.pnach), into centro/perfiles and centro/parches, which win over the package's
// copies (ForjaGameProfile::FactoryPath, Patch.cpp). A game's tested settings then reach every player without a new
// package. Fetched again after a day; /data/ForjaSX2/vm-sin-centro turns it off. A profile that arrives is used from the
// game's next start; patches are read again at once.
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
	constexpr const char* Root = "/data/ForjaSX2";
	constexpr const char* CommunitySource = "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/";
	constexpr const char* CenterSource = "https://raw.githubusercontent.com/josephguerra562-ux/forja-sx2/main/ps4/vm/assets/";
	constexpr const char* CommunityOff = "/data/ForjaSX2/vm-sin-parches-comunidad";
	constexpr const char* CenterOff = "/data/ForjaSX2/vm-sin-centro";
	constexpr size_t MaxBytes = 1024 * 1024;

	enum class Kind
	{
		CommunityPatches,
		CenterProfile,
		CenterPatches,
	};

	struct Job
	{
		Kind kind;
		std::string url;
		std::string path;
	};

	std::atomic<bool> s_busy{false};
	std::atomic<bool> s_arrived{false};
	std::mutex s_mutex;
	std::string s_asked; // the game this launch already asked for ("SERIAL_CRC")

	const char* KindName(Kind kind)
	{
		return kind == Kind::CommunityPatches ? "Community patches" : kind == Kind::CenterProfile ? "Settings center profile" :
		                                                                                        "Settings center patches";
	}

	bool Fresh(const std::string& path, time_t max_age)
	{
		struct stat info{};
		return stat(path.c_str(), &info) == 0 && time(nullptr) - info.st_mtime < max_age;
	}

	std::string Today()
	{
		const time_t now = time(nullptr);
		char date[32] = "?";
		if (const std::tm* utc = gmtime(&now))
			std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
		return date;
	}

	bool Save(const Job& job, const std::string& text)
	{
		const std::string part = job.path + ".tmp";
		bool written = false;
		if (std::FILE* file = std::fopen(part.c_str(), "wb"))
		{
			std::string header;
			if (job.kind == Kind::CommunityPatches)
				header = "// The PCSX2 community's patches (github.com/PCSX2/pcsx2_patches), fetched by Forja SX2 on " + Today() +
				         ".\n// Turn them on in L3+R3 > Parches. Forja fetches this file again after 7 days.\n\n";
			else if (job.kind == Kind::CenterPatches)
				header = "// Forja SX2's patches for this game, from its settings center (the project's GitHub), fetched on " +
				         Today() + ".\n\n";
			else
				header = "; Forja SX2's factory profile for this game, from its settings center (the project's GitHub), fetched on " +
				         Today() + ".\n\n";
			written = std::fputs(header.c_str(), file) >= 0 && std::fwrite(text.data(), 1, text.size(), file) == text.size();
			written = std::fclose(file) == 0 && written;
		}
		if (!written || std::rename(part.c_str(), job.path.c_str()) != 0)
		{
			unlink(part.c_str());
			return false;
		}
		return true;
	}

	void* Fetch(void* argument)
	{
		pthread_setname_np(pthread_self(), "Forja downloads");
		const std::unique_ptr<std::vector<Job>> jobs(static_cast<std::vector<Job>*>(argument));
		for (const Job& job : *jobs)
		{
			std::vector<unsigned char> bytes;
			const bool got = PS4CoverHttp::Get(job.url, "ForjaSX2 downloads", bytes, MaxBytes);
			const std::string text(bytes.begin(), bytes.end());
			const bool looks_right = job.kind == Kind::CenterProfile ? text.find('[') != std::string::npos :
			                                                          text.find("patch=") != std::string::npos;
			if (!got || !looks_right)
			{
				ProbeLog("%s: none at %s (%s)\n", KindName(job.kind), job.url.c_str(), got ? "not the expected format" : "not there, or offline");
				continue;
			}
			if (!Save(job, text))
			{
				ProbeLog("%s: could not save %s\n", KindName(job.kind), job.path.c_str());
				continue;
			}
			ProbeLog("%s: %s saved (%zu bytes)%s\n", KindName(job.kind), job.path.c_str(), text.size(),
				job.kind == Kind::CenterProfile ? "; used from the game's next start" : "");
			if (job.kind != Kind::CenterProfile)
				s_arrived.store(true, std::memory_order_release);
		}
		s_busy.store(false, std::memory_order_release);
		return nullptr;
	}
} // namespace

namespace ForjaPatches
{
	void FetchCommunity(const std::string& serial, uint32_t crc, bool force)
	{
		if (serial.empty() || crc == 0)
			return;
		char name[96];
		std::snprintf(name, sizeof(name), "%s_%08X", serial.c_str(), crc);
		{
			std::lock_guard lock(s_mutex);
			if (s_asked == name && !force)
				return;
			s_asked = name;
		}
		auto jobs = std::make_unique<std::vector<Job>>();
		const std::string root(Root);
		if (access(CommunityOff, F_OK) != 0)
		{
			const std::string path = root + "/patches/" + name + "_comunidad.pnach";
			if (!force && Fresh(path, 7 * 24 * 3600))
				ProbeLog("Community patches: %s fetched less than 7 days ago\n", name);
			else
				jobs->push_back({Kind::CommunityPatches, std::string(CommunitySource) + name + ".pnach", path});
		}
		if (access(CenterOff, F_OK) != 0)
		{
			const std::string profile = root + "/centro/perfiles/" + serial + ".ini";
			const std::string patches = root + "/centro/parches/" + name + "_forja.pnach";
			if (force || !Fresh(profile, 24 * 3600))
				jobs->push_back({Kind::CenterProfile, std::string(CenterSource) + "perfiles/" + serial + ".ini", profile});
			if (force || !Fresh(patches, 24 * 3600))
				jobs->push_back({Kind::CenterPatches, std::string(CenterSource) + "parches/" + name + "_forja.pnach", patches});
		}
		if (jobs->empty() || s_busy.exchange(true, std::memory_order_acq_rel))
			return;
		mkdir((root + "/patches").c_str(), 0777);
		mkdir((root + "/centro").c_str(), 0777);
		mkdir((root + "/centro/perfiles").c_str(), 0777);
		mkdir((root + "/centro/parches").c_str(), 0777);
		std::vector<Job>* argument = jobs.release();
		pthread_t thread{};
		if (pthread_create(&thread, nullptr, Fetch, argument) != 0)
		{
			delete argument;
			s_busy.store(false, std::memory_order_release);
			ProbeLog("Downloads: the thread could not start\n");
			return;
		}
		pthread_detach(thread);
	}

	bool TakeArrived()
	{
		return s_arrived.exchange(false, std::memory_order_acq_rel);
	}
} // namespace ForjaPatches
