// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Forja SX2 (compilation 77): the player's reports (ForjaReport.h). Sent as a Discord webhook message with the
// report attached (multipart/form-data: payload_json and files[0]), ?wait=true so the answer says it arrived. Mentions are
// turned off in the message, so a game's name can't ping the server. One thread at a time, detached, with the stack size the
// app's other threads use; nothing waits for it.
#include "ForjaReport.h"
#include "CoverDownload.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>

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
	};

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
		body += "\r\n--" + boundary + "--\r\n";
		return body;
	}

	void* Run(void* argument)
	{
		pthread_setname_np(pthread_self(), "Forja report");
		const std::unique_ptr<Job> job(static_cast<Job*>(argument));
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
		ProbeLog("Report: %s %s (http %d)\n", job->name.c_str(), sent ? "sent to the reports channel" : "not sent, kept in reportes/", http);
		s_status.store(sent ? ForjaReport::Status::Sent : ForjaReport::Status::Kept, std::memory_order_release);
		return nullptr;
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
		s_status.store(Status::Sending, std::memory_order_release);
		auto* job = new Job{name, summary, text};
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
		{
			delete job;
			ProbeLog("Report: the thread could not start (error %d); %s kept in reportes/\n", result, name.c_str());
			s_status.store(Status::Kept, std::memory_order_release);
		}
		return true;
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
