// SPDX-License-Identifier: GPL-3.0+
#include "common/Error.h"
#include "PhysicalDisc.h"
#include "CDVD/CDVDcommon.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <pthread.h>

extern void ProbeLog(const char* format, ...);

namespace
{
	// (AI-assisted) PS4-only CDVD adapter. A failed read never publishes its buffer.
	std::unique_ptr<PS4Disc::Reader> s_prepared, s_reader;
	std::mutex s_lock;
	std::condition_variable s_wake;
	std::condition_variable s_completed;
	pthread_t s_thread{};
	bool s_running = false, s_stop = false, s_request = false, s_tray_open = false;
	u32 s_sector = 0;
	u64 s_generation = 0;
	s32 s_result = -1;
	std::array<u8, 2048> s_buffer{};
	using Clock = std::chrono::steady_clock;
	Clock::time_point s_requested_at{}, s_next_wait_log{};

	void* ReadWorker(void*)
	{
		pthread_setname_np(pthread_self(), "PS4 DVD reader");
		ProbeLog("Physical disc: asynchronous reader started\n");
		std::unique_lock<std::mutex> lock(s_lock);
		for (;;)
		{
			s_wake.wait(lock, [] { return s_stop || s_request; });
			if (s_stop)
				break;
			const u32 sector = s_sector;
			const u64 generation = s_generation;
			const auto requested_at = s_requested_at;
			s_request = false;
			lock.unlock();
			if (generation <= 8)
				ProbeLog("Physical disc: async begin request=%llu LBA=%u\n", static_cast<unsigned long long>(generation), sector);
			std::array<u8, 2048> buffer{};
			const bool success = s_reader->ReadSector(sector, buffer.data());
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - requested_at).count();
			if (generation <= 8 || generation % 1024 == 0 || !success || elapsed >= 250)
				ProbeLog("Physical disc: async complete request=%llu LBA=%u success=%d elapsed_ms=%lld\n",
					static_cast<unsigned long long>(generation), sector, success, static_cast<long long>(elapsed));
			lock.lock();
			if (generation == s_generation)
			{
				s_result = success ? 0 : -1;
				if (success)
					s_buffer = buffer;
				s_completed.notify_all();
			}
		}
		return nullptr;
	}

	void Close()
	{
		if (s_running)
		{
			{
				std::lock_guard<std::mutex> lock(s_lock);
				s_stop = true;
			}
			s_wake.notify_one();
			pthread_join(s_thread, nullptr);
			s_running = false;
		}
		s_reader.reset();
		s_result = -1;
		s_request = false;
		s_generation = 0;
	}
	bool Open(std::string filename, Error* error)
	{
		Close();
		if (filename != PS4Disc::DevicePath || !s_prepared)
		{
			Error::SetString(error, "Open the Disc panel and refresh the physical DVD before starting it.");
			return false;
		}
		s_reader = std::move(s_prepared);
		if (!s_reader->CheckMedia())
		{
			Error::SetString(error, "DVD changed or became unavailable after selection.");
			s_reader.reset();
			return false;
		}
		// Fail opening the source if the VM cannot read the primary descriptor.
		// Otherwise PCSX2 can continue into BIOS with an empty/Unknown Game identity.
		std::array<u8, 2048> descriptor{};
		if (!s_reader->ReadSector(16, descriptor.data()) || descriptor[0] != 1 ||
			std::memcmp(descriptor.data() + 1, "CD001\1", 6) != 0)
		{
			Error::SetString(error, "Physical DVD boot read failed at sector 16. Check the Physical disc log.");
			ProbeLog("Physical disc: boot preflight failed; refusing to start with an unreadable DVD\n");
			s_reader.reset();
			return false;
		}
		ProbeLog("Physical disc: boot preflight OK LBA=16, single-sector reads\n");
		s_stop = false;
		s_tray_open = false;
		pthread_attr_t attr;
		int result = pthread_attr_init(&attr);
		if (result == 0)
		{
			result = pthread_attr_setstacksize(&attr, 256 * 1024);
			if (result == 0)
				result = pthread_create(&s_thread, &attr, ReadWorker, nullptr);
			pthread_attr_destroy(&attr);
		}
		if (result != 0)
		{
			Error::SetString(error, "Could not start the physical DVD reader thread.");
			ProbeLog("Physical disc: pthread_create failed=%d\n", result);
			s_reader.reset();
			return false;
		}
		s_running = true;
		ProbeLog("Physical disc: CDVD mounted %s, asynchronous DVD reads active\n", s_reader->GetInfo().serial.c_str());
		return true;
	}
	bool Precache(ProgressCallback*, Error* error)
	{
		Error::SetString(error, "Physical DVD precaching is unavailable.");
		return false;
	}
	s32 ReadTrack(u32 sector, int mode)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		++s_generation;
		s_result = -1;
		s_request = false;
		if (!s_reader || s_tray_open || mode != CDVD_MODE_2048 || sector >= s_reader->GetInfo().sectors)
			return -1;
		s_sector = sector;
		s_result = -2;
		s_request = true;
		s_requested_at = Clock::now();
		s_next_wait_log = s_requested_at + std::chrono::seconds(1);
		if (s_generation <= 8)
			ProbeLog("Physical disc: async queued request=%llu LBA=%u\n", static_cast<unsigned long long>(s_generation), sector);
		s_wake.notify_one();
		return 0;
	}
	s32 GetBuffer(u8* buffer)
	{
		std::unique_lock<std::mutex> lock(s_lock);
		// (AI-assisted) The reader inherits the EE affinity. Sleep(0) in the core's
		// polling loop is a no-op on PS4; release the CPU while optical I/O is pending.
		// This scheduling change needs proper testing with physical media.
		if (s_result == -2)
			s_completed.wait_for(lock, std::chrono::milliseconds(1), [] { return s_result != -2; });
		if (s_result == -2 && Clock::now() >= s_next_wait_log)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_requested_at).count();
			ProbeLog("Physical disc: async pending request=%llu LBA=%u elapsed_ms=%lld queued=%d\n",
				static_cast<unsigned long long>(s_generation), s_sector, static_cast<long long>(elapsed), s_request);
			s_next_wait_log = Clock::now() + std::chrono::seconds(1);
		}
		if (s_result == 0)
			std::memcpy(buffer, s_buffer.data(), s_buffer.size());
		return s_result;
	}
	s32 ReadSector(u8* buffer, u32 sector, int mode)
	{
		return s_reader && !s_tray_open && mode == CDVD_MODE_2048 && s_reader->ReadSector(sector, buffer) ? 0 : -1;
	}
	s32 SubQ(u32, cdvdSubQ*) { return -1; }
	s32 GetTN(cdvdTN* tn)
	{
		if (!s_reader || s_tray_open)
			return -1;
		tn->strack = tn->etrack = 1;
		return 0;
	}
	s32 GetTD(u8 track, cdvdTD* td)
	{
		if (!s_reader || s_tray_open || track > 1)
			return -1;
		td->lsn = track ? 0 : s_reader->GetInfo().sectors;
		td->type = CDVD_MODE1_TRACK;
		return 0;
	}
	s32 GetDual(s32* type, u32* layer)
	{
		if (!s_reader || s_tray_open)
			return -1;
		*type = s_reader->GetInfo().dual_type;
		*layer = s_reader->GetInfo().layer1_start;
		return 0;
	}
	s32 GetTOC(void* output)
	{
		if (!s_reader || s_tray_open)
			return -1;
		// DVD TOC encoding matches PCSX2's CDVDdiscReader.cpp (PCSX2 Dev Team, GPL-3.0+).
		const auto& info = s_reader->GetInfo();
		auto* toc = static_cast<u8*>(output);
		std::memset(toc, 0, 2048);
		toc[0] = info.dual_type ? 0x24 : 0x04;
		toc[1] = 2;
		toc[2] = 0xf2;
		toc[4] = info.dual_type ? 0x41 : 0x86;
		toc[5] = info.dual_type ? 0x95 : 0x72;
		toc[12] = 1;
		toc[13] = 2;
		toc[14] = info.dual_type == 2 ? 0x31 : info.dual_type == 1 ? 0x21 :
		                                                             1;
		toc[15] = info.dual_type ? 0x10 : 0;
		toc[17] = 3;
		const u32 last = (info.dual_type ? info.layer1_start : info.sectors) + 0x30000 - 1;
		const unsigned offset = info.dual_type == 2 ? 24 : 20;
		for (unsigned i = 0; i < 4; ++i)
			toc[offset + i] = last >> (24 - i * 8);
		return 0;
	}
	s32 DiskType() { return s_reader && !s_tray_open ? (s_reader->GetInfo().dual_type ? CDVD_TYPE_DETCTDVDD : CDVD_TYPE_DETCTDVDS) : CDVD_TYPE_NODISC; }
	s32 TrayStatus() { return s_tray_open ? CDVD_TRAY_OPEN : CDVD_TRAY_CLOSE; }
	s32 TrayOpen()
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_tray_open = true;
		++s_generation;
		s_request = false;
		s_result = -1;
		s_completed.notify_all();
		return 0; // Virtual tray only. Never eject the PS4 drive from emulated commands.
	}
	s32 TrayClose()
	{
		s_tray_open = false;
		return 0;
	}
	void NewDiscCallback(void (*)()) {} // Initial detection is performed by DoCDVDopen; no hot swapping yet.
} // namespace

void PS4Disc::Prepare(std::unique_ptr<Reader> reader) { s_prepared = std::move(reader); }

const CDVD_API CDVDapi_Disc = {
	Close, Open, Precache, ReadTrack, GetBuffer, SubQ, GetTN, GetTD, GetTOC,
	DiskType, TrayStatus, TrayOpen, TrayClose, NewDiscCallback, ReadSector, GetDual};

