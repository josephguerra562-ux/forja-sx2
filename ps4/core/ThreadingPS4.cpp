// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/Pcsx2Defs.h"
#include <memory>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <ctime>

void Threading::Timeslice()
{
	sched_yield();
}

void Threading::SpinWait()
{
#if defined(ARCH_X86)
	__asm__("pause");
#endif
}

void Threading::EnableHiresScheduler()
{
}

void Threading::DisableHiresScheduler()
{
}

u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

static u64 get_thread_time(uptr id = 0)
{
	clockid_t cid;
	if (id)
	{
		int err = pthread_getcpuclockid(static_cast<pthread_t>(id), &cid);
		if (err)
			return 0;
		// (AI-assisted) FreeBSD 9 returns a caller-relative clock for any handle.
		// Cross-thread reads must not report the reading thread's time as the target.
		if (cid == CLOCK_THREAD_CPUTIME_ID && !pthread_equal(static_cast<pthread_t>(id), pthread_self()))
			return 0;
	}
	else
	{
		cid = CLOCK_THREAD_CPUTIME_ID;
	}

	struct timespec ts;
	int err = clock_gettime(cid, &ts);
	if (err)
		return 0;

	return (static_cast<u64>(ts.tv_sec) * 1000000ULL) + (static_cast<u64>(ts.tv_nsec) / 1000ULL);
}

u64 Threading::GetThreadCpuTime()
{
	return get_thread_time();
}

void Threading::SetNameOfCurrentThread(const char* name)
{
	pthread_setname_np(pthread_self(), name);
}

void Threading::Sleep(int ms)
{
	if (ms <= 0)
		return;
	struct timespec ts{ms / 1000, (ms % 1000) * 1000000};
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
	{
	}
}

void Threading::SleepUntil(u64 ticks)
{
	struct timespec target{static_cast<time_t>(ticks / 1000000000ULL), static_cast<long>(ticks % 1000000000ULL)};
	struct timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (target.tv_sec < now.tv_sec || (target.tv_sec == now.tv_sec && target.tv_nsec <= now.tv_nsec))
		return;
	struct timespec wait{target.tv_sec - now.tv_sec, target.tv_nsec - now.tv_nsec};
	if (wait.tv_nsec < 0)
	{
		wait.tv_sec--;
		wait.tv_nsec += 1000000000;
	}
	while (nanosleep(&wait, &wait) != 0 && errno == EINTR)
	{
	}
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = reinterpret_cast<void*>(pthread_self());
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	if (!m_native_handle)
		return 0;
	return get_thread_time(reinterpret_cast<uptr>(m_native_handle));
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	for (u32 i = 0; i < sizeof(u64) * 8; i++)
	{
		if (processor_mask & (1ULL << i))
			CPU_SET(i, &cpuset);
	}
	return pthread_setaffinity_np(reinterpret_cast<pthread_t>(m_native_handle), sizeof(cpuset), &cpuset) == 0;
}

void* Threading::Thread::ThreadProc(void* param)
{
	auto func = *static_cast<std::function<void()>*>(param);
	delete static_cast<std::function<void()>*>(param);
	func();
	return nullptr;
}

Threading::Thread::Thread(EntryPoint func)
{
	Start(func);
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(std::move(thread))
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::~Thread()
{
	if (m_native_handle && !pthread_equal(reinterpret_cast<pthread_t>(m_native_handle), pthread_self()))
	{
		pthread_detach(reinterpret_cast<pthread_t>(m_native_handle));
	}
	m_native_handle = nullptr;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(std::move(thread));
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

void Threading::Thread::SetStackSize(u32 size)
{
	m_stack_size = size;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pthread_attr_t attrs;
	pthread_attr_init(&attrs);
	if (m_stack_size)
		pthread_attr_setstacksize(&attrs, (m_stack_size + 0x3fff) & ~0x3fff);
	auto* func_copy = new std::function<void()>(std::move(func));
	pthread_t thread;
	int err = pthread_create(&thread, &attrs, ThreadProc, func_copy);
	pthread_attr_destroy(&attrs);
	if (err != 0)
	{
		delete func_copy;
		return false;
	}
	m_native_handle = reinterpret_cast<void*>(thread);
	m_stack_size = 0;
	return true;
}

void Threading::Thread::Detach()
{
	if (m_native_handle)
		pthread_detach(reinterpret_cast<pthread_t>(m_native_handle));
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	if (m_native_handle)
		pthread_join(reinterpret_cast<pthread_t>(m_native_handle), nullptr);
	m_native_handle = nullptr;
}
