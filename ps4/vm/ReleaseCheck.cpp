// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Checks the project's public GitHub release endpoint without
// blocking the launcher or requiring a second networking implementation.
#include "ReleaseCheck.h"
#include "CoverDownload.h"
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <pthread.h>

extern void ProbeLog(const char* format, ...);

namespace
{
	constexpr const char* CurrentVersion = "1.00";
	constexpr const char* LatestReleaseUrl =
		"https://api.github.com/repos/josephguerra562-ux/forja-sx2/releases/latest";
	std::atomic<bool> s_started{false};
	std::atomic<bool> s_has_update{false};
	char s_latest[32]{};

	std::array<unsigned, 3> ParseVersion(std::string_view value, bool& valid)
	{
		std::array<unsigned, 3> result{};
		valid = false;
		size_t pos = 0;
		while (pos < value.size() && !std::isdigit(static_cast<unsigned char>(value[pos])))
			++pos;
		if (pos == value.size())
			return result;
		for (size_t component = 0; component < result.size(); ++component)
		{
			if (pos == value.size() || !std::isdigit(static_cast<unsigned char>(value[pos])))
				return result;
			unsigned number = 0;
			while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])))
			{
				number = number * 10u + static_cast<unsigned>(value[pos] - '0');
				++pos;
			}
			result[component] = number;
			while (pos < value.size() && !std::isdigit(static_cast<unsigned char>(value[pos])))
				++pos;
		}
		valid = true;
		return result;
	}

	bool IsNewer(std::string_view latest)
	{
		bool latest_valid = false, current_valid = false;
		const auto latest_parts = ParseVersion(latest, latest_valid);
		const auto current_parts = ParseVersion(CurrentVersion, current_valid);
		return latest_valid && current_valid && latest_parts > current_parts;
	}

	bool ExtractTag(const std::vector<unsigned char>& bytes, std::string& tag)
	{
		const std::string json(bytes.begin(), bytes.end());
		const std::string key = "\"tag_name\"";
		const size_t key_pos = json.find(key);
		if (key_pos == std::string::npos)
			return false;
		const size_t colon = json.find(':', key_pos + key.size());
		const size_t quote = colon == std::string::npos ? std::string::npos : json.find('"', colon + 1);
		if (quote == std::string::npos)
			return false;
		const size_t end = json.find('"', quote + 1);
		if (end == std::string::npos || end == quote + 1 || end - quote > 31)
			return false;
		tag.assign(json, quote + 1, end - quote - 1);
		return true;
	}

	void* Run(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 release check");
		std::vector<unsigned char> bytes;
		if (!PS4CoverHttp::Get(LatestReleaseUrl, "ForjaSX2/1.00 release-checker", bytes, 64 * 1024))
		{
			ProbeLog("Release check: GitHub request failed\n");
			return nullptr;
		}
		std::string tag;
		if (!ExtractTag(bytes, tag))
		{
			ProbeLog("Release check: tag_name missing\n");
			return nullptr;
		}
		std::snprintf(s_latest, sizeof(s_latest), "%s", tag.c_str());
		const bool newer = IsNewer(tag);
		s_has_update.store(newer, std::memory_order_release);
		ProbeLog("Release check: current=%s latest=%s update=%d\n", CurrentVersion, s_latest, newer ? 1 : 0);
		return nullptr;
	}
}

namespace PS4Release
{
	void Start()
	{
		bool expected = false;
		if (!s_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
			return;
		pthread_t thread{};
		const int result = pthread_create(&thread, nullptr, Run, nullptr);
		if (result != 0)
			ProbeLog("Release check: thread creation failed=%d\n", result);
		else
			pthread_detach(thread);
	}

	bool HasUpdate()
	{
		return s_has_update.load(std::memory_order_acquire);
	}

	const char* LatestVersion()
	{
		return s_latest;
	}
} // namespace PS4Release
