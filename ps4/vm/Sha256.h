// SPDX-License-Identifier: GPL-3.0+
// Forja SX2: SHA-256 (FIPS 180-4), to check a downloaded package against the release's SHA256SUMS.txt.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class Sha256
{
public:
	Sha256() { Reset(); }

	void Reset()
	{
		static constexpr std::uint32_t init[8] = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		std::memcpy(m_state, init, sizeof(m_state));
		m_bytes = 0;
		m_used = 0;
	}

	void Update(const void* data, std::size_t size)
	{
		const auto* p = static_cast<const unsigned char*>(data);
		m_bytes += size;
		while (size > 0)
		{
			const std::size_t take = (size < 64 - m_used) ? size : 64 - m_used;
			std::memcpy(m_block + m_used, p, take);
			m_used += take;
			p += take;
			size -= take;
			if (m_used == 64)
			{
				Compress(m_block);
				m_used = 0;
			}
		}
	}

	// The digest as 64 lowercase hex digits, as sha256sum prints it. The object needs Reset() before reuse.
	std::string FinishHex()
	{
		const std::uint64_t bits = m_bytes * 8;
		const unsigned char pad = 0x80;
		Update(&pad, 1);
		const unsigned char zero = 0;
		while (m_used != 56)
			Update(&zero, 1);
		unsigned char length[8];
		for (int i = 0; i < 8; i++)
			length[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
		Update(length, 8);
		char hex[65];
		for (int i = 0; i < 8; i++)
			std::snprintf(hex + i * 8, 9, "%08x", static_cast<unsigned>(m_state[i]));
		return std::string(hex, 64);
	}

private:
	static std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void Compress(const unsigned char* block)
	{
		static constexpr std::uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		std::uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
			       (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
		for (int i = 16; i < 64; i++)
		{
			const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
		std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
		for (int i = 0; i < 64; i++)
		{
			const std::uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
			const std::uint32_t ch = (e & f) ^ (~e & g);
			const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
			const std::uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
			const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t t2 = S0 + maj;
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		m_state[0] += a;
		m_state[1] += b;
		m_state[2] += c;
		m_state[3] += d;
		m_state[4] += e;
		m_state[5] += f;
		m_state[6] += g;
		m_state[7] += h;
	}

	std::uint32_t m_state[8];
	unsigned char m_block[64];
	std::uint64_t m_bytes;
	std::size_t m_used;
};
