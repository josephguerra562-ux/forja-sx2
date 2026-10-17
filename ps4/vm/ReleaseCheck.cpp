// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Checks the project's public GitHub release endpoint without
// blocking the launcher or requiring a second networking implementation.
// Forja SX2: every build of main is published as a pre-release "compilacion-N"; the ones Joseph approves are promoted to
// normal releases (.github/workflows/aprobar.yml). Two channels: the stable one (the default) reads /releases/latest,
// which only shows those, and the testing one (/data/ForjaSX2/canal-pruebas exists) reads the newest release of any kind.
// The tag's number is compared with this build's. The updater downloads its package into /data/pkg (as DolphinPS4 does
// since v03.36) and checks it against the release's SHA256SUMS.txt.
#include "ReleaseCheck.h"
#include "CoverDownload.h"
#include "Sha256.h"
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
	constexpr const char* TestingUrl = "https://api.github.com/repos/josephguerra562-ux/forja-sx2/releases?per_page=1";
	constexpr const char* TestingMarker = "/data/ForjaSX2/canal-pruebas";
	constexpr const char* PackageDirectory = "/data/pkg";
	constexpr const char* PackagePrefix = "ForjaSX2-compilacion-";

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
		const std::string json(bytes.begin(), bytes.end());
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
		{
			std::lock_guard lock(s_mutex);
			s_package_url = package_url;
			s_sums_url = sums_url;
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

	void* Download(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 updater");
		std::string package_url, sums_url, package_name;
		{
			std::lock_guard lock(s_mutex);
			package_url = s_package_url;
			sums_url = s_sums_url;
			package_name = s_package_name;
		}
		const int latest = s_latest_build.load(std::memory_order_relaxed);
		const auto fail = [&](const std::string& why) {
			ProbeLog("Updater: build %d failed: %s\n", latest, why.c_str());
			SetMessage("No se pudo bajar la " + std::to_string(latest) + ": " + why + " · OPTIONS reintenta");
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
			return fail(sums_url.empty() ? "la versión no trae SHA256SUMS.txt" : "SHA256SUMS.txt, " + error);
		if (sums.compare(0, 3, "\xEF\xBB\xBF") == 0)
			sums.erase(0, 3); // a UTF-8 BOM, if the file was written with one
		const std::string expected = ExpectedHash(sums, package_name);
		if (expected.empty())
			return fail("SHA256SUMS.txt no nombra el paquete");

		mkdir(PackageDirectory, 0777);
		const std::string final_path = std::string(PackageDirectory) + "/" + PackagePrefix + std::to_string(latest) + ".pkg";
		const std::string part_path = final_path + ".part";
		std::FILE* file = std::fopen(part_path.c_str(), "wb");
		if (!file)
			return fail("no se pudo escribir en /data/pkg (" + std::to_string(errno) + ")");
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
			return fail(write_failed || !closed ? "no hay espacio o falló la escritura" : error);
		}
		const std::string actual = sha.FinishHex();
		if (actual != expected)
		{
			unlink(part_path.c_str());
			ProbeLog("Updater: sha256 %s, expected %s\n", actual.c_str(), expected.c_str());
			return fail("la verificación no coincide (se borró)");
		}
		if (std::rename(part_path.c_str(), final_path.c_str()) != 0)
		{
			unlink(part_path.c_str());
			return fail("no se pudo guardar el paquete");
		}
		// Older packages the updater left behind go, so /data/pkg does not fill up with them.
		if (DIR* dir = opendir(PackageDirectory))
		{
			const std::string keep = std::string(PackagePrefix) + std::to_string(latest) + ".pkg";
			std::vector<std::string> old;
			while (dirent* entry = readdir(dir))
			{
				const std::string name = entry->d_name;
				if (name != keep && name.rfind(PackagePrefix, 0) == 0)
					old.push_back(name);
			}
			closedir(dir);
			for (const std::string& name : old)
				unlink((std::string(PackageDirectory) + "/" + name).c_str());
		}
		ProbeLog("Updater: build %d ready at %s (%zu bytes, sha256 %s)\n", latest, final_path.c_str(), received, actual.c_str());
		s_progress.store(100, std::memory_order_relaxed);
		SetMessage(PS4Release::LatestName() + " lista en /data/pkg · instálala con GoldHEN");
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
			return "Compilación " + std::to_string(build);
		return "Versión " + s_latest_version + " (" + std::to_string(build) + ")";
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
			SetMessage("No se pudo iniciar la descarga.");
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
				return "Bajando " + LatestName() + ": " + std::to_string(s_progress.load(std::memory_order_relaxed)) + " %";
			case DownloadState::Ready:
			case DownloadState::Failed:
			{
				std::lock_guard lock(s_mutex);
				return s_message;
			}
			default:
				return HasUpdate() ? LatestName() + " disponible · OPTIONS para bajarla" : std::string();
		}
	}
} // namespace PS4Release
