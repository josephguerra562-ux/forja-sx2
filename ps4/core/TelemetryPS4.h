// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Live telemetry client: PS4 -> PC debug server over TCP.
// Streams logs, heartbeats and execution time to a PC listener.

#pragma once

#include <cstddef>
#include <cstdint>

namespace PS4Telemetry
{
	// Reads /data/PS4SX2/telemetry.conf (host=IP port=NUM) and starts background thread.
	// Returns true if thread started (even if not yet connected). Never blocks EE thread.
	bool Start();

	// Signals thread to stop, drains queue, closes socket. Call before ProbeLogger::Stop.
	void Stop();

	// Enqueue a log line (called from ProbeLogger::WriteSink). Non-blocking, drops if full.
	// Requires Start() to have been called; otherwise no-op.
	void Write(const char* text, std::size_t length);

	// Heartbeat with execution time and VM state. Non-blocking.
	void Heartbeat(double boot_seconds, double vm_seconds, std::uint32_t ee_pc, std::uint32_t iop_pc,
		double ee_mcycles, double polls_per_s, double clock_checks_per_s,
		int vm_state, bool menu_open, int vsync_q, const char* game_file);

	// Lifecycle / stage events.
	void Stage(const char* stage);
	void GameSelect(const char* filename, const char* platform);

	// Returns true if currently connected to PC server.
	bool IsConnected();

	// Returns seconds since Start() (boot execution time).
	double GetBootSeconds();
} // namespace PS4Telemetry
