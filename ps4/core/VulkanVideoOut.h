// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Shared PS4 scanout implementation for the GS and its hardware probe.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <orbis/libkernel.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace PS4Port
{
	// Forja SX2 (compilation 37): the TV's refresh. A PAL game's 50 frames a second on the system's 59.94 Hz output leave one
	// frame in five on screen for two refreshes: a stutter ten times a second, at any speed (God of War II, 2026-10-17: "it
	// runs at full speed but doesn't look smooth"). At 50 Hz each PS2 frame is one refresh. main.cpp asks for 50 while a PAL
	// game runs; the GS thread switches the output in Acquire; g_output_millihz is what VideoOut reports (frame generation's
	// pacing reads it).
	inline std::atomic<int> g_output_hz_request{0}; // 0: the system's own rate; 50: 50 Hz
	inline std::atomic<int> g_output_millihz{59940};

	class VulkanVideoOut
	{
	public:
		static constexpr std::uint32_t Width = 1920;
		static constexpr std::uint32_t Height = 1080;
		// (AI-assisted) Triple buffering decouples CPU/GPU execution from display VSync; needs proper testing on console.
		static constexpr std::uint32_t ImageCount = 3;
		static constexpr VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
		using Logger = void (*)(const char* message);

		struct Dispatch
		{
			PFN_vkGetPhysicalDeviceFormatProperties GetFormatProperties;
			PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties;
			PFN_vkCreateImage CreateImage;
			PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
			PFN_vkAllocateMemory AllocateMemory;
			PFN_vkMapMemory MapMemory;
			PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
			PFN_vkBindImageMemory BindImageMemory;
			PFN_vkUnmapMemory UnmapMemory;
			PFN_vkDestroyImage DestroyImage;
			PFN_vkFreeMemory FreeMemory;
			PFN_vkDeviceWaitIdle DeviceWaitIdle;
		};

		VulkanVideoOut(VkPhysicalDevice gpu, VkDevice device, const Dispatch& dispatch, Logger logger);
		~VulkanVideoOut();
		VulkanVideoOut(const VulkanVideoOut&) = delete;
		VulkanVideoOut& operator=(const VulkanVideoOut&) = delete;

		bool Create();
		bool Acquire();
		bool Present();
		// Forja SX2: GPU-side flip, as DolphinPS4 presents (ac_ps4_submit_flip, in the Mesa of our SDK: RADV on Sony's GNM
		// driver). The flip is queued behind the GPU work submitted so far, so the CPU waits neither for the GPU to finish
		// the frame nor for vblank. Call it right after the frame's vkQueueSubmit. False if the driver refused it (logged).
		bool PresentFromGpu();
		void Release() { m_acquired = false; }
		bool WaitForFlip(OrbisKernelUseconds timeout = 5000000);
		// (AI-assisted) Destroy image views first; GPU completion and console teardown still need proper testing.
		bool Destroy();

		VkImage GetImage(std::uint32_t index) const { return m_images[index].image; }
		std::uint32_t* GetPixels(std::uint32_t index) const { return m_images[index].pixels; }
		std::uint32_t GetPixelPitch() const { return m_pixel_pitch; }
		std::uint32_t GetCurrentIndex() const { return m_current_index; }
		std::uint64_t GetFrameCount() const { return m_frame_count; }

	private:
		void Log(const char* format, ...) const;
		// Forja SX2: drains completed flip events and takes the pending count from the system.
		void SyncPendingFlips();
		// Waits until at most max_pending flips remain, up to timeout microseconds.
		bool WaitPendingAtMost(std::uint32_t max_pending, OrbisKernelUseconds timeout);
		// Forja SX2: with GPU-side flips, until the image of the next frame is off screen (flipArg, as DolphinPS4 does).
		bool WaitNextImageOffScreen(OrbisKernelUseconds timeout);
		// Forja SX2 (compilation 37): g_output_hz_request against the rate set now, and what VideoOut reports.
		void UpdateOutputRate();
		bool ConfigureRefresh(std::uint64_t refresh_rate, const char* why);
		void ReadOutputRate(bool log);
		struct ImageMemory
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			void* mapped = nullptr;
			std::uint32_t* pixels = nullptr;
		};
		VkPhysicalDevice m_gpu;
		VkDevice m_device;
		Dispatch m_dispatch;
		Logger m_logger;
		std::array<ImageMemory, ImageCount> m_images{};
		int m_handle = -1;
		int m_buffer_set = -1;
		OrbisKernelEqueue m_flip_queue = 0;
		// (AI-assisted) The SDK queue handle is unsigned; track ownership separately from its value.
		bool m_flip_queue_created = false;
		std::uint32_t m_pixel_pitch = 0;
		std::uint32_t m_current_index = 0;
		std::uint64_t m_frame_count = 0;
		std::uint32_t m_pending_flips = 0;
		bool m_flip_pending = false;
		bool m_acquired = false;
		// Forja SX2: the last flip was GPU-side, so the system's pending count may not include it yet: Acquire waits by
		// flipArg instead (each flip's argument is its 1-based frame number).
		bool m_gpu_flips = false;
		std::uint64_t m_gpu_flip_count = 0;
		// Forja SX2 (compilation 37): the refresh asked of VideoOut (0: never changed), the frame of the switch to 50 Hz and
		// whether its guard file is down, and when the reported rate is read next.
		int m_output_hz = 0;
		bool m_output_failed = false;
		bool m_output_guard = false;
		std::uint64_t m_output_since_frame = 0;
		std::uint64_t m_output_read_frame = 0;
		std::uint64_t m_output_reported = 0;
	};
} // namespace PS4Port
