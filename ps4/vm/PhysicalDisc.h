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
		bool CheckMedia();

	private:
		Reader();
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	// CPU thread only: transfer the menu's freshly probed handle into CDVD open.
	void Prepare(std::unique_ptr<Reader> reader);
} // namespace PS4Disc
