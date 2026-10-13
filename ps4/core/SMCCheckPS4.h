// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Exact block integrity checks, grouped without skipping guest bytes.
#pragma once

#include "common/emitter/x86emitter.h"
#include <cstring>

namespace PS4SMCCheck
{
	// Called before block register allocation: rax/rcx/xmm0/xmm1 are scratch.
	// SysV discard arguments in rdi/rsi must survive a mismatch.
	inline void Emit(const u8* source, u32 bytes, const void* mismatch)
	{
		using namespace x86Emitter;
		const u32 vector_bytes = bytes >= 32 ? (bytes & ~15U) : 0;
		const u8* snapshot = nullptr;
		if (vector_bytes)
		{
			// Each block owns an immutable snapshot in its code allocation. Sharing
			// recRAMCopy would let a later compilation hide changes from older blocks.
			xForwardJump32 skip_snapshot;
			xAlignPtr(16);
			snapshot = xGetPtr();
			for (u32 i = 0; i < vector_bytes; ++i)
				xWrite8(source[i]);
			skip_snapshot.SetTarget();
		}

		xMOV64(rax, reinterpret_cast<uptr>(source));
		u32 offset = 0;
		while (offset < vector_bytes)
		{
			xMOVDQU(xmm0, ptr128[rax + offset]);
			xPCMP.EQD(xmm0, ptr128[snapshot + offset]);
			offset += 16;
			if (offset < vector_bytes)
			{
				xMOVDQU(xmm1, ptr128[rax + offset]);
				xPCMP.EQD(xmm1, ptr128[snapshot + offset]);
				xPAND(xmm0, xmm1);
				offset += 16;
			}
			xPMOVMSKB(ecx, xmm0);
			xCMP(ecx, 0xffff);
			xJNE(mismatch);
		}
		// Exact 4-byte tail: no vector load may extend past the original block.
		for (; offset < bytes; offset += 4)
		{
			u32 word;
			std::memcpy(&word, source + offset, sizeof(word));
			xCMP(ptr32[rax + offset], word);
			xJNE(mismatch);
		}
	}
} // namespace PS4SMCCheck
