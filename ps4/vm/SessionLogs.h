// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Preserve the last game of each platform across launcher relaunches.
#pragma once

#include "GamePlatform.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace PS4SessionLogs
{
	constexpr const char* MARKER = "/data/ForjaSX2/logs/session-platform.txt";
	// Forja SX2: every game session's logs, newest last, so one test session does not overwrite the one before it
	// (2026-10-16: a God of War II session was lost that way). Folders are "<date>_<serial>"; the oldest go past 10.
	constexpr const char* HISTORY = "/data/ForjaSX2/logs/sesiones";
	constexpr int HISTORY_KEEP = 10;
	struct ArchiveResult
	{
		const char* directory = "previous-menu";
		int moved = 0;
		int error = 0;
		int history = -1; // files copied into logs/sesiones, -1 = not a game session
	};

	inline bool CopyFile(const char* source, const char* target)
	{
		FILE* in = std::fopen(source, "rb");
		if (!in)
			return false;
		FILE* out = std::fopen(target, "wb");
		if (!out)
		{
			std::fclose(in);
			return false;
		}
		std::vector<char> buffer(64 * 1024);
		bool ok = true;
		size_t got;
		while ((got = std::fread(buffer.data(), 1, buffer.size(), in)) > 0)
		{
			if (std::fwrite(buffer.data(), 1, got, out) != got)
			{
				ok = false;
				break;
			}
		}
		std::fclose(in);
		return std::fclose(out) == 0 && ok;
	}

	inline void RemoveDirectory(const std::string& path)
	{
		if (DIR* dir = opendir(path.c_str()))
		{
			while (dirent* entry = readdir(dir))
			{
				if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
					unlink((path + "/" + entry->d_name).c_str());
			}
			closedir(dir);
		}
		rmdir(path.c_str());
	}

	// Copies the archived session (and the sampler's samples.log, when the sampler ran in it) into a new history folder
	// named after the session's last log write (UTC), then drops the oldest folders beyond HISTORY_KEEP.
	inline int KeepHistory(const char* archived, const char* label)
	{
		mkdir(HISTORY, 0777);
		char stamp[32] = "sin-fecha";
		struct stat info{};
		const std::string boot_log = std::string(archived) + "/PCSX2-vm-boot.log";
		const bool dated = stat(boot_log.c_str(), &info) == 0;
		if (dated)
		{
			const std::time_t when = info.st_mtime;
			if (const std::tm* tm = std::gmtime(&when))
				std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
		}
		const std::string target = std::string(HISTORY) + "/" + stamp + "_" + label;
		if (mkdir(target.c_str(), 0777) != 0 && errno != EEXIST)
			return -1;
		int copied = 0;
		constexpr const char* names[] = {
			"PCSX2-vm-boot.log", "PCSX2-vm-stdout.log", "PCSX2-vm-stderr.log", "emulog.txt"};
		for (const char* name : names)
			copied += CopyFile((std::string(archived) + "/" + name).c_str(), (target + "/" + name).c_str()) ? 1 : 0;
		// The sampler rewrites samples.log every 10 s while it runs; an older one is from another session.
		struct stat samples{};
		if (dated && stat("/data/ForjaSX2/samples.log", &samples) == 0 && samples.st_mtime + 120 >= info.st_mtime)
			copied += CopyFile("/data/ForjaSX2/samples.log", (target + "/samples.log").c_str()) ? 1 : 0;

		std::vector<std::string> sessions;
		if (DIR* dir = opendir(HISTORY))
		{
			while (dirent* entry = readdir(dir))
			{
				if (entry->d_name[0] != '.')
					sessions.emplace_back(entry->d_name);
			}
			closedir(dir);
		}
		std::sort(sessions.begin(), sessions.end()); // the date leads the name, so this is oldest first
		for (size_t i = 0; i + HISTORY_KEEP < sessions.size(); i++)
			RemoveDirectory(std::string(HISTORY) + "/" + sessions[i]);
		return copied;
	}

	inline ArchiveResult ArchivePrevious()
	{
		ArchiveResult result;
		char marker[64]{};
		char platform[16]{};
		char serial[32]{};
		if (FILE* file = std::fopen(MARKER, "rb"))
		{
			std::fread(marker, 1, sizeof(marker) - 1, file);
			std::fclose(file);
			// "PS2" or, since compilation 31, "PS2 SCES-54206".
			std::sscanf(marker, "%15s %31s", platform, serial);
			if (std::strcmp(platform, "PS1") == 0)
				result.directory = "last-game-PS1";
			else if (std::strcmp(platform, "PS2") == 0)
				result.directory = "last-game-PS2";
			else
				result.directory = "last-game-other";
		}
		char directory[128];
		std::snprintf(directory, sizeof(directory), "/data/ForjaSX2/logs/%s", result.directory);
		if (mkdir(directory, 0777) != 0 && errno != EEXIST)
		{
			result.error = errno;
			return result;
		}
		// Only these app-owned files rotate. User crash reports and other folders
		// are untouched. Keep one previous game per platform, rather than unbounded logs.
		constexpr const char* names[] = {
			"PCSX2-vm-boot.log", "PCSX2-vm-stdout.log", "PCSX2-vm-stderr.log", "emulog.txt"};
		for (const char* name : names)
		{
			char source[160], target[160];
			std::snprintf(source, sizeof(source), "/data/ForjaSX2/logs/%s", name);
			std::snprintf(target, sizeof(target), "%s/%s", directory, name);
			if (rename(source, target) == 0)
				++result.moved;
			else if (errno == ENOENT)
				unlink(target); // Do not mix an old file into the new archived session.
			else
				result.error = errno;
		}
		if (platform[0] != 0 && result.moved > 0)
			result.history = KeepHistory(directory, serial[0] ? serial : platform);
		if (!result.error)
			unlink(MARKER);
		return result;
	}
	inline bool MarkGame(GamePlatform platform, const std::string& serial)
	{
		char value[64];
		std::snprintf(value, sizeof(value), "%s%s%s", platform == GamePlatform::PS1 ? "PS1" :
			platform == GamePlatform::PS2 ? "PS2" : "Other", serial.empty() ? "" : " ", serial.c_str());
		const int fd = open(MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			return false;
		const auto size = std::strlen(value);
		const bool written = write(fd, value, size) == static_cast<ssize_t>(size);
		const bool synced = fsync(fd) == 0;
		close(fd);
		return written && synced;
	}
}
