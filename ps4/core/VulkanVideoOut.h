// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Shared PS4 scanout implementation for the GS and its hardware probe.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <orbis/libkernel.h>

#include <array>
#include <cstdint>

namespace PS4Port
{
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
	};
} // namespace PS4Port
