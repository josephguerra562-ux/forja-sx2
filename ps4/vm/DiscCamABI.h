/*-
 * Data structures and definitions for CAM Control Blocks (CCBs).
 *
 * Copyright (c) 1997, 1998 Justin T. Gibbs.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: releng/9.3/sys/cam/cam_ccb.h 256216 2013-10-09 18:58:28Z mav $
 */
// (AI-assisted) Minimal FreeBSD 9.3/PS4 CAM 0x16 ABI, not the PS5 CAM ABI.
// Layout checked against ps4-payload-sdk's original cam_ccb.h with Clang's
// x86_64-freebsd9.3 target. Keep the complete ioctl buffer, including reserved bytes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace PS4Disc::CAM
{
	constexpr unsigned long Command = 0xC4A81602UL;
	constexpr unsigned long GetPassthrough = 0xC4A81603UL;
	struct alignas(8) CCB
	{
		uint8_t bytes[0x4a8]{};
		template <typename T>
		T Get(size_t offset) const
		{
			T value;
			std::memcpy(&value, bytes + offset, sizeof(value));
			return value;
		}
		template <typename T>
		void Set(size_t offset, T value)
		{
			std::memcpy(bytes + offset, &value, sizeof(value));
		}
	};
	static_assert(sizeof(CCB) == 0x4a8 && sizeof(void*) == 8);
	constexpr size_t Retry = 0x40, Function = 0x50, Status = 0x54;
	constexpr size_t Path = 0x60, Target = 0x64, Lun = 0x68, Flags = 0x6c, Timeout = 0x90;
	constexpr size_t PeriphName = 0xa0, Unit = 0xb0, DeviceStatus = 0xbc;
	constexpr size_t Data = 0xb0, DataSize = 0xb8, Sense = 0xbc;
	constexpr size_t SenseSize = 0x1b8, CdbSize = 0x1b9, ScsiStatus = 0x1bc;
	constexpr size_t Residual = 0x1c0, Cdb = 0x1c8, Tag = 0x1e2;
} // namespace PS4Disc::CAM
