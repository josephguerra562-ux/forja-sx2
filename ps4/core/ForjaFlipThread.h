// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (154 in the app): "Flip en su propio hilo". The GPU-side flip (VulkanVideoOut::PresentFromGpu -> ac_ps4_submit_flip, in
// the Mesa of our SDK) calls sceGnmSubmitAndFlipCommandBuffers on the calling thread, and that call only returns once the GPU has
// reached the flip: the whole frame's GPU time. God of War II at 2x (2026-11-20, the 153's sampler and its own binary): the GS
// thread spent ~30 % of its time there (call qword ptr [r12+0x110] in ac_ps4_submit_flip), ~10 ms a frame, and the GS was the
// limit (~34 frames a second, never idle). With this on, the GS thread queues the flip to a thread of its own and goes on with the
// next frame; the flip itself goes out as soon as before (no frame of delay, unlike the deferred CPU flip of vm-cpu-flip, which
// left the menu one press behind).
//
// wanted: set by ps4/vm/main.cpp at each game's start ([Forja] FlipEnHilo, vm-flip-hilo, vm-flip-sin-hilo).
// The rest: the meter, printed every 5 s by ps4/vm/HostVM.cpp ("GS flip (154 in the app)"): the time inside the flip call,
// whichever thread made it, and the GS thread's waits for room in the queue.
#pragma once

#include <atomic>
#include <cstdint>

namespace ForjaFlipThread
{
	inline std::atomic<int> wanted{0};
	inline std::atomic<std::uint64_t> flips{0}, ns{0}, max_ns{0}, threaded{0};
	inline std::atomic<std::uint64_t> room_waits{0}, room_wait_ns{0};
	inline std::atomic<std::uint32_t> queue_max{0};

	inline void Record(std::uint64_t elapsed_ns, bool in_own_thread)
	{
		flips.fetch_add(1, std::memory_order_relaxed);
		ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
		if (in_own_thread)
			threaded.fetch_add(1, std::memory_order_relaxed);
		std::uint64_t old = max_ns.load(std::memory_order_relaxed);
		while (elapsed_ns > old && !max_ns.compare_exchange_weak(old, elapsed_ns, std::memory_order_relaxed))
		{
		}
	}
} // namespace ForjaFlipThread
