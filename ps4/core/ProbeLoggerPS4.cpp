// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Keep storage and kernel-log waits off emulation threads.
#include "ProbeLoggerPS4.h"
#include "TelemetryPS4.h"
#include "ThreadAffinityPS4.h"
#include "common/Timer.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string_view>
#include <unistd.h>
#include <orbis/libkernel.h>

extern void ProbeLog(const char* format, ...);

namespace
{
	// (AI-assisted) v0.71 filled 128 slots and dropped another 169 startup lines.
	// Reserve ~2 MiB for bursts; runtime storage stalls must still remain bounded.
	constexpr std::size_t CAPACITY = 512;
	struct Entry
	{
		char text[4096];
		std::size_t length;
	};
	struct Counters
	{
		unsigned long long written = 0, dropped = 0, io_errors = 0;
		unsigned long long kernel_calls = 0;
		std::size_t high_water = 0;
		double sink_ms = 0, write_ms = 0, sync_ms = 0, kernel_ms = 0;
	};
	struct SinkResult
	{
		unsigned int errors = 0, kernel_calls = 0;
		double write_ms = 0, sync_ms = 0, kernel_ms = 0;
	};
	Entry s_queue[CAPACITY];
	pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t s_ready = PTHREAD_COND_INITIALIZER;
	pthread_t s_thread;
	bool s_async = false, s_stopping = false;
	// Forja SX2 (compilation 67): SetDirect's mode, and the worker writing an entry it took from the queue (outside the lock).
	bool s_direct = false, s_worker_busy = false;
	pthread_cond_t s_drained = PTHREAD_COND_INITIALIZER;
	bool s_kernel_mirror = true; // Before Start, no file is available.
	std::size_t s_head = 0, s_count = 0;
	int s_fd = -1;
	Counters s_counters;
	Common::Timer::Value s_last_sync = 0;

	// Only the worker (or synchronous fallback under s_mutex) touches the sink.
	SinkResult WriteSink(const char* text, std::size_t length)
	{
		SinkResult result;
		if (s_fd >= 0)
		{
			const auto write_start = Common::Timer::GetCurrentValue();
			std::size_t offset = 0;
			while (offset < length)
			{
				const ssize_t count = write(s_fd, text + offset, length - offset);
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
				{
					++result.errors;
					break;
				}
				offset += static_cast<std::size_t>(count);
			}
			const auto now = Common::Timer::GetCurrentValue();
			result.write_ms = Common::Timer::ConvertValueToSeconds(now - write_start) * 1000.0;
			const std::string_view message(text, length);
			if (Common::Timer::ConvertValueToSeconds(now - s_last_sync) >= 1.0 ||
				message.starts_with("STAGE:") || message.starts_with("VM "))
			{
				const auto sync_start = Common::Timer::GetCurrentValue();
				int sync_result;
				do
				{
					sync_result = fsync(s_fd);
				} while (sync_result < 0 && errno == EINTR);
				if (sync_result < 0)
					++result.errors;
				result.sync_ms = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - sync_start) * 1000.0;
				s_last_sync = now;
			}
		}
		// (AI-assisted) Keep the full file log. Duplicating every profile line to
		// the kernel is opt-in; retain that fallback when the file cannot be written.
		if (s_kernel_mirror || s_fd < 0 || result.errors != 0)
		{
			const auto kernel_start = Common::Timer::GetCurrentValue();
			sceKernelDebugOutText(0, text);
			result.kernel_ms = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - kernel_start) * 1000.0;
			result.kernel_calls = 1;
		}
		return result;
	}

	// Called under s_mutex, after the worker has finished its unlocked I/O.
	void RecordSink(const SinkResult& result, double elapsed_ms)
	{
		++s_counters.written;
		s_counters.io_errors += result.errors;
		s_counters.kernel_calls += result.kernel_calls;
		s_counters.sink_ms += elapsed_ms;
		s_counters.write_ms += result.write_ms;
		s_counters.sync_ms += result.sync_ms;
		s_counters.kernel_ms += result.kernel_ms;
	}

	void* Run(void*)
	{
		PS4ThreadAffinity::PinCurrentThreadToHelpers("log writer");
		Entry entry;
		for (;;)
		{
			pthread_mutex_lock(&s_mutex);
			while (s_count == 0 && !s_stopping)
				pthread_cond_wait(&s_ready, &s_mutex);
			if (s_count == 0 && s_stopping)
			{
				pthread_mutex_unlock(&s_mutex);
				break;
			}
			entry = s_queue[s_head];
			s_head = (s_head + 1) % CAPACITY;
			--s_count;
			s_worker_busy = true; // compilation 67
			pthread_mutex_unlock(&s_mutex);

			const auto start = Common::Timer::GetCurrentValue();
			const auto result = WriteSink(entry.text, entry.length);
			const double elapsed_ms = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - start) * 1000.0;
			pthread_mutex_lock(&s_mutex);
			RecordSink(result, elapsed_ms);
			s_worker_busy = false; // compilation 67
			pthread_cond_broadcast(&s_drained);
			pthread_mutex_unlock(&s_mutex);
		}
		return nullptr;
	}
} // namespace

void PS4ProbeLogger::SetDirect(bool direct)
{
	pthread_mutex_lock(&s_mutex);
	s_direct = direct;
	pthread_mutex_unlock(&s_mutex);
}

int PS4ProbeLogger::Start(int fd, bool synchronous, bool kernel_mirror)
{
	s_fd = fd;
	s_kernel_mirror = kernel_mirror;
	if (synchronous)
		return 0;
	pthread_attr_t attr;
	int result = pthread_attr_init(&attr);
	if (result == 0)
	{
		result = pthread_attr_setstacksize(&attr, 256 * 1024);
		if (result == 0)
			result = pthread_create(&s_thread, &attr, Run, nullptr);
		pthread_attr_destroy(&attr);
	}
	// Start runs before log producers; failure preserves the old synchronous sink.
	s_async = (result == 0);
	return result;
}

void PS4ProbeLogger::Write(const char* text, std::size_t length)
{
	// Mirror to live telemetry (non-blocking, best-effort). Needs proper testing.
	PS4Telemetry::Write(text, length);

	pthread_mutex_lock(&s_mutex);
	// Compilation 67: in direct mode the line waits for the queue to empty, then is written here, in order.
	while (s_async && s_direct && !s_stopping && (s_count > 0 || s_worker_busy))
		pthread_cond_wait(&s_drained, &s_mutex);
	if (s_async && !s_direct)
	{
		if (s_count == CAPACITY || s_stopping)
			++s_counters.dropped;
		else
		{
			Entry& entry = s_queue[(s_head + s_count) % CAPACITY];
			entry.length = std::min(length, sizeof(entry.text) - 1);
			std::memcpy(entry.text, text, entry.length);
			entry.text[entry.length] = '\0';
			++s_count;
			s_counters.high_water = std::max(s_counters.high_water, s_count);
			pthread_cond_signal(&s_ready);
		}
	}
	else
	{
		const auto start = Common::Timer::GetCurrentValue();
		const auto result = WriteSink(text, length);
		RecordSink(result, Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - start) * 1000.0);
	}
	pthread_mutex_unlock(&s_mutex);
}

void PS4ProbeLogger::LogStatus()
{
	pthread_mutex_lock(&s_mutex);
	const Counters counters = s_counters;
	const auto pending = s_count;
	const bool async = s_async;
	pthread_mutex_unlock(&s_mutex);
	ProbeLog("PS4 log sink: mode=%s pending=%zu capacity=%zu high_water=%zu written=%llu dropped=%llu io_errors=%llu sink_ms=%.2f\n",
		async ? "async" : "sync", pending, CAPACITY, counters.high_water,
		counters.written, counters.dropped, counters.io_errors, counters.sink_ms);
	ProbeLog("PS4 log I/O: kernel_mirror=%d write_ms=%.2f sync_ms=%.2f kernel_ms=%.2f kernel_calls=%llu\n",
		s_kernel_mirror, counters.write_ms, counters.sync_ms, counters.kernel_ms, counters.kernel_calls);
}

void PS4ProbeLogger::Stop()
{
	// Normal shutdown drains all accepted messages. An abrupt crash can lose the
	// queued tail; crash diagnostics and queue pressure need proper console testing.
	pthread_mutex_lock(&s_mutex);
	s_stopping = true;
	const bool async = s_async;
	pthread_cond_signal(&s_ready);
	pthread_mutex_unlock(&s_mutex);
	if (async)
		pthread_join(s_thread, nullptr);
	pthread_mutex_lock(&s_mutex);
	s_async = false;
	char summary[512];
	const int length = std::snprintf(summary, sizeof(summary),
		"PS4 log sink stopped: mode=%s written=%llu dropped=%llu high_water=%zu io_errors=%llu sink_ms=%.2f "
		"write_ms=%.2f sync_ms=%.2f kernel_ms=%.2f kernel_calls=%llu (AI-assisted)\n",
		async ? "async" : "sync", s_counters.written, s_counters.dropped,
		s_counters.high_water, s_counters.io_errors, s_counters.sink_ms,
		s_counters.write_ms, s_counters.sync_ms, s_counters.kernel_ms, s_counters.kernel_calls);
	if (length > 0)
		WriteSink(summary, std::min(static_cast<std::size_t>(length), sizeof(summary) - 1));
	s_fd = -1;
	pthread_mutex_unlock(&s_mutex);
}
