// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "GamePlatform.h"
#include <atomic>
#include <cstddef>
#include <pthread.h>
#include <string>
#include <vector>

// Shared native HTTPS path used by covers and the release checker. Keeping one
// SceHttp context avoids reinitializing SceNet/SceSsl when both run together.
namespace PS4CoverHttp
{
	bool Get(const std::string& url, const char* user_agent, std::vector<unsigned char>& bytes,
		size_t max_bytes);
}

// (AI-assisted) One menu-owned request; no detached work or GPU access.
class DiscCoverDownload
{
public:
	~DiscCoverDownload();
	// cache_name is the launcher title stem. When omitted, the serial is used.
	bool Start(const std::string& serial, const std::string& cache_name = {}, GamePlatform platform = GamePlatform::PS2);
	bool Collect();
	void Cancel() { m_cancel.store(true); }
	bool Active() const { return m_active; }
	const std::string& Serial() const { return m_serial; }
	bool Succeeded() const { return m_success; } // Read only after Collect().
private:
	void Run();
	pthread_t m_thread{};
	std::atomic<bool> m_cancel{false}, m_done{false};
	bool m_active = false, m_success = false;
	std::string m_serial;
	std::string m_cache_name;
	GamePlatform m_platform = GamePlatform::PS2;
};


