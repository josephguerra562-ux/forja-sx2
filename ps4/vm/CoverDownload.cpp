// SPDX-License-Identifier: GPL-3.0+
#include "CoverDownload.h"
#include "stb_image.h"
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern void ProbeLog(const char* format, ...);
// OpenOrbis leaves these declarations untyped. PS4 ABI: (id, microseconds/enabled).
extern "C" int CoverHttpRecvTimeout(int, unsigned int) __asm__("sceHttpSetRecvTimeOut");
extern "C" int CoverHttpAutoRedirect(int, int) __asm__("sceHttpSetAutoRedirect");

namespace
{
	constexpr size_t MaxBytes = 4 * 1024 * 1024;
	int HttpContext()
	{
		// Process-owned contexts, shared by the single sequential menu downloader.
		// Avoid repeated global init/term while other PS4 services are active.
		static const int context = [] {
			for (const auto module : {ORBIS_SYSMODULE_INTERNAL_NET, ORBIS_SYSMODULE_INTERNAL_NETCTL,
					 ORBIS_SYSMODULE_INTERNAL_SSL, ORBIS_SYSMODULE_INTERNAL_HTTP})
			{
				const int rc = sceSysmoduleLoadModuleInternal(module);
				ProbeLog("Disc cover: native module=%#x load=%#x\n", module, rc);
				if (rc < 0)
					return -1;
			}
			const int net = sceNetInit();
			const int ctl = sceNetCtlInit();
			ProbeLog("Disc cover: native net=%#x netctl=%#x\n", net, ctl);
			const int pool = sceNetPoolCreate("PS4SX2 covers", 1024 * 1024, 0);
			ProbeLog("Disc cover: native pool=%#x\n", pool);
			if (pool < 0)
				return -1;
			const int ssl = sceSslInit(1024 * 1024);
			ProbeLog("Disc cover: native ssl=%#x\n", ssl);
			if (ssl < 0)
				return -1;
			const int http = sceHttpInit(pool, ssl, 1024 * 1024);
			ProbeLog("Disc cover: native http=%#x\n", http);
			return http;
		}();
		return context;
	}
	struct Request
	{
		int tmpl = -1, connection = -1, request = -1;
		~Request()
		{
			if (request >= 0)
				sceHttpDeleteRequest(request);
			if (connection >= 0)
				sceHttpDeleteConnection(connection);
			if (tmpl >= 0)
				sceHttpDeleteTemplate(tmpl);
		}
	};
} // namespace

bool PS4CoverHttp::Get(const std::string& url, const char* user_agent, std::vector<unsigned char>& bytes,
	size_t max_bytes)
{
	bytes.clear();
	const int context = HttpContext();
	if (context < 0 || url.empty() || !user_agent || max_bytes == 0)
		return false;
	Request request;
	request.tmpl = sceHttpCreateTemplate(context, user_agent, ORBIS_HTTP_VERSION_1_1, 0);
	ProbeLog("Native HTTPS: template=%#x url=%s\n", request.tmpl, url.c_str());
	if (request.tmpl < 0)
		return false;
	for (int rc : {sceHttpsEnableOption(request.tmpl, 0xbd),
		sceHttpSetResolveTimeOut(request.tmpl, 4000000),
		sceHttpSetConnectTimeOut(request.tmpl, 4000000),
		sceHttpSetSendTimeOut(request.tmpl, 4000000),
		CoverHttpRecvTimeout(request.tmpl, 4000000),
		CoverHttpAutoRedirect(request.tmpl, 0)})
	{
		if (rc < 0)
		{
			ProbeLog("Native HTTPS: setup failed=%#x url=%s\n", rc, url.c_str());
			return false;
		}
	}
	request.connection = sceHttpCreateConnectionWithURL(request.tmpl, url.c_str(), false);
	if (request.connection < 0)
		return false;
	request.request = sceHttpCreateRequestWithURL(request.connection, ORBIS_METHOD_GET, url.c_str(), 0);
	if (request.request < 0)
		return false;
	if (sceHttpAddRequestHeader(request.request, "User-Agent", user_agent, 0) < 0 ||
		sceHttpAddRequestHeader(request.request, "Accept", "application/vnd.github+json", 0) < 0 ||
		sceHttpAddRequestHeader(request.request, "X-GitHub-Api-Version", "2022-11-28", 0) < 0)
	{
		ProbeLog("Native HTTPS: request headers unavailable\n");
		return false;
	}
	const int result = sceHttpSendRequest(request.request, nullptr, 0);
	int http = 0;
	const int status = result < 0 ? result : sceHttpGetStatusCode(request.request, &http);
	ProbeLog("Native HTTPS: send=%#x status=%#x http=%d\n", result, status, http);
	if (status < 0 || http != 200)
		return false;
	std::array<unsigned char, 4096> buffer{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
	for (;;)
	{
		if (std::chrono::steady_clock::now() > deadline)
		{
			sceHttpAbortRequest(request.request);
			ProbeLog("Native HTTPS: transfer deadline exceeded\n");
			return false;
		}
		const int count = sceHttpReadData(request.request, buffer.data(), buffer.size());
		if (count < 0 || static_cast<size_t>(count) > buffer.size() || bytes.size() + static_cast<size_t>(count) > max_bytes)
			return false;
		if (count == 0)
			break;
		bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
	}
	ProbeLog("Native HTTPS: response bytes=%zu\n", bytes.size());
	return true;
}

DiscCoverDownload::~DiscCoverDownload()
{
	Cancel();
	if (m_active)
		pthread_join(m_thread, nullptr);
}

bool DiscCoverDownload::Start(const std::string& serial, const std::string& cache_name, GamePlatform platform)
{
	if (m_active || platform == GamePlatform::Unknown || serial.empty() || serial.size() > 32 ||
		serial.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
		return false;
	if (!cache_name.empty() && (cache_name.size() > 180 || cache_name.find_first_of("/\\\r\n") != std::string::npos))
		return false;
	m_serial = serial;
	m_platform = platform;
	m_cache_name = cache_name.empty() ? serial : cache_name;
	m_success = false;
	m_done.store(false);
	m_cancel.store(false);
	pthread_attr_t attr;
	int rc = pthread_attr_init(&attr);
	if (rc == 0)
	{
		rc = pthread_attr_setstacksize(&attr, 1024 * 1024);
		if (rc == 0)
			rc = pthread_create(&m_thread, &attr, [](void* context) -> void* {
				auto* job = static_cast<DiscCoverDownload*>(context);
				job->Run();
				job->m_done.store(true, std::memory_order_release);
				return nullptr; }, this);
		pthread_attr_destroy(&attr);
	}
	m_active = rc == 0;
	if (!m_active)
		ProbeLog("Disc cover: thread creation failed=%d\n", rc);
	return m_active;
}

bool DiscCoverDownload::Collect()
{
	if (!m_active || !m_done.load(std::memory_order_acquire))
		return false;
	pthread_join(m_thread, nullptr);
	m_active = false;
	return true;
}

void DiscCoverDownload::Run()
{
	pthread_setname_np(pthread_self(), "PS4 cover fetch");
	const int context = HttpContext();
	if (context < 0 || m_cancel.load())
		return;
	// (AI-assisted) Serial prefixes overlap between consoles. Use the image's
	// detected disc type, never its extension or serial prefix, to choose a catalog.
	const char* catalog = m_platform == GamePlatform::PS1 ? "psx-covers" : "ps2-covers";
	const std::string url = std::string("https://raw.githubusercontent.com/xlenore/") +
		catalog + "/main/covers/default/" + m_serial + ".jpg";
	ProbeLog("Disc cover: catalog=%s serial=%s\n", catalog, m_serial.c_str());
	std::vector<unsigned char> bytes;
	Request request;
	request.tmpl = sceHttpCreateTemplate(context, "ForjaSX2/1.00 cover-downloader", ORBIS_HTTP_VERSION_1_1, 0);
	ProbeLog("Disc cover: template=%#x serial=%s\n", request.tmpl, m_serial.c_str());
	if (request.tmpl < 0)
		return;
	// Server, hostname, date and CA checks plus SNI. Never bypass certificate failures.
	for (int rc : {sceHttpsEnableOption(request.tmpl, 0xbd),
			 sceHttpSetResolveTimeOut(request.tmpl, 4000000),
			 sceHttpSetConnectTimeOut(request.tmpl, 4000000),
			 sceHttpSetSendTimeOut(request.tmpl, 4000000),
			 CoverHttpRecvTimeout(request.tmpl, 4000000),
			 CoverHttpAutoRedirect(request.tmpl, 0)})
	{
		if (rc < 0)
		{
			ProbeLog("Disc cover: native setup failed=%#x\n", rc);
			return;
		}
	}
	request.connection = sceHttpCreateConnectionWithURL(request.tmpl, url.c_str(), false);
	ProbeLog("Disc cover: connection=%#x\n", request.connection);
	if (request.connection < 0 || m_cancel.load())
		return;
	request.request = sceHttpCreateRequestWithURL(request.connection, ORBIS_METHOD_GET, url.c_str(), 0);
	ProbeLog("Disc cover: request=%#x\n", request.request);
	if (request.request < 0 || m_cancel.load())
		return;
	const auto started = std::chrono::steady_clock::now();
	ProbeLog("Disc cover: native send begin serial=%s\n", m_serial.c_str());
	const int result = sceHttpSendRequest(request.request, nullptr, 0);
	int http = 0;
	const int status = result < 0 ? result : sceHttpGetStatusCode(request.request, &http);
	ProbeLog("Disc cover: native send=%#x status=%#x http=%d\n", result, status, http);
	if (status < 0 || http != 200 || m_cancel.load())
		return;
	std::array<unsigned char, 16384> buffer{};
	for (;;)
	{
		if (m_cancel.load() || std::chrono::steady_clock::now() - started > std::chrono::seconds(12))
		{
			sceHttpAbortRequest(request.request);
			ProbeLog("Disc cover: cancelled or transfer deadline exceeded\n");
			return;
		}
		const int count = sceHttpReadData(request.request, buffer.data(), buffer.size());
		if (count < 0 || static_cast<size_t>(count) > buffer.size() || static_cast<size_t>(count) > MaxBytes - bytes.size())
		{
			ProbeLog("Disc cover: native read failed=%#x bytes=%zu\n", count, bytes.size());
			return;
		}
		if (count == 0)
			break;
		bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
	}
	ProbeLog("Disc cover: native response bytes=%zu\n", bytes.size());
	if (m_cancel.load() || bytes.size() < 3 || bytes[0] != 0xff || bytes[1] != 0xd8)
		return;
	int w = 0, h = 0, channels = 0;
	if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) ||
		w <= 0 || h <= 0 || static_cast<size_t>(w) * h > 8 * 1024 * 1024)
		return;
	unsigned char* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
	if (!decoded)
		return;
	stbi_image_free(decoded);
	if (m_cancel.load())
		return;
	if (mkdir("/data/ForjaSX2/covers", 0777) != 0 && errno != EEXIST)
	{
		ProbeLog("Disc cover: mkdir failed errno=%d\n", errno);
		return;
	}
	const std::string path = "/data/ForjaSX2/covers/" + m_cache_name + ".jpg";
	const std::string temp = path + ".download";
	FILE* file = std::fopen(temp.c_str(), "wb");
	if (!file)
	{
		ProbeLog("Disc cover: temporary file open failed errno=%d\n", errno);
		return;
	}
	bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	if (!ok)
		ProbeLog("Disc cover: cache write failed errno=%d\n", errno);
	ok = std::fclose(file) == 0 && ok;
	// (AI-assisted) PS4 cache publication failed with link() in v0.99.
	// Rename the completed temporary file within the same directory instead.
	if (ok && !m_cancel.load())
	{
		struct stat existing{};
		if (stat(path.c_str(), &existing) == 0)
		{
			ProbeLog("Disc cover: keeping existing local file %s\n", path.c_str());
			m_success = S_ISREG(existing.st_mode) && existing.st_size > 0;
		}
		else if (errno == ENOENT)
		{
			const int result = std::rename(temp.c_str(), path.c_str());
			const int saved_errno = result == 0 ? 0 : errno;
			m_success = result == 0;
			ProbeLog("Disc cover: cache rename result=%d errno=%d\n", result, saved_errno);
		}
		else
			ProbeLog("Disc cover: cache stat failed errno=%d\n", errno);
	}
	unlink(temp.c_str());
	ProbeLog("Disc cover: cache serial=%s name=%s saved=%d path=%s\n", m_serial.c_str(), m_cache_name.c_str(), m_success, path.c_str());
}
