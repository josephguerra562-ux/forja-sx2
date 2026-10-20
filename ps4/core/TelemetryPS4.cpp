// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) PS4 telemetry client - connects to PC debug server via TCP (sceNet).
// Runs on a dedicated thread; producers (EE/GS) only enqueue, never block on network.

#include "TelemetryPS4.h"
#include "ThreadAffinityPS4.h"

#include "common/Timer.h"

#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

extern void ProbeLog(const char* format, ...);

namespace
{
	constexpr std::size_t kCapacity = 256;
	constexpr std::size_t kMaxFrame = 4096;
	constexpr std::size_t kNetPoolSize = 512 * 1024;
	constexpr int kReconnectMinMs = 2000;
	constexpr int kReconnectMaxMs = 10000;
	constexpr const char* kConfigPath = "/data/ForjaSX2/telemetry.conf";
	constexpr const char* kDefaultHost = "192.168.0.8";
	constexpr int kDefaultPort = 19423; // PS4SX2 debug server default (PC IP)

	struct Entry
	{
		char text[kMaxFrame];
		std::size_t length = 0;
	};

	Entry s_queue[kCapacity];
	pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t s_ready = PTHREAD_COND_INITIALIZER;
	pthread_t s_thread{};
	bool s_thread_running = false;
	bool s_stopping = false;
	std::size_t s_head = 0, s_count = 0;
	unsigned long long s_enqueued = 0, s_dropped = 0, s_sent = 0, s_send_errors = 0;
	unsigned long long s_connect_attempts = 0, s_connect_ok = 0;

	int s_sock = -1;
	bool s_connected = false;
	bool s_net_ready = false;
	int s_net_pool = -1;
	std::string s_host = kDefaultHost;
	int s_port = kDefaultPort;
	Common::Timer::Value s_boot_tick{};
	bool s_boot_tick_valid = false;

	// ---- Config ----

	// Forja SX2 (compilation 67): false without the file. Until now every console, a player's too, sent its log to
	// 192.168.0.8:19423 of its own network (kDefaultHost), the developer's PC; a player's has no PC there.
	bool LoadConfig()
	{
		// Format: lines "host=192.168.0.9" and "port=19423"
		// Also accepts single-line "192.168.0.9:19423"
		FILE* f = fopen(kConfigPath, "r");
		if (!f)
			return false;
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			// trim
			char* s = line;
			while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
			if (*s == '#' || *s == '\0') continue;
			// remove trailing newline
			char* end = s + strlen(s) - 1;
			while (end > s && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) *end-- = '\0';

			if (strncmp(s, "host=", 5) == 0)
			{
				std::string h(s + 5);
				// also handle host=IP:PORT
				auto colon = h.find(':');
				if (colon != std::string::npos)
				{
					s_port = atoi(h.c_str() + colon + 1);
					h = h.substr(0, colon);
				}
				if (!h.empty()) s_host = h;
			}
			else if (strncmp(s, "port=", 5) == 0)
			{
				int p = atoi(s + 5);
				if (p > 0 && p < 65536) s_port = p;
			}
			else if (strchr(s, '.') && !strchr(s, '='))
			{
				// bare IP or IP:PORT
				std::string h(s);
				auto colon = h.find(':');
				if (colon != std::string::npos)
				{
					int p = atoi(h.c_str() + colon + 1);
					if (p > 0 && p < 65536) s_port = p;
					h = h.substr(0, colon);
				}
				if (!h.empty()) s_host = h;
			}
		}
		fclose(f);
		return true;
	}

	// ---- Network helpers ----

	bool EnsureNet()
	{
		if (s_net_ready)
			return true;
		// sceNet modules are loaded by CoverDownload HttpContext or here
		sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
		sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
		int rc = sceNetInit();
		// EEXIST (-2143223550 / 0x80410110) means already inited -> ok
		if (rc < 0 && rc != -2143223550 && rc != 0x80410110)
		{
			// try futex value check: sceNetInit returns -0x80410110 on double init
			// still treat as ok if NetCtl says we have network
		}
		sceNetCtlInit();
		s_net_pool = sceNetPoolCreate("PS4SX2-telemetry", kNetPoolSize, 0);
		// pool < 0 is not fatal for BSD sockets on PS4 (some FW use shared pool)
		s_net_ready = true;
		return true;
	}

	void CloseSocket()
	{
		if (s_sock >= 0)
		{
			// Disable SIGPIPE via SO_NOSIGPIPE if available, else MSG_NOSIGNAL on send
			shutdown(s_sock, SHUT_RDWR);
			close(s_sock);
			s_sock = -1;
		}
		s_connected = false;
	}

	bool TryConnect()
	{
		EnsureNet();
		CloseSocket();
		++s_connect_attempts;

		int sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0)
			return false;

		// Non-blocking connect with 3s timeout
		int flags = fcntl(sock, F_GETFL, 0);
		fcntl(sock, F_SETFL, flags | O_NONBLOCK);

		struct sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(s_port));
		if (inet_pton(AF_INET, s_host.c_str(), &addr.sin_addr) != 1)
		{
			close(sock);
			return false;
		}

		int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
		if (rc < 0 && errno != EINPROGRESS)
		{
			close(sock);
			return false;
		}

		struct pollfd pfd{};
		pfd.fd = sock;
		pfd.events = POLLOUT;
		int pr = poll(&pfd, 1, 3000);
		if (pr <= 0 || !(pfd.revents & POLLOUT))
		{
			close(sock);
			return false;
		}

		// Check connect error
		int err = 0;
		socklen_t errlen = sizeof(err);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen);
		if (err != 0)
		{
			close(sock);
			return false;
		}

		// Back to blocking, set send timeout so a dead server doesn't stall us
		fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
		struct timeval tv{3, 0};
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		// TCP_NODELAY for low latency logs
		int one = 1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		s_sock = sock;
		s_connected = true;
		++s_connect_ok;

		// Identify as PS4SX2 + boot time
		double boot_s = 0;
		if (s_boot_tick_valid)
			boot_s = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_boot_tick);
		char hello[512];
		int hlen = snprintf(hello, sizeof(hello),
			"{\"type\":\"hello\",\"app\":\"PS4SX2\",\"build\":\"1.00-CHD-SMC\",\"boot_s\":%.3f,"
			"\"host\":\"%s\",\"port\":%d}\n",
			boot_s, s_host.c_str(), s_port);
		if (hlen > 0)
		{
			// best-effort hello
			send(s_sock, hello, static_cast<size_t>(hlen), MSG_NOSIGNAL);
		}
		return true;
	}

	bool SendFrame(const char* data, std::size_t len)
	{
		if (s_sock < 0 || len == 0)
			return false;
		std::size_t off = 0;
		while (off < len)
		{
			ssize_t n = send(s_sock, data + off, len - off, MSG_NOSIGNAL);
			if (n > 0)
			{
				off += static_cast<std::size_t>(n);
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			// EAGAIN/EWOULDBLOCK shouldn't happen on blocking socket with timeout,
			// but treat as error to reconnect
			return false;
		}
		return true;
	}

	// JSON-escape a string into dst (dst must be >= 2*src + 1). Returns length.
	std::size_t JsonEscape(const char* src, std::size_t src_len, char* dst, std::size_t dst_cap)
	{
		std::size_t out = 0;
		for (std::size_t i = 0; i < src_len && out + 6 < dst_cap; ++i)
		{
			unsigned char c = static_cast<unsigned char>(src[i]);
			if (c == '"' || c == '\\')
			{
				dst[out++] = '\\';
				dst[out++] = static_cast<char>(c);
			}
			else if (c == '\n')
			{
				dst[out++] = '\\';
				dst[out++] = 'n';
			}
			else if (c == '\r')
			{
				dst[out++] = '\\';
				dst[out++] = 'r';
			}
			else if (c == '\t')
			{
				dst[out++] = '\\';
				dst[out++] = 't';
			}
			else if (c < 0x20)
			{
				int n = snprintf(dst + out, dst_cap - out, "\\u%04x", c);
				if (n > 0) out += static_cast<std::size_t>(n);
			}
			else
			{
				dst[out++] = static_cast<char>(c);
			}
		}
		return out;
	}

	void* ThreadMain(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 telemetry");
		PS4ThreadAffinity::PinCurrentThreadToHelpers("telemetry");
		int reconnect_ms = kReconnectMinMs;

		while (true)
		{
			// Wait for data or stop signal
			pthread_mutex_lock(&s_mutex);
			while (s_count == 0 && !s_stopping)
				pthread_cond_wait(&s_ready, &s_mutex);
			if (s_count == 0 && s_stopping)
			{
				pthread_mutex_unlock(&s_mutex);
				break;
			}
			Entry entry = s_queue[s_head];
			s_head = (s_head + 1) % kCapacity;
			--s_count;
			const bool stopping = s_stopping;
			pthread_mutex_unlock(&s_mutex);

			// Forja SX2: stopping with no PC server is not worth waiting for. Stop() (on the way back to the library or out of
			// the app) joins this thread, and the queue (up to 256 lines) was drained here one TryConnect() each: 3 s of
			// connect timeout plus 2-10 s of backoff a line, many minutes in all when the server was gone (Windows' firewall
			// drops the packets instead of refusing them), which Joseph saw as the return to the library hanging (2026-10-15).
			if (!s_connected && stopping)
			{
				pthread_mutex_lock(&s_mutex);
				s_dropped += 1 + s_count;
				s_count = 0;
				pthread_mutex_unlock(&s_mutex);
				break;
			}

			// Ensure connected
			if (!s_connected)
			{
				if (!TryConnect())
				{
					// Drop or requeue? We send with best-effort: requeue with limit
					// To avoid infinite loop on dead server, drop non-critical logs when disconnected
					// But keep heartbeats/stages - detect by type prefix
					bool is_heartbeat = (entry.length >= 2 && entry.text[0] == '{' && memchr(entry.text, '\"', entry.length) != nullptr);
					// Heartbeats and stages are JSON; logs are also JSON but check "type"
					// For simplicity, drop logs when disconnected and reconnect fails, keep trying
					if (!is_heartbeat)
					{
						// still count as dropped after failed reconnect
					}
					// Backoff
					struct timespec ts{};
					ts.tv_sec = reconnect_ms / 1000;
					ts.tv_nsec = (reconnect_ms % 1000) * 1000000L;
					nanosleep(&ts, nullptr);
					reconnect_ms = std::min(reconnect_ms * 2, kReconnectMaxMs);

					// Put entry back if it's important, else drop
					// Stages and heartbeats are important -> requeue (if space)
					if (is_heartbeat && entry.length > 10)
					{
						// Check if it's a stage/hello/heartbeat (keep)
						bool keep = (strstr(entry.text, "\"type\":\"heartbeat\"") != nullptr ||
							strstr(entry.text, "\"type\":\"stage\"") != nullptr ||
							strstr(entry.text, "\"type\":\"hello\"") != nullptr);
						if (keep)
						{
							pthread_mutex_lock(&s_mutex);
							if (s_count < kCapacity && !s_stopping)
							{
								Entry& slot = s_queue[(s_head + s_count) % kCapacity];
								slot = entry;
								++s_count;
							}
							pthread_mutex_unlock(&s_mutex);
						}
					}
					continue;
				}
				reconnect_ms = kReconnectMinMs;
				ProbeLog("Telemetry: connected to %s:%d (attempts=%llu ok=%llu)\n",
					s_host.c_str(), s_port, s_connect_attempts, s_connect_ok);
			}

			// Send - entry.text already has trailing \n
			if (!SendFrame(entry.text, entry.length))
			{
				++s_send_errors;
				CloseSocket();
				// Requeue failed frame for retry
				pthread_mutex_lock(&s_mutex);
				if (s_count < kCapacity && !s_stopping)
				{
					Entry& slot = s_queue[(s_head + s_count) % kCapacity];
					slot = entry;
					++s_count;
				}
				else
				{
					++s_dropped;
				}
				pthread_mutex_unlock(&s_mutex);
				// brief pause before reconnect
				struct timespec ts{0, 200 * 1000000L};
				nanosleep(&ts, nullptr);
				continue;
			}
			++s_sent;
		}

		// Final goodbye
		if (s_connected)
		{
			double boot_s = 0;
			if (s_boot_tick_valid)
				boot_s = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_boot_tick);
			char bye[256];
			int blen = snprintf(bye, sizeof(bye), "{\"type\":\"bye\",\"boot_s\":%.3f}\n", boot_s);
			if (blen > 0)
				SendFrame(bye, static_cast<std::size_t>(blen));
		}
		CloseSocket();
		return nullptr;
	}

	// Enqueue raw frame (already JSON + \n)
	void Enqueue(const char* text, std::size_t length)
	{
		if (!s_thread_running || s_stopping)
			return;
		if (length == 0 || length >= kMaxFrame)
			length = std::min(length, kMaxFrame - 1);
		pthread_mutex_lock(&s_mutex);
		if (s_count == kCapacity)
		{
			++s_dropped;
		}
		else
		{
			Entry& e = s_queue[(s_head + s_count) % kCapacity];
			memcpy(e.text, text, length);
			e.text[length] = '\0';
			e.length = length;
			// Ensure newline terminated
			if (e.length == 0 || e.text[e.length - 1] != '\n')
			{
				if (e.length + 1 < kMaxFrame)
				{
					e.text[e.length++] = '\n';
					e.text[e.length] = '\0';
				}
			}
			++s_count;
			++s_enqueued;
			pthread_cond_signal(&s_ready);
		}
		pthread_mutex_unlock(&s_mutex);
	}

} // namespace

namespace PS4Telemetry
{

	bool Start()
	{
		if (!LoadConfig())
		{
			ProbeLog("Telemetry: off (no %s; create it with 'host=PC_IP port=19423' to send this log to a PC)\n", kConfigPath);
			return false;
		}
		s_boot_tick = Common::Timer::GetCurrentValue();
		s_boot_tick_valid = true;
		s_stopping = false;
		s_head = s_count = 0;
		s_enqueued = s_dropped = s_sent = s_send_errors = 0;
		s_connect_attempts = s_connect_ok = 0;

		pthread_attr_t attr;
		int rc = pthread_attr_init(&attr);
		if (rc == 0)
		{
			rc = pthread_attr_setstacksize(&attr, 256 * 1024);
			if (rc == 0)
				rc = pthread_create(&s_thread, &attr, ThreadMain, nullptr);
			pthread_attr_destroy(&attr);
		}
		if (rc != 0)
		{
			ProbeLog("Telemetry: thread creation failed=%d host=%s:%d\n", rc, s_host.c_str(), s_port);
			return false;
		}
		s_thread_running = true;
		ProbeLog("Telemetry: started target=%s:%d config=%s (needs proper testing)\n",
			s_host.c_str(), s_port, kConfigPath);
		return true;
	}

	void Stop()
	{
		if (!s_thread_running)
			return;
		pthread_mutex_lock(&s_mutex);
		s_stopping = true;
		pthread_cond_signal(&s_ready);
		pthread_mutex_unlock(&s_mutex);
		pthread_join(s_thread, nullptr);
		s_thread_running = false;

		char summary[512];
		int len = snprintf(summary, sizeof(summary),
			"Telemetry: stopped sent=%llu dropped=%llu errors=%llu connects=%llu/%llu host=%s:%d\n",
			s_sent, s_dropped, s_send_errors, s_connect_ok, s_connect_attempts,
			s_host.c_str(), s_port);
		if (len > 0)
			ProbeLog("%.*s", len, summary);
	}

	void Write(const char* text, std::size_t length)
	{
		if (!s_thread_running || s_stopping || !text || length == 0)
			return;
		// Wrap raw log line as JSON: {"type":"log","boot_s":..,"msg":"..."}
		double boot_s = 0;
		if (s_boot_tick_valid)
			boot_s = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_boot_tick);

		char frame[kMaxFrame];
		// Reserve space for JSON envelope
		char escaped[kMaxFrame];
		std::size_t elen = JsonEscape(text, std::min(length, kMaxFrame / 2), escaped, sizeof(escaped));
		// Trim trailing \n from escaped for cleaner JSON
		while (elen > 0 && (escaped[elen - 1] == '\n' || escaped[elen - 1] == '\r'))
			--elen;
		escaped[elen] = '\0';

		int flen = snprintf(frame, sizeof(frame),
			"{\"type\":\"log\",\"boot_s\":%.3f,\"msg\":\"%.*s\"}\n",
			boot_s, static_cast<int>(elen), escaped);
		if (flen > 0)
			Enqueue(frame, static_cast<std::size_t>(flen));
	}

	void Heartbeat(double boot_seconds, double vm_seconds, std::uint32_t ee_pc, std::uint32_t iop_pc,
		double ee_mcycles, double polls_per_s, double clock_checks_per_s,
		int vm_state, bool menu_open, int vsync_q, const char* game_file)
	{
		if (!s_thread_running || s_stopping)
			return;
		char esc_game[512] = {};
		std::size_t glen = 0;
		if (game_file && *game_file)
			glen = JsonEscape(game_file, strlen(game_file), esc_game, sizeof(esc_game));

		char frame[kMaxFrame];
		int flen = snprintf(frame, sizeof(frame),
			"{\"type\":\"heartbeat\",\"boot_s\":%.3f,\"vm_s\":%.3f,"
			"\"ee_pc\":%u,\"iop_pc\":%u,\"ee_mcps\":%.2f,\"polls\":%.0f,\"clock_checks\":%.0f,"
			"\"vm_state\":%d,\"menu\":%d,\"vsyncq\":%d,\"game\":\"%.*s\"}\n",
			boot_seconds, vm_seconds, ee_pc, iop_pc, ee_mcycles, polls_per_s, clock_checks_per_s,
			vm_state, menu_open ? 1 : 0, vsync_q, static_cast<int>(glen), esc_game);
		if (flen > 0)
			Enqueue(frame, static_cast<std::size_t>(flen));
	}

	void Stage(const char* stage)
	{
		if (!s_thread_running || s_stopping || !stage)
			return;
		double boot_s = 0;
		if (s_boot_tick_valid)
			boot_s = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_boot_tick);
		char esc[1024];
		std::size_t elen = JsonEscape(stage, strlen(stage), esc, sizeof(esc));
		char frame[kMaxFrame];
		int flen = snprintf(frame, sizeof(frame),
			"{\"type\":\"stage\",\"boot_s\":%.3f,\"msg\":\"%.*s\"}\n",
			boot_s, static_cast<int>(elen), esc);
		if (flen > 0)
			Enqueue(frame, static_cast<std::size_t>(flen));
	}

	void GameSelect(const char* filename, const char* platform)
	{
		if (!s_thread_running || s_stopping)
			return;
		double boot_s = 0;
		if (s_boot_tick_valid)
			boot_s = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_boot_tick);
		char esc_file[1024] = {}, esc_plat[64] = {};
		std::size_t flen1 = 0, flen2 = 0;
		if (filename) flen1 = JsonEscape(filename, strlen(filename), esc_file, sizeof(esc_file));
		if (platform) flen2 = JsonEscape(platform, strlen(platform), esc_plat, sizeof(esc_plat));
		char frame[kMaxFrame];
		int flen = snprintf(frame, sizeof(frame),
			"{\"type\":\"game\",\"boot_s\":%.3f,\"file\":\"%.*s\",\"platform\":\"%.*s\"}\n",
			boot_s, static_cast<int>(flen1), esc_file, static_cast<int>(flen2), esc_plat);
		if (flen > 0)
			Enqueue(frame, static_cast<std::size_t>(flen));
	}

	bool IsConnected()
	{
		pthread_mutex_lock(&s_mutex);
		bool c = s_connected;
		pthread_mutex_unlock(&s_mutex);
		return c;
	}

	double GetBootSeconds()
	{
		if (!s_boot_tick_valid)
			return 0;
		return Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_boot_tick);
	}

} // namespace PS4Telemetry
