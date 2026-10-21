// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Native PS4 output for the existing PCSX2 SPU2 audio queue.
#include "AudioStreamPS4.h"
#include "ThreadAffinityPS4.h"
#include "Host/AudioStream.h"
#include "common/Error.h"
#include "common/Threading.h"
#include "3rdparty/cubeb/subprojects/speex/speex_resampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>

extern void ProbeLog(const char* format, ...);

// Forja SX2 (compilation 67): a friend's PS4 (a base or Slim one, 2026-10-20) closed Forja at every game's start, four times out
// of four, between "Creating Cubeb audio stream" and the audio thread's first line, with no crash.log: the system ended the app.
// The library's intro, which opens the same port with fewer calls, played its sound. So the opening is marked: Open writes
// FORJA_AUDIO_TRIAL (with the mode it uses) and the thread removes it after its first block is out. If the app finds it at its
// next start (main.cpp, ForjaCheckAudioTrial), the opening killed it: /data/ForjaSX2/audio-modo goes to 1, the simple mode (the
// intro's calls only: no port state), and after a second death to 2, games without sound (main.cpp, the Null backend).
static constexpr const char* FORJA_AUDIO_TRIAL = "/data/ForjaSX2/audio-en-prueba";

int ForjaAudioMode()
{
	static int s_mode = -1;
	if (s_mode < 0)
	{
		s_mode = 0;
		if (std::FILE* file = std::fopen("/data/ForjaSX2/audio-modo", "r"))
		{
			if (std::fscanf(file, "%d", &s_mode) != 1 || s_mode < 0 || s_mode > 2)
				s_mode = 0;
			std::fclose(file);
		}
	}
	return s_mode;
}

namespace
{
	struct OutputCounters
	{
		std::atomic<bool> attempted{false};
		std::atomic<int> status{0}; // 1 = active, 0 = closed, negative = error
		std::atomic<u64> buffers{0}, pcm_buffers{0}, input_short{0}, paused{0};
		std::atomic<u64> refill_pending{0};
		std::atomic<u32> underruns{0}, cushion_q4{4};
		std::atomic<u64> concealed{0};
		std::atomic<u32> queue_min{0}, queue_max{0};
		std::atomic<u32> peak_q15{0}, rms_q15{0};
		std::atomic<u64> invalid_samples{0}, clipped_samples{0};
		std::atomic<u64> limited_frames{0}; // Forja 1.06: frames the limiter turned down (clipped_samples: over full scale before it)
		std::atomic<int> port_result{0}, port_volume{0};
		std::atomic<u32> port_output{0}, port_channels{0};
		std::atomic<u64> port_flags{0};
	};
	OutputCounters s_output;

	class PS4AudioStream final : public AudioStream
	{
	public:
		PS4AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
			// Forja SX2 (compilation 31): the underrun cushion (Host/AudioStream.h) and the soft cuts (Conceal). The marker
			// brings back compilation 30's audio for comparing.
			const bool before = access("/data/ForjaSX2/vm-audio-cortes-antes", F_OK) == 0;
			m_forja_adaptive_cushion = !before;
			m_conceal = !before;
			// Compilation 32: the boot's underruns do not grow the cushion (Run() turns the count on after STARTUP_BLOCKS).
			m_forja_count_underruns = before;
		}

		~PS4AudioStream() override
		{
			m_quit.store(true, std::memory_order_release);
			if (m_thread_started)
				pthread_join(m_thread, nullptr);
			if (m_resampler)
				speex_resampler_destroy(m_resampler);
			// (AI-assisted) The submitted buffer remains alive until the port closes.
			if (m_handle >= 0)
			{
				const int result = sceAudioOutClose(m_handle);
				ProbeLog("PS4 audio: close=%#x\n", result);
			}
			if (s_output.status.load(std::memory_order_relaxed) == 1)
				s_output.status.store(0, std::memory_order_relaxed);
			PS4Audio::LogStatus();
			unlink(FORJA_AUDIO_TRIAL); // compilation 67: a stream closed before its first block (a start that ended) is no death
		}

		void SetPaused(bool paused) override
		{
			// SetStretchEnabled reallocates the queue after pausing. Wait for any
			// in-flight ReadFrames, and keep hardware waits outside this lock.
			std::lock_guard lock(m_read_mutex);
			if (paused != m_paused && m_resampler)
			{
				// Discard buffered samples/filter history when pausing or rebuilding
				// the base queue. Do not play pre-pause audio after resuming.
				speex_resampler_reset_mem(m_resampler);
				m_source_offset = m_source_frames = 0;
			}
			m_paused = paused;
		}

		bool Open(bool stretch_enabled, Error* error)
		{
			s_output.attempted.store(true, std::memory_order_relaxed);
			s_output.status.store(0, std::memory_order_relaxed);
			s_output.buffers.store(0, std::memory_order_relaxed);
			s_output.pcm_buffers.store(0, std::memory_order_relaxed);
			s_output.input_short.store(0, std::memory_order_relaxed);
			s_output.paused.store(0, std::memory_order_relaxed);
			s_output.refill_pending.store(0, std::memory_order_relaxed);
			s_output.underruns.store(0, std::memory_order_relaxed);
			s_output.cushion_q4.store(4, std::memory_order_relaxed);
			s_output.concealed.store(0, std::memory_order_relaxed);
			s_output.queue_min.store(0, std::memory_order_relaxed);
			s_output.queue_max.store(0, std::memory_order_relaxed);
			s_output.peak_q15.store(0, std::memory_order_relaxed);
			s_output.rms_q15.store(0, std::memory_order_relaxed);
			s_output.invalid_samples.store(0, std::memory_order_relaxed);
			s_output.clipped_samples.store(0, std::memory_order_relaxed);
			s_output.limited_frames.store(0, std::memory_order_relaxed);
			s_output.port_result.store(0, std::memory_order_relaxed);
			s_output.port_output.store(0, std::memory_order_relaxed);
			s_output.port_channels.store(0, std::memory_order_relaxed);
			s_output.port_volume.store(0, std::memory_order_relaxed);
			s_output.port_flags.store(0, std::memory_order_relaxed);
			// (AI-assisted) Keep the queue/time-stretcher at the emulated console's
			// native rate and convert only at the hardware output boundary.
			if (m_sample_rate != OUTPUT_RATE && m_sample_rate != 44100)
			{
				Error::SetStringFmt(error, "PS4 audio supports 44100/48000 Hz input; requested {} Hz", m_sample_rate);
				ProbeLog("PS4 audio: unsupported sample_rate=%u; falling back to Null\n", m_sample_rate);
				s_output.status.store(-1, std::memory_order_relaxed);
				return false;
			}
			if (m_sample_rate != OUTPUT_RATE)
			{
				int result = RESAMPLER_ERR_SUCCESS;
				m_resampler = speex_resampler_init(2, m_sample_rate, OUTPUT_RATE, SPEEX_RESAMPLER_QUALITY_DEFAULT, &result);
				if (!m_resampler || result != RESAMPLER_ERR_SUCCESS)
					return Fail("audio resampler creation", result ? result : -1, error);
			}

			// Compilation 67: the mark of an opening under way (see FORJA_AUDIO_TRIAL), with the mode in use.
			m_simple = ForjaAudioMode() >= 1;
			if (std::FILE* trial = std::fopen(FORJA_AUDIO_TRIAL, "w"))
			{
				std::fprintf(trial, "%d\n", m_simple ? 1 : 0);
				std::fclose(trial);
			}
			ProbeLog("PS4 audio: opening in the %s mode (/data/ForjaSX2/audio-modo)\n", m_simple ? "simple" : "normal");
			const int module_result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
			const int init_result = sceAudioOutInit();
			ProbeLog("PS4 audio: module=%#x init=%#x\n", module_result, init_result);
			if (init_result < 0 && static_cast<u32>(init_result) != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT)
				return Fail("sceAudioOutInit", init_result, error);

			m_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
				0, GRAIN, OUTPUT_RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
			ProbeLog("PS4 audio: open=%#x input_rate=%u output_rate=%u resampler=%s channels=2 grain=%u format=s16_stereo stretch=%s buffer_ms=%u\n",
				m_handle, m_sample_rate, OUTPUT_RATE, m_resampler ? "speex" : "bypass", GRAIN, stretch_enabled ? "on" : "off", m_parameters.buffer_ms);
			if (m_handle < 0)
				return Fail("sceAudioOutOpen", m_handle, error);

			// (AI-assisted) Match DolphinPS4's main-port PCM16 and explicit 0 dB
			// gain. The upstream queue already applies the user's volume/mute.
			if (!m_simple) // compilation 67: the simple mode does only what the library's intro does
				RefreshPortState();
			ProbeLog("PS4 audio port before gain: result=%#x output=%#x channels=%u volume_raw=%d flags=%#llx\n",
				s_output.port_result.load(), s_output.port_output.load(), s_output.port_channels.load(),
				s_output.port_volume.load(), static_cast<unsigned long long>(s_output.port_flags.load()));
			int32_t levels[2] = {32768, 32768};
			const int volume_result = sceAudioOutSetVolume(m_handle, 0x3, levels);
			ProbeLog("PS4 audio: set_volume=%#x left=32768 right=32768 (0 dB)\n", volume_result);
			if (volume_result < 0)
				return Fail("sceAudioOutSetVolume", volume_result, error);
			if (!m_simple)
				RefreshPortState();

			BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
			ProbeLog("PS4 audio stretch: sequence_ms=%u seek_ms=%u overlap_ms=%u quick_seek=%u target_ms=%u\n",
				m_parameters.stretch_sequence_length_ms, m_parameters.stretch_seekwindow_ms,
				m_parameters.stretch_overlap_ms, static_cast<unsigned>(m_parameters.stretch_use_quickseek),
				m_parameters.buffer_ms);
			ProbeLog("PS4 audio underruns: cushion %s (up to %u ms), soft cuts %s (/data/ForjaSX2/vm-audio-cortes-antes turns both off)\n",
				m_forja_adaptive_cushion ? "adaptive" : "off", static_cast<unsigned>(m_parameters.buffer_ms) * 5 / 2, m_conceal ? "on" : "off");
			pthread_attr_t attr;
			int result = pthread_attr_init(&attr);
			if (result == 0)
			{
				result = pthread_attr_setstacksize(&attr, 256 * 1024);
				if (result == 0)
				{
					s_output.status.store(1, std::memory_order_relaxed);
					result = pthread_create(&m_thread, &attr, ThreadEntry, this);
				}
				pthread_attr_destroy(&attr);
			}
			if (result != 0)
				return Fail("audio worker creation", result, error);
			m_thread_started = true;
			return true;
		}

	private:
		static constexpr u32 GRAIN = 512;
		static constexpr u32 OUTPUT_RATE = 48000;
		static constexpr u64 STARTUP_BLOCKS = 20ull * OUTPUT_RATE / GRAIN; // 20 s of output
		// Forja 1.06: the output's limiter (the loop in the output thread). God of War II's mix reaches full scale, and after the
		// stretching and the resampling some samples went past it: 1,104 cut off flat in the first seconds of one movie
		// (2026-10-21), heard as crackles. A frame that would pass the ceiling turns the gain down to fit at once, and the gain
		// comes back in ~50 ms; both channels together, so the stereo picture stays. Under the ceiling nothing changes.
		static constexpr float LIMIT_CEILING = 0.98f; // -0.18 dB
		static constexpr float LIMIT_RELEASE = 1.0f / (0.05f * OUTPUT_RATE);

		// Called with m_read_mutex held. Keep unconsumed input and filter state
		// across hardware blocks: 512 output frames are not 512 PS1 input frames.
		// silent: every read of this block came back as underrun silence.
		bool ReadOutputFrames(u64& input_short, u64& refill_pending, u32& queue_min, u32& queue_max, bool& silent)
		{
			u32 reads = 0, silent_reads = 0;
			const auto read_source = [&](float* samples) {
				const u32 queued = GetBufferedFramesRelaxed();
				input_short += queued < GRAIN;
				queue_min = std::min(queue_min, queued);
				queue_max = std::max(queue_max, queued);
				// ReadFrames plays silence while it refills after an underrun, or when the queue is empty; with a few
				// frames it stretches them over the block instead.
				const bool was_filling = m_filling;
				ReadFrames(samples, GRAIN);
				reads++;
				silent_reads += (was_filling && m_filling) || (!was_filling && queued == 0);
				refill_pending += m_filling;
			};
			silent = false;
			if (!m_resampler)
			{
				read_source(m_samples);
				silent = silent_reads == reads;
				return true;
			}
			u32 produced = 0;
			while (produced < GRAIN)
			{
				if (!m_source_frames)
				{
					read_source(m_source);
					m_source_frames = GRAIN;
					m_source_offset = 0;
				}
				spx_uint32_t consumed = m_source_frames, written = GRAIN - produced;
				const int result = speex_resampler_process_interleaved_float(m_resampler,
					m_source + m_source_offset * 2, &consumed, m_samples + produced * 2, &written);
				if (result != RESAMPLER_ERR_SUCCESS || (consumed == 0 && written == 0))
					return Fail("audio resampler process", result ? result : -1, nullptr);
				m_source_offset += consumed;
				m_source_frames -= consumed;
				produced += written;
			}
			silent = reads != 0 && silent_reads == reads;
			return true;
		}

		// Forja SX2 (compilation 31): an underrun cut the music to dead silence from one sample to the next, a click, and
		// brought it back the same way. The first silent block now plays the last block backwards, so it starts on the very
		// sample the audio stopped on, fading to zero over its 10.7 ms; the block where the audio comes back fades in over
		// 2.7 ms. The gap is still there while the queue refills, but without the clicks.
		void Conceal(bool silent, u64& concealed)
		{
			static constexpr u32 FADE_IN = 128;
			if (silent)
			{
				if (m_last_audible)
				{
					for (u32 i = 0; i < GRAIN; i++)
					{
						const float gain = static_cast<float>(GRAIN - 1 - i) / static_cast<float>(GRAIN);
						const u32 from = GRAIN - 1 - i;
						m_samples[i * 2] = m_last_block[from * 2] * gain;
						m_samples[i * 2 + 1] = m_last_block[from * 2 + 1] * gain;
					}
					concealed++;
				}
				m_last_audible = false;
				return;
			}
			if (!m_last_audible)
			{
				for (u32 i = 0; i < FADE_IN; i++)
				{
					const float gain = static_cast<float>(i) / static_cast<float>(FADE_IN);
					m_samples[i * 2] *= gain;
					m_samples[i * 2 + 1] *= gain;
				}
			}
			std::memcpy(m_last_block, m_samples, sizeof(m_samples));
			m_last_audible = true;
		}

		void RefreshPortState()
		{
			OrbisAudioOutPostState state{};
			static_assert(sizeof(state) == 32);
			const int result = sceAudioOutGetPortState(m_handle, &state);
			s_output.port_result.store(result, std::memory_order_relaxed);
			// A failed query must not look like a new disconnected/muted state.
			if (result < 0)
				return;
			s_output.port_output.store(state.output, std::memory_order_relaxed);
			s_output.port_channels.store(state.channel, std::memory_order_relaxed);
			s_output.port_volume.store(state.volume, std::memory_order_relaxed);
			s_output.port_flags.store(state.flag, std::memory_order_relaxed);
		}

		static bool Fail(const char* operation, int result, Error* error)
		{
			unlink(FORJA_AUDIO_TRIAL); // compilation 67: a failure that Forja survives (the Null stream follows) is no death
			Error::SetStringFmt(error, "{} failed: {:#x}", operation, static_cast<u32>(result));
			ProbeLog("PS4 audio: %s failed=%#x\n", operation, result);
			s_output.status.store(result < 0 ? result : -result, std::memory_order_relaxed);
			return false;
		}

		static void* ThreadEntry(void* context)
		{
			static_cast<PS4AudioStream*>(context)->Run();
			return nullptr;
		}

		void Run()
		{
			Threading::SetNameOfCurrentThread("PS4 AudioOut");
			PS4ThreadAffinity::PinCurrentThread("Audio", 5);
			u64 buffers = 0, pcm_buffers = 0, input_short = 0, paused_buffers = 0;
			u64 invalid_samples = 0, clipped_samples = 0;
			u64 limited_frames = 0;  // Forja 1.06
			float limit_gain = 1.0f; // Forja 1.06: the limiter's gain now (LIMIT_CEILING)
			u64 refill_pending = 0, concealed = 0;
			u32 queue_min = UINT32_MAX, queue_max = 0;
			u32 current_buffer = 0, window_frames = 0;
			float window_peak = 0.0f, window_energy = 0.0f;
			while (!m_quit.load(std::memory_order_acquire))
			{
				{
					std::lock_guard lock(m_read_mutex);
					if (m_paused)
					{
						std::memset(m_samples, 0, sizeof(m_samples));
						paused_buffers++;
						m_last_audible = false; // the game comes back with a fade in
					}
					else
					{
						bool silent = false;
						if (!ReadOutputFrames(input_short, refill_pending, queue_min, queue_max, silent))
							return;
						if (m_conceal)
							Conceal(silent, concealed);
					}
				}
				float peak = 0.0f;
				for (u32 i = 0; i < GRAIN * 2; i += 2)
				{
					float frame[2] = {m_samples[i], m_samples[i + 1]};
					for (float& sample : frame)
					{
						if (!std::isfinite(sample))
						{
							invalid_samples++;
							sample = 0.0f;
						}
						clipped_samples += sample < -1.0f || sample > 1.0f;
					}
					// Forja 1.06: the limiter (LIMIT_CEILING), on the frame's louder channel.
					const float frame_peak = std::max(std::abs(frame[0]), std::abs(frame[1]));
					if (frame_peak * limit_gain > LIMIT_CEILING)
					{
						limit_gain = LIMIT_CEILING / frame_peak;
						limited_frames++;
					}
					for (u32 c = 0; c < 2; c++)
					{
						const float sample = std::clamp(frame[c] * limit_gain, -1.0f, 1.0f);
						// Scaling/clamping before conversion keeps both signed endpoints valid.
						const s32 pcm = static_cast<s32>(sample * 32768.0f);
						m_pcm[current_buffer][i + c] = static_cast<s16>(std::clamp(pcm, -32768, 32767));
						peak = std::max(peak, std::abs(sample));
						window_energy += sample * sample;
					}
					limit_gain += (1.0f - limit_gain) * LIMIT_RELEASE;
				}
				// (AI-assisted) Alternate buffers as in DolphinPS4. Output paces the
				// loop; do not drain/restart the port for every block. Needs proper
				// testing with console capture, pause/resume and shutdown.
				const int result = sceAudioOutOutput(m_handle, m_pcm[current_buffer]);
				if (result < 0)
				{
					Fail("sceAudioOutOutput submit", result, nullptr);
					return;
				}
				current_buffer ^= 1;
				buffers++;
				if (buffers == 1)
				{
					unlink(FORJA_AUDIO_TRIAL); // compilation 67: the sound is out, the opening went well
					ProbeLog("PS4 audio: the first block is out (%s mode)\n", m_simple ? "simple" : "normal");
				}
				if (buffers == STARTUP_BLOCKS)
					m_forja_count_underruns = true; // ReadFrames, the one that counts them, runs on this thread too
				if (peak > 0.00001f)
					pcm_buffers++;
				s_output.buffers.store(buffers, std::memory_order_relaxed);
				s_output.pcm_buffers.store(pcm_buffers, std::memory_order_relaxed);
				s_output.input_short.store(input_short, std::memory_order_relaxed);
				s_output.paused.store(paused_buffers, std::memory_order_relaxed);
				s_output.refill_pending.store(refill_pending, std::memory_order_relaxed);
				s_output.concealed.store(concealed, std::memory_order_relaxed);
				s_output.underruns.store(m_forja_underruns.load(std::memory_order_relaxed), std::memory_order_relaxed);
				s_output.cushion_q4.store(m_forja_cushion_q4.load(std::memory_order_relaxed), std::memory_order_relaxed);
				window_peak = std::max(window_peak, peak);
				window_frames += GRAIN;
				if (window_frames >= OUTPUT_RATE)
				{
					s_output.queue_min.store(queue_min == UINT32_MAX ? 0 : queue_min, std::memory_order_relaxed);
					s_output.queue_max.store(queue_max, std::memory_order_relaxed);
					queue_min = UINT32_MAX;
					queue_max = 0;
					s_output.peak_q15.store(static_cast<u32>(window_peak * 32768.0f), std::memory_order_relaxed);
					s_output.rms_q15.store(static_cast<u32>(std::sqrt(window_energy / (window_frames * 2)) * 32768.0f),
						std::memory_order_relaxed);
					s_output.invalid_samples.store(invalid_samples, std::memory_order_relaxed);
					s_output.clipped_samples.store(clipped_samples, std::memory_order_relaxed);
					s_output.limited_frames.store(limited_frames, std::memory_order_relaxed);
					if (!m_simple)
						RefreshPortState();
					window_frames = 0;
					window_peak = window_energy = 0.0f;
				}
			}
			const int result = sceAudioOutOutput(m_handle, nullptr);
			if (result < 0)
				Fail("sceAudioOutOutput final drain", result, nullptr);
		}

		int m_handle = -1;
		pthread_t m_thread{};
		bool m_thread_started = false;
		bool m_simple = false; // compilation 67: ForjaAudioMode() 1
		std::atomic<bool> m_quit{false};
		std::mutex m_read_mutex;
		SpeexResamplerState* m_resampler = nullptr;
		u32 m_source_offset = 0, m_source_frames = 0;
		bool m_conceal = false;
		bool m_last_audible = false; // the last block played carried the game's audio (not underrun silence)
		alignas(64) float m_last_block[GRAIN * 2]{};
		alignas(64) float m_source[GRAIN * 2]{};
		alignas(64) float m_samples[GRAIN * 2]{};
		alignas(64) s16 m_pcm[2][GRAIN * 2]{};
	};
} // namespace

void PS4Audio::LogStatus()
{
	if (!s_output.attempted.load(std::memory_order_relaxed))
		return;
	ProbeLog("PS4 audio output: status=%d buffers=%llu pcm_buffers=%llu input_short=%llu paused=%llu\n",
		s_output.status.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.buffers.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.pcm_buffers.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.input_short.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.paused.load(std::memory_order_relaxed)));
	ProbeLog("PS4 audio queue: min_frames=%u max_frames=%u refill_pending=%llu underruns=%u soft_cuts=%llu cushion=%u%%\n",
		s_output.queue_min.load(std::memory_order_relaxed), s_output.queue_max.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.refill_pending.load(std::memory_order_relaxed)),
		s_output.underruns.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.concealed.load(std::memory_order_relaxed)),
		s_output.cushion_q4.load(std::memory_order_relaxed) * 25);
	ProbeLog("PS4 audio signal: peak_q15=%u rms_q15=%u invalid_samples=%llu clipped_samples=%llu limited_frames=%llu "
			 "port_result=%#x output=%#x channels=%u volume_raw=%d flags=%#llx\n",
		s_output.peak_q15.load(std::memory_order_relaxed), s_output.rms_q15.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.invalid_samples.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.clipped_samples.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.limited_frames.load(std::memory_order_relaxed)),
		s_output.port_result.load(std::memory_order_relaxed), s_output.port_output.load(std::memory_order_relaxed),
		s_output.port_channels.load(std::memory_order_relaxed), s_output.port_volume.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.port_flags.load(std::memory_order_relaxed)));
}

// (AI-assisted) Reuse the backend slot supplied by the Orbis shim; no cubeb library.
std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
	return {};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char*)
{
	return {};
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	const char*, const char*, bool stretch_enabled, Error* error)
{
	AudioStreamParameters stereo = parameters;
	stereo.expansion_mode = AudioExpansionMode::Disabled;
	auto stream = std::make_unique<PS4AudioStream>(sample_rate, stereo);
	if (!stream->Open(stretch_enabled, error))
		return nullptr;
	return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32, const AudioStreamParameters&, bool, Error* error)
{
	Error::SetString(error, "SDL audio unavailable on PS4; use native AudioOut");
	return nullptr;
}
