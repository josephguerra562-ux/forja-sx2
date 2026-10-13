#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
// Orbis audio shims: cubeb/SDL drivers don't exist here (libSceAudioOut later).
#include "Host/AudioStream.h"
#include "common/Error.h"
#include "USB/usb-mic/audiodev.h"
#include "USB/usb-mic/audiodev-cubeb.h"

// (AudioDevice::CreateDevice/GetInputDeviceList live in usb-mic.cpp already.)

// (AI-assisted) The standalone PS4 VM supplies its synchronized native output.


// No cubeb on Orbis: null device (silence), empty lists.
namespace usb_mic
{
namespace audiodev_cubeb
{
CubebAudioDevice::CubebAudioDevice(AudioDir dir, u32 channels, std::string devname, s32 latency)
  : AudioDevice(dir, channels)
  , mDeviceName(std::move(devname))
{
  (void)dir;
  (void)channels;
  (void)latency;
  mContext = nullptr;
  mDeviceId = nullptr;
}
CubebAudioDevice::~CubebAudioDevice()
{
}
std::vector<std::pair<std::string, std::string>> CubebAudioDevice::GetDeviceList(bool input)
{
  (void)input;
  return {};
}
uint32_t CubebAudioDevice::GetBuffer(int16_t* buff, uint32_t frames)
{
  (void)buff;
  (void)frames;
  return 0;
}
uint32_t CubebAudioDevice::SetBuffer(int16_t* buff, uint32_t frames)
{
  (void)buff;
  (void)frames;
  return 0;
}
bool CubebAudioDevice::GetFrames(uint32_t* size)
{
  if (size)
    *size = 0;
  return false;
}
void CubebAudioDevice::SetResampling(int samplerate)
{
  (void)samplerate;
}
bool CubebAudioDevice::Start()
{
  return false;
}
void CubebAudioDevice::Stop()
{
}
void CubebAudioDevice::ResetBuffers()
{
}
long CubebAudioDevice::DataCallback(struct cubeb_stream* stream, void* user_ptr, void const* input_buffer,
  void* output_buffer, long nframes)
{
  (void)stream;
  (void)user_ptr;
  (void)input_buffer;
  (void)output_buffer;
  return nframes;
}
} // namespace audiodev_cubeb
} // namespace usb_mic
