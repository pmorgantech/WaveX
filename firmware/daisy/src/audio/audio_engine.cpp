#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include <daisy.h>  // For CpuLoadMeter

#include "../memory.h"
#include "arm_math.h"  // For CMSIS-DSP helpers
#include "audio_engine.h"
#include "comm/daisy_uart_link.h"
#include "config/hardware_config.h"
#include "daisy_core.h"  // For memory sections
#include "ff.h"
#include "profiling/profiler.h"
#include "stm32h7xx_ll_cortex.h"  // For ARM atomic operations
#include "sys/dma.h"              // For cache management

#include "../cv/cv_group_router.hpp"
#include "../sampler.hpp"
#include "../timebase.hpp"
#include "output_sink.hpp"
#include "voice_manager.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// CV backend selection (architecture.md §5.3, roadmap Phase 1 item 1).
#if WAVEX_CV_BACKEND == WAVEX_CV_BACKEND_MCP4728
#include "../cv/mcp4728_backend.hpp"
using CvBackendType = WaveX::Cv::Mcp4728Backend;
static_assert(WAVEX_ANALOG_CV_GROUPS == 1,
              "Mcp4728Backend is a single physical chip serving exactly one CV "
              "group (Stage A); use WAVEX_CV_BACKEND_MCP48 for "
              "WAVEX_ANALOG_CV_GROUPS > 1 (Stage B)");
#else
#include "../cv/mcp48_backend.hpp"
using CvBackendType = WaveX::Cv::Mcp48Backend;
#endif

// Output sink selection (architecture.md §5.3, roadmap Phase 1 item 1).
// Declared here so both flag sets are proven to compile; not yet wired into
// Callback() below - see the VoiceManager comment right after this block
// for why.
#if WAVEX_VOICE_OUTPUT_BACKEND == WAVEX_VOICE_OUTPUT_STEREO_MIX
using OutputSinkType = WaveX::AudioEngine::StereoMixSink;
#else
using OutputSinkType = WaveX::AudioEngine::TdmVoiceSink;
#endif

using namespace daisy;
using namespace daisysp;
using namespace WaveX::Protocol;

namespace WaveX {
namespace AudioEngine {

PROFILE_DEFINE_ZONE(audio_callback);
PROFILE_DEFINE_ZONE(wav_pump_io);
PROFILE_DEFINE_ZONE(format_conversion);
PROFILE_DEFINE_ZONE(ring_buffer_push);
PROFILE_DEFINE_ZONE(prebuffer_audio);
PROFILE_DEFINE_ZONE(sd_refill);

static DaisySeed* s_hw = nullptr;

static constexpr uint32_t kMaxMixChannels = 8;
static constexpr uint32_t kScratchPoolBytes = 32 * 1024;
static constexpr uint32_t kScratchPoolSamples = kScratchPoolBytes / sizeof(q15_t);
static constexpr uint32_t kSdBufferCount = 3;
static constexpr uint32_t kSdBufferAlignment = 32;
static uint32_t s_output_channels = static_cast<uint32_t>(AudioOutputMode::StereoSAI1);
static q15_t s_scratch_pool[kScratchPoolSamples];
static uint32_t s_scratch_offset = 0;

// Parameters
struct AudioParameters {
    float volume = 1.0f;
    float filter_cutoff = 2000.0f;
    float filter_resonance = 0.5f;
    float envelope_attack = 0.01f;
    float envelope_decay = 0.1f;
    float envelope_sustain = 0.7f;
    float envelope_release = 0.5f;
    float lfo_rate = 1.0f;
    float lfo_depth = 0.1f;
};
static AudioParameters s_params;

// DSP objects
static Svf s_filter;
static Adsr s_envelope;
static Oscillator s_lfo;
static Oscillator s_oscillator;
static Sampler s_sampler;
static CvBackendType s_cv_backend;
static WaveX::Cv::CvGroupRouter<CvBackendType, WAVEX_ANALOG_CV_GROUPS> s_cv_router(s_cv_backend);
static OutputSinkType s_output_sink;
// Roadmap Phase 1 item 2: 8-voice RAM-resident player (allocation/stealing,
// per-voice gain/pan/pitch - see voice_manager.hpp). Constructed but not yet
// driven by Callback() below: OnNoteOn/OnNoteOff still only touch the test
// oscillator (s_oscillator), because there is no note-to-sample mapping
// policy yet (which MIDI note plays which loaded sample) - that's item 8's
// job, "MIDI note path ... -> voice manager". Wiring Trigger()/Release()/
// Render() into the real-time callback ahead of a real trigger source would
// be unverifiable risk (no hardware here to confirm audio correctness) for
// no behavioral change.
static WaveX::AudioEngine::VoiceManager s_voice_manager;

// Sample RAM Manager (for loaded samples)
static SampleMemMgr s_sample_mem_mgr;

static volatile float s_env_level = 0.0f;
static float s_last_in_block[Timebase::kBlockSize] = {0};
static size_t s_last_block_size = 0;
static BlockMeters s_last_block_meters = {0, 0, 0, 0};

// CPU Load Meter for audio processing performance monitoring
static CpuLoadMeter s_cpu_load_meter;
static float s_sample_rate = 48000.0f;
static int s_block_size = 48;

// Underrun detection state - set in callback, logged in main loop
static volatile bool s_underrun_detected = false;
static bool s_underrun_logged = false;

// Preview state
static std::vector<int16_t> s_preview;
static uint32_t s_prev_sent = 0;

static void SendPreviewChunks() {
    // Prefer to send the entire preview in one frame if it fits.
    constexpr uint16_t kMaxSingleFrameSamples = 900;  // header + 900*2 < 2048 payload limit
    if (s_preview.size() <= kMaxSingleFrameSamples) {
        WaveX::Protocol::WaveChunkMessage header{};
        header.offset = 0;
        header.count = static_cast<uint16_t>(s_preview.size());

        const size_t payload_bytes =
            sizeof(header) + static_cast<size_t>(header.count) * sizeof(int16_t);
        std::vector<uint8_t> payload(payload_bytes);
        memcpy(payload.data(), &header, sizeof(header));
        memcpy(payload.data() + sizeof(header), s_preview.data(), header.count * sizeof(int16_t));

        int res = WaveX::Comm::UartLinkSend(
            WaveX::Protocol::MSG_WAVE_CHUNK, payload.data(), static_cast<uint16_t>(payload_bytes));
        if (s_hw) {
            s_hw->PrintLine("DAISY: Sending wave chunk (single) offset=0 count=%u res=%d",
                            (unsigned)header.count,
                            res);
        }
        return;
    }

    constexpr uint16_t kChunkSamples = 256;

    while (s_prev_sent < s_preview.size()) {
        uint16_t remaining = static_cast<uint16_t>(
            std::min<uint32_t>(kChunkSamples, s_preview.size() - s_prev_sent));

        WaveX::Protocol::WaveChunkMessage header{};
        header.offset = s_prev_sent;
        header.count = remaining;

        const size_t payload_bytes =
            sizeof(header) + static_cast<size_t>(remaining) * sizeof(int16_t);
        std::vector<uint8_t> payload(payload_bytes);
        memcpy(payload.data(), &header, sizeof(header));
        memcpy(payload.data() + sizeof(header),
               s_preview.data() + s_prev_sent,
               remaining * sizeof(int16_t));

        int res = WaveX::Comm::UartLinkSend(
            WaveX::Protocol::MSG_WAVE_CHUNK, payload.data(), static_cast<uint16_t>(payload_bytes));
        if (s_hw) {
            s_hw->PrintLine("DAISY: Sending wave chunk: offset=%u count=%u res=%d",
                            (unsigned)header.offset,
                            (unsigned)header.count,
                            res);
        }

        s_prev_sent += remaining;
    }
}

// Envelope gate state
static bool s_envelope_gate = false;

// Little-endian helpers
static inline uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ============================
// WAV playback state
// ============================
struct WavState {
    bool open;
    FIL file;
    uint32_t data_start;
    uint32_t data_size;
    uint32_t bytes_remaining;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
};
static WavState s_wav = {};

// ============================
// Sample audition state (separate from main WAV playback)
// ============================
struct AuditionState {
    bool active;
    FIL file;
    uint32_t data_start;
    uint32_t data_size;
    uint32_t bytes_remaining;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
    char current_path[96];
};
static AuditionState s_audition = {};

// ============================
// Sample loading state
// ============================
struct SampleLoadState {
    wxsamp_t handle = {};
    uint16_t sample_id = 0;
    uint32_t expected_size = 0;
    uint32_t received_size = 0;
    bool loading = false;
    uint16_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bit_depth = 0;
};
static SampleLoadState s_sample_load = {};
// FIL structure is large (~600 bytes with SDMMC sector buffer); keep it static in normal BSS
// (matches the audition/playback path). Keep I/O buffers in normal BSS as well, but 32-byte aligned
// so cache maintenance in the SD driver works correctly.
static FIL s_sample_load_file;
alignas(32) static uint8_t s_sample_hdr[64];
alignas(32) static uint8_t s_sample_io[1024];

// ============================
// Loaded sample registry (for diagnostics)
// ============================
struct LoadedSampleInfo {
    wxsamp_t handle = {};
    uint16_t sample_id = 0;
    uint32_t allocated_bytes = 0;
    uint32_t loaded_bytes = 0;
    uint16_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bit_depth = 0;
};
static std::vector<LoadedSampleInfo> s_loaded_samples;

static LoadedSampleInfo* find_loaded_sample(uint16_t sample_id) {
    for (auto& entry: s_loaded_samples) {
        if (entry.sample_id == sample_id) {
            return &entry;
        }
    }
    return nullptr;
}

static void remove_loaded_sample(uint16_t sample_id) {
    for (auto it = s_loaded_samples.begin(); it != s_loaded_samples.end(); ++it) {
        if (it->sample_id == sample_id) {
            // Release the allocation tied to this entry
            s_sample_mem_mgr.release(&it->handle);
            s_loaded_samples.erase(it);
            return;
        }
    }
}

static void upsert_loaded_sample(const SampleLoadMessage& sl, const wxsamp_t& handle) {
    // Replace any existing entry for this sample_id
    remove_loaded_sample(sl.sample_id);

    LoadedSampleInfo info;
    info.sample_id = sl.sample_id;
    info.handle = handle;
    info.allocated_bytes = handle.len ? handle.len : sl.sample_size;
    info.loaded_bytes = 0;
    info.sample_rate = sl.sample_rate;
    info.channels = sl.channels;
    info.bit_depth = sl.bit_depth;
    s_loaded_samples.push_back(info);
}

static void update_loaded_sample_progress(uint16_t sample_id, uint32_t loaded_bytes) {
    if (auto* entry = find_loaded_sample(sample_id)) {
        entry->loaded_bytes = loaded_bytes;
    }
}

// Thread-safe single-producer (main loop) / single-consumer (audio IRQ) ring buffer of interleaved
// q15_t frames Uses ARM Cortex-M7 atomic operations and memory barriers for race-condition-free
// operation This eliminates warbling caused by timing variations between main loop and audio
// callback Using regular memory - this buffer is NOT accessed by DMA, only by CPU (main loop
// writes, IRQ reads) Moving out of DMA memory saves 8KB from the 32KB RAM_D2_DMA limit
static const uint32_t RB_CAP_FRAMES = 2048;
static volatile uint32_t s_rb_head = 0;
static volatile uint32_t s_rb_tail = 0;
static q15_t s_rb[RB_CAP_FRAMES * kMaxMixChannels];

// Pre-buffering system for smooth playback start
static const uint32_t PREBUFFER_FRAMES =
    1024;  // ~23ms at 44.1kHz (much more responsive for auditioning)
static q15_t s_prebuffer[PREBUFFER_FRAMES * kMaxMixChannels];  // ~23ms of interleaved audio
static uint32_t s_prebuffer_filled = 0;                        // Number of frames pre-buffered
static bool s_prebuffer_ready = false;  // Whether pre-buffer is ready for playback
static bool s_prebuffering = false;     // Whether we're currently pre-buffering

// Background SD I/O system with larger buffers
static const uint32_t SD_BUFFER_SIZE = 8192;  // 8KB SD read buffer for better performance
static constexpr uint32_t SD_INT16_CAPACITY = SD_BUFFER_SIZE / sizeof(int16_t);
struct SdBufferSlot {
    alignas(kSdBufferAlignment) uint8_t data[SD_BUFFER_SIZE];
    uint32_t bytes = 0;
    uint32_t frames = 0;
    uint32_t consumed = 0;
    bool ready = false;
};
static SdBufferSlot s_sd_buffers[kSdBufferCount];
static uint32_t s_sd_fill_index = 0;
static uint32_t s_sd_consume_index = 0;
static q15_t
    s_conversion_buffer[SD_INT16_CAPACITY * kMaxMixChannels];  // Scratch for conversions/resample
alignas(kSdBufferAlignment) static uint8_t s_prebuffer_sd[SD_BUFFER_SIZE];
// Audio performance instrumentation
static uint32_t s_io_start_time = 0;
static uint32_t s_io_duration = 0;
static uint32_t s_max_io_duration = 0;
static uint32_t s_io_count = 0;
static uint32_t s_last_io_log = 0;
static uint32_t s_last_io_time = 0;  // Last time we did SD I/O (for rate limiting)
static uint32_t s_dwt_callback_cycles = 0;
static uint32_t s_dwt_callback_max = 0;
static uint32_t s_dwt_io_cycles = 0;
static uint32_t s_dwt_io_max = 0;

// Helpers
static inline void ResetScratchPool() {
    s_scratch_offset = 0;
}

static inline q15_t* AcquireScratch(uint32_t samples) {
    if (s_scratch_offset + samples > kScratchPoolSamples) {
        return nullptr;
    }
    q15_t* ptr = &s_scratch_pool[s_scratch_offset];
    s_scratch_offset += samples;
    return ptr;
}

static inline q15_t ReadSample16(const uint8_t* src) {
    int16_t sample = static_cast<int16_t>(src[0] | (src[1] << 8));
    return sample;
}

static inline q15_t ReadSample24(const uint8_t* src) {
    int32_t sample = static_cast<int32_t>(src[0]) | (static_cast<int32_t>(src[1]) << 8) |
                     (static_cast<int32_t>(static_cast<int8_t>(src[2])) << 16);
    return static_cast<q15_t>(sample >> 8);
}

static uint32_t ConvertFramesToOutput(
    const uint8_t* src, q15_t* dst, uint32_t frames, uint16_t src_channels, uint8_t bit_depth) {
    const uint32_t bytes_per_sample = (bit_depth == 24) ? 3u : 2u;
    const uint32_t src_stride = bytes_per_sample * src_channels;

    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t* frame_ptr = src + i * src_stride;
        q15_t sample_vals[2];
        sample_vals[0] = (bit_depth == 24) ? ReadSample24(frame_ptr) : ReadSample16(frame_ptr);
        if (src_channels > 1) {
            sample_vals[1] = (bit_depth == 24) ? ReadSample24(frame_ptr + bytes_per_sample)
                                               : ReadSample16(frame_ptr + bytes_per_sample);
        } else {
            sample_vals[1] = sample_vals[0];
        }

        for (uint32_t ch = 0; ch < s_output_channels; ++ch) {
            q15_t value = 0;
            if (ch < src_channels) {
                value = sample_vals[ch];
            }
            dst[i * s_output_channels + ch] = value;
        }
    }

    return frames;
}

// Linear resampler using CMSIS-DSP arm_linear_interp_q15.
static uint32_t LinearResampleFrames(
    const q15_t* src, uint32_t src_frames, q15_t* dst, uint32_t channels, float ratio) {
    if (ratio <= 0.0f || src_frames < 2) {
        return 0;
    }

    const float step = 1.0f / ratio;
    // Upper bound on output frames; caller allocates using this ratio.
    uint32_t max_output_frames =
        static_cast<uint32_t>(std::ceil((static_cast<float>(src_frames - 1)) * ratio)) + 1;

    // Determine exact output count with the chosen step so every channel uses the same positions.
    uint32_t output_frames = 0;
    {
        float position = 0.0f;
        while ((position + 1.0f) < static_cast<float>(src_frames) &&
               output_frames < max_output_frames) {
            output_frames++;
            position += step;
        }
    }
    if (output_frames == 0) {
        return 0;
    }

    // Scratch buffer for one channel of contiguous samples
    q15_t* ch_buf = AcquireScratch(src_frames);
    if (!ch_buf) {
        return 0;
    }

    // Interpolate per channel using CMSIS arm_linear_interp_q15
    for (uint32_t ch = 0; ch < channels; ++ch) {
        // De-interleave once per channel
        for (uint32_t i = 0; i < src_frames; ++i) {
            ch_buf[i] = src[i * channels + ch];
        }

        float position = 0.0f;
        for (uint32_t out = 0; out < output_frames; ++out) {
            if ((position + 1.0f) >= static_cast<float>(src_frames)) {
                break;
            }
            // arm_linear_interp_q15 expects a 12.20 fixed-point fractional index
            q31_t x_q31 = static_cast<q31_t>(position * 1048576.0f);
            q15_t sample = arm_linear_interp_q15(ch_buf, x_q31, src_frames);
            dst[out * channels + ch] = sample;
            position += step;
        }
    }

    return output_frames;
}

// Resampling temporarily disabled

// Pre-buffering functions
static bool prebuffer_audio() {
    PROFILE_SCOPE(prebuffer_audio);

    if (!s_wav.open || s_prebuffer_ready) {
        return true;
    }

    s_prebuffering = true;
    ResetScratchPool();
    // Calculate bytes per frame (bytes per sample * channels)
    uint32_t bytes_per_sample = (s_wav.bits_per_sample == 24) ? 3u : 2u;
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * bytes_per_sample;
    uint32_t free_prebuffer_frames = PREBUFFER_FRAMES - s_prebuffer_filled;
    float resample_ratio = 1.0f;
    if (s_wav.sample_rate != s_sample_rate) {
        resample_ratio = static_cast<float>(s_sample_rate) / static_cast<float>(s_wav.sample_rate);
    }
    uint32_t frames_to_read = free_prebuffer_frames;

    if (frames_to_read == 0) {
        s_prebuffer_ready = true;
        s_prebuffering = false;
#if WAVEX_DAISY_SD_DEBUG
        if (s_hw)
            s_hw->PrintLine("Pre-buffer complete: %u frames ready", (unsigned)PREBUFFER_FRAMES);
#endif
        return true;
    }

    // Calculate how much to read
    uint32_t max_frames = SD_BUFFER_SIZE / file_bpf;
    uint32_t req_frames = (frames_to_read < max_frames) ? frames_to_read : max_frames;

    // Ensure the resampled output fits into the remaining pre-buffer space
    if (resample_ratio > 1.0f) {
        if (free_prebuffer_frames <= 1) {
            s_prebuffer_ready = true;
            s_prebuffering = false;
            return true;
        }
        uint32_t max_input_by_space =
            static_cast<uint32_t>(static_cast<float>(free_prebuffer_frames - 1) / resample_ratio);
        if (max_input_by_space == 0) {
            s_prebuffer_ready = true;
            s_prebuffering = false;
            return true;
        }
        if (req_frames > max_input_by_space) {
            req_frames = max_input_by_space;
        }
    }
    uint32_t req_bytes = req_frames * file_bpf;

    if (req_bytes > s_wav.bytes_remaining) {
        req_bytes = s_wav.bytes_remaining;
        req_frames = req_bytes / file_bpf;
    }

    if (req_bytes == 0) {
        // End of file reached during pre-buffering
        s_prebuffer_ready = true;
        s_prebuffering = false;
#if WAVEX_DAISY_SD_DEBUG
        if (s_hw)
            s_hw->PrintLine("Pre-buffer complete: %u frames (end of file)",
                            (unsigned)s_prebuffer_filled);
#endif
        return true;
    }

    // Read from SD card
    UINT br = 0;
    s_io_start_time = System::GetTick();  // Start timing
    FRESULT fr = f_read(&s_wav.file, s_prebuffer_sd, req_bytes, &br);
    s_io_duration = System::GetTick() - s_io_start_time;  // End timing

    // Track I/O performance
    s_io_count++;
    if (s_io_duration > s_max_io_duration) {
        s_max_io_duration = s_io_duration;
    }

    // Log I/O performance every 100 operations
    if (s_io_count % 100 == 0) {
        if (s_hw)
            s_hw->PrintLine("SD I/O Stats: count=%u, max_duration=%u ms, last_duration=%u ms",
                            (unsigned)s_io_count,
                            (unsigned)s_max_io_duration,
                            (unsigned)s_io_duration);
    }

    if (fr != FR_OK || br == 0) {
#if WAVEX_DAISY_SD_DEBUG
        if (s_hw)
            s_hw->PrintLine("Pre-buffer read error: fr=%d, br=%u", (int)fr, (unsigned)br);
#endif
        s_prebuffering = false;
        return false;
    }

    s_wav.bytes_remaining -= br;

    uint32_t frames_read = br / file_bpf;
    q15_t* conversion_output = AcquireScratch(frames_read * s_output_channels);
    if (conversion_output == nullptr) {
        // Fallback: write directly into prebuffer
        conversion_output = &s_prebuffer[s_prebuffer_filled * s_output_channels];
    }

    const uint8_t* src = s_prebuffer_sd;
    PROFILE_SCOPE(format_conversion);
    ConvertFramesToOutput(
        src, conversion_output, frames_read, s_wav.num_channels, s_wav.bits_per_sample);

    q15_t* to_push = conversion_output;
    uint32_t output_frames = frames_read;
    if (resample_ratio != 1.0f) {
        uint32_t max_out_frames =
            static_cast<uint32_t>(std::ceil(frames_read * resample_ratio)) + 1;
        q15_t* resample_buffer = AcquireScratch(max_out_frames * s_output_channels);
        if (resample_buffer != nullptr) {
            uint32_t resampled = LinearResampleFrames(
                conversion_output, frames_read, resample_buffer, s_output_channels, resample_ratio);
            if (resampled > 0) {
                to_push = resample_buffer;
                output_frames = resampled;
            }
        }
    }

    if (output_frames > free_prebuffer_frames) {
        output_frames = free_prebuffer_frames;
    }

    // Push into the pre-buffer
    q15_t* dst = &s_prebuffer[s_prebuffer_filled * s_output_channels];
    arm_copy_q15(to_push, dst, output_frames * s_output_channels);
    s_prebuffer_filled += output_frames;

#if WAVEX_DAISY_SD_DEBUG
    if (s_hw)
        s_hw->PrintLine("Pre-buffer progress: %u/%u frames",
                        (unsigned)s_prebuffer_filled,
                        (unsigned)PREBUFFER_FRAMES);
#endif

    if (s_prebuffer_filled >= PREBUFFER_FRAMES) {
        s_prebuffer_ready = true;
        s_prebuffering = false;
    }

    return true;
}

// Background SD I/O functions
static bool refill_sd_buffer() {
    if (!s_wav.open)
        return false;

    PROFILE_SCOPE(sd_refill);

    uint32_t bytes_per_sample = (s_wav.bits_per_sample == 24) ? 3u : 2u;
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * bytes_per_sample;
    uint32_t max_frames = SD_BUFFER_SIZE / file_bpf;

    for (uint32_t attempt = 0; attempt < kSdBufferCount; ++attempt) {
        uint32_t idx = (s_sd_fill_index + attempt) % kSdBufferCount;
        SdBufferSlot& slot = s_sd_buffers[idx];
        if (slot.ready) {
            continue;
        }

        uint32_t req_frames = max_frames;
        uint32_t req_bytes = req_frames * file_bpf;

        if (req_bytes > s_wav.bytes_remaining) {
            req_bytes = s_wav.bytes_remaining;
            req_frames = req_bytes / file_bpf;
        }

        if (req_bytes == 0) {
            f_lseek(&s_wav.file, s_wav.data_start);
            s_wav.bytes_remaining = s_wav.data_size;
            req_frames = std::min(max_frames, s_wav.bytes_remaining / file_bpf);
            req_bytes = req_frames * file_bpf;
#if WAVEX_DAISY_SD_DEBUG
            if (s_hw)
                s_hw->PrintLine("WAV loop: rewinding to data start");
#endif
        }

        if (req_bytes == 0)
            return false;

        UINT br = 0;
        s_io_start_time = System::GetTick();
        FRESULT fr = f_read(&s_wav.file, slot.data, req_bytes, &br);
        s_io_duration = System::GetTick() - s_io_start_time;

        if (fr != FR_OK || br == 0) {
#if WAVEX_DAISY_SD_DEBUG
            if (s_hw)
                s_hw->PrintLine("WAV read error: fr=%d, br=%u", (int)fr, (unsigned)br);
#endif
            return false;
        }

        slot.bytes = br;
        slot.frames = br / file_bpf;
        slot.consumed = 0;
        slot.ready = true;
        s_sd_fill_index = (idx + 1) % kSdBufferCount;
        s_wav.bytes_remaining -= br;

        s_io_count++;
        if (s_io_duration > s_max_io_duration) {
            s_max_io_duration = s_io_duration;
        }

        return true;
    }

    return true;
}

// Thread-safe ring buffer operations with atomic access and memory barriers
static inline uint32_t rb_count_frames() {
    // Atomic read with memory barrier to ensure consistency
    __DMB();  // Data Memory Barrier - ensure all previous memory operations complete
    uint32_t head = s_rb_head;
    uint32_t tail = s_rb_tail;
    __DMB();  // Ensure reads are completed before calculation
    return (head - tail) & (RB_CAP_FRAMES - 1u);
}

static inline uint32_t rb_free_frames() {
    return (RB_CAP_FRAMES - 1u) - rb_count_frames();
}

static inline void rb_push_frames(const q15_t* samples, uint32_t frames) {
    if (frames == 0 || samples == nullptr)
        return;

    const uint32_t mask = RB_CAP_FRAMES - 1u;
    uint32_t head = s_rb_head;
    uint32_t first_chunk = RB_CAP_FRAMES - (head & mask);
    uint32_t chunk = (frames < first_chunk) ? frames : first_chunk;
    uint32_t samples_per_channel = s_output_channels;
    uint32_t dst_idx = (head & mask) * samples_per_channel;

    PROFILE_SCOPE(ring_buffer_push);
    arm_copy_q15(samples, &s_rb[dst_idx], chunk * samples_per_channel);
    if (frames > chunk) {
        arm_copy_q15(
            samples + chunk * samples_per_channel, s_rb, (frames - chunk) * samples_per_channel);
    }

    __DMB();  // Ensure writes complete before updating head
    s_rb_head = head + frames;
    __DMB();  // Ensure head is visible to consumer
}

static inline bool rb_pop_stereo(int16_t& l, int16_t& r) {
    // Atomic read with memory barriers for thread safety
    __DMB();  // Data Memory Barrier - ensure all previous operations complete
    uint32_t tail = s_rb_tail;
    uint32_t head = s_rb_head;

    // Check if buffer is empty (atomic comparison)
    if (tail == head) {
        __DMB();  // Ensure comparison is complete
        return false;
    }

    // Read data
    uint32_t stride = s_output_channels;  // Must match writer stride
    uint32_t idx = (tail & (RB_CAP_FRAMES - 1u)) * stride;
    l = s_rb[idx + 0];
    r = (stride > 1) ? s_rb[idx + 1] : s_rb[idx + 0];

    // Memory barrier to ensure data is read before updating tail
    __DMB();  // Data Memory Barrier - ensure data reads complete

    // Atomic update of tail pointer
    s_rb_tail = tail + 1u;

    // Final memory barrier to ensure tail update is visible
    __DMB();  // Ensure tail update is committed to memory

    return true;
}

// Resampling temporarily disabled - using direct playback

void Init(DaisySeed& hw, float sample_rate) {
    s_hw = &hw;
    s_sample_rate = sample_rate;

    WaveX::Profiling::InitHardware();
    PROFILE_REGISTER_ZONE(audio_callback);
    PROFILE_REGISTER_ZONE(wav_pump_io);
    PROFILE_REGISTER_ZONE(format_conversion);
    PROFILE_REGISTER_ZONE(ring_buffer_push);
    PROFILE_REGISTER_ZONE(prebuffer_audio);
    PROFILE_REGISTER_ZONE(sd_refill);

    s_filter.Init(sample_rate);
    s_filter.SetFreq(s_params.filter_cutoff);
    s_filter.SetRes(s_params.filter_resonance);
    s_filter.SetDrive(0.5f);

    s_envelope.Init(sample_rate);
    s_envelope.SetAttackTime(s_params.envelope_attack);
    s_envelope.SetDecayTime(s_params.envelope_decay);
    s_envelope.SetSustainLevel(s_params.envelope_sustain);
    s_envelope.SetReleaseTime(s_params.envelope_release);

    s_lfo.Init(sample_rate);
    s_lfo.SetWaveform(Oscillator::WAVE_SIN);
    s_lfo.SetFreq(s_params.lfo_rate);
    s_lfo.SetAmp(s_params.lfo_depth);

    s_oscillator.Init(sample_rate);
    s_oscillator.SetWaveform(Oscillator::WAVE_SIN);
    s_oscillator.SetFreq(1000.0f);  // Use 1kHz - simple test frequency
    s_oscillator.SetAmp(0.3f);      // Reduce amplitude slightly

#if WAVEX_CV_BACKEND == WAVEX_CV_BACKEND_MCP4728
    s_cv_backend.Init(0x60);
#else
    s_cv_backend.Init();
#endif

    // Initialize Sample RAM Manager (conditionally)
    // SDRAM is mapped at 0xC0000000, typically 64MB available on Daisy Seed
    // Use 32MB for sample storage, reserve 32MB for other uses
    const uint32_t SDRAM_BASE = 0xC0000000;
    const uint32_t SDRAM_SIZE = 64 * 1024 * 1024;       // 64MB
    const uint32_t SAMPLE_MEM_SIZE = 32 * 1024 * 1024;  // 32MB for samples
    const uint32_t SMALL_POOL_SIZE = 4 * 1024 * 1024;   // 4MB for small samples

    // Initialize Sample RAM Manager
    if (s_hw) {
        s_hw->PrintLine("AUDIO_ENGINE: Initializing Sample RAM Manager...");
    }
    if (s_hw) {
        s_hw->PrintLine("AUDIO_ENGINE: Calling SampleMemMgr.init()...");
    }
    s_sample_mem_mgr.init((void*)SDRAM_BASE, SAMPLE_MEM_SIZE, SMALL_POOL_SIZE);
    if (s_hw) {
        s_hw->PrintLine("AUDIO_ENGINE: SampleMemMgr.init() completed");
    }

    // Sampler's recording buffer must be preallocated from SampleMemMgr, so
    // it must init() after the manager above (roadmap Phase 1 item 3: no
    // heap growth in the audio path - see sampler.hpp).
    constexpr uint32_t kSamplerMaxRecordSeconds = 30;
    s_sampler.Init(sample_rate, s_sample_mem_mgr, sample_rate * kSamplerMaxRecordSeconds);

    // Test basic allocation to ensure SDRAM is working
    if (s_hw) {
        s_hw->PrintLine("AUDIO_ENGINE: Testing Sample RAM allocation...");
    }
    wxsamp_t test_handle = {};
    bool test_alloc = s_sample_mem_mgr.alloc(1024, &test_handle);  // Try to allocate 1KB
    if (test_alloc) {
        if (s_hw) {
            s_hw->PrintLine(
                "AUDIO_ENGINE: Sample RAM test allocation successful (handle: cls=%u page=%u "
                "slot=%u)",
                (unsigned)test_handle.cls,
                (unsigned)test_handle.page,
                (unsigned)test_handle.slot);
        }
        s_sample_mem_mgr.release(&test_handle);  // Clean up test allocation
        if (s_hw) {
            s_hw->PrintLine("AUDIO_ENGINE: Sample RAM test completed successfully");
        }
    } else {
        if (s_hw) {
            s_hw->PrintLine(
                "AUDIO_ENGINE: Sample RAM test allocation FAILED - SDRAM may not be initialized");
        }
    }

    // Initialize CPU load meter for audio processing performance monitoring
    // Use default block size of 48 and 200-block averaging window
    s_cpu_load_meter.Init(sample_rate, 48, 200);
}

void Callback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    PROFILE_SCOPE(audio_callback);
    uint32_t callback_cycles_start = WaveX::Profiling::GetCycles();
    // Start CPU load measurement for this audio block
    s_cpu_load_meter.OnBlockStart();

    (void)in;
    for (size_t i = 0; i < size; i++) {
        int16_t l16 = 0, r16 = 0;
        if (!rb_pop_stereo(l16, r16)) {
            // Check if we have audition playback active
            if (s_audition.active && s_wav.open) {
                // Use pre-buffering system for audition to avoid blocking audio callback
                // The main loop will handle file I/O via PumpWavIO()
                // For now, output silence and let the pre-buffering system handle the data
                l16 = 0;
                r16 = 0;

                // Check if audition is complete (this will be handled by the main loop)
                // The audio callback should never do file I/O
            } else if (!s_wav.open) {
                // No audio should play on startup - output silence until audition commands
                // Requirement: "When daisy starts, no audio plays (no oscillator, no .wavs)"
                out[0][i] = 0.0f;
                out[1][i] = 0.0f;
                continue;
            } else {
                // WAV is playing but buffer is empty - output silence to prevent glitches
                out[0][i] = 0.0f;
                out[1][i] = 0.0f;
                // Signal underrun detection (logging handled in main loop)
                s_underrun_detected = true;
                continue;
            }
        }

        out[0][i] = (float)l16 / 32768.0f;
        out[1][i] = (float)r16 / 32768.0f;
    }

    // Compute per-block meters
    float sumL = 0.f, sumR = 0.f;
    float pkL = 0.f, pkR = 0.f;
    for (size_t i = 0; i < size; ++i) {
        float l = out[0][i];
        float r = out[1][i];
        sumL += l * l;
        sumR += r * r;
        float al = fabsf(l);
        float ar = fabsf(r);
        if (al > pkL)
            pkL = al;
        if (ar > pkR)
            pkR = ar;
    }
    s_last_block_meters.rmsL = sqrtf(sumL / (float)size);
    s_last_block_meters.rmsR = sqrtf(sumR / (float)size);
    s_last_block_meters.peakL = pkL;
    s_last_block_meters.peakR = pkR;

    Timebase::Tick1kHz([] {
        // Future control logic
    });

    // End CPU load measurement for this audio block
    s_cpu_load_meter.OnBlockEnd();
    s_dwt_callback_cycles = WaveX::Profiling::GetCycles() - callback_cycles_start;
    s_dwt_callback_max = std::max(s_dwt_callback_max, s_dwt_callback_cycles);
}

void OnControlChange(const ControlChangeMessage& ctrl_msg) {
    switch (ctrl_msg.parameter) {
        case PARAM_VOLUME:
            s_params.volume = ctrl_msg.value / 65535.0f;
            break;
        case PARAM_FILTER_CUTOFF:
            s_params.filter_cutoff = 20.0f + (ctrl_msg.value / 65535.0f) * 8000.0f;
            s_filter.SetFreq(s_params.filter_cutoff);
            break;
        case PARAM_FILTER_RESONANCE:
            s_params.filter_resonance = ctrl_msg.value / 65535.0f;
            s_filter.SetRes(s_params.filter_resonance);
            break;
        case PARAM_ENVELOPE_ATTACK:
            s_params.envelope_attack = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
            s_envelope.SetAttackTime(s_params.envelope_attack);
            break;
        case PARAM_ENVELOPE_DECAY:
            s_params.envelope_decay = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
            s_envelope.SetDecayTime(s_params.envelope_decay);
            break;
        case PARAM_ENVELOPE_SUSTAIN:
            s_params.envelope_sustain = ctrl_msg.value / 65535.0f;
            s_envelope.SetSustainLevel(s_params.envelope_sustain);
            break;
        case PARAM_ENVELOPE_RELEASE:
            s_params.envelope_release = 0.001f + (ctrl_msg.value / 65535.0f) * 2.0f;
            s_envelope.SetReleaseTime(s_params.envelope_release);
            break;
        case PARAM_LFO_RATE:
            s_params.lfo_rate = 0.1f + (ctrl_msg.value / 65535.0f) * 10.0f;
            s_lfo.SetFreq(s_params.lfo_rate);
            break;
        case PARAM_LFO_DEPTH:
            s_params.lfo_depth = ctrl_msg.value / 65535.0f;
            break;
    }
}

void OnNoteOn(const NoteMessage& note_msg) {
    float freq = mtof(note_msg.note);
    s_oscillator.SetFreq(freq);
    s_envelope_gate = true;
    s_envelope.Retrigger(false);
    if (s_hw)
        s_hw->PrintLine("RX NOTE_ON: note=%u vel=%u ch=%u freq=%.2f",
                        (unsigned)note_msg.note,
                        (unsigned)note_msg.velocity,
                        (unsigned)note_msg.channel,
                        (double)freq);
}

void OnNoteOff(const NoteMessage& note_msg) {
    (void)note_msg;
    s_envelope_gate = false;
    if (s_hw)
        s_hw->PrintLine(
            "RX NOTE_OFF: note=%u ch=%u", (unsigned)note_msg.note, (unsigned)note_msg.channel);
}

void OnSampleCtrl(const SampleCtrlMessage& sc) {
    switch (sc.cmd) {
        case SAMPLE_REC_START:
            s_sampler.StartRec();
            break;
        case SAMPLE_REC_STOP:
            s_sampler.StopRec();
            break;
        case SAMPLE_PLAY_START:
            s_sampler.StartPlay(sc.rate);
            break;
        case SAMPLE_PLAY_STOP:
            s_sampler.StopPlay();
            break;
    }
}

void OnPreviewReq(const PreviewReqMessage& pr) {
    s_prev_sent = 0;
    s_preview.clear();

    // Pick the most recently loaded sample; fall back to empty if none.
    if (s_loaded_samples.empty()) {
        if (s_hw) {
            s_hw->PrintLine("PREVIEW: No loaded samples; skipping preview");
        }
        return;
    }

    const LoadedSampleInfo& src = s_loaded_samples.back();
    void* sample_ptr = nullptr;
    if (!s_sample_mem_mgr.ptr(src.handle, &sample_ptr) || !sample_ptr) {
        if (s_hw) {
            s_hw->PrintLine("PREVIEW: Failed to get pointer for sample_id=%u",
                            (unsigned)src.sample_id);
        }
        return;
    }

    const uint32_t bytes_total = src.loaded_bytes ? src.loaded_bytes : src.handle.len;
    const uint32_t bytes_per_frame = (src.bit_depth / 8) * src.channels;
    if (bytes_per_frame == 0) {
        if (s_hw) {
            s_hw->PrintLine("PREVIEW: Invalid bytes_per_frame=0 for sample_id=%u",
                            (unsigned)src.sample_id);
        }
        return;
    }

    const uint32_t total_frames = bytes_total / bytes_per_frame;
    uint32_t start = pr.start;
    uint32_t end = pr.end > 0 ? std::min<uint32_t>(pr.end, total_frames) : total_frames;
    if (start > end)
        start = end;
    uint16_t decim = pr.decim ? pr.decim : 1;

    const int16_t* samples16 = reinterpret_cast<const int16_t*>(sample_ptr);
    const uint8_t* samples24 = reinterpret_cast<const uint8_t*>(sample_ptr);

    // Reserve rough size after decimation
    s_preview.reserve((end - start) / decim + 1);

    for (uint32_t i = start; i < end; i += decim) {
        int16_t v = 0;
        if (src.bit_depth == 16) {
            if (src.channels == 1) {
                v = samples16[i];
            } else {
                // Interleaved stereo: take left channel
                v = samples16[i * src.channels];
            }
        } else if (src.bit_depth == 24) {
            // 24-bit little endian; take left channel, sign-extend to 16-bit for display
            uint32_t byte_index = i * bytes_per_frame;
            int32_t s = (int32_t)(samples24[byte_index] | (samples24[byte_index + 1] << 8) |
                                  (samples24[byte_index + 2] << 16));
            // sign extend 24-bit to 32-bit then scale down to 16-bit
            if (s & 0x00800000)
                s |= 0xFF000000;
            v = (int16_t)(s >> 8);
        }
        s_preview.push_back(v);
    }

    if (s_hw) {
        s_hw->PrintLine(
            "PREVIEW: Built preview for sample_id=%u frames=%lu decim=%u preview_len=%u",
            (unsigned)src.sample_id,
            (unsigned long)total_frames,
            (unsigned)decim,
            (unsigned)s_preview.size());
    }

    SendPreviewChunks();
}

void OnSampleLoad(const SampleLoadMessage& sl) {
    if (s_hw) {
        s_hw->PrintLine(
            "SAMPLE_LOAD: [1/10] Entry - path='%s' id=%u", sl.path, (unsigned)sl.sample_id);
    }

    // CRITICAL: Stop ALL SD activity (audition/playback) and ensure PumpWavIO is not running.
    // FatFS + SDMMC are NOT thread-safe or re-entrant. The main loop calls PumpWavIO() which
    // will conflict with f_open/f_read calls here if s_wav.open is true.
    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [2/10] Calling StopAudition()...");
    }
    StopAudition();

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [3/10] Calling CloseWav()...");
    }
    CloseWav();

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [4/10] Delaying 10ms for DMA settle...");
    }
    // Add a small delay to ensure any in-flight SD DMA completes
    System::Delay(10);

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [5/10] Getting FIL reference...");
    }
    // Use static FIL (too large for stack - ~600 bytes with SDMMC buffer).
    // Keep in normal BSS like s_wav.file so cache maintenance works correctly.
    // CRITICAL: Do NOT memset() the FIL - it has internal buffer pointers managed by FatFS.
    FIL& file = s_sample_load_file;

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [6/10] Calling f_open('%s')...", sl.path);
    }
    // Try raw path first (matches playback/audition). If that fails and path does not include a
    // drive prefix, retry with "0:" prefix to be tolerant of mount styles.
    FRESULT fr = f_open(&file, sl.path, FA_READ);
    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [6a/10] f_open result: %d", (int)fr);
    }

    if (fr != FR_OK && strncmp(sl.path, "0:", 2) != 0) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: [6b/10] Retrying with 0: prefix...");
        }
        char alt_path[128];
        snprintf(alt_path, sizeof(alt_path), "0:%s", sl.path);
        fr = f_open(&file, alt_path, FA_READ);
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: [6c/10] f_open alt result: %d", (int)fr);
        }
        if (fr != FR_OK && s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: f_open failed (%d) for '%s' and alt '%s'",
                            (int)fr,
                            sl.path,
                            alt_path);
        }
    } else if (fr != FR_OK && s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: f_open failed (%d) for '%s'", (int)fr, sl.path);
    }
    if (fr != FR_OK) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: [EXIT-ERR] f_open failed, returning");
        }
        return;
    }

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [7/10] File opened successfully, reading header...");
    }
    // Read header into DMA-safe buffer (RAM_D2) so SDMMC DMA can access it.
    uint8_t* hdr = s_sample_hdr;
    UINT br = 0;
    const UINT kHdrSize = 44;
    fr = f_read(&file, hdr, kHdrSize, &br);
    if (s_hw) {
        s_hw->PrintLine(
            "SAMPLE_LOAD: [7a/10] f_read header result: fr=%d, br=%u", (int)fr, (unsigned)br);
    }

    if (fr != FR_OK || br < 44 || memcmp(hdr + 0, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        if (s_hw) {
            s_hw->PrintLine(
                "SAMPLE_LOAD: invalid WAV header (%d, bytes=%u)", (int)fr, (unsigned)br);
        }
        f_close(&file);
        return;
    }

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [8/10] Valid WAV header, parsing chunks...");
    }

    uint16_t audio_fmt = 0;
    uint16_t num_ch = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_off = 0;
    uint32_t data_size = 0;

    f_lseek(&file, 12);
    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [8a/10] Seeking to chunk 12, starting chunk parse loop...");
    }

    while (true) {
        uint8_t* chdr = s_sample_hdr;  // reuse DMA-safe buffer
        fr = f_read(&file, chdr, 8, &br);
        if (fr != FR_OK || br < 8) {
            if (s_hw) {
                s_hw->PrintLine("SAMPLE_LOAD: [8b/10] Chunk read failed or EOF: fr=%d, br=%u",
                                (int)fr,
                                (unsigned)br);
            }
            f_close(&file);
            return;
        }
        uint32_t cid = read_le32(chdr);
        uint32_t csz = read_le32(chdr + 4);
        if (cid == 0x20746d66) {          // 'fmt '
            uint8_t* fmt = s_sample_hdr;  // reuse DMA-safe buffer
            if (csz < 16) {
                f_close(&file);
                return;
            }
            fr = f_read(&file, fmt, 16, &br);
            if (fr != FR_OK || br < 16) {
                f_close(&file);
                return;
            }
            audio_fmt = read_le16(fmt + 0);
            num_ch = read_le16(fmt + 2);
            sample_rate = read_le32(fmt + 4);
            bits = read_le16(fmt + 14);
            if (csz > 16)
                f_lseek(&file, f_tell(&file) + (csz - 16));
        } else if (cid == 0x61746164) {  // 'data'
            data_off = f_tell(&file);
            data_size = csz;
            break;
        } else {
            f_lseek(&file, f_tell(&file) + csz);
        }
    }

    if (audio_fmt != 1 || (bits != 16 && bits != 24) || (num_ch != 1 && num_ch != 2)) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: unsupported format fmt=%u bits=%u ch=%u",
                            (unsigned)audio_fmt,
                            (unsigned)bits,
                            (unsigned)num_ch);
        }
        f_close(&file);
        return;
    }

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [9/10] Format valid: sr=%lu ch=%u bits=%u, data_size=%lu",
                        (unsigned long)sample_rate,
                        (unsigned)num_ch,
                        (unsigned)bits,
                        (unsigned long)data_size);
    }

    wxsamp_t handle = {};
    if (!s_sample_mem_mgr.alloc(data_size, &handle)) {
        if (s_hw) {
            wxsamp_stats_t st{};
            s_sample_mem_mgr.stats(&st);
            s_hw->PrintLine(
                "SAMPLE_LOAD: alloc failed for %lu bytes (largest_free=%lu, free_total=%lu)",
                (unsigned long)data_size,
                (unsigned long)st.largest_free_bytes,
                (unsigned long)st.large_free_bytes + (unsigned long)st.small_free_bytes);
        }
        f_close(&file);
        return;
    }

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [9a/10] Memory allocated, getting pointer...");
    }

    void* sample_ptr = nullptr;
    if (!s_sample_mem_mgr.ptr(handle, &sample_ptr)) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: [9b/10] Failed to get pointer");
        }
        s_sample_mem_mgr.release(&handle);
        f_close(&file);
        return;
    }

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [10/10] Seeking to data_off=%lu, starting data read loop...",
                        (unsigned long)data_off);
    }

    f_lseek(&file, data_off);
    uint32_t remaining = data_size;
    uint32_t written = 0;
    // Use DMA-safe buffer in RAM_D2; stack (DTCM) is not accessible to SDMMC DMA.
    uint8_t* temp = s_sample_io;
    constexpr UINT kIoChunk = sizeof(s_sample_io);

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [10a/10] ENTERING f_read loop, remaining=%lu...",
                        (unsigned long)remaining);
    }

    while (remaining > 0) {
        UINT to_read = (remaining > kIoChunk) ? kIoChunk : remaining;

        if (s_hw && written == 0) {
            s_hw->PrintLine("SAMPLE_LOAD: [10b/10] CALLING f_read for first chunk, to_read=%u...",
                            (unsigned)to_read);
        }

        fr = f_read(&file, temp, to_read, &br);

        if (s_hw && written == 0) {
            s_hw->PrintLine(
                "SAMPLE_LOAD: [10c/10] RETURNED from f_read: fr=%d, br=%u", (int)fr, (unsigned)br);
        }

        if (fr != FR_OK || br == 0) {
            if (s_hw) {
                s_hw->PrintLine(
                    "SAMPLE_LOAD: read error %d after %lu bytes", (int)fr, (unsigned long)written);
            }
            s_sample_mem_mgr.release(&handle);
            f_close(&file);
            return;
        }
        memcpy(static_cast<uint8_t*>(sample_ptr) + written, temp, br);
        written += br;
        remaining -= br;
    }

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: [SUCCESS] Data read complete, closing file...");
    }

    f_close(&file);

    upsert_loaded_sample(sl, handle);
    update_loaded_sample_progress(sl.sample_id, data_size);

    s_sample_load.loading = false;

    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: Loaded %lu bytes for sample %u",
                        (unsigned long)data_size,
                        (unsigned)sl.sample_id);
    }

    // Notify host (ESP32) that sample load completed.
    SampleStatusMessage status{};
    status.sample_id = sl.sample_id;
    status.state = 0x10;  // load complete
    status.channels = num_ch;
    status.sample_rate = sample_rate;
    status.frames_played = data_size / ((bits / 8) * num_ch);  // total frames loaded
    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_STATUS, &status, sizeof(status));
}

void OnSampleData(const uint8_t* data, size_t length) {
    if (!s_sample_load.loading) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_DATA: Received data but not in loading state");
        }
        return;
    }

    // Get pointer to sample memory
    void* sample_ptr = nullptr;
    if (!s_sample_mem_mgr.ptr(s_sample_load.handle, &sample_ptr)) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_DATA: Failed to get sample memory pointer");
        }
        return;
    }

    // Check if we would exceed allocated size
    if (s_sample_load.received_size + length > s_sample_load.expected_size) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_DATA: Data would exceed allocated size (%lu + %lu > %lu)",
                            (unsigned long)s_sample_load.received_size,
                            (unsigned long)length,
                            (unsigned long)s_sample_load.expected_size);
        }
        return;
    }

    // Copy data to SDRAM
    uint8_t* dest = static_cast<uint8_t*>(sample_ptr) + s_sample_load.received_size;
    memcpy(dest, data, length);
    s_sample_load.received_size += length;
    update_loaded_sample_progress(s_sample_load.sample_id, s_sample_load.received_size);

    // Check if loading is complete
    if (s_sample_load.received_size >= s_sample_load.expected_size) {
        s_sample_load.loading = false;
        update_loaded_sample_progress(s_sample_load.sample_id, s_sample_load.expected_size);
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: Completed loading sample %u (%lu bytes)",
                            (unsigned)s_sample_load.sample_id,
                            (unsigned long)s_sample_load.received_size);
        }
    }
}

void GetSampleMemStatus(SampleMemStatusMessage& out) {
    memset(&out, 0, sizeof(out));
    out.category = STATUS_CATEGORY_SAMPLE_MEM;

    wxsamp_stats_t stats = {};
    s_sample_mem_mgr.stats(&stats);

    out.small_total_bytes = stats.small_total_bytes;
    out.small_free_bytes = stats.small_free_bytes;
    out.large_total_bytes = stats.large_total_bytes;
    out.large_free_bytes = stats.large_free_bytes;
    out.largest_free_bytes = stats.largest_free_bytes;
    out.in_use_bytes = stats.in_use_bytes;
    out.failed_allocs = stats.failed_allocs;

    const size_t count = std::min<size_t>(s_loaded_samples.size(), WAVEX_SAMPLE_STATUS_MAX_ENTRIES);
    out.sample_count = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& src = s_loaded_samples[i];
        auto& dst = out.entries[i];
        dst.sample_id = src.sample_id;
        dst.allocated_bytes = src.allocated_bytes;
        dst.loaded_bytes = src.loaded_bytes;
        dst.cls = src.handle.cls;
        dst.page = src.handle.page;
        dst.slot = src.handle.slot;
        dst.sample_rate = src.sample_rate;
        dst.channels = src.channels;
        dst.bit_depth = src.bit_depth;
    }
}

void GetInputMeters(float& rms, float& peak) {
    Sampler::BlockMeters(s_last_in_block, s_last_block_size, rms, peak);
}

void GetMeters(BlockMeters& out) {
    out = s_last_block_meters;
}

// ============================
// CPU Load Monitoring Functions
// ============================

// Check for underruns detected in audio callback and log them (called from main loop)
void CheckAndLogUnderruns() {
    if (s_underrun_detected && !s_underrun_logged) {
        if (s_hw)
            s_hw->PrintLine("AUDIO: Ring buffer underrun - outputting silence");
        s_underrun_logged = true;
        s_underrun_detected = false;  // Reset detection flag
    } else if (!s_underrun_detected && s_underrun_logged) {
        // Reset logging flag when underruns stop
        s_underrun_logged = false;
    }
}

float GetAvgCpuLoad() {
    return s_cpu_load_meter.GetAvgCpuLoad();
}

float GetMinCpuLoad() {
    return s_cpu_load_meter.GetMinCpuLoad();
}

float GetMaxCpuLoad() {
    return s_cpu_load_meter.GetMaxCpuLoad();
}

float GetBlockPeriodMs() {
    return 1000.0f * (float)s_block_size / s_sample_rate;  // Block period in milliseconds
}

// ============================
// WAV playback implementation
// ============================

bool OpenWav(const char* path) {
    CloseWav();

    FRESULT fr = f_open(&s_wav.file, path, FA_READ);
    if (fr != FR_OK) {
        if (s_hw)
            s_hw->PrintLine("WAV open failed: f_open error %d for path %s", (int)fr, path);
        return false;
    }

    uint8_t hdr[44];
    UINT br = 0;
    fr = f_read(&s_wav.file, hdr, sizeof(hdr), &br);
    if (fr != FR_OK || br < 44) {
        if (s_hw)
            s_hw->PrintLine(
                "WAV open failed: header read error %d, bytes read %u", (int)fr, (unsigned)br);
        f_close(&s_wav.file);
        return false;
    }
    // Validate RIFF/WAVE
    if (memcmp(hdr + 0, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        if (s_hw)
            s_hw->PrintLine("WAV open failed: not a RIFF/WAVE file");
        f_close(&s_wav.file);
        return false;
    }

    // Parse chunks to find fmt and data
    uint16_t audio_fmt = 0;
    uint16_t num_ch = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_off = 0;
    uint32_t data_size = 0;

    // Rewind and iterate using f_lseek for generality
    f_lseek(&s_wav.file, 12);
    while (true) {
        uint8_t chdr[8];
        fr = f_read(&s_wav.file, chdr, 8, &br);
        if (fr != FR_OK || br < 8) {
            if (s_hw)
                s_hw->PrintLine("WAV open failed: chunk header read error %d, bytes read %u",
                                (int)fr,
                                (unsigned)br);
            f_close(&s_wav.file);
            return false;
        }
        uint32_t cid = read_le32(chdr);
        uint32_t csz = read_le32(chdr + 4);
        if (cid == 0x20746d66) {  // 'fmt '
            uint8_t fmt[16];
            if (csz < 16) {
                if (s_hw)
                    s_hw->PrintLine("WAV open failed: fmt chunk too small");
                f_close(&s_wav.file);
                return false;
            }
            fr = f_read(&s_wav.file, fmt, 16, &br);
            if (fr != FR_OK || br < 16) {
                if (s_hw)
                    s_hw->PrintLine("WAV open failed: fmt chunk read error");
                f_close(&s_wav.file);
                return false;
            }
            audio_fmt = read_le16(fmt + 0);
            num_ch = read_le16(fmt + 2);
            sample_rate = read_le32(fmt + 4);
            bits = read_le16(fmt + 14);
            // Skip any extra fmt bytes
            if (csz > 16)
                f_lseek(&s_wav.file, f_tell(&s_wav.file) + (csz - 16));
        } else if (cid == 0x61746164) {  // 'data'
            data_off = f_tell(&s_wav.file);
            data_size = csz;
            // Position after header for reading
            break;
        } else {
            // skip unknown chunk
            f_lseek(&s_wav.file, f_tell(&s_wav.file) + csz);
        }
    }

    // Support PCM format (fmt=1), 16-bit or 24-bit, mono or stereo
    if (audio_fmt != 1 || (bits != 16 && bits != 24) || (num_ch != 1 && num_ch != 2)) {
        if (s_hw)
            s_hw->PrintLine("WAV open failed: unsupported format fmt=%u bits=%u ch=%u",
                            (unsigned)audio_fmt,
                            (unsigned)bits,
                            (unsigned)num_ch);
        f_close(&s_wav.file);
        return false;  // only PCM16/24 mono/stereo supported
    }

    s_wav.open = true;
    s_wav.data_start = data_off;
    s_wav.data_size = data_size;
    s_wav.bytes_remaining = data_size;
    s_wav.num_channels = num_ch;
    s_wav.bits_per_sample = bits;
    s_wav.sample_rate = sample_rate;

    // Reset buffers
    s_rb_head = 0;
    s_rb_tail = 0;

#if WAVEX_DAISY_SD_DEBUG
    if (s_hw)
        s_hw->PrintLine("WAV open ok: %s ch=%u sr=%lu bits=%u size=%lu",
                        path,
                        (unsigned)num_ch,
                        (unsigned long)sample_rate,
                        (unsigned)bits,
                        (unsigned long)data_size);
#endif

    // Reset pre-buffer state and start pre-buffering
    s_prebuffer_filled = 0;
    s_prebuffer_ready = false;
    s_prebuffering = false;

    return true;
}

void CloseWav() {
    if (s_wav.open) {
        if (s_hw)
            s_hw->PrintLine("CloseWav: closing WAV file and clearing state");
        f_close(&s_wav.file);
        s_wav = {};
    } else {
        if (s_hw)
            s_hw->PrintLine("CloseWav: called but no WAV was open");
    }

    // Reset SD buffer state
    for (auto& slot: s_sd_buffers) {
        slot.frames = 0;
        slot.bytes = 0;
        slot.consumed = 0;
        slot.ready = false;
    }
    s_sd_fill_index = 0;
    s_sd_consume_index = 0;
    s_last_io_time = 0;

    // Reset pre-buffer state
    s_prebuffer_filled = 0;
    s_prebuffer_ready = false;
    s_prebuffering = false;

    // Clear ring buffer to stop any remaining audio immediately
    s_rb_head = 0;
    s_rb_tail = 0;
}

bool IsWavPlaying() {
    return s_wav.open;
}

bool IsPrebufferReady() {
    return s_prebuffer_ready;
}

void GetIOStats(uint32_t& count, uint32_t& max_duration, uint32_t& last_duration) {
    count = s_io_count;
    max_duration = s_max_io_duration;
    last_duration = s_io_duration;
}

void SetOutputMode(AudioOutputMode mode) {
    s_output_channels = static_cast<uint32_t>(mode);
}

AudioOutputMode GetOutputMode() {
    return static_cast<AudioOutputMode>(s_output_channels);
}

uint32_t GetOutputChannelCount() {
    return s_output_channels;
}

void GetDwtStats(uint32_t& callback_cycles, uint32_t& io_cycles) {
    callback_cycles = s_dwt_callback_cycles;
    io_cycles = s_dwt_io_cycles;
}

bool ShouldPumpWavIO() {
    if (!s_wav.open)
        return false;

    // If we're pre-buffering, always pump I/O
    if (s_prebuffering) {
        return true;
    }

    // If pre-buffer is ready, use adaptive I/O pumping logic
    if (s_prebuffer_ready) {
        const uint32_t count = rb_count_frames();
        const uint32_t critical_threshold = RB_CAP_FRAMES / 4;  // 25% of capacity
        const uint32_t normal_threshold = RB_CAP_FRAMES / 2;    // 50% of capacity

        // When the buffer is low, bypass rate limiting to avoid underruns
        if (count < critical_threshold) {
            return true;
        }

        // Otherwise, apply a light rate limit to reduce contention
        uint32_t now = System::GetNow();
        if (now - s_last_io_time < 2) {  // Minimum 2ms between I/O operations
            return false;
        }

        return count < normal_threshold;
    }

    // If pre-buffer is not ready, start pre-buffering
    return true;
}

void PumpWavIO() {
    if (!s_wav.open)
        return;

    PROFILE_SCOPE(wav_pump_io);
    uint32_t block_cycles_start = WaveX::Profiling::GetCycles();

    // Update I/O timing
    s_last_io_time = System::GetNow();

    // If we're pre-buffering, do that first
    if (s_prebuffering || !s_prebuffer_ready) {
        if (!prebuffer_audio()) {
            return;
        }
        return;
    }

    // If pre-buffer is ready, transfer data from pre-buffer to ring buffer
    if (s_prebuffer_ready && s_prebuffer_filled > 0) {
        uint32_t free_frames = rb_free_frames();
        if (free_frames == 0) {
            s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
            s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
            return;
        }

        uint32_t frames_to_transfer = std::min(s_prebuffer_filled, free_frames);

        if (frames_to_transfer == 0)
            return;

        rb_push_frames(s_prebuffer, frames_to_transfer);

        s_prebuffer_filled -= frames_to_transfer;
        if (s_prebuffer_filled > 0) {
            memmove(s_prebuffer,
                    &s_prebuffer[frames_to_transfer * s_output_channels],
                    s_prebuffer_filled * s_output_channels * sizeof(q15_t));
        }

#if WAVEX_DAISY_SD_DEBUG
        if (s_hw)
            s_hw->PrintLine("Transferred %u frames from pre-buffer to ring buffer",
                            (unsigned)frames_to_transfer);
#endif

        s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
        s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
        return;
    }

    // Normal I/O pumping (when pre-buffer is empty)
    if (!refill_sd_buffer()) {
        return;
    }

    SdBufferSlot& slot = s_sd_buffers[s_sd_consume_index];
    if (!slot.ready) {
        return;
    }

    uint32_t free_frames = rb_free_frames();
    if (free_frames == 0) {
        s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
        s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
        return;
    }

    uint32_t available_frames = slot.frames - slot.consumed;
    float resample_ratio =
        (s_wav.sample_rate != s_sample_rate)
            ? static_cast<float>(s_sample_rate) / static_cast<float>(s_wav.sample_rate)
            : 1.0f;
    uint32_t frames_to_transfer = std::min(available_frames, free_frames);

    // Prevent ring buffer overflow when upsampling (e.g., 22kHz -> 48kHz)
    if (resample_ratio > 1.0f) {
        if (free_frames <= 1) {
            s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
            s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
            return;
        }
        uint32_t max_input_by_space =
            static_cast<uint32_t>(static_cast<float>(free_frames - 1) / resample_ratio);
        if (max_input_by_space == 0) {
            s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
            s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
            return;
        }
        if (frames_to_transfer > max_input_by_space) {
            frames_to_transfer = max_input_by_space;
        }
    }
    if (frames_to_transfer == 0) {
        s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
        s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
        return;
    }

    ResetScratchPool();
    uint32_t bytes_per_sample = (s_wav.bits_per_sample == 24) ? 3u : 2u;
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * bytes_per_sample;
    const uint8_t* src = slot.data + (slot.consumed * file_bpf);

    q15_t* conversion_output = AcquireScratch(frames_to_transfer * s_output_channels);
    if (conversion_output == nullptr)
        return;

    PROFILE_SCOPE(format_conversion);
    ConvertFramesToOutput(
        src, conversion_output, frames_to_transfer, s_wav.num_channels, s_wav.bits_per_sample);

    q15_t* final_buffer = conversion_output;
    uint32_t final_frames = frames_to_transfer;
    if (resample_ratio != 1.0f) {
        uint32_t max_out_frames =
            static_cast<uint32_t>(std::ceil(frames_to_transfer * resample_ratio)) + 1;
        q15_t* resample_buffer = AcquireScratch(max_out_frames * s_output_channels);
        if (resample_buffer != nullptr) {
            uint32_t resampled = LinearResampleFrames(conversion_output,
                                                      frames_to_transfer,
                                                      resample_buffer,
                                                      s_output_channels,
                                                      resample_ratio);
            if (resampled > 0) {
                final_buffer = resample_buffer;
                final_frames = resampled;
            }
        }
    }

    if (final_frames > free_frames) {
        final_frames = free_frames;
    }

    rb_push_frames(final_buffer, final_frames);
    slot.consumed += frames_to_transfer;
    if (slot.consumed >= slot.frames) {
        slot.ready = false;
        slot.consumed = 0;
        s_sd_consume_index = (s_sd_consume_index + 1) % kSdBufferCount;
    }

    s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
    s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);

#if WAVEX_DAISY_SD_DEBUG
    if (s_hw)
        s_hw->PrintLine("Transferred %u frames from SD buffer to ring buffer",
                        (unsigned)final_frames);
#endif
}

// ============================================================================
// Sample Audition Functions (for Sample Load/Save page)
// ============================================================================

bool AuditionSample(const char* path) {
    // Stop any current audition first
    StopAudition();

    // Use the existing WAV playback system for audition
    // This integrates with the pre-buffering system and avoids blocking I/O
    if (!OpenWav(path)) {
        if (s_hw)
            s_hw->PrintLine("AuditionSample: Failed to open WAV file for %s", path);
        return false;
    }

    // Mark audition as active for tracking
    s_audition.active = true;
    std::strncpy(s_audition.current_path, path, sizeof(s_audition.current_path) - 1);
    s_audition.current_path[sizeof(s_audition.current_path) - 1] = '\0';

    if (s_hw) {
        s_hw->PrintLine("AuditionSample: Started audition of %s using WAV playback system", path);
    }

    return true;
}

void StopAudition() {
    if (s_audition.active) {
        // Stop the WAV playback system
        CloseWav();
        s_audition.active = false;
        if (s_hw)
            s_hw->PrintLine("AuditionSample: Stopped audition");
    }
}

}  // namespace AudioEngine
}  // namespace WaveX

#endif  // WAVEX_AUDIO_ENGINE_ENABLED
