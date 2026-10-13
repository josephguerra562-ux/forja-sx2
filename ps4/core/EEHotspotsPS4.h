// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Counts and event-boundary samples, not a CPU-time profiler.
#pragma once

#include "common/Pcsx2Types.h"

namespace PS4EEHotspots
{
	extern bool enabled; // Configured and used only by the EE thread.
	extern bool block_sampling_enabled;
	extern u32 block_sample_countdown;
	void Reset(bool enable);
	void ResetBlockSampling(bool enable);
	void RegisterBlock(u32 pc, u32 guest_words, u32 host_bytes);
	void RecordBlock(u32 pc);
	void SampleEvent(u32 pc, u32 pending);
	void RecordIPU(u32 pc, u32 address, bool write, bool wide);
	void RecordWorker(u32 command);
	void LogInterval(double seconds);
} // namespace PS4EEHotspots
