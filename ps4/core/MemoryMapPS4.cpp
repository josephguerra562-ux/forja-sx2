// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// (AI-assisted) POSIX memory-map backend shared by PCSX2 startup and PS4 probes.

#include "MemoryMapPS4.h"

#include "pcsx2/Memory.h"
#include "common/HostSys.h"
#include "common/Assertions.h"

#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace SysMemory
{
	u8* s_data_memory = nullptr;
	void* s_data_memory_file_handle = nullptr;
	u8* s_code_memory = nullptr;
} // namespace SysMemory

namespace HostMemoryMap
{
	extern "C" {
	__attribute__((visibility("default"), used)) uptr EEmem, IOPmem, VUmem;
	}
} // namespace HostMemoryMap

// ps4/vm/PlatformVM.cpp: a "STAGE:" line with the PS4's free flexible memory (weak: the probes link without it).
extern "C" void orbis_stage(const char* stage) __attribute__((weak));

bool SysMemory::AllocateMemoryMap()
{
	if (s_data_memory && s_code_memory)
		return true;

	std::printf("[PS4] memmap: MainSize=%u CodeSize=%u\n", HostMemoryMap::MainSize, HostMemoryMap::CodeSize);
	if (orbis_stage)
		orbis_stage("PS2 memory: before its block");
	s_data_memory_file_handle = HostSys::CreateSharedMemory(
		HostSys::GetFileMappingName("pcsx2").c_str(), HostMemoryMap::MainSize);
	if (!s_data_memory_file_handle)
		return false;

	const int data_fd = static_cast<int>(reinterpret_cast<intptr_t>(s_data_memory_file_handle));
	void* data = mmap(nullptr, HostMemoryMap::MainSize, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
	if (data == MAP_FAILED)
	{
		HostSys::DestroySharedMemory(s_data_memory_file_handle);
		s_data_memory_file_handle = nullptr;
		return false;
	}
	if (orbis_stage)
		orbis_stage("PS2 memory block mapped (shared memory, not written yet)");

	void* code = mmap(nullptr, HostMemoryMap::CodeSize, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED)
	{
		munmap(data, HostMemoryMap::MainSize);
		HostSys::DestroySharedMemory(s_data_memory_file_handle);
		s_data_memory_file_handle = nullptr;
		return false;
	}

	s_data_memory = static_cast<u8*>(data);
	s_code_memory = static_cast<u8*>(code);
	HostMemoryMap::EEmem = reinterpret_cast<uptr>(s_data_memory + HostMemoryMap::EEmemOffset);
	HostMemoryMap::IOPmem = reinterpret_cast<uptr>(s_data_memory + HostMemoryMap::IOPmemOffset);
	HostMemoryMap::VUmem = reinterpret_cast<uptr>(s_data_memory + HostMemoryMap::VUmemOffset);
	std::printf("[PS4] memmap: data=%p code=%p\n", static_cast<void*>(s_data_memory), static_cast<void*>(s_code_memory));
	if (orbis_stage)
		orbis_stage("JIT code block mapped (system flexible memory)");
	return true;
}

void SysMemory::ReleaseMemoryMap()
{
	// (AI-assisted) Require successful teardown before clearing mapping ownership.
	if (s_code_memory)
	{
		pxAssertRel(munmap(s_code_memory, HostMemoryMap::CodeSize) == 0, "Cannot release PS4 core code map");
		s_code_memory = nullptr;
	}
	if (s_data_memory)
	{
		pxAssertRel(munmap(s_data_memory, HostMemoryMap::MainSize) == 0, "Cannot release PS4 core data map");
		s_data_memory = nullptr;
	}
	if (s_data_memory_file_handle)
	{
		HostSys::DestroySharedMemory(s_data_memory_file_handle);
		s_data_memory_file_handle = nullptr;
	}
	HostMemoryMap::EEmem = HostMemoryMap::IOPmem = HostMemoryMap::VUmem = 0;
}

u8* SysMemory::GetDataPtr(size_t offset)
{
	return (s_data_memory && offset <= HostMemoryMap::MainSize) ? s_data_memory + offset : nullptr;
}

u8* SysMemory::GetCodePtr(size_t offset)
{
	return (s_code_memory && offset <= HostMemoryMap::CodeSize) ? s_code_memory + offset : nullptr;
}

void* SysMemory::GetDataFileHandle()
{
	return s_data_memory_file_handle;
}
