// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Read-only PS4 DVD transport. CD/raw audio media are not supported.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace PS4Disc
{
	inline constexpr const char* DevicePath = "/dev/cd0";
	struct Info
	{
		std::string serial;
		uint32_t sectors = 0;
		uint32_t layer1_start = 0;
		int dual_type = 0;
	};

	class Reader
	{
	public:
		~Reader();
		static std::unique_ptr<Reader> Open(std::string& error);
		const Info& GetInfo() const;
		bool ReadSector(uint32_t sector, uint8_t* buffer);
		// Forja SX2 (compilation 41): the disc copy's reads (DiscCopy.cpp): `count` sectors in one command while the drive
		// takes it, else one at a time. Unlike ReadSector, a failed read doesn't stop the handle: CheckMedia says if the disc is
		// still there.
		bool ReadSectors(uint32_t sector, uint32_t count, uint8_t* buffer);
		bool CheckMedia();

	private:
		Reader();
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	// CPU thread only: transfer the menu's freshly probed handle into CDVD open.
	void Prepare(std::unique_ptr<Reader> reader);
} // namespace PS4Disc
