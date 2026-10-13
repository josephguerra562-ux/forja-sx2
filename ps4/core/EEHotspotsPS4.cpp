// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) All collection, consumption and reset occur on the EE owner.
#include "EEHotspotsPS4.h"
#include "pcsx2/vtlb.h"

#include <algorithm>
#include <array>
#include <cstring>

extern void ProbeLog(const char*, ...);

namespace PS4EEHotspots
{
	bool enabled = false;
	bool block_sampling_enabled = false;
	u32 block_sample_countdown = 0xffffffffU;
	namespace
	{
		struct Entry
		{
			u32 pc = 0;
			u32 count = 0;
		};
		struct Histogram
		{
			std::array<Entry, 4096> entries{};
			u32 samples = 0;
			u32 dropped = 0;
			void Add(u32 pc)
			{
				++samples;
				const u32 hash = ((pc >> 2) * 2654435761U) >> 20;
				for (u32 i = 0; i < 32; ++i)
				{
					auto& entry = entries[(hash + i) & 4095];
					if (entry.count == 0 || entry.pc == pc)
					{
						entry.pc = pc;
						++entry.count;
						return;
					}
				}
				++dropped;
			}
		};
		struct BlockMetadata
		{
			u32 pc = 0;
			u16 guest_words = 0;
			u16 host_bytes = 0;
		};
		struct BlockEntry
		{
			u32 pc = 0;
			u32 hits = 0;
			u64 guest_words = 0;
			u64 host_bytes = 0;
		};
		struct BlockHistogram
		{
			std::array<BlockEntry, 4096> entries{};
			u32 samples = 0;
			u32 dropped = 0;
			void Add(u32 pc, u32 guest_words, u32 host_bytes)
			{
				++samples;
				const u32 hash = ((pc >> 2) * 2654435761U) >> 20;
				for (u32 i = 0; i < 32; ++i)
				{
					auto& entry = entries[(hash + i) & 4095];
					if (entry.hits == 0 || entry.pc == pc)
					{
						entry.pc = pc;
						++entry.hits;
						entry.guest_words += guest_words;
						entry.host_bytes += host_bytes;
						return;
					}
				}
				++dropped;
			}
		};
		Histogram s_events, s_ipu, s_blocks;
		BlockHistogram s_weighted_blocks;
		std::array<BlockMetadata, 65536> s_block_metadata{};
		u64 s_access[2][2][64]{}; // read/write, 32/64 bits, register offset / 4.
		u64 s_workers[16]{};
		u32 s_pending[32]{};
		u32 s_event_random = 0x4a39b70d;
		u32 s_ipu_random = 0x89abcdef;
		u32 s_block_random = 0x618b7a35;

		bool Select(u32& random, u32 mask)
		{
			random ^= random << 13;
			random ^= random >> 17;
			random ^= random << 5;
			return (random & mask) == 0;
		}

		void LogHistogram(const char* kind, Histogram& histogram)
		{
			ProbeLog("EE hotspots: kind=%s samples=%u dropped=%u metric=occurrences_not_CPU_time\n",
				kind, histogram.samples, histogram.dropped);
			std::array<Entry, 8> top{};
			for (const auto& entry : histogram.entries)
			{
				if (entry.count <= top.back().count)
					continue;
				top.back() = entry;
				std::sort(top.begin(), top.end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });
			}
			for (const auto& entry : top)
			{
				if (!entry.count)
					break;
				ProbeLog("EE hotspot: kind=%s pc=%08x hits=%u sample_share_pct=%.2f\n", kind, entry.pc,
					entry.count, 100.0 * entry.count / histogram.samples);
				// Read the mapped RAM/ROM directly; never invoke a device handler for
				// diagnostics. Stay within one VTLB page, even at its boundary.
				// The reported PC is an event/call boundary, not an instruction-time sample.
				const u32 first = (entry.pc & ~15U) - ((entry.pc & 0xfff) >= 16 ? 16 : 0);
				const auto mapping = vtlb_private::vtlbdata.vmap[first >> vtlb_private::VTLB_PAGE_BITS];
				if (!mapping.isHandler(first))
				{
					u32 words[8]{};
					const u32 count = std::min(8U, (4096 - (first & 4095)) / 4);
					std::memcpy(words, reinterpret_cast<const void*>(mapping.assumePtr(first)), count * 4);
					ProbeLog("EE hotspot context: start=%08x words=%u data=%08x %08x %08x %08x %08x %08x %08x %08x\n",
						first, count, words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
				}
			}
			histogram = {};
		}

		BlockMetadata FindBlockMetadata(u32 pc)
		{
			const u32 hash = ((pc >> 2) * 2654435761U) >> 16;
			for (u32 i = 0; i < 128; ++i)
			{
				const auto& metadata = s_block_metadata[(hash + i) & 65535];
				if (metadata.guest_words == 0 || metadata.pc == pc)
					return metadata;
			}
			return {};
		}

		void LogWeightedBlocks()
		{
			ProbeLog("EE block work: samples=%u dropped=%u metric=sampled_guest_words_not_CPU_time\n",
				s_weighted_blocks.samples, s_weighted_blocks.dropped);
			std::array<BlockEntry, 12> top{};
			for (const auto& entry : s_weighted_blocks.entries)
			{
				if (entry.guest_words <= top.back().guest_words)
					continue;
				top.back() = entry;
				std::sort(top.begin(), top.end(), [](const BlockEntry& a, const BlockEntry& b) {
					return a.guest_words > b.guest_words;
				});
			}
			for (const auto& entry : top)
			{
				if (!entry.hits)
					break;
				const u32 words = static_cast<u32>(entry.guest_words / entry.hits);
				const u32 host_bytes = static_cast<u32>(entry.host_bytes / entry.hits);
				ProbeLog("EE block work item: pc=%08x hits=%u guest_words=%u host_bytes=%u sampled_guest_words=%llu\n",
					entry.pc, entry.hits, words, host_bytes, static_cast<unsigned long long>(entry.guest_words));
				const auto mapping = vtlb_private::vtlbdata.vmap[entry.pc >> vtlb_private::VTLB_PAGE_BITS];
				if (!mapping.isHandler(entry.pc))
				{
					u32 opcodes[32]{};
					const u32 count = std::min({words, 32U, (4096 - (entry.pc & 4095)) / 4});
					std::memcpy(opcodes, reinterpret_cast<const void*>(mapping.assumePtr(entry.pc)), count * 4);
					for (u32 offset = 0; offset < count; offset += 8)
					{
						const u32 remaining = std::min(8U, count - offset);
						ProbeLog("EE block opcodes: pc=%08x offset=%u words=%u data=%08x %08x %08x %08x %08x %08x %08x %08x\n",
							entry.pc, offset, remaining, opcodes[offset], opcodes[offset + 1], opcodes[offset + 2],
							opcodes[offset + 3], opcodes[offset + 4], opcodes[offset + 5], opcodes[offset + 6],
							opcodes[offset + 7]);
					}
				}
			}
			s_weighted_blocks = {};
		}
	} // namespace

	void Reset(bool enable)
	{
		enabled = enable;
		s_events = {};
		s_ipu = {};
		std::memset(s_access, 0, sizeof(s_access));
		std::memset(s_workers, 0, sizeof(s_workers));
		std::memset(s_pending, 0, sizeof(s_pending));
		s_event_random = 0x4a39b70d;
		s_ipu_random = 0x89abcdef;
	}

	void ResetBlockSampling(bool enable)
	{
		block_sampling_enabled = enable;
		s_blocks = {};
		s_weighted_blocks = {};
		s_block_metadata = {};
		s_block_random = 0x618b7a35;
		block_sample_countdown = enable ? 2048U : 0xffffffffU;
	}

	void RegisterBlock(u32 pc, u32 guest_words, u32 host_bytes)
	{
		if (!block_sampling_enabled || guest_words == 0)
			return;
		const u32 hash = ((pc >> 2) * 2654435761U) >> 16;
		for (u32 i = 0; i < 128; ++i)
		{
			auto& metadata = s_block_metadata[(hash + i) & 65535];
			if (metadata.guest_words == 0 || metadata.pc == pc)
			{
				metadata.pc = pc;
				metadata.guest_words = static_cast<u16>(std::min(guest_words, 65535U));
				metadata.host_bytes = static_cast<u16>(std::min(host_bytes, 65535U));
				return;
			}
		}
	}

	void RecordBlock(u32 pc)
	{
		if (block_sampling_enabled)
		{
			s_blocks.Add(pc);
			const BlockMetadata metadata = FindBlockMetadata(pc);
			s_weighted_blocks.Add(pc, metadata.guest_words, metadata.host_bytes);
		}
		// Vary the interval to avoid aliasing with a repeating guest loop. This is
		// an execution-frequency sample, so it still needs proper testing before
		// being used to justify a permanent optimization.
		s_block_random ^= s_block_random << 13;
		s_block_random ^= s_block_random >> 17;
		s_block_random ^= s_block_random << 5;
		block_sample_countdown = block_sampling_enabled ? 2048U + (s_block_random & 2047U) : 0xffffffffU;
	}

	void SampleEvent(u32 pc, u32 pending)
	{
		if (!enabled || !Select(s_event_random, 255))
			return;
		s_events.Add(pc);
		for (unsigned bit = 0; bit < 32; ++bit)
			s_pending[bit] += (pending >> bit) & 1;
	}

	void RecordIPU(u32 pc, u32 address, bool write, bool wide)
	{
		if (!enabled)
			return;
		++s_access[write][wide][(address & 255) >> 2];
		if (!write && Select(s_ipu_random, 1023))
			s_ipu.Add(pc);
	}

	void RecordWorker(u32 command)
	{
		if (enabled)
			++s_workers[command & 15];
	}

	void LogInterval(double seconds)
	{
		if (seconds <= 0)
			return;
		if (block_sampling_enabled)
		{
			LogHistogram("JIT_block_entry_random_1of2048to4095", s_blocks);
			LogWeightedBlocks();
		}
		if (!enabled)
			return;
		LogHistogram("event_entry_1of256", s_events);
		LogHistogram("IPU_read_boundary_1of1024", s_ipu);
		for (unsigned bit = 0; bit < 32; ++bit)
		{
			if (s_pending[bit])
				ProbeLog("EE pending: bit=%u sampled_events=%u\n", bit, s_pending[bit]);
			s_pending[bit] = 0;
		}
		for (unsigned write = 0; write < 2; ++write)
			for (unsigned wide = 0; wide < 2; ++wide)
				for (unsigned reg = 0; reg < 64; ++reg)
				{
					auto& count = s_access[write][wide][reg];
					if (count)
						ProbeLog("IPU access: write=%u bits=%u addr=%08x calls=%llu calls_per_second=%.0f\n",
							write, wide ? 64 : 32, 0x10002000 + reg * 4, static_cast<unsigned long long>(count), count / seconds);
					count = 0;
				}
		for (unsigned command = 0; command < 16; ++command)
		{
			if (s_workers[command])
				ProbeLog("IPU worker calls: command=%u calls=%llu calls_per_second=%.0f\n", command,
					static_cast<unsigned long long>(s_workers[command]), s_workers[command] / seconds);
			s_workers[command] = 0;
		}
	}
} // namespace PS4EEHotspots
