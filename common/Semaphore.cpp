// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/HostSys.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include <cerrno>
#include <limits>
#if defined(PCSX2_PS4)
#include <orbis/libkernel.h>
#endif

// --------------------------------------------------------------------------------------
//  Semaphore Implementations
// --------------------------------------------------------------------------------------

bool Threading::WorkSema::CheckForWork()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));

	// we want to switch to the running state, but preserve the waiting empty bit for RUNNING_N -> RUNNING_0
	// otherwise, we clear the waiting flag (since we're notifying the waiter that we're empty below)
	while (!m_state.compare_exchange_weak(value,
		IsReadyForSleep(value) ? STATE_RUNNING_0 : (value & STATE_FLAG_WAITING_EMPTY),
		std::memory_order_acq_rel, std::memory_order_relaxed))
	{
	}

	// if we're not empty, we have work to do
	if (!IsReadyForSleep(value))
		return true;

	// this means we're empty, so notify any waiters
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();

	// no work to do
	return false;
}

void Threading::WorkSema::WaitForWork()
{
	// State change:
	// SLEEPING, SPINNING: This is the worker thread and it's clearly not asleep or spinning, so these states should be impossible
	// RUNNING_0: Change state to SLEEPING, wake up thread if WAITING_EMPTY
	// RUNNING_N: Change state to RUNNING_0 (and preserve WAITING_EMPTY flag)
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));
	while (!m_state.compare_exchange_weak(value, NextStateWaitForWork(value), std::memory_order_acq_rel, std::memory_order_relaxed))
		;
	if (IsReadyForSleep(value))
	{
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();
		m_sema.Wait();
		// Acknowledge any additional work added between wake up request and getting here
		m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
	}
}

void Threading::WorkSema::WaitForWorkWithSpin()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));
	while (IsReadyForSleep(value))
	{
		if (m_state.compare_exchange_weak(value, STATE_SPINNING, std::memory_order_release, std::memory_order_relaxed))
		{
			if (value & STATE_FLAG_WAITING_EMPTY)
				m_empty_sema.Post();
			value = STATE_SPINNING;
			break;
		}
	}
	u32 waited = 0;
	while (value < 0)
	{
		if (waited > SPIN_TIME_NS)
		{
			if (!m_state.compare_exchange_weak(value, STATE_SLEEPING, std::memory_order_relaxed))
				continue;
			m_sema.Wait();
			break;
		}
		waited += ShortSpin();
		value = m_state.load(std::memory_order_relaxed);
	}
	// Clear back to STATE_RUNNING_0 (but preserve waiting empty flag)
	m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
}

bool Threading::WorkSema::WaitForEmpty()
{
	s32 value = m_state.load(std::memory_order_acquire);
	while (true)
	{
		if (value < 0)
			return !IsDead(value); // STATE_SLEEPING or STATE_SPINNING, queue is empty!
		// Note: We technically only need memory_order_acquire on *failure* (because that's when we could leave without sleeping), but libstdc++ still asserts on failure < success
		if (m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	m_empty_sema.Wait();
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

bool Threading::WorkSema::WaitForEmptyWithSpin()
{
	s32 value = m_state.load(std::memory_order_acquire);
	u32 waited = 0;
	while (true)
	{
		if (value < 0)
			return !IsDead(value); // STATE_SLEEPING or STATE_SPINNING, queue is empty!
		if (waited > SPIN_TIME_NS && m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
		waited += ShortSpin();
		value = m_state.load(std::memory_order_acquire);
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	m_empty_sema.Wait();
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

bool Threading::WorkSema::TimedWaitForEmpty(u32 timeout_ms)
{
	// (AI-assisted) Diagnostic timeout for hang detection. Preserves infinite WaitForEmpty in steady state.
	// Needs proper testing on PS4 timeout semantics and ring drain timing.
	s32 value = m_state.load(std::memory_order_acquire);
	while (true)
	{
		if (value < 0)
			return !IsDead(value);
		if (m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	if (!m_empty_sema.TimedWait(timeout_ms))
	{
		// Clear the wait flag so a subsequent WaitForEmpty can re-arm. The GS may still drain later.
		m_state.fetch_and(~STATE_FLAG_WAITING_EMPTY, std::memory_order_acq_rel);
		return false;
	}
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

void Threading::WorkSema::Kill()
{
	s32 value = m_state.exchange(std::numeric_limits<s32>::min(), std::memory_order_release);
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();
}

void Threading::WorkSema::Reset()
{
	m_state = STATE_RUNNING_0;
}

#if !defined(__APPLE__) // macOS implementations are in DarwinThreads

Threading::KernelSemaphore::KernelSemaphore()
{
#ifdef _WIN32
	m_sema = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
#elif defined(PCSX2_PS4)
	// (AI-assisted) Use the native semaphore instead of assuming libc sem_init is implemented.
	pxAssertRel(sceKernelCreateSema(&m_sema, "PCSX2", ORBIS_KERNEL_SEMA_ATTR_TH_FIFO, 0, std::numeric_limits<s32>::max(), nullptr) == 0,
		"PS4 kernel semaphore creation failed");
#else
	sem_init(&m_sema, false, 0);
#endif
}

Threading::KernelSemaphore::~KernelSemaphore()
{
#ifdef _WIN32
	CloseHandle(m_sema);
#elif defined(PCSX2_PS4)
	pxAssertRel(sceKernelDeleteSema(m_sema) == 0, "PS4 kernel semaphore deletion failed");
#else
	sem_destroy(&m_sema);
#endif
}

void Threading::KernelSemaphore::Post()
{
#ifdef _WIN32
	ReleaseSemaphore(m_sema, 1, nullptr);
#elif defined(PCSX2_PS4)
	pxAssertRel(sceKernelSignalSema(m_sema, 1) == 0, "PS4 kernel semaphore signal failed");
#else
	sem_post(&m_sema);
#endif
}

void Threading::KernelSemaphore::Wait()
{
#ifdef _WIN32
	WaitForSingleObject(m_sema, INFINITE);
#elif defined(PCSX2_PS4)
	pxAssertRel(sceKernelWaitSema(m_sema, 1, nullptr) == 0, "PS4 kernel semaphore wait failed");
#else
	// A signal handler (the PS5 port's EE profiler, vk-285-8) makes sem_wait return EINTR without a post.
	while (sem_wait(&m_sema) != 0 && errno == EINTR)
	{
	}
#endif
}

bool Threading::KernelSemaphore::TryWait()
{
#ifdef _WIN32
	return WaitForSingleObject(m_sema, 0) == WAIT_OBJECT_0;
#elif defined(PCSX2_PS4)
	return sceKernelPollSema(m_sema, 1) == 0;
#else
	return sem_trywait(&m_sema) == 0;
#endif
}

bool Threading::KernelSemaphore::TimedWait(u32 timeout_ms)
{
#ifdef _WIN32
	return WaitForSingleObject(m_sema, timeout_ms) == WAIT_OBJECT_0;
#elif defined(PCSX2_PS4)
	OrbisKernelUseconds usec = static_cast<OrbisKernelUseconds>(timeout_ms) * 1000ULL;
	const int rc = sceKernelWaitSema(m_sema, 1, &usec);
	// 0 = signaled, 0x8002003C = ETIMEDOUT, otherwise error/dead.
	return rc == 0;
#else
	timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec += ts.tv_nsec / 1000000000L;
		ts.tv_nsec %= 1000000000L;
	}
	while (sem_timedwait(&m_sema, &ts) != 0)
	{
		if (errno == ETIMEDOUT)
			return false;
		if (errno != EINTR)
			return false;
	}
	return true;
#endif
}

#endif
