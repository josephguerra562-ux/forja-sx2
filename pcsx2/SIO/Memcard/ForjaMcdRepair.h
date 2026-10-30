// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (compilation 97): repairs the ECC of a PS2 memory card image when it is opened. A page whose ECC doesn't match its
// data stops every save that touches its erase block: the PS2's mcman reads the block's pages before rewriting it and gives up
// on one it can't correct, and the game says "error saving". Joseph's card (2026-10-30) had 11.7 KB of noise over pages 288-310
// from a write cut short on 2026-10-18, right over the first free cluster: no game could save for 12 days. A free cluster's bad
// page is erased; a used cluster's page keeps its data and gets the right ECC. Only the allocatable clusters are looked at (page
// 1 holds PCSX2's checksum and has no ECC of its own, on purpose). No PCSX2 headers, so it can be tested on a PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ForjaMcdRepair
{
	constexpr size_t PageData = 512;
	constexpr size_t PageRaw = 528;
	constexpr uint32_t FatFree = 0x7fffffffu;

	struct Result
	{
		uint32_t erased = 0;    // pages of free clusters erased
		uint32_t ecc_fixed = 0; // pages of used clusters given the right ECC
		bool looked = false;    // a card this understands
	};

	// One ECC triple (3 bytes for each 128 bytes of a page), as the PS2 computes it.
	inline void Ecc(const uint8_t* data, uint8_t* out)
	{
		static uint8_t s_parity[256], s_column[256];
		static bool s_ready = false;
		if (!s_ready)
		{
			const auto parity = [](unsigned a) {
				a ^= a >> 1;
				a ^= a >> 2;
				a ^= a >> 4;
				return static_cast<uint8_t>(a & 1);
			};
			static constexpr uint8_t masks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
			for (unsigned b = 0; b < 256; b++)
			{
				s_parity[b] = parity(b);
				unsigned column = 0;
				for (unsigned i = 0; i < 7; i++)
					column |= static_cast<unsigned>(parity(b & masks[i])) << i;
				s_column[b] = static_cast<uint8_t>(column);
			}
			s_ready = true;
		}
		unsigned column = 0x77, line0 = 0x7f, line1 = 0x7f;
		for (unsigned i = 0; i < 128; i++)
		{
			const uint8_t b = data[i];
			column ^= s_column[b];
			if (s_parity[b])
			{
				line0 ^= ~i;
				line1 ^= i;
			}
		}
		out[0] = static_cast<uint8_t>(column);
		out[1] = static_cast<uint8_t>(line0 & 0x7f);
		out[2] = static_cast<uint8_t>(line1);
	}

	inline uint32_t ReadU32(const uint8_t* at)
	{
		uint32_t value;
		std::memcpy(&value, at, sizeof(value));
		return value;
	}

	// on_change(address, size) is called for each page changed in `image`.
	template <typename OnChange>
	Result Repair(uint8_t* image, size_t size, OnChange on_change)
	{
		Result result;
		static constexpr char magic[] = "Sony PS2 Memory Card Format ";
		if (size < PageRaw * 2 || std::memcmp(image, magic, sizeof(magic) - 1) != 0)
			return result;
		const auto u16_at = [image](size_t at) { return static_cast<uint32_t>(image[at] | (image[at + 1] << 8)); };
		const uint32_t clusters = ReadU32(image + 48), alloc_offset = ReadU32(image + 52), alloc_end = ReadU32(image + 56);
		const bool uses_ecc = (image[337] & 1) != 0;
		if (u16_at(40) != PageData || u16_at(42) != 2 || u16_at(44) != 16 || !uses_ecc || clusters == 0 ||
			size != static_cast<size_t>(clusters) * 2 * PageRaw || alloc_end > clusters || alloc_offset > clusters - alloc_end)
			return result;
		// A 32-bit word of an (absolute) cluster, false when it is outside the image.
		const auto cluster_word = [&](uint32_t cluster, uint32_t index, uint32_t& value) {
			const size_t byte = static_cast<size_t>(index) * 4;
			const size_t at = (static_cast<size_t>(cluster) * 2 + byte / PageData) * PageRaw + byte % PageData;
			if (cluster >= clusters || index >= 256 || at + 4 > size)
				return false;
			value = ReadU32(image + at);
			return true;
		};
		// The FAT: ifc list (superblock, offset 80) > indirect cluster > FAT cluster > entry.
		const auto fat_entry = [&](uint32_t index, uint32_t& value) {
			const uint32_t list = index / (256 * 256);
			if (list >= 32)
				return false;
			const uint32_t indirect = ReadU32(image + 80 + static_cast<size_t>(list) * 4);
			uint32_t table = 0;
			if (!cluster_word(indirect, (index / 256) % 256, table))
				return false;
			return cluster_word(table, index % 256, value);
		};
		uint8_t blank[PageRaw];
		std::memset(blank, 0xff, sizeof(blank));
		result.looked = true;
		for (uint32_t index = 0; index < alloc_end; index++)
		{
			uint32_t entry = 0;
			if (!fat_entry(index, entry))
			{
				result.looked = false;
				return result; // a FAT that can't be read: nothing more is touched
			}
			const size_t first_page = (static_cast<size_t>(alloc_offset) + index) * 2;
			for (size_t page = first_page; page < first_page + 2; page++)
			{
				uint8_t* const raw = image + page * PageRaw;
				if (std::memcmp(raw, blank, PageRaw) == 0)
					continue;
				uint8_t ecc[12];
				for (unsigned chunk = 0; chunk < 4; chunk++)
					Ecc(raw + static_cast<size_t>(chunk) * 128, ecc + static_cast<size_t>(chunk) * 3);
				if (std::memcmp(raw + PageData, ecc, sizeof(ecc)) == 0)
					continue;
				if (entry == FatFree)
				{
					std::memset(raw, 0xff, PageRaw);
					result.erased++;
				}
				else
				{
					std::memcpy(raw + PageData, ecc, sizeof(ecc));
					result.ecc_fixed++;
				}
				on_change(static_cast<uint32_t>(page * PageRaw), static_cast<uint32_t>(PageRaw));
			}
		}
		return result;
	}
} // namespace ForjaMcdRepair
