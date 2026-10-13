// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Optional video capture/dump features are unavailable in BIOS Boot.
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSLzma.h"
#include "common/Error.h"
#include "common/Threading.h"
// Video capture needs ffmpeg (absent); bring-up never captures.
bool GSCapture::IsCapturing()
{
	return false;
}
const Threading::ThreadHandle& GSCapture::GetEncoderThreadHandle()
{
	static const Threading::ThreadHandle handle;
	return handle;
}
void GSCapture::DeliverAudioPacket(const float* frames)
{
	(void)frames;
}
void GSCapture::EndCapture()
{
}
bool GSCapture::IsCapturingVideo()
{
	return false;
}
GSVector2i GSCapture::GetSize()
{
	return GSVector2i(0);
}
std::string GSCapture::GetNextCaptureFileName()
{
	return std::string();
}
void GSCapture::Flush()
{
}
bool GSCapture::DeliverVideoFrame(GSTexture* stex)
{
	(void)stex;
	return false;
}
bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
	(void)fps;
	(void)recommendedResolution;
	(void)aspect;
	(void)filename;
	return false;
}

// GS dumps need zlib/lzma/zstd (absent); bring-up never dumps.
bool GSDumpBase::VSync(int field, bool last, const GSPrivRegSet* regs)
{
	(void)field;
	(void)last;
	(void)regs;
	return false;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string& fn, const std::string& serial,
	unsigned int crc, unsigned int screenshot_width, unsigned int screenshot_height, const unsigned int* screenshot_pixels,
	const freezeData& fd, const GSPrivRegSet* regs)
{
	(void)fn;
	(void)serial;
	(void)crc;
	(void)screenshot_width;
	(void)screenshot_height;
	(void)screenshot_pixels;
	(void)fd;
	(void)regs;
	return nullptr;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string& fn, const std::string& serial,
	unsigned int crc, unsigned int screenshot_width, unsigned int screenshot_height, const unsigned int* screenshot_pixels,
	const freezeData& fd, const GSPrivRegSet* regs)
{
	(void)fn;
	(void)serial;
	(void)crc;
	(void)screenshot_width;
	(void)screenshot_height;
	(void)screenshot_pixels;
	(void)fd;
	(void)regs;
	return nullptr;
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string& fn, const std::string& serial,
	unsigned int crc, unsigned int screenshot_width, unsigned int screenshot_height, const unsigned int* screenshot_pixels,
	const freezeData& fd, const GSPrivRegSet* regs)
{
	(void)fn;
	(void)serial;
	(void)crc;
	(void)screenshot_width;
	(void)screenshot_height;
	(void)screenshot_pixels;
	(void)fd;
	(void)regs;
	return nullptr;
}

// Dump factories above refuse capture; there is no active dump object.
void GSDumpBase::Transfer(int index, const u8* mem, size_t size)
{
	(void)index;
	(void)mem;
	(void)size;
}

void GSDumpBase::ReadFIFO(u32) {}
std::unique_ptr<GSDumpFile> GSDumpFile::OpenGSDump(const char*, Error* error)
{
	Error::SetString(error, "GS dump playback is unavailable in BIOS Boot.");
	return nullptr;
}
bool GSDumpFile::ReadFile(Error* error)
{
	Error::SetString(error, "GS dump playback is unavailable in BIOS Boot.");
	return false;
}
