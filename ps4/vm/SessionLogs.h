// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Preserve the last game of each platform across launcher relaunches.
#pragma once

#include "GamePlatform.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PS4SessionLogs
{
	constexpr const char* MARKER = "/data/PS4SX2/logs/session-platform.txt";
	struct ArchiveResult
	{
		const char* directory = "previous-menu";
		int moved = 0;
		int error = 0;
	};
	inline ArchiveResult ArchivePrevious()
	{
		ArchiveResult result;
		char platform[16]{};
		if (FILE* file = std::fopen(MARKER, "rb"))
		{
			std::fread(platform, 1, sizeof(platform) - 1, file);
			std::fclose(file);
			if (std::strcmp(platform, "PS1") == 0)
				result.directory = "last-game-PS1";
			else if (std::strcmp(platform, "PS2") == 0)
				result.directory = "last-game-PS2";
			else
				result.directory = "last-game-other";
		}
		char directory[128];
		std::snprintf(directory, sizeof(directory), "/data/PS4SX2/logs/%s", result.directory);
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
			std::snprintf(source, sizeof(source), "/data/PS4SX2/logs/%s", name);
			std::snprintf(target, sizeof(target), "%s/%s", directory, name);
			if (rename(source, target) == 0)
				++result.moved;
			else if (errno == ENOENT)
				unlink(target); // Do not mix an old file into the new archived session.
			else
				result.error = errno;
		}
		if (!result.error)
			unlink(MARKER);
		return result;
	}
	inline bool MarkGame(GamePlatform platform)
	{
		const char* value = platform == GamePlatform::PS1 ? "PS1" :
			platform == GamePlatform::PS2 ? "PS2" : "Other";
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
