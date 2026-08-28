#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include <daisy.h>  // For CpuLoadMeter

#include "../memory.h"
#include "../sdram_layout.h"
#include "arm_math.h"  // For CMSIS-DSP helpers
#include "audio_engine.h"
#include "comm/daisy_uart_link.h"
#include "config/hardware_config.h"
#include "config/link_config.h"
#include "daisy_core.h"  // For memory sections
#include "ff.h"
#include "profiling/profiler.h"
#include "stm32h7xx_ll_cortex.h"  // For ARM atomic operations
#include "sys/dma.h"              // For cache management

#include "../cv/cv_cal_store.hpp"
#include "../cv/cv_group_router.hpp"
#include "../sequencer/sequencer_transport.hpp"
#include "../storage/fatfs_wav_reader.hpp"
#include "../timebase.hpp"
#include "instrument.hpp"
#include "linear_resampler.hpp"
#include "output_sink.hpp"
#include "paraphonic_envelope.hpp"
#include "voice_manager.hpp"
#include "wav/wav_header_parser.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

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
// Callback() below - the voice manager currently mixes straight into the
// stereo out[] (equivalent to StereoMixSink); routing through the sink
// abstraction lands with the Stage B TDM path.
#if WAVEX_VOICE_OUTPUT_BACKEND == WAVEX_VOICE_OUTPUT_STEREO_MIX
using OutputSinkType = WaveX::AudioEngine::StereoMixSink;
#else
using OutputSinkType = WaveX::AudioEngine::TdmVoiceSink;
#endif

using namespace daisy;
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
static bool s_sample_memory_available = false;

static_assert(WaveX::SdramLayout::kSmallSamplePoolBytes <=
                  WXM_SMALL_MAX_PAGES_PER_CLASS * WXM_SMALL_PAGE_BYTES,
              "the production slab reservation exceeds one class's usable page capacity");
static_assert((((WaveX::SdramLayout::kSampleArenaBytes -
                 WaveX::SdramLayout::kSmallSamplePoolBytes) /
                    WXM_LARGE_PAGE_BYTES +
                1u) /
               2u) <= WXM_LARGE_MAX_RUNS,
              "large extent free-run table cannot represent worst-case fragmentation");

static constexpr uint32_t kMaxMixChannels = 8;
static constexpr uint32_t kScratchPoolBytes = 32 * 1024;
static constexpr uint32_t kScratchPoolSamples = kScratchPoolBytes / sizeof(q15_t);
static constexpr uint32_t kSdBufferCount = 3;
static constexpr uint32_t kSdBufferAlignment = 32;
static uint32_t s_output_channels = static_cast<uint32_t>(AudioOutputMode::StereoSAI1);
static q15_t s_scratch_pool[kScratchPoolSamples];
static uint32_t s_scratch_offset = 0;

// (Review C2: a legacy mono-synth DSP surface sat here - Svf/Adsr/LFO/test
// oscillator, an AudioParameters bank written by OnControlChange, and a
// Sampler instance - none of it ever rendered by Callback(). Deleted;
// per-voice filter/envelope live in voice_manager.hpp, and real parameter
// routing arrives with Phase 2.)
static CvBackendType s_cv_backend;
static WaveX::Cv::CvGroupRouter<CvBackendType, WAVEX_ANALOG_CV_GROUPS> s_cv_router(s_cv_backend);
static OutputSinkType s_output_sink;
// Roadmap Phase 1 items 2+8: 8-voice RAM-resident player (allocation/
// stealing, per-voice gain/pan/pitch - see voice_manager.hpp), driven by
// the MIDI note path: OnNoteOn/OnNoteOff (main-loop message-handler
// context) resolve notes to loaded samples and hand events to Callback()
// (audio IRQ context) through the SPSC queue below; Callback() drains the
// queue and mixes Render() output on top of the streaming path.
static WaveX::AudioEngine::VoiceManager s_voice_manager;

// Sequencer transport (roadmap Phase 2). Owns the step scheduler + MIDI
// tempo follower. Edits/transport/clock arrive from main-loop message
// handlers (OnSeqTransport / OnSeqPatternOp / OnMidiClockEvent / OnMidiCc)
// and mutate this object in main-loop context only, so no locking is needed
// yet. NOTE (deliberate next stage): Tick() is NOT yet driven from
// Callback(). Advancing the scheduler from the 1 kHz control tick and
// converting its TriggerEvents into voice triggers requires (a) the
// double-buffered "edits applied between steps" discipline from
// sequencer.md §4 so a main-loop edit can't tear a step the callback is
// reading, and (b) a track->sample (kit) mapping that the instrument model
// (Phase 2.5) provides. Until that lands the transport accumulates fully
// unit-tested state (sequencer_transport_test) but does not yet drive audio
// - the same honest intermediate the voice manager passed through before
// its note mapping existed.
static WaveX::Sequencer::SequencerTransport s_seq_transport;

// --- Stage A paraphonic analog path (roadmap item 5; analog-voice-board.md
// §0). One shared envelope drives the shared VCF/VCA CVs; values are
// STAGED at the 1 kHz control tick (audio context, callback-safe - the
// router/backend only write member fields) and FLUSHED from the main loop
// (blocking I2C ~225 us, §7.1.4) via FlushCv() below.
static ParaphonicEnvelope s_para_env;

// Shared-path control values. Written from main-loop context (OnControlChange
// maps MSG_CONTROL_CHANGE here), read at the tick in audio context: plain
// aligned float stores are atomic on Cortex-M7 and each field has a single
// writer, so per-field tearing cannot occur (same handoff contract as the
// old parameter bank, now with an actual consumer).
struct ParaphonicParams {
    float cutoff_base = 0.2f;    // 0..1 filter cutoff floor
    float env_to_cutoff = 0.8f;  // envelope -> cutoff modulation depth
    float resonance = 0.2f;      // 0..1
    // Shared-envelope ADSR (seconds at the 1 kHz tick). Pushed into
    // s_para_env via SetParams whenever one of them changes.
    float attack_s = 0.005f;
    float decay_s = 0.050f;
    float sustain = 0.8f;
    float release_s = 0.150f;
};
static ParaphonicParams s_para_params;

// Set by the tick after staging fresh CV values; consumed by FlushCv().
static volatile bool s_cv_dirty = false;

// Calibration-procedure CV override (MSG_CV_TEST): while active the tick
// stages these fixed control values instead of the paraphonic law, so the
// user can measure corner frequencies / verify VCA silence with a steady
// CV. Main-loop writes, tick reads (same per-field atomicity contract as
// s_para_params).
static volatile bool s_cv_test_active = false;
static uint8_t s_cv_test_group = 0;
static float s_cv_test_cutoff = 0.0f;
static float s_cv_test_res = 0.0f;
static float s_cv_test_vca = 0.0f;

// Dedicated FIL for the calibration table (main-loop file I/O only).
static FIL s_cvcal_file;

// --- MIDI note-event handoff (roadmap Phase 1 item 8) ---
// Voice state must only be touched from one context: Trigger()/Release()
// write fields Render() reads, so calling them from the main loop while
// the audio IRQ renders would race. Events are therefore fully resolved
// (sample pointer, frames, rate) in the main loop and passed through a
// single-producer/single-consumer ring - release/acquire index pair, same
// discipline as the ESP32-side ui_update_pending fix (dma-timing-review
// Finding 11). Producer: OnNoteOn/OnNoteOff (main loop). Consumer:
// Callback() at block start (1 ms cadence, so worst-case added latency is
// one block - well inside item 8's < 5 ms in-to-sound budget).
struct NoteEvent {
    bool is_trigger = false;  // true = Trigger(params), false = Release(note)
    uint8_t note = 0;
    WaveX::AudioEngine::VoiceTriggerParams params;
};
static constexpr uint32_t kNoteQueueSize = 16;  // power of two (index math wraps)
static NoteEvent s_note_queue[kNoteQueueSize];
static uint32_t s_note_q_write = 0;  // advanced by main loop only
static uint32_t s_note_q_read = 0;   // advanced by audio callback only

// Set by the main loop before releasing/rewriting loaded-sample memory
// (OnSampleLoad); consumed by Callback(), which drains the queue and then
// hard-stops every voice so nothing keeps reading freed SDRAM.
static bool s_voice_stop_all = false;

static bool note_queue_push(const NoteEvent& ev) {
    const uint32_t w = s_note_q_write;  // single producer: plain read of own index
    const uint32_t r = __atomic_load_n(&s_note_q_read, __ATOMIC_ACQUIRE);
    if (w - r >= kNoteQueueSize) {
        return false;  // full; caller logs (main-loop context)
    }
    s_note_queue[w % kNoteQueueSize] = ev;
    __atomic_store_n(&s_note_q_write, w + 1, __ATOMIC_RELEASE);
    return true;
}

// Audio-callback side: apply every pending note event, then honor a
// pending hard-stop. Order matters - a stop request must also kill
// triggers queued before it (they reference the memory being released).
// Returns true if any trigger was applied - the paraphonic envelope's
// note-on edge (item 5).
static bool drain_note_queue() {
    bool any_trigger = false;
    const uint32_t w = __atomic_load_n(&s_note_q_write, __ATOMIC_ACQUIRE);
    uint32_t r = s_note_q_read;  // single consumer: plain read of own index
    while (r != w) {
        const NoteEvent& ev = s_note_queue[r % kNoteQueueSize];
        if (ev.is_trigger) {
            s_voice_manager.Trigger(ev.params);
            any_trigger = true;
        } else {
            s_voice_manager.Release(ev.note);
        }
        ++r;
    }
    __atomic_store_n(&s_note_q_read, r, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&s_voice_stop_all, false, __ATOMIC_ACQUIRE)) {
        s_voice_manager.StopAll();
    }
    return any_trigger;
}

// Sample RAM Manager (for loaded samples)
static SampleMemMgr s_sample_mem_mgr;

static BlockMeters s_last_block_meters = {0, 0, 0, 0};

// CPU Load Meter for audio processing performance monitoring
static CpuLoadMeter s_cpu_load_meter;
static float s_sample_rate = 48000.0f;
static int s_block_size = 48;

// Underrun detection state - set in callback, logged in main loop
static volatile bool s_underrun_detected = false;
static bool s_underrun_logged = false;

// Preview state (review M7): fixed-capacity, not a heap vector. The old
// std::vector reserved (end-start)/decim+1 elements straight from wire-
// controlled values - a PreviewReq with decim=1 over a long sample asked
// for megabytes of newlib heap, and with exceptions disabled a failed
// allocation terminates the firmware. 4096 points comfortably covers the
// 1280-px waveform view; OnPreviewReq widens decim to fit.
static constexpr uint32_t kMaxPreviewPoints = 4096;
static int16_t s_preview[kMaxPreviewPoints];
static uint32_t s_preview_len = 0;
static uint32_t s_prev_sent = 0;
// Staging for one outbound preview frame (header + samples); static so
// chunk sends don't heap-allocate per frame.
static uint8_t s_preview_frame[sizeof(WaveX::Protocol::WaveChunkMessage) +
                               kMaxPreviewPoints * sizeof(int16_t)];

static void SendPreviewChunks() {
    // Prefer to send the entire preview in one frame if it fits.
    constexpr uint16_t kMaxSingleFrameSamples = 900;  // header + 900*2 < 2048 payload limit
    if (s_preview_len <= kMaxSingleFrameSamples) {
        WaveX::Protocol::WaveChunkMessage header{};
        header.offset = 0;
        header.count = static_cast<uint16_t>(s_preview_len);

        const size_t payload_bytes =
            sizeof(header) + static_cast<size_t>(header.count) * sizeof(int16_t);
        memcpy(s_preview_frame, &header, sizeof(header));
        memcpy(s_preview_frame + sizeof(header), s_preview, header.count * sizeof(int16_t));

        int res = WaveX::Comm::UartLinkSend(
            WaveX::Protocol::MSG_WAVE_CHUNK, s_preview_frame, static_cast<uint16_t>(payload_bytes));
        if (res < 0) {
            // Queue full: drain one frame and retry once (review Finding 5).
            WaveX::Comm::UartLinkPumpTx();
            res = WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_WAVE_CHUNK,
                                            s_preview_frame,
                                            static_cast<uint16_t>(payload_bytes));
        }
        return;
    }

    constexpr uint16_t kChunkSamples = 256;

    // The TX queue is only 4 deep and normally drains one frame per
    // main-loop pass; the old version queued every chunk in a tight loop,
    // ignored the queue-full return, and advanced s_prev_sent regardless -
    // silently dropping everything past the 4th chunk and leaving holes in
    // the waveform preview (review Finding 5). Now: on queue-full, pump the
    // TX queue directly (UartLinkPumpTx is TX-only, safe from this
    // message-handler context) and retry the same chunk. kMaxPumps bounds
    // the extra main-loop blocking this adds (~2.6ms wire time per 522-byte
    // chunk pumped at 2 Mbaud); if the link is genuinely stalled we abort
    // loudly with the tail missing rather than punching silent mid-stream
    // gaps.
    uint32_t pumps_remaining = 32;

    while (s_prev_sent < s_preview_len) {
        uint16_t remaining =
            static_cast<uint16_t>(std::min<uint32_t>(kChunkSamples, s_preview_len - s_prev_sent));

        WaveX::Protocol::WaveChunkMessage header{};
        header.offset = s_prev_sent;
        header.count = remaining;

        const size_t payload_bytes =
            sizeof(header) + static_cast<size_t>(remaining) * sizeof(int16_t);
        memcpy(s_preview_frame, &header, sizeof(header));
        memcpy(
            s_preview_frame + sizeof(header), s_preview + s_prev_sent, remaining * sizeof(int16_t));

        int res = WaveX::Comm::UartLinkSend(
            WaveX::Protocol::MSG_WAVE_CHUNK, s_preview_frame, static_cast<uint16_t>(payload_bytes));
        if (res < 0) {
            if (pumps_remaining == 0) {
                if (s_hw)
                    s_hw->PrintLine("DAISY: preview send aborted at offset %u (TX stalled)",
                                    (unsigned)s_prev_sent);
                return;  // partial preview; chunk offsets make the gap visible upstream
            }
            --pumps_remaining;
            WaveX::Comm::UartLinkPumpTx();
            continue;  // retry the same chunk; s_prev_sent unchanged
        }
        s_prev_sent += remaining;
    }
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
// (Review C2: a SampleLoadState tracker for MSG_SAMPLE_DATA streaming sat
// here; its `loading` flag was never set true anywhere, so the whole
// push-sample-data-over-the-link receive path was unreachable. Removed -
// samples load from the Daisy's own SD card via OnSampleLoad below.)
//
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
static constexpr size_t kLoadedSampleCapacity = kMaxZones;
static LoadedSampleInfo s_loaded_samples[kLoadedSampleCapacity];
static size_t s_loaded_sample_count = 0;

static LoadedSampleInfo* find_loaded_sample(uint16_t sample_id) {
    for (size_t i = 0; i < s_loaded_sample_count; ++i) {
        auto& entry = s_loaded_samples[i];
        if (entry.sample_id == sample_id) {
            return &entry;
        }
    }
    return nullptr;
}

// Drops `sample_id` from the registry and returns its memory to the arena.
// Entries stay in load order (oldest first), so removal closes the gap by
// shifting rather than swapping with the tail: find_playable_sample() and
// OnPreviewReq() both read the last entry as "most recently loaded", and a
// swap would quietly hand them an older sample.
static void remove_loaded_sample(uint16_t sample_id) {
    for (size_t i = 0; i < s_loaded_sample_count; ++i) {
        if (s_loaded_samples[i].sample_id != sample_id) {
            continue;
        }
        s_sample_mem_mgr.release(&s_loaded_samples[i].handle);
        for (size_t j = i + 1; j < s_loaded_sample_count; ++j) {
            s_loaded_samples[j - 1] = s_loaded_samples[j];
        }
        --s_loaded_sample_count;
        return;
    }
}

// Retires the least recently loaded sample. Callers MUST have passed
// OnSampleLoad's stop-all barrier first: this frees SDRAM that a playing
// voice would otherwise still be reading through its non-owning
// Voice::sample pointer.
static bool evict_oldest_loaded_sample() {
    if (s_loaded_sample_count == 0) {
        return false;
    }
    remove_loaded_sample(s_loaded_samples[0].sample_id);
    return true;
}

static bool upsert_loaded_sample(const SampleLoadMessage& sl, const wxsamp_t& handle) {
    LoadedSampleInfo info;
    info.sample_id = sl.sample_id;
    info.handle = handle;
    info.allocated_bytes = handle.len ? handle.len : sl.sample_size;
    info.loaded_bytes = 0;
    info.sample_rate = sl.sample_rate;
    info.channels = sl.channels;
    info.bit_depth = sl.bit_depth;

    // Re-loading an id retires the previous entry and appends a fresh one, so
    // the reloaded sample becomes the newest rather than staying at its old
    // index. Updating in place made "most recently loaded" wrong for any id
    // that was ever loaded twice.
    remove_loaded_sample(sl.sample_id);
    while (s_loaded_sample_count >= kLoadedSampleCapacity) {
        if (!evict_oldest_loaded_sample()) {
            return false;
        }
    }
    s_loaded_samples[s_loaded_sample_count++] = info;
    return true;
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
    1024;  // ~21ms at 48kHz (much more responsive for auditioning)
static q15_t s_prebuffer[PREBUFFER_FRAMES * kMaxMixChannels];  // ~23ms of interleaved audio
static uint32_t s_prebuffer_filled = 0;                        // Number of frames pre-buffered
static bool s_prebuffer_ready = false;  // Whether pre-buffer is ready for playback
static bool s_prebuffering = false;     // Whether we're currently pre-buffering

// Background SD I/O system with larger buffers
static const uint32_t SD_BUFFER_SIZE = 8192;  // 8KB SD read buffer for better performance
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
// (A 64 KB s_conversion_buffer scratch array sat here, unreferenced since
// the scratch-pool refactor - deleted, review H1. Conversion/resample
// scratch comes from s_scratch_pool via AcquireScratch().)
alignas(kSdBufferAlignment) static uint8_t s_prebuffer_sd[SD_BUFFER_SIZE];
// Audio performance instrumentation
static uint32_t s_io_start_time = 0;
static uint32_t s_io_duration = 0;
static uint32_t s_max_io_duration = 0;
static uint32_t s_io_count = 0;
// Streaming telemetry counters (WAVEX_DAISY_STREAM_DEBUG in
// hardware_config.h). Record why the streaming path discarded its last pass
// without consuming. Always updated - the writes are a few registers and
// keeping them unconditional avoids a second code path that only exists in
// debug builds; only the reporting in main.cpp is gated.
static uint32_t s_dbg_free = 0;       // rb_free_frames() at the top of the pass
static uint32_t s_dbg_want = 0;       // frames_to_transfer after all caps
static uint32_t s_dbg_resampled = 0;  // LinearResampleFrames() result
static uint32_t s_dbg_pushes = 0;     // passes that actually reached rb_push_frames
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

// Multi-channel linear resample over an interleaved buffer. The math lives
// in linear_resampler.hpp so it can be host-tested (see
// tests/unit/audio/linear_resampler_test.cpp); this wrapper keeps the old
// signature and the scratch-pool contract its callers were written against.
//
// The header reads each channel through a stride, so the per-channel
// de-interleave copy the CMSIS version needed is gone - one fewer full pass
// over the chunk, and the scratch buffer it used is no longer acquired here.
static uint32_t LinearResampleFrames(
    const q15_t* src, uint32_t src_frames, q15_t* dst, uint32_t channels, float ratio) {
    return ResampleInterleaved(src, src_frames, dst, channels, ratio);
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

    // When resampling, cap the request so all scratch users of this pass fit
    // the pool together - same formula and reasoning as PumpWavIO (review
    // Finding 6): conversion (f x out_ch) + resampler's per-channel buffer
    // (f) + resample output ((ceil(f x ratio) + 1) x out_ch). This must
    // happen BEFORE the SD read, because bytes are consumed from the file
    // as soon as they're read - there is no retry path here.
    if (resample_ratio != 1.0f) {
        const float per_frame_cost = static_cast<float>(s_output_channels) + 1.0f +
                                     resample_ratio * static_cast<float>(s_output_channels);
        const uint32_t fixed_cost = s_output_channels + 1;
        uint32_t max_by_scratch = static_cast<uint32_t>(
            static_cast<float>(kScratchPoolSamples - fixed_cost) / per_frame_cost);
        if (max_by_scratch == 0) {
            s_prebuffering = false;
            return false;  // cannot make progress; unreachable with a 32KB pool
        }
        if (req_frames > max_by_scratch) {
            req_frames = max_by_scratch;
        }

        // LinearResampleFrames() returns 0 for fewer than 2 input frames, and
        // a 0 return is treated below as "drop this chunk" - which does not
        // advance s_prebuffer_filled. Once the remaining space caps the
        // request at 1 frame, that combination spins forever: the pre-buffer
        // parks a few frames short of PREBUFFER_FRAMES, never reports ready,
        // nothing is ever handed to the ring buffer, and the main loop burns
        // a 4-byte SD read per iteration while audio stays silent. The
        // pre-buffer is latency headroom, not an exact target, so treat
        // "cannot resample any further" as full. Only non-48kHz files reach
        // this path at all (resample_ratio == 1.0 skips the whole branch).
        if (req_frames < 2) {
            s_prebuffer_ready = true;
            s_prebuffering = false;
            return true;
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

#if WAVEX_DAISY_SD_DEBUG
    // Log I/O performance every 100 operations
    if (s_io_count % 100 == 0) {
        if (s_hw)
            s_hw->PrintLine("SD I/O Stats: count=%u, max_duration=%u ms, last_duration=%u ms",
                            (unsigned)s_io_count,
                            (unsigned)s_max_io_duration,
                            (unsigned)s_io_duration);
    }
#endif

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
        if (resample_ratio != 1.0f) {
            // Can't resample without scratch, and the bytes are already
            // consumed from the file (no retry path). Drop the chunk rather
            // than writing wrong-pitch audio into the prebuffer (review
            // Finding 6). Unreachable in practice: req_frames is capped by
            // scratch capacity above.
            return true;
        }
        // Pitch-correct fallback (ratio == 1.0 only): convert directly into
        // the prebuffer. frames_read <= req_frames <= free_prebuffer_frames,
        // so this cannot overflow.
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
        uint32_t resampled = 0;
        if (resample_buffer != nullptr) {
            resampled = LinearResampleFrames(
                conversion_output, frames_read, resample_buffer, s_output_channels, resample_ratio);
        }
        if (resampled == 0) {
            // Same reasoning as the null-scratch case above: drop, don't
            // push unresampled (wrong-pitch) audio (review Finding 6).
            return true;
        }
        to_push = resample_buffer;
        output_frames = resampled;
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

void Init(DaisySeed& hw, float sample_rate, bool sdram_available) {
    s_hw = &hw;
    s_sample_rate = sample_rate;

    WaveX::Profiling::InitHardware();
    PROFILE_REGISTER_ZONE(audio_callback);
    PROFILE_REGISTER_ZONE(wav_pump_io);
    PROFILE_REGISTER_ZONE(format_conversion);
    PROFILE_REGISTER_ZONE(ring_buffer_push);
    PROFILE_REGISTER_ZONE(prebuffer_audio);
    PROFILE_REGISTER_ZONE(sd_refill);

#if WAVEX_CV_BACKEND == WAVEX_CV_BACKEND_MCP4728
    s_cv_backend.Init(0x60);
#else
    s_cv_backend.Init();
#endif

    // Initialize only when hardware SDRAM bring-up succeeded. The centralized
    // layout gives samples 60 MiB and reserves the final 4 MiB for the offline
    // render scratch described in docs/features/offline-sample-editing.md.
    s_sample_memory_available =
        sdram_available && s_sample_mem_mgr.init(reinterpret_cast<void*>(WaveX::SdramLayout::kBase),
                                                 WaveX::SdramLayout::kSampleArenaBytes,
                                                 WaveX::SdramLayout::kSmallSamplePoolBytes);
    if (s_hw) {
        s_hw->PrintLine("AUDIO_ENGINE: Sample RAM %s (arena=%lu, render scratch=%lu)",
                        s_sample_memory_available ? "ready" : "disabled",
                        (unsigned long)WaveX::SdramLayout::kSampleArenaBytes,
                        (unsigned long)WaveX::SdramLayout::kRenderScratchBytes);
    }

    s_voice_manager.Init(static_cast<uint32_t>(sample_rate));

    // Sequencer transport uses the same sample-rate/block-size timebase as
    // the audio engine so its scheduler frames line up with the callback.
    s_seq_transport.Init(static_cast<uint32_t>(sample_rate), Timebase::kBlockSize);

    // Stage A paraphonic envelope runs at the control-tick rate (1 kHz).
    // Defaults are musical bring-up values; stage 3 maps ENVELOPE_* wire
    // parameters onto SetParams.
    s_para_env.Init(1000);
    s_para_env.SetParams(s_para_params.attack_s,
                         s_para_params.decay_s,
                         s_para_params.sustain,
                         s_para_params.release_s);

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

    // MIDI note path (roadmap Phase 1 item 8): apply pending note events,
    // then mix the RAM-resident voices on top of the streaming/ring
    // content above. Render() only runs when a voice is active, so the
    // startup silence requirement is preserved. All callback-safe: fixed
    // buffers, no allocation, no I/O, no logging.
    const bool any_note_on = drain_note_queue();
    if (s_voice_manager.ActiveVoiceCount() > 0 &&
        size <= static_cast<size_t>(Timebase::kBlockSize)) {
        static float vm_l[Timebase::kBlockSize];
        static float vm_r[Timebase::kBlockSize];
        s_voice_manager.Render(vm_l, vm_r, size);
        for (size_t i = 0; i < size; ++i) {
            out[0][i] += vm_l[i];
            out[1][i] += vm_r[i];
        }
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

    Timebase::Tick1kHz([&] {
        // Stage A paraphonic control law (item 5): the shared envelope
        // gates the analog VCA and modulates the shared VCF cutoff above
        // its base. Staging is callback-safe (QueueGroup only writes
        // fields); the I2C transaction happens in FlushCv() on the main
        // loop. A flush racing a tick can read one channel from the
        // previous tick - benign, self-corrects on the next flush.
        const bool any_held = s_voice_manager.HeldVoiceCount() > 0;
        const float env = s_para_env.Tick(any_note_on, any_held);
        if (s_cv_test_active) {
            // Calibration override: steady, user-commanded CVs.
            s_cv_router.QueueVoice(s_cv_test_group, s_cv_test_cutoff, s_cv_test_res, s_cv_test_vca);
        } else {
            const float cutoff =
                CvClamp01(s_para_params.cutoff_base + s_para_params.env_to_cutoff * env);
            s_cv_router.QueueVoice(0, cutoff, s_para_params.resonance, env);
        }
        __atomic_store_n(&s_cv_dirty, true, __ATOMIC_RELEASE);
    });

    // End CPU load measurement for this audio block
    s_cpu_load_meter.OnBlockEnd();
    s_dwt_callback_cycles = WaveX::Profiling::GetCycles() - callback_cycles_start;
    s_dwt_callback_max = std::max(s_dwt_callback_max, s_dwt_callback_cycles);
}

// MSG_CONTROL_CHANGE -> Stage A paraphonic path (item 5 stage 3). These
// write s_para_params from main-loop message-handler context; the control
// tick reads them in audio context (aligned float stores are atomic on
// Cortex-M7, single writer per field). Envelope-time changes push the full
// ADSR set into the shared envelope; a tick landing between two of those
// field writes briefly mixes old/new rates - inaudible and self-correcting.
// Per-voice / kit parameter routing remains Phase 2 front-panel work.
void OnControlChange(const ControlChangeMessage& ctrl_msg) {
    const float norm = static_cast<float>(ctrl_msg.value) / 65535.0f;
    switch (ctrl_msg.parameter) {
        case PARAM_FILTER_CUTOFF:
            s_para_params.cutoff_base = norm;
            break;
        case PARAM_FILTER_RESONANCE:
            s_para_params.resonance = norm;
            break;
        case PARAM_ENVELOPE_ATTACK:
        case PARAM_ENVELOPE_DECAY:
        case PARAM_ENVELOPE_SUSTAIN:
        case PARAM_ENVELOPE_RELEASE: {
            // Times span 1 ms .. 2 s; sustain is the raw 0..1 level.
            const float seconds = 0.001f + norm * 2.0f;
            if (ctrl_msg.parameter == PARAM_ENVELOPE_ATTACK)
                s_para_params.attack_s = seconds;
            else if (ctrl_msg.parameter == PARAM_ENVELOPE_DECAY)
                s_para_params.decay_s = seconds;
            else if (ctrl_msg.parameter == PARAM_ENVELOPE_SUSTAIN)
                s_para_params.sustain = norm;
            else
                s_para_params.release_s = seconds;
            s_para_env.SetParams(s_para_params.attack_s,
                                 s_para_params.decay_s,
                                 s_para_params.sustain,
                                 s_para_params.release_s);
            break;
        }
        case PARAM_MODULATION_MATRIX:
            // Repurposed for Stage A as the envelope->cutoff modulation
            // depth until Phase 2 defines a real mod matrix.
            s_para_params.env_to_cutoff = norm;
            break;
        default:
            // PARAM_VOLUME / LFO_*: no Stage A consumer (the analog VCA is
            // the level control; a global LFO is future work).
            break;
    }
}

// --- CV calibration workflow (item 5 stage 4; analog-voice-board.md §3).
// All main-loop message-handler context: SetGroupCal writes the backend's
// cal table (read at the tick - float fields, same handoff contract as
// s_para_params), SD I/O is blocking FatFS on the main loop.

static void SendCvCalResp(uint8_t group) {
    const CvCal& c = s_cv_backend.GroupCal(group);
    CvCalMessage resp(group,
                      0,
                      c.vcf_cut_gain,
                      c.vcf_cut_off,
                      c.vcf_q_gain,
                      c.vcf_q_off,
                      c.vca_gain,
                      c.vca_off,
                      c.cutoff_k);
    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_CV_CAL_RESP, &resp, sizeof(resp));
}

void OnCvCalSet(const CvCalMessage& m) {
    if (m.group >= WAVEX_ANALOG_CV_GROUPS_MAX) {
        if (s_hw)
            s_hw->PrintLine("CV CAL: group %u out of range", (unsigned)m.group);
        return;
    }
    CvCal cal;
    cal.vcf_cut_gain = m.vcf_cut_gain;
    cal.vcf_cut_off = m.vcf_cut_off;
    cal.vcf_q_gain = m.vcf_q_gain;
    cal.vcf_q_off = m.vcf_q_off;
    cal.vca_gain = m.vca_gain;
    cal.vca_off = m.vca_off;
    cal.cutoff_k = m.cutoff_k;
    s_cv_backend.SetGroupCal(m.group, cal);

    if (m.persist) {
        CvCal table[WAVEX_ANALOG_CV_GROUPS_MAX];
        for (uint8_t g = 0; g < WAVEX_ANALOG_CV_GROUPS_MAX; ++g) {
            table[g] = s_cv_backend.GroupCal(g);
        }
        const bool saved = WaveX::Cv::SaveCvCalTable(s_cvcal_file, table);
        if (s_hw)
            s_hw->PrintLine(
                "CV CAL: group %u applied, persist %s", (unsigned)m.group, saved ? "OK" : "FAILED");
    }
    SendCvCalResp(m.group);
}

void OnCvCalGet(const CvCalGetMessage& m) {
    if (m.group >= WAVEX_ANALOG_CV_GROUPS_MAX) {
        return;
    }
    SendCvCalResp(m.group);
}

void OnCvTest(const CvTestMessage& m) {
    s_cv_test_group = m.group < WAVEX_ANALOG_CV_GROUPS_MAX ? m.group : 0;
    s_cv_test_cutoff = m.cutoff;
    s_cv_test_res = m.resonance;
    s_cv_test_vca = m.vca;
    // Write the flag last: once true, the tick may read the values above.
    __atomic_store_n(&s_cv_test_active, m.enable != 0, __ATOMIC_RELEASE);
    if (s_hw)
        s_hw->PrintLine("CV TEST: %s (cut=%d res=%d vca=%d x1000)",
                        m.enable ? "ON" : "off",
                        (int)(m.cutoff * 1000),
                        (int)(m.resonance * 1000),
                        (int)(m.vca * 1000));
}

void LoadCvCalFromSd() {
    CvCal table[WAVEX_ANALOG_CV_GROUPS_MAX];
    if (!WaveX::Cv::LoadCvCalTable(s_cvcal_file, table)) {
        if (s_hw)
            s_hw->PrintLine("CV CAL: no stored table (using defaults)");
        return;
    }
    for (uint8_t g = 0; g < WAVEX_ANALOG_CV_GROUPS_MAX; ++g) {
        s_cv_backend.SetGroupCal(g, table[g]);
    }
    if (s_hw)
        s_hw->PrintLine("CV CAL: table loaded from SD");
}

// ---- Sequencer / transport / MIDI-clock (roadmap Phase 2) ----
// All four run in main-loop message-handler context and forward to the
// engine-owned SequencerTransport, which is only mutated from this context
// (see the s_seq_transport declaration for why Tick() is not yet driven
// from the callback). Real forwarding, not stubs - the transport's state
// changes are covered by sequencer_transport_test; message_dispatch_test
// pins that the wire message reaches here.
void OnSeqTransport(const SeqTransportMessage& m) {
    s_seq_transport.ApplyTransport(m);
#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (s_hw)
        s_hw->PrintLine("RX SEQ_TRANSPORT: cmd=%u src=%u bpm=%u",
                        (unsigned)m.command,
                        (unsigned)m.clock_source,
                        (unsigned)m.tempo_bpm_x100);
#endif
}

void OnSeqPatternOp(const SeqPatternOpMessage& m) {
    s_seq_transport.ApplyPatternOp(m);
}

void OnMidiClockEvent(const MidiClockEventMessage& m) {
    s_seq_transport.OnMidiClock(m);
}

void OnMidiCc(const MidiCcMessage& m) {
    s_seq_transport.OnMidiCc(m);
}

// Note-to-sample mapping policy for item 8: the most recently loaded
// playable sample, treated as root note 60 (a kit/pad mapping concept
// arrives with the Phase 2 sequencer). "Playable" means 16-bit PCM, mono
// or stereo - the voice manager reads int16 interleaved data directly;
// 24-bit files would need a load-time conversion pass (not yet built).
static const LoadedSampleInfo* find_playable_sample() {
    for (size_t i = s_loaded_sample_count; i > 0; --i) {
        const auto& entry = s_loaded_samples[i - 1];
        if (entry.bit_depth == 16 && (entry.channels == 1 || entry.channels == 2)) {
            return &entry;
        }
    }
    return nullptr;
}

void OnNoteOn(const NoteMessage& note_msg) {
    static constexpr uint8_t kDefaultRootNote = 60;

    const LoadedSampleInfo* src = find_playable_sample();
    void* sample_ptr = nullptr;
    if (src && (!s_sample_mem_mgr.ptr(src->handle, &sample_ptr) || !sample_ptr)) {
        src = nullptr;
    }
    if (!src) {
        // Nothing playable loaded (or only 24-bit files): drop the note.
        // A previous "test oscillator fallback" here set state on DSP
        // objects Callback() never rendered - silent while claiming
        // otherwise (review C2) - so it was removed rather than fixed;
        // load a 16-bit sample to verify the MIDI path end-to-end.
        if (s_hw)
            s_hw->PrintLine("RX NOTE_ON: note=%u vel=%u ch=%u -> dropped (no playable sample)",
                            (unsigned)note_msg.note,
                            (unsigned)note_msg.velocity,
                            (unsigned)note_msg.channel);
        return;
    }

    const uint32_t bytes = src->loaded_bytes ? src->loaded_bytes : src->handle.len;
    const uint32_t bytes_per_frame = 2u * src->channels;

    NoteEvent ev;
    ev.is_trigger = true;
    ev.note = note_msg.note;
    ev.params.sample = static_cast<const int16_t*>(sample_ptr);
    ev.params.sample_frames = bytes / bytes_per_frame;
    ev.params.channels = src->channels;
    ev.params.note = note_msg.note;
    ev.params.velocity = note_msg.velocity;
    ev.params.root_note = kDefaultRootNote;
    ev.params.sample_rate_hz = src->sample_rate;  // 44.1k content pitches correctly on 48k engine

    const bool queued = note_queue_push(ev);
    if (!queued && s_hw)
        s_hw->PrintLine("RX NOTE_ON: note=%u DROPPED - note queue full", (unsigned)note_msg.note);
#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (queued && s_hw)
        s_hw->PrintLine("RX NOTE_ON: note=%u vel=%u ch=%u -> sample_id=%u (%lu frames)",
                        (unsigned)note_msg.note,
                        (unsigned)note_msg.velocity,
                        (unsigned)note_msg.channel,
                        (unsigned)src->sample_id,
                        (unsigned long)ev.params.sample_frames);
#endif
}

void OnNoteOff(const NoteMessage& note_msg) {
    NoteEvent ev;
    ev.is_trigger = false;
    ev.note = note_msg.note;
    note_queue_push(ev);  // Release() of an unknown note is a no-op, safe to always send

#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (s_hw)
        s_hw->PrintLine(
            "RX NOTE_OFF: note=%u ch=%u", (unsigned)note_msg.note, (unsigned)note_msg.channel);
#endif
}

// Wire hook for MSG_SAMPLE_CTRL (record/play transport from the UI's
// record page). Deliberately a no-op today (review C2): the Sampler these
// commands drove was inert end-to-end - nothing fed it input and nothing
// rendered its playback - so it was deleted rather than left pretending to
// record. Rebuild against the voice/streaming architecture when recording
// is actually scheduled (offline-editing work, Phase 4).
void OnSampleCtrl(const SampleCtrlMessage& sc) {
    if (s_hw)
        s_hw->PrintLine("SAMPLE_CTRL cmd=%u ignored (recording not implemented - review C2)",
                        (unsigned)sc.cmd);
}

void OnPreviewReq(const PreviewReqMessage& pr) {
    s_prev_sent = 0;
    s_preview_len = 0;

    // Pick the most recently loaded sample; fall back to empty if none.
    if (s_loaded_sample_count == 0) {
        if (s_hw) {
            s_hw->PrintLine("PREVIEW: No loaded samples; skipping preview");
        }
        return;
    }

    const LoadedSampleInfo& src = s_loaded_samples[s_loaded_sample_count - 1];
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
    uint32_t decim = pr.decim ? pr.decim : 1;

    // Clamp the point count to the fixed preview buffer by widening the
    // decimation instead of truncating the range (review M7): the full
    // selection stays visible, just coarser. Wire-controlled start/end/
    // decim can no longer request an unbounded heap allocation.
    const uint32_t span = end - start;
    if (span / decim + 1 > kMaxPreviewPoints) {
        decim = span / (kMaxPreviewPoints - 1) + 1;
    }

    const int16_t* samples16 = reinterpret_cast<const int16_t*>(sample_ptr);
    const uint8_t* samples24 = reinterpret_cast<const uint8_t*>(sample_ptr);

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
        s_preview[s_preview_len++] = v;
        if (s_preview_len >= kMaxPreviewPoints) {
            break;  // defensive; the decim widening above should prevent this
        }
    }

    if (s_hw) {
        s_hw->PrintLine(
            "PREVIEW: Built preview for sample_id=%u frames=%lu decim=%u preview_len=%u",
            (unsigned)src.sample_id,
            (unsigned long)total_frames,
            (unsigned)decim,
            (unsigned)s_preview_len);
    }

    SendPreviewChunks();
}

void OnSampleLoad(const SampleLoadMessage& sl) {
    if (!s_sample_memory_available) {
        if (s_hw)
            s_hw->PrintLine("SAMPLE_LOAD: rejected because SDRAM is unavailable");
        return;
    }
    if (s_hw) {
        s_hw->PrintLine("SAMPLE_LOAD: path='%s' id=%u", sl.path, (unsigned)sl.sample_id);
    }
    // CRITICAL: Stop ALL SD activity (audition/playback) and ensure PumpWavIO is not running.
    // FatFS + SDMMC are NOT thread-safe or re-entrant. The main loop calls PumpWavIO() which
    // will conflict with f_open/f_read calls here if s_wav.open is true.
    StopAudition();

    CloseWav();

    // Voices may still be reading the sample memory this load is about to
    // release/rewrite (upsert_loaded_sample below). Ask the audio callback
    // to hard-stop all voices; the delay below (blocks are 1 ms) guarantees
    // it has acted before any memory is touched.
    __atomic_store_n(&s_voice_stop_all, true, __ATOMIC_RELEASE);
    // Add a small delay to ensure any in-flight SD DMA completes
    System::Delay(10);

    // Use static FIL (too large for stack - ~600 bytes with SDMMC buffer).
    // Keep in normal BSS like s_wav.file so cache maintenance works correctly.
    // CRITICAL: Do NOT memset() the FIL - it has internal buffer pointers managed by FatFS.
    FIL& file = s_sample_load_file;

    // Try raw path first (matches playback/audition). If that fails and path does not include a
    // drive prefix, retry with "0:" prefix to be tolerant of mount styles.
    FRESULT fr = f_open(&file, sl.path, FA_READ);
    if (fr != FR_OK && strncmp(sl.path, "0:", 2) != 0) {
        char alt_path[128];
        snprintf(alt_path, sizeof(alt_path), "0:%s", sl.path);
        fr = f_open(&file, alt_path, FA_READ);
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
        return;
    }

    // Shared RIFF walk (wav/wav_header_parser.hpp, review M12 - the old
    // hand-rolled copy here skipped odd-sized chunks without the RIFF pad
    // byte and mis-parsed WAVs with odd LIST/INFO chunks before data).
    WaveX::Wav::WavInfo wav_info;
    WaveX::Storage::FatFsWavReader reader(file);
    const auto parse_result = WaveX::Wav::ParseWavHeader(reader, wav_info);
    if (parse_result != WaveX::Wav::ParseResult::Ok) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: invalid WAV header (parse result %d)", (int)parse_result);
        }
        f_close(&file);
        return;
    }
    const uint16_t num_ch = wav_info.num_channels;
    const uint32_t sample_rate = wav_info.sample_rate;
    const uint16_t bits = wav_info.bits_per_sample;
    const uint32_t data_off = wav_info.data_offset;
    const uint32_t data_size = wav_info.data_size;

    if (wav_info.audio_format != 1 || (bits != 16 && bits != 24) || (num_ch != 1 && num_ch != 2)) {
        if (s_hw) {
            s_hw->PrintLine("SAMPLE_LOAD: unsupported format fmt=%u bits=%u ch=%u",
                            (unsigned)wav_info.audio_format,
                            (unsigned)bits,
                            (unsigned)num_ch);
        }
        f_close(&file);
        return;
    }

    // The browser hands out a fresh sample_id for every audition, so no load
    // ever replaces an earlier one and nothing reclaims the arena on its own.
    // Retire the least recently loaded samples until this one fits. The
    // stop-all barrier above already drained the note queue and idled every
    // voice, so the memory this releases has no remaining readers.
    wxsamp_t handle = {};
    while (!s_sample_mem_mgr.alloc(data_size, &handle)) {
        if (!evict_oldest_loaded_sample()) {
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
            s_hw->PrintLine("SAMPLE_LOAD: evicted oldest sample to fit %lu bytes",
                            (unsigned long)data_size);
        }
    }

    void* sample_ptr = nullptr;
    if (!s_sample_mem_mgr.ptr(handle, &sample_ptr)) {
        s_sample_mem_mgr.release(&handle);
        f_close(&file);
        return;
    }

    f_lseek(&file, data_off);
    uint32_t remaining = data_size;
    uint32_t written = 0;
    UINT br = 0;
    // Use a 32-byte-aligned AXI-SRAM staging buffer; stack/DTCM is not
    // accessible to SDMMC IDMA.
    uint8_t* temp = s_sample_io;
    constexpr UINT kIoChunk = sizeof(s_sample_io);

    while (remaining > 0) {
        UINT to_read = (remaining > kIoChunk) ? kIoChunk : remaining;

        fr = f_read(&file, temp, to_read, &br);

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

    f_close(&file);

    if (!upsert_loaded_sample(sl, handle)) {
        if (s_hw)
            s_hw->PrintLine("SAMPLE_LOAD: registry full (%u entries)",
                            (unsigned)kLoadedSampleCapacity);
        s_sample_mem_mgr.release(&handle);
        return;
    }
    update_loaded_sample_progress(sl.sample_id, data_size);

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

    const size_t count = std::min<size_t>(s_loaded_sample_count, WAVEX_SAMPLE_STATUS_MAX_ENTRIES);
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

void GetMeters(BlockMeters& out) {
    out = s_last_block_meters;
}

// ============================
// CPU Load Monitoring Functions
// ============================

// Check for underruns detected in audio callback and log them (called from main loop)
void CheckAndLogUnderruns() {
    // Rate-limited on purpose. This logged once per underrun EPISODE, which
    // during intermittent starvation meant a blocking USB CDC write every few
    // main-loop passes - 13k+ lines in a single audition - stealing the very
    // main-loop time the ring refill needs. That is a feedback loop: the
    // logging deepens the starvation it reports, exactly like the 1 ms
    // LONG I/O threshold did. Count every episode, report at most once per
    // second, and include the count so bursts stay visible.
    static uint32_t episodes = 0;
    static uint32_t last_report_ms = 0;

    if (s_underrun_detected && !s_underrun_logged) {
        episodes++;
        s_underrun_logged = true;
        s_underrun_detected = false;  // Reset detection flag
    } else if (!s_underrun_detected && s_underrun_logged) {
        // Reset logging flag when underruns stop
        s_underrun_logged = false;
    }

    if (episodes == 0) {
        return;
    }
    const uint32_t now = System::GetNow();
    if (last_report_ms != 0 && (now - last_report_ms) < 1000u) {
        return;
    }
    last_report_ms = now;
    if (s_hw) {
        s_hw->PrintLine("AUDIO: Ring buffer underrun - outputting silence (%u in last ~1s)",
                        (unsigned)episodes);
    }
    episodes = 0;
}

// Main-loop only: performs the blocking CV DAC transaction (~225 us
// MCP4728 fast-write) for values staged at the control tick - never in the
// callback (§7.1.4 / analog-voice-board.md §0 timing rules). If the DAC
// is absent (bench without the Stage A breadboard), eight consecutive I2C
// failures disable the flush with one log line instead of paying the
// transaction timeout every millisecond forever.
void FlushCv() {
    static uint32_t consecutive_failures = 0;
    static bool disabled_logged = false;
    constexpr uint32_t kMaxConsecutiveFailures = 8;

    if (consecutive_failures >= kMaxConsecutiveFailures) {
        if (!disabled_logged) {
            disabled_logged = true;
            if (s_hw)
                s_hw->PrintLine("CV: MCP4728 not responding after %u attempts - CV flush disabled",
                                (unsigned)kMaxConsecutiveFailures);
        }
        return;
    }
    if (!__atomic_exchange_n(&s_cv_dirty, false, __ATOMIC_ACQUIRE)) {
        return;  // nothing staged since the last flush
    }
    if (s_cv_router.Flush()) {
        consecutive_failures = 0;
    } else {
        consecutive_failures++;
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

    // Shared RIFF walk (wav/wav_header_parser.hpp, review M12 - the old
    // hand-rolled copy here skipped odd-sized chunks without the RIFF pad
    // byte and mis-parsed WAVs with odd LIST/INFO chunks before data).
    WaveX::Wav::WavInfo wav_info;
    WaveX::Storage::FatFsWavReader reader(s_wav.file);
    const auto parse_result = WaveX::Wav::ParseWavHeader(reader, wav_info);
    if (parse_result != WaveX::Wav::ParseResult::Ok) {
        if (s_hw)
            s_hw->PrintLine(
                "WAV open failed: header parse error %d for %s", (int)parse_result, path);
        f_close(&s_wav.file);
        return false;
    }

    // Support PCM format (fmt=1), 16-bit or 24-bit, mono or stereo
    if (wav_info.audio_format != 1 ||
        (wav_info.bits_per_sample != 16 && wav_info.bits_per_sample != 24) ||
        (wav_info.num_channels != 1 && wav_info.num_channels != 2)) {
        if (s_hw)
            s_hw->PrintLine("WAV open failed: unsupported format fmt=%u bits=%u ch=%u",
                            (unsigned)wav_info.audio_format,
                            (unsigned)wav_info.bits_per_sample,
                            (unsigned)wav_info.num_channels);
        f_close(&s_wav.file);
        return false;  // only PCM16/24 mono/stereo supported
    }

    // Leave the file positioned at the data payload for streaming.
    f_lseek(&s_wav.file, wav_info.data_offset);

    s_wav.open = true;
    s_wav.data_start = wav_info.data_offset;
    s_wav.data_size = wav_info.data_size;
    s_wav.bytes_remaining = wav_info.data_size;
    s_wav.num_channels = wav_info.num_channels;
    s_wav.bits_per_sample = wav_info.bits_per_sample;
    s_wav.sample_rate = wav_info.sample_rate;

    // Reset buffers
    s_rb_head = 0;
    s_rb_tail = 0;

#if WAVEX_DAISY_SD_DEBUG
    if (s_hw)
        s_hw->PrintLine("WAV open ok: %s ch=%u sr=%lu bits=%u size=%lu",
                        path,
                        (unsigned)wav_info.num_channels,
                        (unsigned long)wav_info.sample_rate,
                        (unsigned)wav_info.bits_per_sample,
                        (unsigned long)wav_info.data_size);
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

// Streaming telemetry accessor - see WAVEX_DAISY_STREAM_DEBUG.
void GetStreamDebug(uint32_t& prebuf_filled,
                    uint32_t& prebuf_target,
                    uint32_t& wav_sample_rate,
                    uint8_t& wav_channels,
                    uint8_t& wav_bits) {
    prebuf_filled = s_prebuffer_filled;
    prebuf_target = PREBUFFER_FRAMES;
    wav_sample_rate = s_wav.sample_rate;
    wav_channels = s_wav.num_channels;
    wav_bits = s_wav.bits_per_sample;
}

// Streaming telemetry accessor - see WAVEX_DAISY_STREAM_DEBUG.
void GetStreamDiscardDebug(uint32_t& free_frames,
                           uint32_t& want_frames,
                           uint32_t& resampled,
                           uint32_t& pushes) {
    free_frames = s_dbg_free;
    want_frames = s_dbg_want;
    resampled = s_dbg_resampled;
    pushes = s_dbg_pushes;
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
    s_dbg_free = free_frames;
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

    // A 1-frame slot tail cannot be resampled: LinearResampleFrames() needs
    // >= 2 input frames to interpolate, and the skip-without-consume paths
    // below would then retry this same 1-frame request on every pump forever
    // - the ring drains, nothing refills it, and playback stalls into
    // permanent underrun (the "stalls after ~15s" bug; STREAM2 caught it as
    // free=2047 want=1 resampled=0 with pushes frozen). Retire the slot
    // instead. The linear resampler already discards the final input frame
    // of every chunk at chunk boundaries, so dropping this tail frame
    // (~23 us of audio per affected 8KB slot) matches the existing quality.
    if (resample_ratio != 1.0f && available_frames < 2) {
        slot.ready = false;
        slot.consumed = 0;
        s_sd_consume_index = (s_sd_consume_index + 1) % kSdBufferCount;
        return;
    }

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

    // When resampling, cap the input so ALL scratch users of this pass fit
    // the pool together: conversion (f x out_ch) + LinearResampleFrames'
    // internal per-channel buffer (f) + the resample output
    // ((ceil(f x ratio) + 1) x out_ch). Without this cap, a full 8KB slot of
    // mono audio needing resample (e.g. a 44.1kHz file on the 48kHz engine)
    // exceeds the 16K-sample pool on every pass, and the old fallback below
    // then pushed the audio UNRESAMPLED - i.e. at the wrong pitch
    // (docs/dma-timing-review-2026-07-03.md, Finding 6).
    if (resample_ratio != 1.0f) {
        const float per_frame_cost = static_cast<float>(s_output_channels) /*conversion*/
                                     + 1.0f /*per-channel de-interleave*/
                                     + resample_ratio * static_cast<float>(s_output_channels);
        const uint32_t fixed_cost = s_output_channels + 1;  // ceil()+1 output slack
        uint32_t max_by_scratch = static_cast<uint32_t>(
            static_cast<float>(kScratchPoolSamples - fixed_cost) / per_frame_cost);
        if (max_by_scratch == 0) {
            return;  // cannot make progress; should not happen with a 32KB pool
        }
        if (frames_to_transfer > max_by_scratch) {
            frames_to_transfer = max_by_scratch;
        }
    }

    s_dbg_want = frames_to_transfer;
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
        uint32_t resampled = 0;
        if (resample_buffer != nullptr) {
            resampled = LinearResampleFrames(conversion_output,
                                             frames_to_transfer,
                                             resample_buffer,
                                             s_output_channels,
                                             resample_ratio);
        }
        s_dbg_resampled = resampled;
        if (resampled == 0) {
            // Resampling unavailable this pass (scratch exhausted or the
            // resampler failed). The old fallback pushed conversion_output
            // UNRESAMPLED - audio at the wrong pitch (review Finding 6).
            // Skip instead: nothing is consumed from the slot, so the same
            // data is retried next pump with a fresh scratch pool.
            s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
            s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
            return;
        }
        final_buffer = resample_buffer;
        final_frames = resampled;
    }

    if (final_frames > free_frames) {
        // Output slightly exceeds ring space (the ceil()+1 sizing slack at
        // the upsampling boundary). The old code truncated the push while
        // still consuming the full input - silently dropping frames (review
        // Finding 6). Skip instead; the ring drains ~48 frames/ms, so the
        // retry lands almost immediately.
        s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
        s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
        return;
    }

    ++s_dbg_pushes;
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
