// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Bounded background sink for the standalone PS4 VM log.
#pragma once

#include <cstddef>

namespace PS4ProbeLogger
{
	// Call before producers start. The caller retains ownership of fd.
	int Start(int fd, bool synchronous, bool kernel_mirror);
	void Write(const char* text, std::size_t length);
	// Forja SX2 (compilation 67): each line written before Write returns (after the queue is drained), while a game starts
	// (main.cpp): a start that dies keeps its last line. Off again at the game's first heartbeat.
	void SetDirect(bool direct);
	void LogStatus();
	// Call after producers stop; drains the queue before detaching fd.
	void Stop();
} // namespace PS4ProbeLogger
