// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// (AI-assisted) Small POSIX host layer for the OpenOrbis interpreter POC.

#include "common/Assertions.h"
#include "common/HostSys.h"

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>

// OpenOrbis' SDK header has an outdated by-value declaration for this API;
// the console ABI writes the result through the pointer, as used by the PS5
// port's existing call sites.
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t* size);
extern "C" size_t sceKernelGetDirectMemorySize(void);
extern "C" int sceKernelReserveVirtualRange(void** addr, size_t size, int flags, size_t alignment);
extern "C" int sceKernelMmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset, void** result);
extern "C" int sceKernelMunmap(void* addr, size_t size);

static constexpr size_t ORBIS_FASTMEM_AREA_SIZE = 0x100000000ULL;
static constexpr int ORBIS_MAP_FIXED = 0x10;

// (AI-assisted) Minimal PS4 frontends can link this backend; a frontend's diagnostic logger overrides it.
__attribute__((weak)) void pxOnAssertFail(const char* file, int line, const char* function, const char* message)
{
	std::fprintf(stderr, "PS4 assertion %s:%d %s: %s\n", file, line, function, message);
	std::abort();
}

u64 GetTickFrequency()
{
	return 1000000000ULL;
}

u64 GetCPUTicks()
{
	struct timespec ts = {};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (static_cast<u64>(ts.tv_sec) * 1000000000ULL) + static_cast<u64>(ts.tv_nsec);
}

u64 GetPhysicalMemory()
{
	// OpenOrbis exposes the system's direct-memory pool size, which is the
	// relevant ceiling for this homebrew process, rather than PC-style RAM.
	return static_cast<u64>(sceKernelGetDirectMemorySize());
}

u64 GetAvailablePhysicalMemory()
{
	size_t available = 0;
	if (sceKernelAvailableFlexibleMemorySize(&available) != 0)
		return 0;

	return static_cast<u64>(available);
}

std::string GetOSVersionString()
{
	return "Orbis OS (PS4 homebrew)";
}

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	int prot = PROT_NONE;
	if (mode.CanRead())
		prot |= PROT_READ;
	if (mode.CanWrite())
		prot |= PROT_WRITE;
	if (mode.CanExecute())
		prot |= PROT_EXEC;

	if (mprotect(baseaddr, size, prot) != 0)
		pxFail("mprotect() failed on PS4");
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	char name[96];
	std::snprintf(name, sizeof(name), "/%s_%u", prefix, static_cast<unsigned>(getpid()));
	return name;
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		return nullptr;
	shm_unlink(name);
	if (ftruncate(fd, static_cast<off_t>(size)) != 0)
	{
		close(fd);
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}

void HostSys::DestroySharedMemory(void* ptr)
{
	pxAssertRel(close(static_cast<int>(reinterpret_cast<intptr_t>(ptr))) == 0, "Cannot close PS4 shared memory handle");
}

size_t HostSys::GetRuntimePageSize()
{
	return 0x4000;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	return 64;
}

// (AI-assisted) POSIX shared views for PS4, confined to an area owned by this object.
SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "Shared views must be removed before area release");
	if (m_size == ORBIS_FASTMEM_AREA_SIZE)
		pxAssertRel(sceKernelMunmap(m_base_ptr, m_size) == 0, "Cannot release PS4 Fastmem virtual range");
	else
		pxAssertRel(munmap(m_base_ptr, m_size) == 0, "Cannot release PS4 shared mapping area");
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	if (jit || size == 0 || (size & (__pagesize - 1)) != 0)
		return nullptr;
	// (AI-assisted) PS4 mmap sends PROT_NONE anonymous ranges through the flexible-memory allocator.
	// Fastmem needs a 4 GiB address reservation, not 4 GiB of flexible memory, so use the kernel VMM API.
	if (size == ORBIS_FASTMEM_AREA_SIZE)
	{
		void* base = nullptr;
		const int rc = sceKernelReserveVirtualRange(&base, size, 0, 0);
		std::printf("[PS4] Fastmem virtual reservation: size=%zu rc=0x%x base=%p\n", size,
			static_cast<unsigned>(rc), rc == 0 ? base : nullptr);
		std::fflush(stdout);
		if (rc != 0 || !base)
			return nullptr;
		return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(static_cast<u8*>(base), size,
			size / __pagesize));
	}
	void* base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return nullptr;
	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(static_cast<u8*>(base), size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	const uptr offset = reinterpret_cast<uptr>(map_base) - reinterpret_cast<uptr>(m_base_ptr);
	if (map_size == 0 || offset > m_size || map_size > m_size - offset ||
		((offset | map_size | file_offset) & (__pagesize - 1)) != 0)
		return nullptr;
	const int prot = (mode.CanRead() ? PROT_READ : 0) | (mode.CanWrite() ? PROT_WRITE : 0) | (mode.CanExecute() ? PROT_EXEC : 0);
	if (m_size == ORBIS_FASTMEM_AREA_SIZE)
	{
		const int fd = file_handle ? static_cast<int>(reinterpret_cast<intptr_t>(file_handle)) : -1;
		const int flags = (file_handle ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS) | MAP_FIXED;
		void* mapped = map_base;
		const int rc = sceKernelMmap(map_base, map_size, prot, flags, fd, static_cast<off_t>(file_offset), &mapped);
		if (rc != 0 || mapped != map_base)
		{
			static int s_logged = 0;
			if (s_logged++ < 16)
				std::printf("[PS4] Fastmem page map failed: addr=%p size=%zu offset=0x%zx rc=0x%x result=%p\n",
					map_base, map_size, file_offset, static_cast<unsigned>(rc), mapped);
			return nullptr;
		}
		m_num_mappings++;
		return static_cast<u8*>(mapped);
	}
	if (file_handle)
	{
		const int fd = static_cast<int>(reinterpret_cast<intptr_t>(file_handle));
		if (mmap(map_base, map_size, prot, MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(file_offset)) == MAP_FAILED)
			return nullptr;
	}
	else if (mprotect(map_base, map_size, prot) != 0)
		return nullptr;
	m_num_mappings++;
	return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool)
{
	const uptr offset = reinterpret_cast<uptr>(map_base) - reinterpret_cast<uptr>(m_base_ptr);
	if (!m_num_mappings || map_size == 0 || offset > m_size || map_size > m_size - offset ||
		((offset | map_size) & (__pagesize - 1)) != 0)
		return false;
	if (m_size == ORBIS_FASTMEM_AREA_SIZE)
	{
		if (sceKernelMunmap(map_base, map_size) != 0)
			return false;
		void* reserved = map_base;
		if (sceKernelReserveVirtualRange(&reserved, map_size, ORBIS_MAP_FIXED, 0) != 0 || reserved != map_base)
			return false;
		m_num_mappings--;
		return true;
	}
	if (mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
		return false;
	m_num_mappings--;
	return true;
}
