// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (compilation 116, AI-assisted): "Grabar dibujos en otro hilo", the No tocar row ([Forja] GrabarEnOtroHilo).
// The GS thread decides each hardware draw as before (pipeline, uniforms, descriptors, dirty flags); the Vulkan calls that
// write it into the command buffer (pipeline, index buffer, dynamic state, the UBO set, the pushed textures, push
// constants, feedback barriers and the draw) go into a queue instead, and this file's thread makes them on the same command
// buffer, in the same order. Everything else that writes the command buffer takes it through
// GSDeviceVK::GetCurrentCommandBuffer() or GetCurrentInitCommandBuffer(), and a submission drains first: both wait until the
// queue is empty, so the command stream is the one the GS thread alone would have written.
// Why (informes/PASO0_ENVIO_A_LA_GRAFICA_2026-11-04.md, the 115 with vm-profile-gs): vk_bind + vk_send were 9 % of the GS
// thread in Dragon Ball BT4, 16 % in Most Wanted, 11 % in Guitar Hero III at 2x and 27 % in Shadow of the Colossus at 2x; the
// submission itself (vkQueueSubmit) under 1 %. How much of that is the driver, and so moves here, needs proper testing on the
// console: the "GS recorder:" line says what the queue cost and what this thread did.
// Off by default. Only GSDeviceVK.cpp includes this file.
#pragma once

#if defined(PCSX2_PS4_VM_BOOT)

#include "common/Threading.h"
#include "common/Timer.h"

#include "ps4/core/ThreadAffinityPS4.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

#include <immintrin.h>
#include <pthread.h>

extern void ProbeLog(const char* format, ...);

// What the player chose for the running game (main.cpp: the row, and ApplyGameProfile at the game's start). The GS thread
// takes it at the next command buffer, when the queue is empty (ForjaVkRec::FrameBoundary).
std::atomic<bool> g_forja_draw_thread_wanted{false};

namespace ForjaVkRec
{
	// GS thread: the draws' commands go to the queue now. Only FrameBoundary changes it, with the queue empty.
	bool g_on = false;

	enum Op : u32
	{
		OP_WRAP, // the rest of the ring is unused: go on at its start
		OP_BIND_PIPELINE,
		OP_BIND_INDEX_BUFFER,
		OP_SET_VIEWPORT,
		OP_SET_SCISSOR,
		OP_SET_BLEND_CONSTANTS,
		OP_SET_LINE_WIDTH,
		OP_BIND_DESCRIPTOR_SETS,
		OP_PUSH_CONSTANTS,
		OP_PUSH_DESCRIPTOR_SET,
		OP_IMAGE_BARRIER,
		OP_DRAW,
		OP_DRAW_INDEXED,
	};

	// Every record starts with this and is a multiple of 16 bytes, so a wrap header always fits before the ring's end.
	struct Header
	{
		u32 op;
		u32 size;
		VkCommandBuffer cmd;
	};
	static_assert(sizeof(Header) == 16);

	struct RBindPipeline { Header h; VkPipelineBindPoint bind_point; u32 pad; VkPipeline pipeline; };
	struct RBindIndexBuffer { Header h; VkBuffer buffer; VkDeviceSize offset; VkIndexType type; u32 pad; };
	struct RViewport { Header h; VkViewport viewport; };
	struct RScissor { Header h; VkRect2D rect; };
	struct RBlendConstants { Header h; float color[4]; };
	struct RLineWidth { Header h; float width; u32 pad; };
	static constexpr u32 MAX_DYNAMIC_OFFSETS = 4;
	struct RBindDescriptorSets
	{
		Header h;
		VkPipelineBindPoint bind_point;
		u32 first_set;
		VkPipelineLayout layout;
		VkDescriptorSet set;
		u32 dynamic_count;
		u32 dynamic_offsets[MAX_DYNAMIC_OFFSETS];
	};
	struct RPushConstants { Header h; VkPipelineLayout layout; VkShaderStageFlags stages; u32 offset; u32 size; u32 pad; }; // + data
	// + VkWriteDescriptorSet[write_count], their VkDescriptorImageInfo, their VkDescriptorBufferInfo
	struct RPushDescriptorSet { Header h; VkPipelineBindPoint bind_point; u32 set; VkPipelineLayout layout; u32 write_count; u32 pad; };
	struct RImageBarrier
	{
		Header h;
		VkPipelineStageFlags src_stages, dst_stages;
		VkDependencyFlags dependency_flags;
		u32 pad;
		VkImageMemoryBarrier barrier;
	};
	struct RDraw { Header h; u32 vertex_count, instance_count, first_vertex, first_instance; };
	struct RDrawIndexed { Header h; u32 index_count, instance_count, first_index; s32 vertex_offset; u32 first_instance, pad; };

	// A Dragon Ball BT4 frame is ~2,150 draws of ~10 records (~250 bytes a draw): the ring holds about two, and every render
	// pass change, upload or readback empties it anyway.
	static constexpr u32 RING_BYTES = 1u << 20;

	static std::unique_ptr<u8[]> s_ring;
	// Byte counters that only grow (a 64-bit counter does not wrap). s_head and s_tail_seen are the GS thread's, s_tail the
	// worker's; the published copies are how each one sees the other.
	static u64 s_head = 0, s_tail_seen = 0, s_tail = 0;
	alignas(64) static std::atomic<u64> s_head_published{0};
	alignas(64) static std::atomic<u64> s_tail_published{0};
	static Threading::WorkSema s_sema;
	static std::atomic<bool> s_quit{false};
	static pthread_t s_thread;
	static bool s_thread_started = false;
	static bool s_start_failed = false;
	// From Shutdown until the next device (DeviceCreated): no start, whatever the player wants.
	static bool s_device_closed = false;
	static std::atomic<int> s_core{-1};

	// For the "GS recorder:" line, per interval. s_worker_ticks is the worker's; the rest the GS thread's.
	static u32 s_draws = 0, s_records = 0, s_drains_waited = 0, s_room_waits = 0;
	static Common::Timer::Value s_drain_ticks = 0;
	static std::atomic<Common::Timer::Value> s_worker_ticks{0};
	static Common::Timer::Value s_interval_start = 0;

	static constexpr u32 RoundUp16(u32 n) { return (n + 15u) & ~15u; }

	static void Replay(const Header* h)
	{
		switch (h->op)
		{
			case OP_BIND_PIPELINE:
			{
				const auto* r = reinterpret_cast<const RBindPipeline*>(h);
				vkCmdBindPipeline(h->cmd, r->bind_point, r->pipeline);
				break;
			}
			case OP_BIND_INDEX_BUFFER:
			{
				const auto* r = reinterpret_cast<const RBindIndexBuffer*>(h);
				vkCmdBindIndexBuffer(h->cmd, r->buffer, r->offset, r->type);
				break;
			}
			case OP_SET_VIEWPORT:
				vkCmdSetViewport(h->cmd, 0, 1, &reinterpret_cast<const RViewport*>(h)->viewport);
				break;
			case OP_SET_SCISSOR:
				vkCmdSetScissor(h->cmd, 0, 1, &reinterpret_cast<const RScissor*>(h)->rect);
				break;
			case OP_SET_BLEND_CONSTANTS:
				vkCmdSetBlendConstants(h->cmd, reinterpret_cast<const RBlendConstants*>(h)->color);
				break;
			case OP_SET_LINE_WIDTH:
				vkCmdSetLineWidth(h->cmd, reinterpret_cast<const RLineWidth*>(h)->width);
				break;
			case OP_BIND_DESCRIPTOR_SETS:
			{
				const auto* r = reinterpret_cast<const RBindDescriptorSets*>(h);
				vkCmdBindDescriptorSets(h->cmd, r->bind_point, r->layout, r->first_set, 1, &r->set, r->dynamic_count,
					r->dynamic_offsets);
				break;
			}
			case OP_PUSH_CONSTANTS:
			{
				const auto* r = reinterpret_cast<const RPushConstants*>(h);
				vkCmdPushConstants(h->cmd, r->layout, r->stages, r->offset, r->size, r + 1);
				break;
			}
			case OP_PUSH_DESCRIPTOR_SET:
			{
				const auto* r = reinterpret_cast<const RPushDescriptorSet*>(h);
				vkCmdPushDescriptorSetKHR(h->cmd, r->bind_point, r->layout, r->set, r->write_count,
					reinterpret_cast<const VkWriteDescriptorSet*>(r + 1));
				break;
			}
			case OP_IMAGE_BARRIER:
			{
				const auto* r = reinterpret_cast<const RImageBarrier*>(h);
				vkCmdPipelineBarrier(h->cmd, r->src_stages, r->dst_stages, r->dependency_flags, 0, nullptr, 0, nullptr, 1,
					&r->barrier);
				break;
			}
			case OP_DRAW:
			{
				const auto* r = reinterpret_cast<const RDraw*>(h);
				vkCmdDraw(h->cmd, r->vertex_count, r->instance_count, r->first_vertex, r->first_instance);
				break;
			}
			case OP_DRAW_INDEXED:
			{
				const auto* r = reinterpret_cast<const RDrawIndexed*>(h);
				vkCmdDrawIndexed(h->cmd, r->index_count, r->instance_count, r->first_index, r->vertex_offset, r->first_instance);
				break;
			}
			default:
				break;
		}
	}

	// Worker: every record published so far, publishing its progress after each one (the GS thread waits on it for room and
	// to drain).
	static void ReplayPublished()
	{
		u64 head = s_head_published.load(std::memory_order_acquire);
		while (s_tail != head)
		{
			const Header* h = reinterpret_cast<const Header*>(&s_ring[s_tail & (RING_BYTES - 1)]);
			if (h->op != OP_WRAP)
				Replay(h);
			s_tail += h->size;
			s_tail_published.store(s_tail, std::memory_order_release);
			if (s_tail == head)
				head = s_head_published.load(std::memory_order_acquire);
		}
	}

	static void* WorkerMain(void*)
	{
		Threading::SetNameOfCurrentThread("GS recorder");
		s_core.store(PS4ThreadAffinity::PinCurrentThreadToRecorder(), std::memory_order_relaxed);
		for (;;)
		{
			s_sema.WaitForWorkWithSpin();
			if (s_quit.load(std::memory_order_acquire))
				break;
			const Common::Timer::Value start = Common::Timer::GetCurrentValue();
			ReplayPublished();
			s_worker_ticks.fetch_add(Common::Timer::GetCurrentValue() - start, std::memory_order_relaxed);
		}
		return nullptr;
	}

	static bool StartWorker()
	{
		if (s_thread_started)
			return true;
		if (s_start_failed)
			return false;
		s_ring.reset(new (std::nothrow) u8[RING_BYTES]);
		if (!s_ring)
		{
			s_start_failed = true;
			ProbeLog("GS recorder: no memory for its queue (%u KiB), stays off\n", RING_BYTES / 1024);
			return false;
		}
		s_head = s_tail_seen = s_tail = 0;
		s_head_published.store(0, std::memory_order_relaxed);
		s_tail_published.store(0, std::memory_order_relaxed);
		s_quit.store(false, std::memory_order_relaxed);
		s_sema.Reset();
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 512 * 1024); // the driver's draw path, not deep
		const int rc = pthread_create(&s_thread, &attr, WorkerMain, nullptr);
		pthread_attr_destroy(&attr);
		if (rc != 0)
		{
			s_start_failed = true;
			s_ring.reset();
			ProbeLog("GS recorder: could not start its thread (%d), stays off\n", rc);
			return false;
		}
		s_thread_started = true;
		return true;
	}

	// GS thread: wait until the worker has made every queued command. The fast case (nothing queued) is one load.
	void Drain()
	{
		if (s_tail_published.load(std::memory_order_acquire) == s_head)
			return;
		s_drains_waited++;
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		s_sema.NotifyOfWork();
		u32 spins = 0;
		while (s_tail_published.load(std::memory_order_acquire) != s_head)
		{
			_mm_pause();
			if (++spins == 4096)
			{
				spins = 0;
				std::this_thread::yield();
			}
		}
		s_tail_seen = s_head;
		s_drain_ticks += Common::Timer::GetCurrentValue() - start;
	}

	// GS thread: room for one record of size bytes (a multiple of 16), after a wrap record if it does not fit before the end.
	static u8* Reserve(u32 size)
	{
		const u32 pos = static_cast<u32>(s_head & (RING_BYTES - 1));
		const u32 to_end = RING_BYTES - pos;
		const u32 need = (to_end < size) ? to_end + size : size;
		if (s_head + need - s_tail_seen > RING_BYTES)
		{
			s_tail_seen = s_tail_published.load(std::memory_order_acquire);
			if (s_head + need - s_tail_seen > RING_BYTES)
			{
				s_room_waits++;
				s_sema.NotifyOfWork();
				while (s_head + need - (s_tail_seen = s_tail_published.load(std::memory_order_acquire)) > RING_BYTES)
					_mm_pause();
			}
		}
		if (to_end < size)
		{
			Header* wrap = reinterpret_cast<Header*>(&s_ring[pos]);
			wrap->op = OP_WRAP;
			wrap->size = to_end;
			wrap->cmd = VK_NULL_HANDLE;
			s_head += to_end;
			return &s_ring[0];
		}
		return &s_ring[pos];
	}

	template <typename T>
	static T* Begin(Op op, VkCommandBuffer cmd, u32 extra = 0)
	{
		const u32 size = RoundUp16(static_cast<u32>(sizeof(T)) + extra);
		T* r = reinterpret_cast<T*>(Reserve(size));
		r->h.op = op;
		r->h.size = size;
		r->h.cmd = cmd;
		return r;
	}

	static void Commit(const Header& h)
	{
		s_head += h.size;
		s_records++;
		s_head_published.store(s_head, std::memory_order_release);
	}

	// The calls. queue = the caller is the hardware draw's path (ApplyTFXState, SendHWDraw, Draw); with it false, or the
	// recorder off, they call Vulkan at once, as before.

	static void CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdBindPipeline(cmd, bind_point, pipeline);
		RBindPipeline* r = Begin<RBindPipeline>(OP_BIND_PIPELINE, cmd);
		r->bind_point = bind_point;
		r->pipeline = pipeline;
		Commit(r->h);
	}

	static void CmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdBindIndexBuffer(cmd, buffer, offset, type);
		RBindIndexBuffer* r = Begin<RBindIndexBuffer>(OP_BIND_INDEX_BUFFER, cmd);
		r->buffer = buffer;
		r->offset = offset;
		r->type = type;
		Commit(r->h);
	}

	static void CmdSetViewport(VkCommandBuffer cmd, const VkViewport& viewport, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdSetViewport(cmd, 0, 1, &viewport);
		RViewport* r = Begin<RViewport>(OP_SET_VIEWPORT, cmd);
		r->viewport = viewport;
		Commit(r->h);
	}

	static void CmdSetScissor(VkCommandBuffer cmd, const VkRect2D& rect, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdSetScissor(cmd, 0, 1, &rect);
		RScissor* r = Begin<RScissor>(OP_SET_SCISSOR, cmd);
		r->rect = rect;
		Commit(r->h);
	}

	static void CmdSetBlendConstants(VkCommandBuffer cmd, const float color[4], bool queue)
	{
		if (!queue || !g_on)
			return vkCmdSetBlendConstants(cmd, color);
		RBlendConstants* r = Begin<RBlendConstants>(OP_SET_BLEND_CONSTANTS, cmd);
		std::memcpy(r->color, color, sizeof(r->color));
		Commit(r->h);
	}

	static void CmdSetLineWidth(VkCommandBuffer cmd, float width, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdSetLineWidth(cmd, width);
		RLineWidth* r = Begin<RLineWidth>(OP_SET_LINE_WIDTH, cmd);
		r->width = width;
		Commit(r->h);
	}

	// One set (all the hardware draws bind).
	static void CmdBindDescriptorSet(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout,
		u32 first_set, VkDescriptorSet set, u32 dynamic_count, const u32* dynamic_offsets, bool queue)
	{
		if (!queue || !g_on || dynamic_count > MAX_DYNAMIC_OFFSETS)
		{
			if (queue && g_on)
				Drain();
			return vkCmdBindDescriptorSets(cmd, bind_point, layout, first_set, 1, &set, dynamic_count, dynamic_offsets);
		}
		RBindDescriptorSets* r = Begin<RBindDescriptorSets>(OP_BIND_DESCRIPTOR_SETS, cmd);
		r->bind_point = bind_point;
		r->first_set = first_set;
		r->layout = layout;
		r->set = set;
		r->dynamic_count = dynamic_count;
		std::memcpy(r->dynamic_offsets, dynamic_offsets, dynamic_count * sizeof(u32));
		Commit(r->h);
	}

	static void CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset,
		u32 size, const void* data, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdPushConstants(cmd, layout, stages, offset, size, data);
		RPushConstants* r = Begin<RPushConstants>(OP_PUSH_CONSTANTS, cmd, size);
		r->layout = layout;
		r->stages = stages;
		r->offset = offset;
		r->size = size;
		std::memcpy(r + 1, data, size);
		Commit(r->h);
	}

	// A copy of the writes and of the image and buffer infos they point to, pointing into the copy (it stays in the ring until
	// the worker has made the push). Writes with a pNext or texel buffer views (none in the hardware draws' texture set) go to
	// Vulkan at once, after the queue.
	static void CmdPushDescriptorSet(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout, u32 set,
		u32 write_count, const VkWriteDescriptorSet* writes, bool queue)
	{
		u32 images = 0, buffers = 0;
		bool plain = queue && g_on;
		for (u32 i = 0; plain && i < write_count; i++)
		{
			if (writes[i].pNext || writes[i].pTexelBufferView)
				plain = false;
			if (writes[i].pImageInfo)
				images += writes[i].descriptorCount;
			if (writes[i].pBufferInfo)
				buffers += writes[i].descriptorCount;
		}
		if (!plain)
		{
			if (queue && g_on)
				Drain();
			return vkCmdPushDescriptorSetKHR(cmd, bind_point, layout, set, write_count, writes);
		}
		const u32 extra = write_count * static_cast<u32>(sizeof(VkWriteDescriptorSet)) +
		                  images * static_cast<u32>(sizeof(VkDescriptorImageInfo)) +
		                  buffers * static_cast<u32>(sizeof(VkDescriptorBufferInfo));
		RPushDescriptorSet* r = Begin<RPushDescriptorSet>(OP_PUSH_DESCRIPTOR_SET, cmd, extra);
		r->bind_point = bind_point;
		r->set = set;
		r->layout = layout;
		r->write_count = write_count;
		static_assert(sizeof(RPushDescriptorSet) % alignof(VkWriteDescriptorSet) == 0);
		VkWriteDescriptorSet* w = reinterpret_cast<VkWriteDescriptorSet*>(r + 1);
		VkDescriptorImageInfo* image = reinterpret_cast<VkDescriptorImageInfo*>(w + write_count);
		VkDescriptorBufferInfo* buffer = reinterpret_cast<VkDescriptorBufferInfo*>(image + images);
		for (u32 i = 0; i < write_count; i++)
		{
			w[i] = writes[i];
			if (writes[i].pImageInfo)
			{
				std::memcpy(image, writes[i].pImageInfo, writes[i].descriptorCount * sizeof(VkDescriptorImageInfo));
				w[i].pImageInfo = image;
				image += writes[i].descriptorCount;
			}
			if (writes[i].pBufferInfo)
			{
				std::memcpy(buffer, writes[i].pBufferInfo, writes[i].descriptorCount * sizeof(VkDescriptorBufferInfo));
				w[i].pBufferInfo = buffer;
				buffer += writes[i].descriptorCount;
			}
		}
		Commit(r->h);
	}

	static void CmdImageBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stages, VkPipelineStageFlags dst_stages,
		VkDependencyFlags dependency_flags, const VkImageMemoryBarrier& barrier, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdPipelineBarrier(cmd, src_stages, dst_stages, dependency_flags, 0, nullptr, 0, nullptr, 1, &barrier);
		RImageBarrier* r = Begin<RImageBarrier>(OP_IMAGE_BARRIER, cmd);
		r->src_stages = src_stages;
		r->dst_stages = dst_stages;
		r->dependency_flags = dependency_flags;
		r->barrier = barrier;
		Commit(r->h);
	}

	// A draw wakes the worker if it sleeps (the commands before it can wait for it).
	static void CmdDraw(VkCommandBuffer cmd, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance,
		bool queue)
	{
		if (!queue || !g_on)
			return vkCmdDraw(cmd, vertex_count, instance_count, first_vertex, first_instance);
		RDraw* r = Begin<RDraw>(OP_DRAW, cmd);
		r->vertex_count = vertex_count;
		r->instance_count = instance_count;
		r->first_vertex = first_vertex;
		r->first_instance = first_instance;
		Commit(r->h);
		s_draws++;
		s_sema.NotifyOfWork();
	}

	static void CmdDrawIndexed(VkCommandBuffer cmd, u32 index_count, u32 instance_count, u32 first_index,
		s32 vertex_offset, u32 first_instance, bool queue)
	{
		if (!queue || !g_on)
			return vkCmdDrawIndexed(cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
		RDrawIndexed* r = Begin<RDrawIndexed>(OP_DRAW_INDEXED, cmd);
		r->index_count = index_count;
		r->instance_count = instance_count;
		r->first_index = first_index;
		r->vertex_offset = vertex_offset;
		r->first_instance = first_instance;
		Commit(r->h);
		s_draws++;
		s_sema.NotifyOfWork();
	}

	static void LogInterval(double seconds)
	{
		const double worker_ms = Common::Timer::ConvertValueToMilliseconds(s_worker_ticks.exchange(0, std::memory_order_relaxed));
		ProbeLog("GS recorder: interval=%.3f on=%d core=%d draws=%u records=%u drains_waited=%u drain_wait_ms=%.2f "
				 "worker_busy_ms=%.2f room_waits=%u\n",
			seconds, g_on ? 1 : 0, s_core.load(std::memory_order_relaxed), s_draws, s_records, s_drains_waited,
			Common::Timer::ConvertValueToMilliseconds(s_drain_ticks), worker_ms, s_room_waits);
		s_draws = s_records = s_drains_waited = s_room_waits = 0;
		s_drain_ticks = 0;
	}

	// GS thread, at the start of each command buffer (ActivateCommandBuffer, after a submission drained the queue): take the
	// player's choice, and every ~5 s while on, the line.
	void FrameBoundary()
	{
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		const bool wanted = g_forja_draw_thread_wanted.load(std::memory_order_relaxed) && !s_device_closed;
		if (wanted != g_on)
		{
			pxAssert(s_tail_published.load(std::memory_order_acquire) == s_head);
			if (g_on && s_interval_start != 0)
				LogInterval(Common::Timer::ConvertValueToSeconds(now - s_interval_start));
			if (!wanted || StartWorker())
			{
				g_on = wanted;
				s_interval_start = now;
				s_draws = s_records = s_drains_waited = s_room_waits = 0;
				s_drain_ticks = 0;
				s_worker_ticks.store(0, std::memory_order_relaxed);
				ProbeLog("GS recorder: %s\n", g_on ? "on (the draws' Vulkan calls on their own thread)" : "off");
			}
		}
		else if (g_on && Common::Timer::ConvertValueToSeconds(now - s_interval_start) >= 5.0)
		{
			LogInterval(Common::Timer::ConvertValueToSeconds(now - s_interval_start));
			s_interval_start = now;
		}
	}

	// GS thread, before the device goes (GSDeviceVK::Destroy, its last submission already made).
	void Shutdown()
	{
		s_device_closed = true;
		if (!s_thread_started)
		{
			g_on = false;
			return;
		}
		Drain();
		g_on = false;
		s_quit.store(true, std::memory_order_release);
		s_sema.NotifyOfWork();
		pthread_join(s_thread, nullptr);
		s_thread_started = false;
		s_ring.reset();
		ProbeLog("GS recorder: stopped with the device\n");
	}

	// GSDeviceVK::Create: a new device, so the recorder may start again.
	void DeviceCreated()
	{
		s_device_closed = false;
	}
} // namespace ForjaVkRec

#endif // PCSX2_PS4_VM_BOOT
