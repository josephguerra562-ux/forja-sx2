// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Checks the project's public GitHub release endpoint without
// blocking the launcher or requiring a second networking implementation.
// Forja SX2: every build of main is published as a pre-release "compilacion-N"; the ones Joseph approves are promoted to
// normal releases (.github/workflows/aprobar.yml). Two channels: the stable one (the default) reads /releases/latest,
// which only shows those, and the testing one (/data/ForjaSX2/canal-pruebas exists) reads the newest releases of any kind
// and takes the highest build among them.
// The tag's number is compared with this build's. The updater downloads its package into /data/pkg (as DolphinPS4 does
// since v03.36) and checks it against the release's SHA256SUMS.txt.
#include "ReleaseCheck.h"
#include "CoverDownload.h"
#include "Sha256.h"
#include "ForjaLang.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <pthread.h>

extern void ProbeLog(const char* format, ...);

namespace
{
	constexpr const char* StableUrl = "https://api.github.com/repos/josephguerra562-ux/forja-sx2/releases/latest";
	// Compilation 52: the newest few, not one (Check takes the highest build among them).
	constexpr const char* TestingUrl = "https://api.github.com/repos/josephguerra562-ux/forja-sx2/releases?per_page=5";
	constexpr const char* TestingMarker = "/data/ForjaSX2/canal-pruebas";
	constexpr const char* PackageDirectory = "/data/pkg";
	constexpr const char* PackagePrefix = "ForjaSX2-compilacion-";
	constexpr const char* VersionedPrefix = "ForjaSX2-Emulador-v"; // compilation 58

	std::atomic<bool> s_started{false};
	std::atomic<bool> s_has_update{false};
	std::atomic<int> s_current_build{0};
	std::atomic<int> s_latest_build{0};
	std::atomic<PS4Release::DownloadState> s_state{PS4Release::DownloadState::Idle};
	std::atomic<unsigned> s_progress{0};
	std::mutex s_mutex; // guards the strings below
	std::string s_package_url, s_sums_url, s_package_name, s_message;
	std::string s_latest_version; // "1.01", from the release's title; empty for the builds named before versions

	// The value of the first "key": "value" pair at or after `from`; npos-safe.
	bool JsonString(const std::string& json, const char* key, size_t from, std::string& value, size_t* end_out = nullptr)
	{
		const std::string quoted = std::string("\"") + key + "\"";
		const size_t key_pos = json.find(quoted, from);
		if (key_pos == std::string::npos)
			return false;
		const size_t colon = json.find(':', key_pos + quoted.size());
		const size_t quote = colon == std::string::npos ? std::string::npos : json.find('"', colon + 1);
		if (quote == std::string::npos)
			return false;
		const size_t end = json.find('"', quote + 1);
		if (end == std::string::npos)
			return false;
		value.assign(json, quote + 1, end - quote - 1);
		if (end_out)
			*end_out = end;
		return true;
	}

	// The download URL of the first asset whose URL ends with `suffix`.
	std::string AssetUrl(const std::string& json, std::string_view suffix)
	{
		size_t from = 0;
		std::string url;
		while (JsonString(json, "browser_download_url", from, url, &from))
		{
			if (url.size() >= suffix.size() && url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0)
				return url;
		}
		return {};
	}

	// Forja SX2 (compilation 48): the same asset through GitHub's API (/releases/assets/<id>, asked for with Accept:
	// application/octet-stream), the "url" that comes first in its entry. github.com's /releases/download/ answers with a
	// redirect carrying ~5 KB of headers (a 3.7 KB Content-Security-Policy), more than the PS4's HTTP library takes:
	// sceHttpSendRequest gave 0x80431073 (too large a response header) for build 47's SHA256SUMS.txt (2026-10-18). The
	// API's redirect has ~2 KB.
	std::string AssetApiUrl(const std::string& json, std::string_view suffix)
	{
		size_t from = 0;
		std::string url;
		while (JsonString(json, "browser_download_url", from, url, &from))
		{
			if (url.size() < suffix.size() || url.compare(url.size() - suffix.size(), suffix.size(), suffix) != 0)
				continue;
			const size_t key = json.rfind("/releases/assets/", from);
			const size_t start = key == std::string::npos ? std::string::npos : json.rfind('"', key);
			const size_t end = key == std::string::npos ? std::string::npos : json.find('"', key);
			if (start == std::string::npos || end == std::string::npos)
				return {};
			std::string api = json.substr(start + 1, end - start - 1);
			return api.rfind("https://api.github.com/", 0) == 0 ? api : std::string();
		}
		return {};
	}

	// "compilacion-31" -> 31; 0 if the tag is not one of ours.
	int BuildFromTag(const std::string& tag)
	{
		const size_t dash = tag.rfind('-');
		if (dash == std::string::npos || dash + 1 >= tag.size())
			return 0;
		return std::atoi(tag.c_str() + dash + 1);
	}

	// "Forja SX2 v1.01 (prueba 38)" -> "1.01"; empty if the title has no version.
	std::string VersionFromTitle(const std::string& title)
	{
		for (size_t v = title.find('v'); v != std::string::npos; v = title.find('v', v + 1))
		{
			size_t end = v + 1;
			while (end < title.size() && ((title[end] >= '0' && title[end] <= '9') || title[end] == '.'))
				end++;
			if (end > v + 1 && title[v + 1] >= '0' && title[v + 1] <= '9' && title.find('.', v + 1) < end)
				return title.substr(v + 1, end - v - 1);
		}
		return {};
	}

	void SetMessage(const std::string& message)
	{
		std::lock_guard lock(s_mutex);
		s_message = message;
	}

	void* Check(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 release check");
		const bool testing = access(TestingMarker, F_OK) == 0;
		const char* channel = testing ? "pruebas" : "estable";
		std::vector<unsigned char> bytes;
		if (!PS4CoverHttp::Get(testing ? TestingUrl : StableUrl, "ForjaSX2 release-checker", bytes, 256 * 1024))
		{
			// The stable channel gets a 404 while no build has been approved yet.
			ProbeLog("Release check: channel=%s GitHub request failed\n", channel);
			return nullptr;
		}
		std::string json(bytes.begin(), bytes.end());
		// Compilation 52: of the releases in the answer, the one with the highest build, from its "tag_name" to the next one's
		// (its assets and notes come after its tag). GitHub lists the release marked Latest first, whatever its date: once
		// build 50 became v1.02 (2026-10-18), "per_page=1" answered 50 and build 50's updater never offered 51.
		{
			size_t best_start = std::string::npos, best_end = std::string::npos;
			int best = -1;
			for (size_t pos = json.find("\"tag_name\""); pos != std::string::npos;)
			{
				const size_t next = json.find("\"tag_name\"", pos + 10);
				std::string value;
				if (JsonString(json, "tag_name", pos, value) && BuildFromTag(value) > best)
				{
					best = BuildFromTag(value);
					best_start = pos;
					best_end = next;
				}
				pos = next;
			}
			if (best_start != std::string::npos)
				json = json.substr(best_start, best_end == std::string::npos ? std::string::npos : best_end - best_start);
		}
		std::string tag;
		size_t tag_end = 0;
		if (!JsonString(json, "tag_name", 0, tag, &tag_end))
		{
			ProbeLog("Release check: channel=%s tag_name missing\n", channel);
			return nullptr;
		}
		// The release's title comes right after its tag in GitHub's answer ("name": "Forja SX2 v1.01 (prueba 38)").
		std::string title;
		JsonString(json, "name", tag_end, title);
		const int latest = BuildFromTag(tag);
		const int current = s_current_build.load(std::memory_order_relaxed);
		std::string package_url = AssetUrl(json, ".pkg");
		std::string sums_url = AssetUrl(json, "SHA256SUMS.txt");
		// Compilation 48: downloaded through the API; the package's name still comes from its github.com address.
		const std::string package_api = AssetApiUrl(json, ".pkg");
		const std::string sums_api = AssetApiUrl(json, "SHA256SUMS.txt");
		{
			std::lock_guard lock(s_mutex);
			s_package_url = package_api.empty() ? package_url : package_api;
			s_sums_url = sums_api.empty() ? sums_url : sums_api;
			const size_t slash = package_url.rfind('/');
			s_package_name = slash == std::string::npos ? std::string() : package_url.substr(slash + 1);
			s_latest_version = VersionFromTitle(title);
		}
		s_latest_build.store(latest, std::memory_order_relaxed);
		const bool newer = current > 0 && latest > current && !package_url.empty();
		s_has_update.store(newer, std::memory_order_release);
		ProbeLog("Release check: channel=%s current=%d latest=%s (%d) update=%d package=%s sums=%s\n", channel, current,
			tag.c_str(), latest, newer ? 1 : 0, package_url.empty() ? "-" : "yes", sums_url.empty() ? "-" : "yes");
		return nullptr;
	}

	// The hash SHA256SUMS.txt gives for `name` ("<64 hex>  <name>" lines; sha256sum may mark the name with '*').
	std::string ExpectedHash(const std::string& sums, const std::string& name)
	{
		size_t start = 0;
		while (start < sums.size())
		{
			size_t end = sums.find('\n', start);
			if (end == std::string::npos)
				end = sums.size();
			const std::string line = sums.substr(start, end - start);
			start = end + 1;
			if (line.size() < 66)
				continue;
			std::string file = line.substr(64);
			while (!file.empty() && (file.front() == ' ' || file.front() == '*'))
				file.erase(file.begin());
			while (!file.empty() && (file.back() == '\r' || file.back() == ' '))
				file.pop_back();
			if (file == name)
			{
				std::string hash = line.substr(0, 64);
				for (char& c : hash)
					c = static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
				return hash;
			}
		}
		return {};
	}

	// Compilation 58: the package is named after its version and build ("ForjaSX2-Emulador-v1.04-58.pkg"), as Joseph asked
	// (2026-10-19): GoldHEN's Package Installer showed "ForjaSX2-compilacion-57.pkg", which did not say which version it was.
	// A release whose title has no version keeps the old name.
	std::string PackageFileName(int build, const std::string& version)
	{
		if (version.empty())
			return std::string(PackagePrefix) + std::to_string(build) + ".pkg";
		return std::string(VersionedPrefix) + version + "-" + std::to_string(build) + ".pkg";
	}

	// A package an updater left in /data/pkg: "ForjaSX2-compilacion-N.pkg" or "ForjaSX2-Emulador-vX.YY-N.pkg". One copied there
	// by hand ("ForjaSX2-Emulador-v1.03.pkg", no build after the version) is not one of them.
	bool UpdaterPackage(const std::string& name)
	{
		if (name.size() < 4 || name.compare(name.size() - 4, 4, ".pkg") != 0)
			return false;
		if (name.rfind(PackagePrefix, 0) == 0)
			return true;
		if (name.rfind(VersionedPrefix, 0) != 0)
			return false;
		const size_t dash = name.rfind('-');
		if (dash == std::string::npos || dash < std::strlen(VersionedPrefix))
			return false;
		const std::string digits = name.substr(dash + 1, name.size() - 4 - dash - 1);
		return !digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos;
	}

	void* Download(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 updater");
		std::string package_url, sums_url, package_name, version;
		{
			std::lock_guard lock(s_mutex);
			package_url = s_package_url;
			sums_url = s_sums_url;
			package_name = s_package_name;
			version = s_latest_version;
		}
		const int latest = s_latest_build.load(std::memory_order_relaxed);
		const auto fail = [&](const std::string& why) {
			ProbeLog("Updater: build %d failed: %s\n", latest, why.c_str());
			SetMessage(ForjaLang::F(ForjaLang::TF("No se pudo bajar la %d: %s · OPTIONS reintenta"), latest, why.c_str()));
			s_state.store(PS4Release::DownloadState::Failed, std::memory_order_release);
			return nullptr;
		};

		// The checksum first: a small file, and without it the package could not be checked.
		std::string sums, error;
		size_t total = 0;
		if (sums_url.empty() || !PS4CoverHttp::Stream(sums_url, "ForjaSX2 updater",
				[&](const unsigned char* data, size_t size) {
					sums.append(reinterpret_cast<const char*>(data), size);
					return sums.size() < 64 * 1024;
				},
				total, 60, error))
			return fail(sums_url.empty() ? std::string(ForjaLang::T("la versión no trae SHA256SUMS.txt")) : "SHA256SUMS.txt, " + error);
		if (sums.compare(0, 3, "\xEF\xBB\xBF") == 0)
			sums.erase(0, 3); // a UTF-8 BOM, if the file was written with one
		const std::string expected = ExpectedHash(sums, package_name);
		if (expected.empty())
			return fail(ForjaLang::T("SHA256SUMS.txt no nombra el paquete"));

		mkdir(PackageDirectory, 0777);
		const std::string file_name = PackageFileName(latest, version);
		const std::string final_path = std::string(PackageDirectory) + "/" + file_name;
		const std::string part_path = final_path + ".part";
		std::FILE* file = std::fopen(part_path.c_str(), "wb");
		if (!file)
		{
			const int error_number = errno;
			return fail(ForjaLang::F(ForjaLang::TF("no se pudo escribir en /data/pkg (%d)"), error_number));
		}
		Sha256 sha;
		size_t received = 0;
		bool write_failed = false;
		const bool ok = PS4CoverHttp::Stream(package_url, "ForjaSX2 updater",
			[&](const unsigned char* data, size_t size) {
				if (std::fwrite(data, 1, size, file) != size)
				{
					write_failed = true;
					return false;
				}
				sha.Update(data, size);
				received += size;
				if (total > 0)
					s_progress.store(static_cast<unsigned>(received * 100 / total), std::memory_order_relaxed);
				return true;
			},
			total, 1800, error);
		const bool closed = std::fclose(file) == 0;
		if (!ok || !closed)
		{
			unlink(part_path.c_str());
			return fail(write_failed || !closed ? std::string(ForjaLang::T("no hay espacio o falló la escritura")) : error);
		}
		const std::string actual = sha.FinishHex();
		if (actual != expected)
		{
			unlink(part_path.c_str());
			ProbeLog("Updater: sha256 %s, expected %s\n", actual.c_str(), expected.c_str());
			return fail(ForjaLang::T("la verificación no coincide (se borró)"));
		}
		if (std::rename(part_path.c_str(), final_path.c_str()) != 0)
		{
			unlink(part_path.c_str());
			return fail(ForjaLang::T("no se pudo guardar el paquete"));
		}
		// Older packages the updater left behind go, so /data/pkg does not fill up with them.
		if (DIR* dir = opendir(PackageDirectory))
		{
			std::vector<std::string> old;
			while (dirent* entry = readdir(dir))
			{
				const std::string name = entry->d_name;
				if (name != file_name && UpdaterPackage(name))
					old.push_back(name);
			}
			closedir(dir);
			for (const std::string& name : old)
				unlink((std::string(PackageDirectory) + "/" + name).c_str());
		}
		ProbeLog("Updater: build %d ready at %s (%zu bytes, sha256 %s)\n", latest, final_path.c_str(), received, actual.c_str());
		s_progress.store(100, std::memory_order_relaxed);
		SetMessage(ForjaLang::F(ForjaLang::TF("%s lista en /data/pkg · instálala con GoldHEN"), PS4Release::LatestName().c_str()));
		s_state.store(PS4Release::DownloadState::Ready, std::memory_order_release);
		return nullptr;
	}
}

namespace PS4Release
{
	void Start(int current_build)
	{
		bool expected = false;
		if (!s_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
			return;
		s_current_build.store(current_build, std::memory_order_relaxed);
		pthread_t thread{};
		const int result = pthread_create(&thread, nullptr, Check, nullptr);
		if (result != 0)
			ProbeLog("Release check: thread creation failed=%d\n", result);
		else
			pthread_detach(thread);
	}

	bool HasUpdate()
	{
		return s_has_update.load(std::memory_order_acquire);
	}

	int LatestBuild()
	{
		return s_latest_build.load(std::memory_order_relaxed);
	}

	std::string LatestName()
	{
		const int build = LatestBuild();
		std::lock_guard lock(s_mutex);
		if (s_latest_version.empty())
			return ForjaLang::F(ForjaLang::TF("Compilación %d"), build);
		return ForjaLang::F(ForjaLang::TF("Versión %s (%d)"), s_latest_version.c_str(), build);
	}

	void StartDownload()
	{
		if (!HasUpdate())
			return;
		DownloadState state = s_state.load(std::memory_order_acquire);
		if (state == DownloadState::Downloading || state == DownloadState::Ready)
			return;
		s_progress.store(0, std::memory_order_relaxed);
		s_state.store(DownloadState::Downloading, std::memory_order_release);
		pthread_t thread{};
		const int result = pthread_create(&thread, nullptr, Download, nullptr);
		if (result != 0)
		{
			SetMessage(ForjaLang::T("No se pudo iniciar la descarga."));
			s_state.store(DownloadState::Failed, std::memory_order_release);
			return;
		}
		pthread_detach(thread);
		ProbeLog("Updater: downloading build %d\n", LatestBuild());
	}

	DownloadState State()
	{
		return s_state.load(std::memory_order_acquire);
	}

	std::string StatusText()
	{
		switch (State())
		{
			case DownloadState::Downloading:
				return ForjaLang::F(ForjaLang::TF("Bajando %s: %u %%"), LatestName().c_str(), s_progress.load(std::memory_order_relaxed));
			case DownloadState::Ready:
			case DownloadState::Failed:
			{
				std::lock_guard lock(s_mutex);
				return s_message;
			}
			default:
				return HasUpdate() ? ForjaLang::F(ForjaLang::TF("%s disponible · OPTIONS para bajarla"), LatestName().c_str()) : std::string();
		}
	}
} // namespace PS4Release
