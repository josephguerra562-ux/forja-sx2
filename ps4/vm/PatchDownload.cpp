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
// Compilation 54: fetched again at every game start (after 10 minutes). With a day, a profile fetched in the morning held
// all day: Dragon Ball's from build 47 (SaltarCuadros = 1) kept coming back after GitHub's said 0 since build 49, and once
// the player's own profile was reset the frame skip went on by itself in the fights (the picture white and pixelated,
// 2026-10-18).
// Compilation 61: X on L3+R3 > Extras > Ajustes recomendados fetches the profile and main.cpp uses it as soon as the download
// ends (ForjaPatches::Busy), the player's own file set aside, instead of from the next start under the player's settings.
// Forja 1.06: each version fetches its own center. The stable channel reads the branch centro-v<version> (centro-v1.06),
// made from the approved build by .github/workflows/aprobar.yml: a public version gets the fixes made for it, not main's,
// which follow the code being tested (the PDF of 2026-10-20: 1.04 was already fetching 1.05's profiles). Until that branch
// exists, the build's own tag (compilacion-N): the files it was built with, which also replace older copies left in centro/.
// The testing channel (/data/ForjaSX2/canal-pruebas, as ReleaseCheck.cpp's) and local builds keep main.
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
// Forja SX2: GitHub Actions' run number and the VERSION file (CMakeLists.txt); 0 for a local build.
#ifndef FORJA_BUILD_NUMBER
#define FORJA_BUILD_NUMBER 0
#endif
#ifndef FORJA_VERSION_MAJOR
#define FORJA_VERSION_MAJOR 1
#define FORJA_VERSION_MINOR 0
#endif

namespace
{
	constexpr const char* Root = "/data/ForjaSX2";
	constexpr const char* CommunitySource = "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/";
	// Forja 1.06: the center's files are <ProjectRaw><branch or tag><AssetsPath><file> (CenterBase).
	constexpr const char* ProjectRaw = "https://raw.githubusercontent.com/josephguerra562-ux/forja-sx2/";
	constexpr const char* AssetsPath = "/ps4/vm/assets/";
	constexpr const char* BranchApi = "https://api.github.com/repos/josephguerra562-ux/forja-sx2/branches/";
	constexpr const char* TestingMarker = "/data/ForjaSX2/canal-pruebas";
	constexpr const char* CommunityOff = "/data/ForjaSX2/vm-sin-parches-comunidad";
	constexpr const char* CenterOff = "/data/ForjaSX2/vm-sin-centro";
	constexpr size_t MaxBytes = 1024 * 1024;

	enum class Kind
	{
		CommunityPatches,
		CenterProfile,
		CenterPatches,
		CenterRules, // compilation 55: huellas.ini, the draw fingerprints for every game
	};

	struct Job
	{
		Kind kind;
		std::string url; // the center's: the file under its assets (Fetch puts CenterBase in front)
		std::string path;
	};

	std::atomic<bool> s_busy{false};
	std::atomic<bool> s_arrived{false};
	std::mutex s_mutex;
	std::string s_asked; // the game this launch already asked for ("SERIAL_CRC")

	const char* KindName(Kind kind)
	{
		return kind == Kind::CommunityPatches ? "Community patches" : kind == Kind::CenterProfile ? "Settings center profile" :
		       kind == Kind::CenterRules ? "Settings center fingerprints" :
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
			else if (job.kind == Kind::CenterRules)
				header = "; Forja SX2's draw fingerprints (huellas), from its settings center (the project's GitHub), fetched on " +
				         Today() + ".\n\n";
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

	// Forja 1.06: the base URL of this version's settings center (see the top of the file). Only the download thread calls it,
	// one at a time (s_busy). A branch found is kept for the app's run; without one the next download asks again, in case it
	// was only the network.
	std::string CenterBase()
	{
		static std::string s_branch_base;
		char branch[32];
		std::snprintf(branch, sizeof(branch), "centro-v%d.%02d", FORJA_VERSION_MAJOR, FORJA_VERSION_MINOR);
		std::string ref, why;
		if (FORJA_BUILD_NUMBER <= 0 || access(TestingMarker, F_OK) == 0)
		{
			ref = "main";
			why = FORJA_BUILD_NUMBER <= 0 ? "a local build" : "the testing channel";
		}
		else if (!s_branch_base.empty())
			return s_branch_base;
		else
		{
			std::vector<unsigned char> bytes;
			if (PS4CoverHttp::Get(std::string(BranchApi) + branch, "ForjaSX2 downloads", bytes, 256 * 1024))
			{
				ref = branch;
				why = "this version's branch";
			}
			else
			{
				ref = "compilacion-" + std::to_string(FORJA_BUILD_NUMBER);
				why = std::string("this build's own files: no branch ") + branch + " yet, or offline";
			}
		}
		const std::string base = std::string(ProjectRaw) + ref + AssetsPath;
		ProbeLog("Settings center: %s (%s)\n", ref.c_str(), why.c_str());
		if (ref == branch)
			s_branch_base = base;
		return base;
	}

	void* Fetch(void* argument)
	{
		pthread_setname_np(pthread_self(), "Forja downloads");
		const std::unique_ptr<std::vector<Job>> jobs(static_cast<std::vector<Job>*>(argument));
		std::string center_base;
		for (const Job& job : *jobs)
		{
			std::string url = job.url;
			if (job.kind != Kind::CommunityPatches)
			{
				if (center_base.empty())
					center_base = CenterBase();
				url = center_base + job.url;
			}
			std::vector<unsigned char> bytes;
			const bool got = PS4CoverHttp::Get(url, "ForjaSX2 downloads", bytes, MaxBytes);
			const std::string text(bytes.begin(), bytes.end());
			const bool looks_right = job.kind == Kind::CenterProfile || job.kind == Kind::CenterRules ? text.find('[') != std::string::npos :
			                                                          text.find("patch=") != std::string::npos;
			if (!got || !looks_right)
			{
				ProbeLog("%s: none at %s (%s)\n", KindName(job.kind), url.c_str(), got ? "not the expected format" : "not there, or offline");
				continue;
			}
			if (!Save(job, text))
			{
				ProbeLog("%s: could not save %s\n", KindName(job.kind), job.path.c_str());
				continue;
			}
			ProbeLog("%s: %s saved (%zu bytes)%s\n", KindName(job.kind), job.path.c_str(), text.size(),
				job.kind == Kind::CenterRules ? "; used from the game's next start" :
				job.kind == Kind::CenterProfile ? "; used from the game's next start, or at once by Ajustes recomendados" : "");
			if (job.kind != Kind::CenterProfile && job.kind != Kind::CenterRules)
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
			constexpr time_t center_age = 10 * 60;
			if (force || !Fresh(profile, center_age))
				jobs->push_back({Kind::CenterProfile, "perfiles/" + serial + ".ini", profile});
			if (force || !Fresh(patches, center_age))
				jobs->push_back({Kind::CenterPatches, std::string("parches/") + name + "_forja.pnach", patches});
			const std::string rules = root + "/centro/huellas.ini"; // compilation 55
			if (force || !Fresh(rules, center_age))
				jobs->push_back({Kind::CenterRules, "huellas.ini", rules});
		}
		if (jobs->empty() || s_busy.exchange(true, std::memory_order_acq_rel))
			return;
		mkdir((root + "/patches").c_str(), 0777);
		mkdir((root + "/centro").c_str(), 0777);
		mkdir((root + "/centro/perfiles").c_str(), 0777);
		mkdir((root + "/centro/parches").c_str(), 0777);
		std::vector<Job>* argument = jobs.release();
		// Compilation 59: a stack of its own size, as the app's other threads. With the default one it could not start once a game
		// had started (every God of War II session of 2026-10-19 said "the thread could not start").
		pthread_t thread{};
		pthread_attr_t attr;
		int result = pthread_attr_init(&attr);
		if (result == 0)
		{
			pthread_attr_setstacksize(&attr, 256 * 1024);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			result = pthread_create(&thread, &attr, Fetch, argument);
			pthread_attr_destroy(&attr);
		}
		if (result != 0)
		{
			delete argument;
			s_busy.store(false, std::memory_order_release);
			ProbeLog("Downloads: the thread could not start (error %d)\n", result);
			return;
		}
	}

	bool TakeArrived()
	{
		return s_arrived.exchange(false, std::memory_order_acq_rel);
	}

	bool Busy()
	{
		return s_busy.load(std::memory_order_acquire);
	}
} // namespace ForjaPatches
