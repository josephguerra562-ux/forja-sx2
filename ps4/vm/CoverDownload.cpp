// SPDX-License-Identifier: GPL-3.0+
#include "CoverDownload.h"
#include "ForjaLang.h"
#include "stb_image.h"
#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>
#include <array>
#include <cctype>
#include <string_view>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern void ProbeLog(const char* format, ...);
// PlatformVM.cpp: a "STAGE:" line with the PS4's free flexible memory.
extern "C" void orbis_stage(const char* stage);
// OpenOrbis leaves these declarations untyped. PS4 ABI: (id, microseconds/enabled).
extern "C" int CoverHttpRecvTimeout(int, unsigned int) __asm__("sceHttpSetRecvTimeOut");
extern "C" int CoverHttpAutoRedirect(int, int) __asm__("sceHttpSetAutoRedirect");
// Forja SX2 (compilation 48): GitHub's redirects can carry more header than sceHttp's default allows (0x80431073).
extern "C" int CoverHttpHeaderMaxSize(int, size_t) __asm__("sceHttpSetResponseHeaderMaxSize");
// Forja SX2 (compilation 50): a response's headers, for the Location of a redirect followed by hand.
extern "C" int CoverHttpAllResponseHeaders(int, char**, size_t*) __asm__("sceHttpGetAllResponseHeaders");

namespace
{
	constexpr size_t MaxBytes = 4 * 1024 * 1024;
	int HttpContext()
	{
		// Process-owned contexts, shared by the single sequential menu downloader.
		// Avoid repeated global init/term while other PS4 services are active.
		static const int context = [] {
			orbis_stage("network: before its modules");
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
			orbis_stage("network ready (Net, NetCtl, Ssl and Http modules, three 1 MiB pools)");
			return http;
		}();
		return context;
	}
	// A header's value in a response's headers ("name: value" lines), the name in any case; empty if it is not there.
	std::string HeaderValue(std::string_view headers, std::string_view name)
	{
		size_t line = 0;
		while (line < headers.size())
		{
			size_t end = headers.find('\n', line);
			if (end == std::string_view::npos)
				end = headers.size();
			std::string_view text = headers.substr(line, end - line);
			if (!text.empty() && text.back() == '\r')
				text.remove_suffix(1);
			const size_t colon = text.find(':');
			if (colon == name.size())
			{
				bool same = true;
				for (size_t i = 0; i < colon && same; i++)
					same = std::tolower(static_cast<unsigned char>(text[i])) == std::tolower(static_cast<unsigned char>(name[i]));
				if (same)
				{
					std::string_view value = text.substr(colon + 1);
					while (!value.empty() && value.front() == ' ')
						value.remove_prefix(1);
					return std::string(value);
				}
			}
			line = end + 1;
		}
		return {};
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

bool PS4CoverHttp::Post(const std::string& url, const char* user_agent, const std::string& content_type, const std::string& body, int& http,
	std::string& response)
{
	http = 0;
	response.clear();
	const int context = HttpContext();
	if (context < 0 || url.empty() || !user_agent)
		return false;
	Request request;
	request.tmpl = sceHttpCreateTemplate(context, user_agent, ORBIS_HTTP_VERSION_1_1, 0);
	if (request.tmpl < 0)
		return false;
	for (int rc : {sceHttpsEnableOption(request.tmpl, 0xbd),
		sceHttpSetResolveTimeOut(request.tmpl, 8000000),
		sceHttpSetConnectTimeOut(request.tmpl, 8000000),
		sceHttpSetSendTimeOut(request.tmpl, 20000000),
		CoverHttpRecvTimeout(request.tmpl, 20000000),
		CoverHttpAutoRedirect(request.tmpl, 0)})
	{
		if (rc < 0)
		{
			ProbeLog("Native HTTPS post: setup failed=%#x\n", rc);
			return false;
		}
	}
	request.connection = sceHttpCreateConnectionWithURL(request.tmpl, url.c_str(), false);
	if (request.connection < 0)
		return false;
	request.request = sceHttpCreateRequestWithURL(request.connection, ORBIS_METHOD_POST, url.c_str(), body.size());
	if (request.request < 0)
		return false;
	if (sceHttpAddRequestHeader(request.request, "User-Agent", user_agent, 0) < 0 ||
		sceHttpAddRequestHeader(request.request, "Content-Type", content_type.c_str(), 0) < 0)
	{
		ProbeLog("Native HTTPS post: request headers unavailable\n");
		return false;
	}
	const int result = sceHttpSendRequest(request.request, body.data(), body.size());
	const int status = result < 0 ? result : sceHttpGetStatusCode(request.request, &http);
	// The URL is not logged: a report's carries the channel's key.
	ProbeLog("Native HTTPS post: %zu bytes, send=%#x status=%#x http=%d\n", body.size(), result, status, http);
	if (status < 0)
		return false;
	std::array<unsigned char, 4096> buffer{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (response.size() < 16 * 1024 && std::chrono::steady_clock::now() < deadline)
	{
		const int count = sceHttpReadData(request.request, buffer.data(), buffer.size());
		if (count <= 0 || static_cast<size_t>(count) > buffer.size())
			break;
		response.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(count));
	}
	return http >= 200 && http < 300;
}

bool PS4CoverHttp::Stream(const std::string& url, const char* user_agent,
	const std::function<bool(const unsigned char*, size_t)>& sink, size_t& total, unsigned deadline_s, std::string& error)
{
	total = 0;
	const int context = HttpContext();
	if (context < 0 || url.empty() || !user_agent)
	{
		error = ForjaLang::T("sin red");
		return false;
	}
	Request request;
	request.tmpl = sceHttpCreateTemplate(context, user_agent, ORBIS_HTTP_VERSION_1_1, 0);
	ProbeLog("Native HTTPS stream: template=%#x url=%s\n", request.tmpl, url.c_str());
	if (request.tmpl < 0)
	{
		error = ForjaLang::T("sin conexión");
		return false;
	}
	// GitHub's release downloads answer with a redirect to their storage host. Compilation 50: followed by hand below; with
	// sceHttp's own (auto redirect on), build 48's first download through GitHub's API never came back from
	// sceHttpSendRequest, neither data nor an error: "0 %" for good (2026-10-18).
	for (int rc : {sceHttpsEnableOption(request.tmpl, 0xbd),
		sceHttpSetResolveTimeOut(request.tmpl, 8000000),
		sceHttpSetConnectTimeOut(request.tmpl, 8000000),
		sceHttpSetSendTimeOut(request.tmpl, 8000000),
		CoverHttpRecvTimeout(request.tmpl, 30000000),
		CoverHttpAutoRedirect(request.tmpl, 0)})
	{
		if (rc < 0)
		{
			ProbeLog("Native HTTPS stream: setup failed=%#x\n", rc);
			error = ForjaLang::T("no se pudo preparar la conexión");
			return false;
		}
	}
	// Compilation 48: room for large response headers (GitHub's redirects); a console that refuses it keeps the default.
	if (const int rc = CoverHttpHeaderMaxSize(request.tmpl, 64 * 1024); rc < 0)
		ProbeLog("Native HTTPS stream: header size limit not raised=%#x\n", rc);
	// Each hop is a request of its own (a new connection when the host changes), with the timeouts on the request too.
	std::string current = url;
	int http = 0;
	int status = 0;
	for (int hop = 0;; hop++)
	{
		if (request.request >= 0)
			sceHttpDeleteRequest(request.request);
		if (request.connection >= 0)
			sceHttpDeleteConnection(request.connection);
		request.request = request.connection = -1;
		request.connection = sceHttpCreateConnectionWithURL(request.tmpl, current.c_str(), false);
		if (request.connection >= 0)
			request.request = sceHttpCreateRequestWithURL(request.connection, ORBIS_METHOD_GET, current.c_str(), 0);
		if (request.request < 0 || sceHttpAddRequestHeader(request.request, "User-Agent", user_agent, 0) < 0 ||
			sceHttpAddRequestHeader(request.request, "Accept", "application/octet-stream", 0) < 0)
		{
			error = ForjaLang::T("no se pudo crear la petición");
			return false;
		}
		sceHttpSetConnectTimeOut(request.request, 8000000);
		sceHttpSetSendTimeOut(request.request, 8000000);
		CoverHttpRecvTimeout(request.request, 30000000);
		const int result = sceHttpSendRequest(request.request, nullptr, 0);
		http = 0;
		status = result < 0 ? result : sceHttpGetStatusCode(request.request, &http);
		ProbeLog("Native HTTPS stream: hop=%d send=%#x status=%#x http=%d\n", hop, result, status, http);
		const bool redirect = http == 301 || http == 302 || http == 303 || http == 307 || http == 308;
		if (status < 0 || !redirect)
			break;
		if (hop >= 5)
		{
			error = ForjaLang::T("demasiadas redirecciones");
			return false;
		}
		char* headers = nullptr;
		size_t headers_size = 0;
		std::string location;
		if (CoverHttpAllResponseHeaders(request.request, &headers, &headers_size) >= 0 && headers)
			location = HeaderValue(std::string_view(headers, headers_size), "location");
		if (location.rfind("https://", 0) != 0)
		{
			ProbeLog("Native HTTPS stream: redirect %d without an https Location (%zu bytes of headers)\n", http, headers_size);
			error = ForjaLang::T("redirección sin destino");
			return false;
		}
		const size_t host_end = location.find('/', 8);
		ProbeLog("Native HTTPS stream: redirect %d to %s\n", http, location.substr(0, host_end).c_str());
		current = location;
	}
	if (status < 0 || http != 200)
	{
		char text[256];
		if (status < 0)
			std::snprintf(text, sizeof(text), ForjaLang::TF("error de red %#x"), static_cast<unsigned>(status));
		else
			std::snprintf(text, sizeof(text), ForjaLang::TF("respuesta HTTP %d"), http);
		error = text;
		return false;
	}
	int length_result = 0;
	size_t length = 0;
	if (sceHttpGetResponseContentLength(request.request, &length_result, &length) == 0 && length_result == 0)
		total = length;
	std::vector<unsigned char> buffer(256 * 1024);
	size_t received = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(deadline_s);
	for (;;)
	{
		if (std::chrono::steady_clock::now() > deadline)
		{
			sceHttpAbortRequest(request.request);
			error = ForjaLang::T("tardó demasiado");
			return false;
		}
		const int count = sceHttpReadData(request.request, buffer.data(), static_cast<unsigned>(buffer.size()));
		if (count < 0)
		{
			char text[256];
			std::snprintf(text, sizeof(text), ForjaLang::TF("se cortó la descarga (%#x)"), static_cast<unsigned>(count));
			error = text;
			return false;
		}
		if (count == 0)
			break;
		received += static_cast<size_t>(count);
		if (!sink(buffer.data(), static_cast<size_t>(count)))
		{
			sceHttpAbortRequest(request.request);
			if (error.empty())
				error = ForjaLang::T("cancelada");
			return false;
		}
	}
	ProbeLog("Native HTTPS stream: bytes=%zu content_length=%zu\n", received, total);
	if (total != 0 && received != total)
	{
		error = ForjaLang::T("llegó incompleta");
		return false;
	}
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
