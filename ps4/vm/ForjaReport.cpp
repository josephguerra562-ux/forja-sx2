// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 77): the player's reports (ForjaReport.h). Sent as a Discord webhook message with the
// report attached (multipart/form-data: payload_json and files[0]), ?wait=true so the answer says it arrived. Mentions are
// turned off in the message, so a game's name can't ping the server. One thread at a time, detached, with the stack size the
// app's other threads use; nothing waits for it.
#include "ForjaReport.h"
#include "CoverDownload.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

extern void ProbeLog(const char* format, ...);

// The channel's address in hexadecimal (ps4/vm/CMakeLists.txt): plain text would show in the package to any scanner looking
// for Discord webhooks.
#ifndef FORJA_REPORT_HEX
#define FORJA_REPORT_HEX ""
#endif

namespace
{
	constexpr const char* Folder = "/data/ForjaSX2/reportes";
	std::atomic<ForjaReport::Status> s_status{ForjaReport::Status::Idle};
	std::mutex s_mutex;
	std::string s_last_name;

	std::string ChannelUrl()
	{
		static const std::string url = [] {
			const std::string hex = FORJA_REPORT_HEX;
			const auto nibble = [](char c) {
				return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			};
			std::string text;
			for (size_t i = 0; i + 1 < hex.size(); i += 2)
			{
				const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
				if (high < 0 || low < 0)
					return std::string();
				text += static_cast<char>(high * 16 + low);
			}
			return text.rfind("https://", 0) == 0 ? text : std::string();
		}();
		return url;
	}

	std::string JsonString(const std::string& text)
	{
		std::string out = "\"";
		for (const unsigned char c : text)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += static_cast<char>(c);
			}
			else if (c == '\n')
				out += "\\n";
			else if (c < 0x20)
				out += ' ';
			else
				out += static_cast<char>(c);
		}
		return out + "\"";
	}

	struct Job
	{
		std::string name, summary, text;
		std::string log_gz; // compilation 85: the session's whole log, gzipped (Run fills it)
		bool resend = false; // compilation 86: a pending report sent again (no log: that session is over)
	};

	// Compilation 86: when the report being sent started (steady clock, ms; 0: none), and whether a pending one is being sent
	// again in the background (that one doesn't show in the menu).
	std::atomic<long long> s_send_started_ms{0};
	std::atomic<bool> s_resending{false};

	long long NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::string PendingPath(const std::string& name)
	{
		return std::string(Folder) + "/" + name + ".pendiente";
	}

	// Compilation 85: the session's whole log goes with the report, gzipped, as a second file. Until 84 a report carried the
	// summary and the meters' lines of its last ~2 minutes (~50 KB): not how the game started, its fixes and patches, nor
	// anything earlier. A 12-minute log of ~700 KB is ~70 KB gzipped. Its last 16 MB at most.
	constexpr const char* SessionLog = "/data/ForjaSX2/logs/PCSX2-vm-boot.log";
	constexpr long MaxLogBytes = 16L * 1024 * 1024;

	std::string GzippedLog()
	{
		std::string raw;
		if (std::FILE* file = std::fopen(SessionLog, "rb"))
		{
			std::fseek(file, 0, SEEK_END);
			const long size = std::ftell(file);
			const long start = size > MaxLogBytes ? size - MaxLogBytes : 0;
			std::fseek(file, start, SEEK_SET);
			raw.resize(size > start ? static_cast<size_t>(size - start) : 0);
			raw.resize(raw.empty() ? 0 : std::fread(raw.data(), 1, raw.size(), file));
			std::fclose(file);
		}
		if (raw.empty())
			return {};
		z_stream stream{};
		if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) // 15 + 16: gzip
			return {};
		std::string out(deflateBound(&stream, static_cast<uLong>(raw.size())), '\0');
		stream.next_in = reinterpret_cast<Bytef*>(raw.data());
		stream.avail_in = static_cast<uInt>(raw.size());
		stream.next_out = reinterpret_cast<Bytef*>(out.data());
		stream.avail_out = static_cast<uInt>(out.size());
		const int result = deflate(&stream, Z_FINISH);
		out.resize(stream.total_out);
		deflateEnd(&stream);
		return result == Z_STREAM_END ? out : std::string();
	}

	// Compilation 79: a forum channel takes a webhook's message only as a new post, which needs its title (thread_name, up to 100
	// characters); a text channel refuses one. So the message goes as it is, and again with the title if the channel says it is a
	// forum (Discord's 220001: "Webhooks posted to forum channels must have a thread_name or thread_id").
	std::string Body(const Job& job, const std::string& boundary, bool forum)
	{
		std::string title;
		if (forum)
		{
			title = job.summary.substr(0, 100);
			while (!title.empty() && (static_cast<unsigned char>(title.back()) & 0xC0) == 0x80)
				title.pop_back(); // not a letter cut in half
			if (!title.empty() && (static_cast<unsigned char>(title.back()) & 0x80) != 0)
				title.pop_back();
		}
		std::string body;
		body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n";
		body += "{\"username\":\"Forja SX2\",\"allowed_mentions\":{\"parse\":[]},\"content\":" + JsonString(job.summary);
		if (forum)
			body += ",\"thread_name\":" + JsonString(title);
		body += "}\r\n";
		body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"" + job.name +
		        ".txt\"\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n";
		body += job.text;
		if (!job.log_gz.empty()) // compilation 85
		{
			body += "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"files[1]\"; filename=\"" + job.name +
			        "_registro.log.gz\"\r\nContent-Type: application/gzip\r\n\r\n";
			body += job.log_gz;
		}
		body += "\r\n--" + boundary + "--\r\n";
		return body;
	}

	void* Run(void* argument)
	{
		pthread_setname_np(pthread_self(), "Forja report");
		const std::unique_ptr<Job> job(static_cast<Job*>(argument));
		if (!job->resend)
		{
			job->log_gz = GzippedLog(); // here, off the game's threads (compilation 85)
			ProbeLog("Report: the session's log attached, %zu bytes gzipped\n", job->log_gz.size());
		}
		const std::string boundary = "ForjaSX2-report-boundary-7f3a";
		const std::string type = "multipart/form-data; boundary=" + boundary;
		int http = 0;
		std::string response;
		bool sent = PS4CoverHttp::Post(ChannelUrl() + "?wait=true", "ForjaSX2 reports", type, Body(*job, boundary, false), http, response);
		if (!sent && http == 400 && (response.find("220001") != std::string::npos || response.find("thread_name") != std::string::npos))
		{
			ProbeLog("Report: the channel is a forum, sending it as a new post\n");
			sent = PS4CoverHttp::Post(ChannelUrl() + "?wait=true", "ForjaSX2 reports", type, Body(*job, boundary, true), http, response);
		}
		ProbeLog("Report: %s %s%s (http %d)\n", job->name.c_str(), job->resend ? "sent again: " : "",
			sent ? "sent to the reports channel" : "not sent, kept in reportes/ to send again", http);
		if (sent)
			unlink(PendingPath(job->name).c_str()); // compilation 86: it arrived, nothing pending
		if (job->resend)
			s_resending.store(false, std::memory_order_release);
		else
		{
			s_send_started_ms.store(0, std::memory_order_relaxed);
			s_status.store(sent ? ForjaReport::Status::Sent : ForjaReport::Status::Kept, std::memory_order_release);
		}
		return nullptr;
	}

	// The report thread, detached, with the stack size the app's other threads use. False if it could not start.
	bool StartThread(Job* job)
	{
		pthread_t thread{};
		pthread_attr_t attr;
		int result = pthread_attr_init(&attr);
		if (result == 0)
		{
			pthread_attr_setstacksize(&attr, 256 * 1024);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			result = pthread_create(&thread, &attr, Run, job);
			pthread_attr_destroy(&attr);
		}
		if (result != 0)
			ProbeLog("Report: the thread could not start (error %d); %s kept in reportes/\n", result, job->name.c_str());
		return result == 0;
	}

	std::string ReadFile(const std::string& path)
	{
		std::string text;
		if (std::FILE* file = std::fopen(path.c_str(), "rb"))
		{
			char buffer[4096];
			size_t count;
			while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
				text.append(buffer, count);
			std::fclose(file);
		}
		return text;
	}
} // namespace

namespace ForjaReport
{
	bool HasChannel()
	{
		return !ChannelUrl().empty();
	}

	bool Send(const std::string& name, const std::string& summary, const std::string& text)
	{
		if (s_status.load(std::memory_order_acquire) == Status::Sending)
			return false;
		{
			std::lock_guard lock(s_mutex);
			s_last_name = name;
		}
		mkdir(Folder, 0777);
		const std::string path = std::string(Folder) + "/" + name + ".txt";
		if (std::FILE* file = std::fopen(path.c_str(), "wb"))
		{
			std::fwrite(text.data(), 1, text.size(), file);
			std::fclose(file);
		}
		else
			ProbeLog("Report: could not keep %s (errno %d)\n", path.c_str(), errno);
		if (!HasChannel())
		{
			ProbeLog("Report: %s kept in reportes/ (this build has no reports channel)\n", name.c_str());
			s_status.store(Status::Kept, std::memory_order_release);
			return true;
		}
		// Compilation 86: pending until the channel takes it; the marker holds the message's line, for ResendPending.
		if (std::FILE* file = std::fopen(PendingPath(name).c_str(), "wb"))
		{
			std::fwrite(summary.data(), 1, summary.size(), file);
			std::fclose(file);
		}
		s_status.store(Status::Sending, std::memory_order_release);
		s_send_started_ms.store(NowMs(), std::memory_order_relaxed);
		auto* job = new Job{name, summary, text};
		if (!StartThread(job))
		{
			delete job;
			s_send_started_ms.store(0, std::memory_order_relaxed);
			s_status.store(Status::Kept, std::memory_order_release);
		}
		return true;
	}

	int SendingSeconds()
	{
		const long long started = s_send_started_ms.load(std::memory_order_relaxed);
		return started > 0 ? static_cast<int>((NowMs() - started) / 1000) : 0;
	}

	void ResendPending()
	{
		if (!HasChannel() || s_status.load(std::memory_order_acquire) == Status::Sending || s_resending.load(std::memory_order_acquire))
			return;
		// The oldest pending one: the names start with the serial and end with the date, so the oldest by its file's time.
		std::string oldest;
		time_t oldest_time = 0;
		if (DIR* dir = opendir(Folder))
		{
			while (const dirent* entry = readdir(dir))
			{
				const std::string file = entry->d_name;
				const std::string suffix = ".pendiente";
				if (file.size() <= suffix.size() || file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
					continue;
				struct stat info{};
				const std::string path = std::string(Folder) + "/" + file;
				if (stat(path.c_str(), &info) == 0 && (oldest.empty() || info.st_mtime < oldest_time))
				{
					oldest = file.substr(0, file.size() - suffix.size());
					oldest_time = info.st_mtime;
				}
			}
			closedir(dir);
		}
		if (oldest.empty())
			return;
		const std::string text = ReadFile(std::string(Folder) + "/" + oldest + ".txt");
		if (text.empty())
		{
			unlink(PendingPath(oldest).c_str()); // its report is gone: nothing to send
			return;
		}
		const std::string summary = "(reenviado) " + ReadFile(PendingPath(oldest));
		auto* job = new Job{oldest, summary, text};
		job->resend = true;
		s_resending.store(true, std::memory_order_release);
		ProbeLog("Report: sending %s again (it didn't reach the channel before)\n", oldest.c_str());
		if (!StartThread(job))
		{
			delete job;
			s_resending.store(false, std::memory_order_release);
		}
	}

	Status GetStatus()
	{
		return s_status.load(std::memory_order_acquire);
	}

	std::string LastName()
	{
		std::lock_guard lock(s_mutex);
		return s_last_name;
	}
} // namespace ForjaReport
