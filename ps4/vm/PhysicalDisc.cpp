// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) PS4 CAM discovery follows Al-Azif/ps-disc-detect (see DISC_SUPPORT.md).
#include "PhysicalDisc.h"
#include "DiscCamABI.h"
#include "ForjaLang.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

extern void ProbeLog(const char* format, ...);

namespace PS4Disc
{
	namespace
	{
		uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
		uint32_t LE32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
		void PutBE32(uint8_t* p, uint32_t n)
		{
			p[0] = n >> 24;
			p[1] = n >> 16;
			p[2] = n >> 8;
			p[3] = n;
		}
		bool DVDProfile(uint16_t profile)
		{
			switch (profile)
			{
				case 0xff61:
				case 0x10:
				case 0x11:
				case 0x12:
				case 0x13:
				case 0x14:
				case 0x15:
				case 0x16:
				case 0x1a:
				case 0x1b:
				case 0x2a:
				case 0x2b:
					return true;
				default:
					return false;
			}
		}
	} // namespace

	struct Reader::Impl
	{
		int fd = -1;
		uint32_t path = 0, target = 0, lun = 0, sense = 0;
		uint32_t cam_status = 0;
		uint64_t sectors_read = 0, read_retries = 0;
		Info info;
		std::string error;
		std::mutex mutex;
		bool failed = false;
		std::array<uint8_t, 2048> cache{};
		uint32_t cache_start = 0, cache_count = 0;
		~Impl()
		{
			if (fd >= 0)
			{
				ProbeLog("Physical disc: closing handle sectors_read=%llu read_retries=%llu failed=%d\n",
					static_cast<unsigned long long>(sectors_read), static_cast<unsigned long long>(read_retries), failed);
				close(fd);
			}
		}

		bool SystemError(const char* operation)
		{
			const int code = errno;
			// 2026-10-19: the player reads these in the Disc panel, in the library's language (ForjaLang.h).
			error = code == EACCES || code == EPERM  ? ForjaLang::T("El sistema no deja usar la unidad de disco.") :
			        code == ENOENT || code == ENODEV ? ForjaLang::T("La unidad de disco no está disponible.") :
			                                           ForjaLang::T("Falló la lectura de la unidad. Mira el registro.");
			ProbeLog("Physical disc: %s errno=%d (%s)\n", operation, code, std::strerror(code));
			return false;
		}

		bool Connect()
		{
			const int xpt = open("/dev/xpt0", O_RDWR);
			if (xpt < 0)
				return SystemError("open /dev/xpt0");
			CAM::CCB ccb;
			ccb.Set<uint32_t>(CAM::Function, 3); // XPT_GDEVLIST
			std::memcpy(ccb.bytes + CAM::PeriphName, "cd", 3);
			const int result = ioctl(xpt, CAM::GetPassthrough, &ccb);
			const int saved_errno = errno;
			close(xpt);
			errno = saved_errno;
			if (result < 0)
				return SystemError("CAMGETPASSTHRU cd0");
			if (ccb.Get<uint32_t>(CAM::DeviceStatus) == 3 ||
				std::memcmp(ccb.bytes + CAM::PeriphName, "pass\0", 5) != 0)
			{
				error = ForjaLang::T("La unidad de disco no tiene dispositivo CAM pass.");
				return false;
			}
			char device[64];
			std::snprintf(device, sizeof(device), "/dev/pass%u", ccb.Get<uint32_t>(CAM::Unit));
			fd = open(device, O_RDWR);
			if (fd < 0)
				return SystemError(device);
			ccb = {};
			ccb.Set<uint32_t>(CAM::Function, 3);
			if (ioctl(fd, CAM::GetPassthrough, &ccb) < 0)
				return SystemError("CAMGETPASSTHRU pass");
			if (ccb.Get<uint32_t>(CAM::DeviceStatus) == 3)
			{
				error = ForjaLang::T("No se encontró la dirección de la unidad de disco.");
				return false;
			}
			path = ccb.Get<uint32_t>(CAM::Path);
			target = ccb.Get<uint32_t>(CAM::Target);
			lun = ccb.Get<uint32_t>(CAM::Lun);
			ProbeLog("Physical disc: cd0 -> %s path=%u target=%u lun=%u\n", device, path, target, lun);
			return true;
		}

		bool Command(const uint8_t* cdb, uint8_t length, void* data, uint32_t bytes, bool exact = true, uint32_t minimum = 0, uint32_t timeout_ms = 2000)
		{
			CAM::CCB ccb;
			ccb.Set<uint32_t>(CAM::Retry, 0);
			ccb.Set<uint32_t>(CAM::Function, 0x901); // XPT_SCSI_IO
			ccb.Set<uint32_t>(CAM::Path, path);
			ccb.Set<uint32_t>(CAM::Target, target);
			ccb.Set<uint32_t>(CAM::Lun, lun);
			// CAM_DEV_QFRZDIS: a failed request must not intentionally leave the device queue frozen.
			ccb.Set<uint32_t>(CAM::Flags, (bytes ? 0x40 : 0xc0) | 0x400); // DATA IN / no data; never DATA OUT
			ccb.Set<uint32_t>(CAM::Timeout, timeout_ms);
			ccb.Set<void*>(CAM::Data, data);
			ccb.Set<uint32_t>(CAM::DataSize, bytes);
			ccb.bytes[CAM::SenseSize] = 252;
			ccb.bytes[CAM::CdbSize] = length;
			ccb.bytes[CAM::Tag] = 0x20;
			std::memcpy(ccb.bytes + CAM::Cdb, cdb, length);
			sense = 0;
			cam_status = 0;
			const auto begin = std::chrono::steady_clock::now();
			const int result = ioctl(fd, CAM::Command, &ccb);
			const int saved_errno = errno;
			const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
			if (result < 0)
			{
				ProbeLog("Physical disc: ioctl failed opcode=%02x bytes=%u timeout_ms=%u elapsed_ms=%.2f\n", cdb[0], bytes, timeout_ms, elapsed_ms);
				errno = saved_errno;
				return SystemError("CAMIOCOMMAND");
			}
			const uint32_t status = cam_status = ccb.Get<uint32_t>(CAM::Status);
			const uint32_t residual = ccb.Get<uint32_t>(CAM::Residual);
			if ((status & 0x3f) == 1 && residual <= bytes && bytes - residual >= minimum && (!exact || residual == 0))
			{
				error.clear();
				return true;
			}
			if ((status & 0xbf) == 0x8c && ccb.bytes[CAM::ScsiStatus] == 2)
			{
				const uint8_t* s = ccb.bytes + CAM::Sense;
				if ((s[0] & 0x7e) == 0x72)
					sense = (s[1] & 15) << 16 | s[2] << 8 | s[3];
				else
					sense = (s[2] & 15) << 16 | (s[7] >= 5 ? s[12] << 8 : 0) | (s[7] >= 6 ? s[13] : 0);
			}
			error = (status & 0x3f) == 0x0b        ? ForjaLang::T("La unidad tardó demasiado. Espera y actualiza Disco.") :
			        (sense & 0xffff00) == 0x023a00 ? ForjaLang::T("No hay disco. Mete un DVD de PS2 y actualiza.") :
			        (sense >> 16) == 6             ? ForjaLang::T("Cambió el disco. Actualiza el panel Disco.") :
			        (sense & 0xffff00) == 0x020400 ? ForjaLang::T("La unidad se está preparando. Actualiza en unos segundos.") :
			        (sense & 0xffff00) == 0x023000 ? ForjaLang::T("La unidad no puede leer este tipo de disco.") :
			                                         ForjaLang::T("Falló una orden al disco. Mira el registro y actualiza.");
			ProbeLog("Physical disc: opcode=%02x CAM=%08x SCSI=%02x sense=%06x residual=%u/%u timeout_ms=%u elapsed_ms=%.2f queue_frozen=%d (%s)\n",
				cdb[0], status, ccb.bytes[CAM::ScsiStatus], sense, residual, bytes, timeout_ms, elapsed_ms, (status & 0x40) != 0, error.c_str());
			return false;
		}

		bool Ready()
		{
			const uint8_t cdb[6]{}; // TEST UNIT READY; do not swallow media changes during play.
			return Command(cdb, sizeof(cdb), nullptr, 0, true, 0, 5000);
		}

		bool Read(uint32_t sector, uint32_t count, uint8_t* data)
		{
			if (!count || count > 16 || sector >= info.sectors || count > info.sectors - sector)
				return false;
			// v0.95 identified Iron Man with 2 KiB reads, then timed out prefetching
			// 32 KiB at LBA 16. Use the same single-sector transfer for probe and VM.
			// The fallback still needs proper testing on the physical PS4 drive.
			std::array<uint8_t, 2048> buffer{};
			for (uint32_t i = 0; i < count; ++i)
			{
				uint8_t cdb[12]{0xa8};
				PutBE32(cdb + 2, sector + i);
				PutBE32(cdb + 6, 1);
				if (!Command(cdb, sizeof(cdb), buffer.data(), buffer.size(), true, 0, 5000))
				{
					ProbeLog("Physical disc: single-sector read failed LBA=%u\n", sector + i);
					// Only retry a transport timeout once, after confirming that the same
					// media is still ready. Never swallow Unit Attention or retry removal.
					if ((cam_status & 0x3f) != 0x0b || !Ready())
						return false;
					++read_retries;
					ProbeLog("Physical disc: retry LBA=%u count=1 timeout_ms=8000\n", sector + i);
					if (!Command(cdb, sizeof(cdb), buffer.data(), buffer.size(), true, 0, 8000))
						return false;
					ProbeLog("Physical disc: recovered read LBA=%u\n", sector + i);
				}
				std::memcpy(data + i * 2048, buffer.data(), buffer.size());
				++sectors_read;
				if (sectors_read <= 8 || sectors_read % 1024 == 0)
					ProbeLog("Physical disc: read OK LBA=%u count=1 total_sectors=%llu\n", sector + i, static_cast<unsigned long long>(sectors_read));
			}
			return true;
		}

		// Forja SX2 (compilation 41): `count` sectors in one READ(12) while the drive takes it; after the first refusal, one at a
		// time (Read) for the rest of this handle.
		bool bulk = true;
		bool ReadMany(uint32_t sector, uint32_t count, uint8_t* data)
		{
			if (!count || sector >= info.sectors || count > info.sectors - sector)
				return false;
			if (bulk && count > 1)
			{
				uint8_t cdb[12]{0xa8};
				PutBE32(cdb + 2, sector);
				PutBE32(cdb + 6, count);
				if (Command(cdb, sizeof(cdb), data, count * 2048, true, 0, 10000))
				{
					sectors_read += count;
					return true;
				}
				bulk = false;
				ProbeLog("Physical disc: %u-sector read refused at LBA=%u; one sector at a time from now on\n", count, sector);
				if (!Ready())
					return false;
			}
			for (uint32_t done = 0; done < count; done += 16)
			{
				if (!Read(sector + done, std::min(16u, count - done), data + static_cast<size_t>(done) * 2048))
					return false;
			}
			return true;
		}

		bool Identify()
		{
			if (!Connect())
				return false;
			if (!Ready())
			{
				if ((sense >> 16) != 6 && (cam_status & 0x3f) != 0x0b)
					return false;
				ProbeLog("Physical disc: initial readiness retry CAM=%08x sense=%06x\n", cam_status, sense);
				if (!Ready())
					return false;
			}
			ProbeLog("Physical disc: drive ready; querying DVD profile\n");
			std::array<uint8_t, 2052> response{};
			const uint8_t config[10]{0x46, 0, 0, 0, 0, 0, 0, 8, 4, 0};
			if (!Command(config, sizeof(config), response.data(), response.size(), false, 8))
				return false;
			const uint16_t profile = uint16_t(response[6]) << 8 | response[7];
			ProbeLog("Physical disc: current profile=%04x\n", profile);
			if (!DVDProfile(profile))
			{
				error = profile == 0 ? ForjaLang::T("No hay disco.") : ForjaLang::T("Disco no compatible. Esta versión solo lee DVD de PS2.");
				return false;
			}
			response.fill(0);
			const uint8_t capacity[10]{0x25};
			if (!Command(capacity, sizeof(capacity), response.data(), 8))
				return false;
			const uint32_t last = BE32(response.data());
			if (BE32(response.data() + 4) != 2048 || last < 16 || last >= 0x800000)
			{
				error = ForjaLang::T("Tamaño de sector o capacidad del DVD no compatible.");
				return false;
			}
			info.sectors = last + 1;
			response.fill(0);
			const uint8_t physical[12]{0xad, 0, 0, 0, 0, 0, 0, 0, 8, 4, 0, 0};
			if (!Command(physical, sizeof(physical), response.data(), response.size(), false, 20))
				return false;
			if ((unsigned(response[0]) << 8 | response[1]) < 18)
			{
				error = ForjaLang::T("El DVD no dio su descripción completa.");
				return false;
			}
			const unsigned layers = (response[6] >> 5) & 3;
			if (layers > 1)
			{
				error = ForjaLang::T("Capas del DVD no compatibles.");
				return false;
			}
			if (layers)
			{
				info.dual_type = (response[6] & 0x10) ? 2 : 1;
				const uint32_t first = BE32(response.data() + 8) & 0xffffff;
				const uint32_t end = BE32(response.data() + (info.dual_type == 2 ? 16 : 12)) & 0xffffff;
				if (end < first || end - first + 1 >= info.sectors)
				{
					error = ForjaLang::T("El cambio de capa del DVD no es válido.");
					return false;
				}
				info.layer1_start = end - first + 1;
			}
			// Bounded ISO9660 root traversal. No dependency on the running VM's global CDVD.
			std::array<uint8_t, 2048> pvd{};
			if (!Read(16, 1, pvd.data()))
				return false;
			if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001\1", 6) != 0 ||
				pvd[128] != 0 || pvd[129] != 8 || pvd[156] < 34)
			{
				error = ForjaLang::T("Este DVD no tiene un sistema de archivos ISO9660 compatible.");
				return false;
			}
			const uint32_t root = LE32(pvd.data() + 158), root_size = LE32(pvd.data() + 166);
			if (!root_size || root_size > 65536 || root != BE32(pvd.data() + 162) || root_size != BE32(pvd.data() + 170))
			{
				error = ForjaLang::T("La carpeta raíz del DVD no es compatible.");
				return false;
			}
			const uint32_t blocks = (root_size + 2047) / 2048;
			if (root >= info.sectors || blocks > info.sectors - root)
				return false;
			std::vector<uint8_t> directory(blocks * 2048);
			for (uint32_t i = 0; i < blocks; i += 16)
				if (!Read(root + i, std::min(16u, blocks - i), directory.data() + i * 2048))
					return false;
			for (size_t offset = 0; offset < root_size;)
			{
				const uint8_t* record = directory.data() + offset;
				const unsigned length = record[0];
				if (!length)
				{
					offset = (offset / 2048 + 1) * 2048;
					continue;
				}
				if (length < 34 || length > root_size - offset || length > 2048 - offset % 2048 || record[32] > length - 33)
					break;
				std::string name(reinterpret_cast<const char*>(record + 33), record[32]);
				for (char& c : name)
					c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				if ((name == "SYSTEM.CNF;1" || name == "SYSTEM.CNF") && !(record[25] & 0x82))
				{
					const uint32_t extent = LE32(record + 2), size = LE32(record + 10);
					if (!size || size > 32768 || extent != BE32(record + 6) || size != BE32(record + 14))
						break;
					std::vector<uint8_t> cnf(((size + 2047) / 2048) * 2048);
					if (!Read(extent, static_cast<uint32_t>(cnf.size() / 2048), cnf.data()))
						return false;
					std::string text(reinterpret_cast<const char*>(cnf.data()), size);
					for (char& c : text)
						c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
					size_t pos = 0;
					while (pos < text.size())
					{
						const size_t end = text.find_first_of("\r\n", pos);
						std::string line = text.substr(pos, end == std::string::npos ? end : end - pos);
						line.erase(0, line.find_first_not_of(" \t"));
						if (line.compare(0, 5, "BOOT2") == 0)
						{
							const size_t eq = line.find_first_not_of(" \t", 5);
							const size_t colon = line.find(':', eq);
							if (eq != std::string::npos && line[eq] == '=' && colon != std::string::npos && line.find("CDROM", eq) < colon)
							{
								const size_t first = line.find_first_not_of("\\/ \t", colon + 1);
								if (first != std::string::npos)
								{
									std::string serial = line.substr(first, line.find_first_of("; \t", first) - first);
									const size_t slash = serial.find_last_of("\\/");
									if (slash != std::string::npos)
										serial.erase(0, slash + 1);
									serial.erase(std::remove(serial.begin(), serial.end(), '.'), serial.end());
									std::replace(serial.begin(), serial.end(), '_', '-');
									if (!serial.empty() && serial.size() <= 64)
									{
										info.serial = serial;
										ProbeLog("Physical disc: PS2 DVD %s sectors=%u dual=%d layer1=%u\n", serial.c_str(), info.sectors, info.dual_type, info.layer1_start);
										return true;
									}
								}
							}
						}
						if (end == std::string::npos)
							break;
						pos = end + 1;
					}
					break;
				}
				offset += length;
			}
			error = ForjaLang::T("SYSTEM.CNF no tiene la línea BOOT2 de la PS2.");
			return false;
		}
	};

	Reader::Reader()
		: m_impl(std::make_unique<Impl>())
	{
	}
	Reader::~Reader() = default;
	std::unique_ptr<Reader> Reader::Open(std::string& error)
	{
		ProbeLog("Physical disc: detection started (single-sector read profile)\n");
		auto reader = std::unique_ptr<Reader>(new Reader());
		if (!reader->m_impl->Identify())
		{
			error = reader->m_impl->error.empty() ? std::string(ForjaLang::T("Los datos del disco no son válidos.")) : reader->m_impl->error;
			ProbeLog("Physical disc: probe failed: %s\n", error.c_str());
			return {};
		}
		error.clear();
		return reader;
	}
	const Info& Reader::GetInfo() const { return m_impl->info; }
	bool Reader::CheckMedia()
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		if (m_impl->failed)
			return false;
		if (!m_impl->Ready())
			m_impl->failed = true;
		return !m_impl->failed;
	}
	bool Reader::ReadSectors(uint32_t sector, uint32_t count, uint8_t* buffer)
	{
		auto& r = *m_impl;
		std::lock_guard<std::mutex> lock(r.mutex);
		if (r.failed)
			return false;
		return r.ReadMany(sector, count, buffer);
	}
	bool Reader::ReadSector(uint32_t sector, uint8_t* buffer)
	{
		auto& r = *m_impl;
		std::lock_guard<std::mutex> lock(r.mutex);
		if (r.failed || sector >= r.info.sectors)
			return false;
		if (!r.cache_count || sector < r.cache_start || sector - r.cache_start >= r.cache_count)
		{
			r.cache_count = 0;
			r.cache_start = sector;
			constexpr uint32_t count = 1;
			if (!r.Ready() || !r.Read(sector, count, r.cache.data()))
			{
				r.failed = true; // Never return stale sectors or silently switch media after removal.
				ProbeLog("Physical disc: read stopped at LBA=%u; leave the game and refresh Disc\n", sector);
				return false;
			}
			r.cache_count = count;
		}
		std::memcpy(buffer, r.cache.data() + (sector - r.cache_start) * 2048, 2048);
		return true;
	}
} // namespace PS4Disc
