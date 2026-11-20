// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Linear Vulkan images registered directly with the PS4 VideoOut service.
#include "VulkanVideoOut.h"
#include "ForjaFlipThread.h" // 154 in the app
#include "ThreadAffinityPS4.h"

#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <unistd.h>

// Forja SX2: in the Mesa of our SDK (DolphinPS4's port/mesa/ac_ps4_drm.c, RADV on GNM): queues a VideoOut flip of `buffer`
// behind the GPU work submitted so far (sceGnmSubmitAndFlipCommandBuffers); `arg` comes back as flipArg once it is on screen.
extern "C" int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, std::int64_t arg);

namespace
{
	// Forja SX2 (compilation 37): the output mode, as shadPS4 (src/core/libraries/videoout) and fpPS4 (ps4_libscevideoout.pas)
	// describe SceVideoOutMode and sceVideoOutConfigureOutputMode_. The SDK's header declares both functions without
	// their arguments, so they are called through these types.
	struct OutputMode
	{
		std::uint32_t size;
		std::uint8_t signal_encoding;
		std::uint8_t signal_range;
		std::uint8_t colorimetry;
		std::uint8_t depth;
		std::uint64_t refresh_rate;
		std::uint64_t resolution;
		std::uint8_t content_type;
		std::uint8_t reserved0[3];
		std::uint32_t reserved1;
	};
	static_assert(sizeof(OutputMode) == 32, "SceVideoOutMode is 32 bytes");

	struct OutputOptions
	{
		std::uint16_t vertical_offset;
		std::uint16_t reserved0;
		std::uint32_t reserved1[3];
	};

	using ModeSetAnyFn = void (*)(OutputMode*, std::uint32_t);
	using ConfigureOutputModeFn = int (*)(int, std::uint32_t, const OutputMode*, const OutputOptions*, std::uint32_t, std::uint32_t);

	constexpr std::uint64_t REFRESH_50HZ = 2; // SCE_VIDEO_OUT_REFRESH_RATE_50HZ
	constexpr std::uint64_t REFRESH_ANY = ~std::uint64_t{0};
	// Left while the first 90 s at 50 Hz run: a TV that shows nothing at 50 Hz gets the app closed from the PS button, and the
	// next game finds it here and stays at the system's rate (vm-sin-50hz is written; main.cpp).
	constexpr const char* OUTPUT_GUARD = "/data/ForjaSX2/salida-50hz-en-prueba";

	int RefreshMillihertz(std::uint64_t refresh_rate)
	{
		switch (refresh_rate)
		{
			case 1: return 23976;
			case 2: return 50000;
			case 3: return 59940;
			case 6: return 29970;
			case 13: return 119880;
			case 35: return 89910;
			default: return 0;
		}
	}
} // namespace

PS4Port::VulkanVideoOut::VulkanVideoOut(VkPhysicalDevice gpu, VkDevice device, const Dispatch& dispatch, Logger logger)
	: m_gpu(gpu)
	, m_device(device)
	, m_dispatch(dispatch)
	, m_logger(logger)
{
}

PS4Port::VulkanVideoOut::~VulkanVideoOut()
{
	Destroy();
}

void PS4Port::VulkanVideoOut::Log(const char* format, ...) const
{
	if (!m_logger)
		return;
	char message[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	m_logger(message);
}

bool PS4Port::VulkanVideoOut::Create()
{
	if (m_handle >= 0 || m_flip_queue_created || m_images[0].image != VK_NULL_HANDLE)
		return false;
	if (!m_gpu || !m_device || !m_dispatch.GetFormatProperties || !m_dispatch.GetMemoryProperties ||
		!m_dispatch.CreateImage || !m_dispatch.GetImageMemoryRequirements || !m_dispatch.AllocateMemory ||
		!m_dispatch.MapMemory || !m_dispatch.GetImageSubresourceLayout || !m_dispatch.BindImageMemory ||
		!m_dispatch.UnmapMemory || !m_dispatch.DestroyImage || !m_dispatch.FreeMemory || !m_dispatch.DeviceWaitIdle)
	{
		Log("VideoOut: incomplete Vulkan dispatch");
		return false;
	}
	VkFormatProperties properties{};
	m_dispatch.GetFormatProperties(m_gpu, Format, &properties);
	if (!(properties.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
	{
		Log("VideoOut: linear R8G8B8A8 color attachments are unavailable");
		return false;
	}
	int result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
	Log("VideoOut: load module = %#x", result);
	result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
	Log("VideoOut: load UserService = %#x", result);
	result = sceUserServiceInitialize(nullptr);
	if (result != 0 && static_cast<std::uint32_t>(result) != ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED)
	{
		Log("VideoOut: UserService initialization failed: %#x", result);
		return false;
	}
	m_handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
	if (m_handle < 0)
	{
		Log("VideoOut: open failed: %#x", m_handle);
		return false;
	}
	result = sceKernelCreateEqueue(&m_flip_queue, "PCSX2 Vulkan VideoOut");
	if (result == 0)
	{
		m_flip_queue_created = true;
		result = sceVideoOutAddFlipEvent(m_flip_queue, m_handle, nullptr);
	}
	if (result != 0)
	{
		Log("VideoOut: flip queue creation failed: %#x", result);
		return false;
	}

	VkPhysicalDeviceMemoryProperties memory_properties{};
	m_dispatch.GetMemoryProperties(m_gpu, &memory_properties);
	std::array<void*, ImageCount> addresses{};
	const VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D,
		Format, {Width, Height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
	constexpr VkDeviceSize scanout_alignment = 0x200000;
	for (std::uint32_t i = 0; i < ImageCount; i++)
	{
		ImageMemory& owned = m_images[i];
		VkResult vr = m_dispatch.CreateImage(m_device, &image_info, nullptr, &owned.image);
		if (vr != VK_SUCCESS)
		{
			Log("VideoOut: create image %u failed: %d", i, vr);
			return false;
		}
		VkMemoryRequirements requirements{};
		m_dispatch.GetImageMemoryRequirements(m_device, owned.image, &requirements);
		std::uint32_t memory_type = UINT32_MAX;
		constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
		                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (std::uint32_t t = 0; t < memory_properties.memoryTypeCount; t++)
		{
			if ((requirements.memoryTypeBits & (1u << t)) &&
				(memory_properties.memoryTypes[t].propertyFlags & required) == required)
			{
				memory_type = t;
				break;
			}
		}
		if (memory_type == UINT32_MAX || requirements.alignment == 0 ||
			requirements.alignment > std::numeric_limits<VkDeviceSize>::max() - scanout_alignment ||
			requirements.size > std::numeric_limits<VkDeviceSize>::max() - scanout_alignment - requirements.alignment)
		{
			Log("VideoOut: unsuitable memory requirements for image %u", i);
			return false;
		}
		const VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
			requirements.size + scanout_alignment + requirements.alignment, memory_type};
		vr = m_dispatch.AllocateMemory(m_device, &allocation, nullptr, &owned.memory);
		if (vr == VK_SUCCESS)
			vr = m_dispatch.MapMemory(m_device, owned.memory, 0, VK_WHOLE_SIZE, 0, &owned.mapped);
		if (vr != VK_SUCCESS)
		{
			Log("VideoOut: allocate/map image %u failed: %d", i, vr);
			return false;
		}
		const VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
		VkSubresourceLayout layout{};
		m_dispatch.GetImageSubresourceLayout(m_device, owned.image, &subresource, &layout);
		const auto base = reinterpret_cast<std::uintptr_t>(owned.mapped);
		VkDeviceSize offset = (scanout_alignment - ((base + layout.offset) % scanout_alignment)) % scanout_alignment;
		if (const VkDeviceSize remainder = offset % requirements.alignment; remainder != 0)
			offset += requirements.alignment - remainder;
		if (offset + requirements.size > allocation.allocationSize ||
			(base + offset + layout.offset) % scanout_alignment != 0)
		{
			Log("VideoOut: image %u cannot meet Vulkan and 2 MiB scanout alignment", i);
			return false;
		}
		vr = m_dispatch.BindImageMemory(m_device, owned.image, owned.memory, offset);
		if (vr != VK_SUCCESS)
		{
			Log("VideoOut: bind image %u failed: %d", i, vr);
			return false;
		}
		addresses[i] = reinterpret_cast<void*>(base + offset + layout.offset);
		owned.pixels = static_cast<std::uint32_t*>(addresses[i]);
		const VkDeviceSize pitch = layout.rowPitch / sizeof(std::uint32_t);
		if (layout.rowPitch % sizeof(std::uint32_t) != 0 || pitch < Width || pitch > UINT32_MAX ||
			(i != 0 && pitch != m_pixel_pitch))
		{
			Log("VideoOut: invalid or inconsistent row pitch for image %u", i);
			return false;
		}
		m_pixel_pitch = static_cast<std::uint32_t>(pitch);
		Log("VideoOut: image %u memory type=%u size=%llu bind=%llu pixels=%p pitch=%u", i, memory_type,
			static_cast<unsigned long long>(requirements.size), static_cast<unsigned long long>(offset), addresses[i], m_pixel_pitch);
	}
	OrbisVideoOutBufferAttribute attribute{};
	sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
		ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, Width, Height, m_pixel_pitch);
	m_buffer_set = sceVideoOutRegisterBuffers(m_handle, 0, addresses.data(), ImageCount, &attribute);
	Log("VideoOut: register %u buffers = %#x", ImageCount, m_buffer_set);
	return m_buffer_set >= 0;
}

// Forja SX2: the old code counted one completion per flip event, but the flip queue is a kqueue and flips
// that complete together can arrive as one event. The count then stayed one too high for good: on the
// console every shutdown logged "flip wait failed ... pending=1" twice (the drain and the blank flip timed
// out), and during play one of the three buffers was a phantom in flight. The system's own count
// (sceVideoOutGetFlipStatus numFlipPending) is used instead; event counting remains only as a fallback.
void PS4Port::VulkanVideoOut::SyncPendingFlips()
{
	while (m_pending_flips > 0)
	{
		OrbisKernelEvent event{};
		int count = 0;
		OrbisKernelUseconds timeout = 0;
		const int result = sceKernelWaitEqueue(m_flip_queue, &event, 1, &count, &timeout);
		if (result != 0 || count != 1)
			break;
		m_pending_flips--;
	}
	if (m_handle < 0)
		return;
	OrbisVideoOutFlipStatus status{};
	if (sceVideoOutGetFlipStatus(m_handle, &status) == 0)
		m_pending_flips = static_cast<std::uint32_t>(std::max<std::int32_t>(status.numFlipPending, 0));
}

bool PS4Port::VulkanVideoOut::WaitPendingAtMost(std::uint32_t max_pending, OrbisKernelUseconds timeout)
{
	// Short waits on the flip queue, re-reading the system's count after each, until the deadline.
	constexpr OrbisKernelUseconds slice = 2000;
	OrbisKernelUseconds waited = 0;
	SyncPendingFlips();
	while (m_pending_flips > max_pending)
	{
		if (waited >= timeout)
		{
			Log("VideoOut: flip wait failed: timeout_us=%u frame=%llu pending=%u", static_cast<unsigned>(timeout),
				static_cast<unsigned long long>(m_frame_count), m_pending_flips);
			return false;
		}
		OrbisKernelEvent event{};
		int count = 0;
		OrbisKernelUseconds current = slice;
		const int result = sceKernelWaitEqueue(m_flip_queue, &event, 1, &count, &current);
		if (result == 0 && count == 1 && m_pending_flips > 0)
			m_pending_flips--; // fallback count, corrected just below when the system answers
		waited += slice;
		SyncPendingFlips();
	}
	return true;
}

bool PS4Port::VulkanVideoOut::WaitForFlip(OrbisKernelUseconds timeout)
{
	DrainFlipThread(); // 154 in the app: the queued GPU-side flips first
	if (!WaitPendingAtMost(0, timeout))
		return false;
	m_flip_pending = false;
	return true;
}

// Forja SX2: frame n draws into image n % ImageCount, last shown by frame n - ImageCount, which stays on screen until the flip
// of frame n - ImageCount + 1 is. Flip arguments are 1-based frame numbers, so that flip's is n - ImageCount + 2.
bool PS4Port::VulkanVideoOut::WaitNextImageOffScreen(OrbisKernelUseconds timeout)
{
	if (m_frame_count < ImageCount)
		return true;
	const std::int64_t needed = static_cast<std::int64_t>(m_frame_count - ImageCount + 2);
	constexpr OrbisKernelUseconds slice = 2000;
	OrbisKernelUseconds waited = 0;
	for (;;)
	{
		OrbisVideoOutFlipStatus status{};
		if (sceVideoOutGetFlipStatus(m_handle, &status) != 0 || status.flipArg >= needed)
			return true;
		if (waited >= timeout)
		{
			Log("VideoOut: GPU flip wait failed: timeout_us=%u frame=%llu shown=%lld needed=%lld gpu_pending=%d pending=%d",
				static_cast<unsigned>(timeout), static_cast<unsigned long long>(m_frame_count), static_cast<long long>(status.flipArg),
				static_cast<long long>(needed), status.numGpuFlipPending, status.numFlipPending);
			return false;
		}
		OrbisKernelEvent event{};
		int count = 0;
		OrbisKernelUseconds current = slice;
		sceKernelWaitEqueue(m_flip_queue, &event, 1, &count, &current);
		waited += slice;
	}
}

// Forja SX2 (compilation 37): the output's refresh, on the GS thread before a frame. Asks VideoOut for g_output_hz_request when it
// changes; reads the rate VideoOut reports about once a second.
void PS4Port::VulkanVideoOut::UpdateOutputRate()
{
	const int wanted = g_output_hz_request.load(std::memory_order_relaxed);
	if (wanted != m_output_hz && !(wanted == 50 && m_output_failed))
	{
		const bool fifty = wanted == 50;
		if (fifty && !m_output_guard)
		{
			if (std::FILE* guard = std::fopen(OUTPUT_GUARD, "w"))
			{
				std::fclose(guard);
				m_output_guard = true;
			}
		}
		const bool ok = ConfigureRefresh(fifty ? REFRESH_50HZ : REFRESH_ANY, fifty ? "a PAL game" : "the system's rate again");
		if (fifty && !ok)
		{
			m_output_failed = true; // not asked again in this game
			if (m_output_guard)
			{
				unlink(OUTPUT_GUARD);
				m_output_guard = false;
			}
		}
		m_output_hz = ok || !fifty ? wanted : m_output_hz;
		m_output_since_frame = m_frame_count;
		m_output_read_frame = m_frame_count + 30; // the TV takes a moment to switch
	}
	if (m_output_guard && m_output_hz == 50 && m_frame_count - m_output_since_frame >= 90 * 50)
	{
		unlink(OUTPUT_GUARD);
		m_output_guard = false;
		Log("VideoOut: 90 s at 50 Hz, guard cleared");
	}
	if (m_frame_count >= m_output_read_frame)
	{
		m_output_read_frame = m_frame_count + 60;
		ReadOutputRate(false);
	}
}

bool PS4Port::VulkanVideoOut::ConfigureRefresh(std::uint64_t refresh_rate, const char* why)
{
	if (m_handle < 0)
		return false;
	OutputMode mode{};
	reinterpret_cast<ModeSetAnyFn>(&sceVideoOutModeSetAny_)(&mode, sizeof(mode));
	mode.size = sizeof(mode);
	mode.refresh_rate = refresh_rate;
	const auto configure = reinterpret_cast<ConfigureOutputModeFn>(&sceVideoOutConfigureOutputMode_);
	OutputOptions options{};
	int result = configure(m_handle, 0, &mode, &options, sizeof(mode), sizeof(options));
	int without_options = 0;
	if (result != 0)
		result = without_options = configure(m_handle, 0, &mode, nullptr, sizeof(mode), 0);
	Log("VideoOut: output refresh %s for %s: result=%#x%s", refresh_rate == REFRESH_50HZ ? "50 Hz" : "any",
		why, static_cast<unsigned>(result), without_options != 0 ? " (also without options)" : "");
	return result == 0;
}

void PS4Port::VulkanVideoOut::ReadOutputRate(bool log)
{
	OrbisVideoOutResolutionStatus status{};
	if (m_handle < 0 || sceVideoOutGetResolutionStatus(m_handle, &status) != 0)
		return;
	const int millihz = RefreshMillihertz(status.refreshRate);
	if (millihz != 0)
		g_output_millihz.store(millihz, std::memory_order_relaxed);
	if (log || status.refreshRate != m_output_reported)
	{
		m_output_reported = status.refreshRate;
		Log("VideoOut: the TV now at %.2f Hz (refreshRate=%llu, %ux%u)", millihz / 1000.0,
			static_cast<unsigned long long>(status.refreshRate), status.width, status.height);
	}
}

// Forja SX2 (compilation 41): Acquire waits while ImageCount - 1 flips are still to show (with GPU flips, by flipArg: the frame
// on screen); one fewer is a free image. The fluid frame generation asks before presenting its frame (GSDeviceVK.cpp).
std::uint32_t PS4Port::VulkanVideoOut::FreeImages()
{
	if (m_buffer_set < 0)
		return 0;
	std::uint64_t in_flight = 0;
	if (m_gpu_flips)
	{
		OrbisVideoOutFlipStatus status{};
		if (sceVideoOutGetFlipStatus(m_handle, &status) != 0)
			return 0;
		const std::uint64_t shown = status.flipArg > 0 ? static_cast<std::uint64_t>(status.flipArg) : 0;
		in_flight = m_frame_count > shown ? m_frame_count - shown : 0;
	}
	else
	{
		SyncPendingFlips();
		in_flight = m_pending_flips;
	}
	return in_flight + 1 >= ImageCount ? 0u : static_cast<std::uint32_t>(ImageCount - 1 - in_flight);
}

bool PS4Port::VulkanVideoOut::Acquire()
{
	if (m_buffer_set < 0)
		return false;
	UpdateOutputRate();

	if (m_gpu_flips)
	{
		if (!WaitNextImageOffScreen(5000000))
			return false;
	}
	// (AI-assisted) With triple buffering, wait only when all pipelined buffers
	// are in flight (pending >= ImageCount - 1), decoupling rendering from VSync.
	else if (!WaitPendingAtMost(ImageCount - 2, 5000000))
		return false;

	m_flip_pending = (m_pending_flips > 0);
	m_current_index = static_cast<std::uint32_t>(m_frame_count % ImageCount);
	m_acquired = true;
	return true;
}

bool PS4Port::VulkanVideoOut::Present()
{
	if (!m_acquired || m_buffer_set < 0)
		return false;
	DrainFlipThread(); // 154 in the app: a CPU flip goes out after the GPU-side flips already queued
	const int result = sceVideoOutSubmitFlip(m_handle, static_cast<int>(m_current_index), ORBIS_VIDEO_OUT_FLIP_VSYNC,
		static_cast<std::int64_t>(m_frame_count + 1));
	if (result != 0)
	{
		Log("VideoOut: flip failed: %#x buffer=%u frame=%llu", result, m_current_index,
			static_cast<unsigned long long>(m_frame_count));
		return false;
	}
	m_frame_count++;
	m_pending_flips++;
	m_flip_pending = true;
	m_acquired = false;
	m_gpu_flips = false;
	return true;
}

static std::uint64_t ForjaFlipNowNs()
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool PS4Port::VulkanVideoOut::PresentFromGpu()
{
	if (!m_acquired || m_buffer_set < 0)
		return false;
	const std::int64_t arg = static_cast<std::int64_t>(m_frame_count + 1);
	// 154 in the app, "Flip en su propio hilo" (ForjaFlipThread.h): the same call, from the flip thread.
	const bool threaded = ForjaFlipThread::wanted.load(std::memory_order_relaxed) != 0;
	if (m_flip_refused.load(std::memory_order_acquire))
	{
		DrainFlipThread();
		return false; // GSDeviceVK: this frame and the rest of the session with the CPU flip
	}
	if (threaded && QueueFlip(m_current_index, arg))
	{
		if (m_gpu_flip_count++ < 3)
			Log("VideoOut: GPU-side flip %llu: buffer=%u (in its own thread)", static_cast<unsigned long long>(arg), m_current_index);
	}
	else
	{
		DrainFlipThread(); // the switch went off between games: the flips still queued go first
		const std::uint64_t t0 = ForjaFlipNowNs();
		const int result = ac_ps4_submit_flip(m_handle, m_current_index, ORBIS_VIDEO_OUT_FLIP_VSYNC, arg);
		ForjaFlipThread::Record(ForjaFlipNowNs() - t0, false);
		if (result != 0)
		{
			Log("VideoOut: GPU-side flip refused: %#x buffer=%u frame=%llu", result, m_current_index,
				static_cast<unsigned long long>(m_frame_count));
			return false;
		}
		if (m_gpu_flip_count++ < 3)
			Log("VideoOut: GPU-side flip %llu: buffer=%u", static_cast<unsigned long long>(arg), m_current_index);
	}
	m_frame_count++;
	m_flip_pending = true;
	m_acquired = false;
	m_gpu_flips = true;
	return true;
}

// 154 in the app, "Flip en su propio hilo": the GS thread's side. The flips go out in the order they were queued; a full queue (it
// never is: Acquire waits for the image two frames back to be on screen) waits for room, and the meter counts it.
bool PS4Port::VulkanVideoOut::PresentAfterFence(VkFence fence)
{
	if (!m_acquired || m_buffer_set < 0 || fence == VK_NULL_HANDLE || !m_dispatch.WaitForFences)
		return false;
	if (m_flip_refused.load(std::memory_order_acquire))
	{
		DrainFlipThread();
		return false;
	}
	const std::int64_t arg = static_cast<std::int64_t>(m_frame_count + 1);
	if (!QueueFlip(m_current_index, arg, fence))
		return false;
	if (m_gpu_flip_count++ < 3)
		Log("VideoOut: flip %llu: buffer=%u (from the CPU in its own thread, once the frame is done)", static_cast<unsigned long long>(arg),
			m_current_index);
	m_frame_count++;
	m_flip_pending = true;
	m_acquired = false;
	m_gpu_flips = true; // Acquire and FreeImages wait by flipArg: a flip still in the queue is not in the system's pending count yet
	return true;
}

void PS4Port::VulkanVideoOut::WaitFlipsUsingFence(VkFence fence)
{
	if (fence == VK_NULL_HANDLE)
		return;
	std::unique_lock lock(m_flip_mutex);
	bool used = m_flip_busy && m_flip_busy_fence == fence;
	for (std::uint32_t i = m_flip_head; !used && i != m_flip_tail; i++)
		used = m_flip_jobs[i % FlipQueueSize].fence == fence;
	if (used)
		m_flip_idle.wait(lock, [this] { return m_flip_head == m_flip_tail && !m_flip_busy; });
}

bool PS4Port::VulkanVideoOut::QueueFlip(std::uint32_t buffer, std::int64_t arg, VkFence fence)
{
	std::unique_lock lock(m_flip_mutex);
	if (!m_flip_thread.joinable())
	{
		m_flip_stop = false;
		try
		{
			m_flip_thread = std::thread(&VulkanVideoOut::FlipThreadMain, this);
		}
		catch (...)
		{
			Log("VideoOut: the flip thread could not start; the GS thread flips");
			return false;
		}
		Log("VideoOut: flip thread started (154 in the app, \"Flip en su propio hilo\")");
	}
	if (m_flip_tail - m_flip_head >= FlipQueueSize)
	{
		const std::uint64_t t0 = ForjaFlipNowNs();
		m_flip_idle.wait(lock, [this] { return m_flip_tail - m_flip_head < FlipQueueSize; });
		ForjaFlipThread::room_waits.fetch_add(1, std::memory_order_relaxed);
		ForjaFlipThread::room_wait_ns.fetch_add(ForjaFlipNowNs() - t0, std::memory_order_relaxed);
	}
	m_flip_jobs[m_flip_tail % FlipQueueSize] = FlipJob{buffer, arg, fence};
	m_flip_tail++;
	const std::uint32_t depth = m_flip_tail - m_flip_head;
	std::uint32_t old = ForjaFlipThread::queue_max.load(std::memory_order_relaxed);
	while (depth > old && !ForjaFlipThread::queue_max.compare_exchange_weak(old, depth, std::memory_order_relaxed))
	{
	}
	lock.unlock();
	m_flip_wake.notify_one();
	return true;
}

// The flip thread: on the helpers' core (it mostly sleeps inside the driver's call), one flip at a time, in order. A refused flip
// is logged once; the ones queued after it are dropped and PresentFromGpu answers false from then on, so GSDeviceVK goes to the
// CPU flip for the rest of the session (as when the GS thread's own call is refused).
void PS4Port::VulkanVideoOut::FlipThreadMain()
{
	PS4ThreadAffinity::PinCurrentThreadToHelpers("GS flip");
	std::unique_lock lock(m_flip_mutex);
	for (;;)
	{
		m_flip_wake.wait(lock, [this] { return m_flip_stop || m_flip_head != m_flip_tail; });
		if (m_flip_head == m_flip_tail)
			break; // stop, and nothing left to flip
		const FlipJob job = m_flip_jobs[m_flip_head % FlipQueueSize];
		m_flip_busy = true;
		m_flip_busy_fence = job.fence;
		lock.unlock();
		if (!m_flip_refused.load(std::memory_order_acquire))
		{
			const std::uint64_t t0 = ForjaFlipNowNs();
			if (job.fence != VK_NULL_HANDLE)
			{
				// 155 in the app, "Flip sin candado": the frame's fence (the bridge only takes its lock for an instant at each poll), then
				// the CPU flip (no lock of the bridge). 2 s is a GPU hang: no flip, and the session goes back to the GS thread's flips.
				const VkResult wait = m_dispatch.WaitForFences(m_device, 1, &job.fence, VK_TRUE, 2000000000ull);
				const int result = wait == VK_SUCCESS ?
				                       sceVideoOutSubmitFlip(m_handle, static_cast<int>(job.buffer), ORBIS_VIDEO_OUT_FLIP_VSYNC, job.arg) :
				                       -1;
				ForjaFlipThread::Record(ForjaFlipNowNs() - t0, true);
				if (result != 0)
				{
					Log("VideoOut: flip from its own thread failed: fence %d, flip %#x, buffer=%u frame=%lld; the GS thread flips from the next "
						"frame", static_cast<int>(wait), result, job.buffer, static_cast<long long>(job.arg));
					m_flip_refused.store(true, std::memory_order_release);
				}
			}
			else
			{
				const int result = ac_ps4_submit_flip(m_handle, job.buffer, ORBIS_VIDEO_OUT_FLIP_VSYNC, job.arg);
				ForjaFlipThread::Record(ForjaFlipNowNs() - t0, true);
				if (result != 0)
				{
					Log("VideoOut: GPU-side flip refused in its own thread: %#x buffer=%u frame=%lld; the CPU flip from the next frame", result,
						job.buffer, static_cast<long long>(job.arg));
					m_flip_refused.store(true, std::memory_order_release);
				}
			}
		}
		lock.lock();
		m_flip_head++;
		m_flip_busy = false;
		m_flip_busy_fence = VK_NULL_HANDLE;
		m_flip_idle.notify_all();
	}
}

void PS4Port::VulkanVideoOut::DrainFlipThread()
{
	std::unique_lock lock(m_flip_mutex);
	m_flip_idle.wait(lock, [this] { return m_flip_head == m_flip_tail && !m_flip_busy; });
}

void PS4Port::VulkanVideoOut::StopFlipThread()
{
	{
		std::lock_guard lock(m_flip_mutex);
		if (!m_flip_thread.joinable())
			return;
		m_flip_stop = true;
	}
	m_flip_wake.notify_all();
	m_flip_thread.join(); // it sends what is queued first
	std::lock_guard lock(m_flip_mutex);
	m_flip_stop = false;
}

bool PS4Port::VulkanVideoOut::Destroy()
{
	StopFlipThread(); // 154 in the app: before the images and the output go
	if (m_handle < 0 && !m_flip_queue_created &&
		std::all_of(m_images.begin(), m_images.end(), [](const ImageMemory& image) { return !image.image && !image.memory && !image.mapped; }))
		return true;
	bool cleanup_ok = true;
	if (m_images[0].image && m_dispatch.DeviceWaitIdle)
	{
		const VkResult result = m_dispatch.DeviceWaitIdle(m_device);
		if (result != VK_SUCCESS)
		{
			Log("VideoOut: GPU idle failed: %d", result);
			cleanup_ok = false;
		}
	}
	// (AI-assisted) Draining flips during shutdown needs a short timeout (100 ms)
	// so the application closes cleanly without freezing when the game loop has stopped.
	// Needs proper testing across console teardowns.
	const bool flip_ok = WaitForFlip(100000);
	cleanup_ok = cleanup_ok && flip_ok;
	// Forja SX2 (compilation 37): the system's rate back before the output closes, and a normal close clears the 50 Hz guard.
	if (m_handle >= 0 && m_output_hz != 0)
	{
		ConfigureRefresh(REFRESH_ANY, "the end of the game");
		m_output_hz = 0;
	}
	if (m_output_guard)
	{
		unlink(OUTPUT_GUARD);
		m_output_guard = false;
	}
	g_output_millihz.store(59940, std::memory_order_relaxed);
	if (m_handle >= 0)
	{
		if (m_buffer_set >= 0)
		{
			// (AI-assisted) Retiring a flip leaves its buffer on scanout; detach it with index -1 before unregistering.
			if (m_frame_count != 0)
			{
				const int blank_result = sceVideoOutSubmitFlip(m_handle, -1, ORBIS_VIDEO_OUT_FLIP_VSYNC,
					static_cast<std::int64_t>(m_frame_count + 1));
				Log("VideoOut: blank flip = %#x", blank_result);
				if (blank_result == 0)
				{
					m_pending_flips++;
					m_flip_pending = true;
					const bool blank_ok = WaitForFlip(100000);
					Log("VideoOut: blank flip completed = %s", blank_ok ? "OK" : "FAILED");
					cleanup_ok = cleanup_ok && blank_ok;
				}
				else
					cleanup_ok = false;
			}
			const int result = sceVideoOutUnregisterBuffers(m_handle, m_buffer_set);
			Log("VideoOut: unregister = %#x", result);
			if (result == 0)
				m_buffer_set = -1;
			else
				cleanup_ok = false;
		}
		const int result = sceVideoOutClose(m_handle);
		Log("VideoOut: close = %#x", result);
		if (result != 0)
		{
			Log("VideoOut: cleanup = FAILED (close failed; image memory retained)");
			return false;
		}
		m_handle = -1;
		m_buffer_set = -1;
	}
	if (m_flip_queue_created)
	{
		const int result = sceKernelDeleteEqueue(m_flip_queue);
		Log("VideoOut: delete flip queue = %#x", result);
		cleanup_ok = cleanup_ok && result == 0;
		if (result == 0)
		{
			m_flip_queue_created = false;
			m_flip_queue = 0;
		}
	}
	for (ImageMemory& image : m_images)
	{
		if (image.mapped)
			m_dispatch.UnmapMemory(m_device, image.memory);
		if (image.image)
			m_dispatch.DestroyImage(m_device, image.image, nullptr);
		if (image.memory)
			m_dispatch.FreeMemory(m_device, image.memory, nullptr);
		image = {};
	}
	m_frame_count = 0;
	m_current_index = 0;
	m_pixel_pitch = 0;
	m_flip_pending = false;
	m_acquired = false;
	m_gpu_flips = false;
	Log("VideoOut: cleanup = %s", cleanup_ok ? "OK" : "FAILED");
	return cleanup_ok;
}
