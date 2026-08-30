#include "comm/log_ring.h"

#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include <daisy.h>  // For CpuLoadMeter

extern "C" SD_HandleTypeDef hsd1;  // libDaisy per/sdmmc.cpp

#include "../memory.h"
#include "../memory_sections.h"  // For WAVEX_DTCM_DATA
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
#include "storage/sd_sdio.h"
#include "sys/dma.h"  // For cache management

#include "../cv/cv_cal_store.hpp"
#include "../cv/cv_group_router.hpp"
#include "../sequencer/sequencer_transport.hpp"
#include "../storage/fatfs_wav_reader.hpp"
#include "../timebase.hpp"
#include "fade.hpp"
#include "instrument.hpp"
#include "linear_resampler.hpp"
#include "output_sink.hpp"
#include "paraphonic_envelope.hpp"
#include "voice_manager.hpp"
#include "wav/wav_header_parser.hpp"
#include <algorithm>
#include <atomic>
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
// DTCM: read/written every sample for every active voice from Callback()
// itself, CPU-only (never DMA'd), and small (~1 KB for 8 voices) - the
// canonical "hot DSP state" case for the Cortex-M7 tightly-coupled RAM
// (docs/daisy_rt_audio_coding_guide.md §2/§8).
static WaveX::AudioEngine::VoiceManager s_voice_manager WAVEX_DTCM_DATA;

// Digital voice base parameters - what MSG_CONTROL_CHANGE edits for the
// all-digital path (features/digital-voice-audition.md stage 1). Written from
// main-loop message-handler context, read in audio context: the same handoff
// contract as s_para_params below (aligned float stores are atomic on
// Cortex-M7, single writer per field). A block landing between two field
// writes of one gesture briefly mixes old and new values, which is inaudible
// and self-correcting.
//
// ENGINE-GLOBAL, not per-slot, and deliberately so. param-locks-and-
// modulation.md scopes base values to an instrument slot, but nothing can
// address a slot differently yet - OnNoteOn does not even set one - so a
// 16-entry table would be 16 copies of the same values with no way to reach
// 15 of them. This mirrors s_para_params, which is engine-global for the
// analog path for the same reason. It becomes per-slot with the instrument
// model (Phase 2.5), which is also when a slot becomes addressable.
//
// DTCM for the same reason as s_voice_manager: read from Callback(), CPU-only,
// tiny.
static WaveX::AudioEngine::VoiceLiveParams s_voice_live_params WAVEX_DTCM_DATA;

// Set by OnControlChange (main loop), consumed by Callback() (audio context).
// Without it every block would push identical values onto all 8 voices and
// recompute a filter coefficient per voice for nothing.
static volatile bool s_voice_live_dirty WAVEX_DTCM_DATA = false;

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
// DTCM: ticked once per block from Callback() itself, CPU-only, small - same
// case as s_voice_manager above.
static ParaphonicEnvelope s_para_env WAVEX_DTCM_DATA;

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

// The four statics below are all written every audio block from Callback()
// itself (CPU-only, never DMA'd) and are individually tiny - DTCM per the
// same reasoning as s_voice_manager/s_para_env above, extended to the
// per-block performance-stat counters rather than just DSP state.
static BlockMeters s_last_block_meters WAVEX_DTCM_DATA = {0, 0, 0, 0};

// CPU Load Meter for audio processing performance monitoring
static CpuLoadMeter s_cpu_load_meter WAVEX_DTCM_DATA;
// Counts audio callbacks. The callback is driven by SAI DMA interrupts, not
// the main loop, so comparing its rate against the expected 1 kHz separates
// "the ring starved" from "the callback stopped running" - the latter reports
// no underrun at all, because underruns are only detected inside it.
static volatile uint32_t s_callback_blocks WAVEX_DTCM_DATA = 0;
// Lowest ring occupancy seen since the last report, sampled once per audio
// callback. Zero underruns only proves the ring never hit empty; it says
// nothing about how close it came. A dip toward empty is what a brief gap
// sounds like, and this is the only thing that can show it.
static volatile uint32_t s_rb_low_water WAVEX_DTCM_DATA = 0xFFFFFFFFu;
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

// Sends one preview frame per call and is driven from the main loop every
// pass (like PumpEnvelopeJob/PumpWavIO), NOT looped-and-retried inline.
//
// UartLinkPumpTx() (== process_tx_queue()) only ever STARTS or RETIRES a DMA
// transfer; it never waits for one to complete. A frame takes ~2.6 ms of wire
// time at 2 Mbaud, so spin-calling it dozens of times back-to-back (the old
// approach) burns microseconds, not milliseconds, and the queue-full retry
// budget exhausts before the in-flight frame has actually drained - previews
// past ~1150 points (4 queued chunks) were truncated on nearly every
// request. UartLinkProcess() already calls process_tx_queue() once per
// main-loop pass regardless, so simply trying one send per pass and leaving
// the rest of the preview queued for the next pass(es) needs no pumping of
// its own and cannot truncate: it only ever waits, never gives up.
void PumpPreviewSend() {
    if (s_prev_sent >= s_preview_len) {
        return;  // nothing pending (also covers preview_len == 0)
    }

    // Prefer to send the entire preview in one frame if it fits - only
    // meaningful for the first chunk, since a partially-sent preview by
    // definition no longer fits in one frame the way this check means it.
    constexpr uint16_t kMaxSingleFrameSamples = 900;  // header + 900*2 < 2048 payload limit
    constexpr uint16_t kChunkSamples = 256;
    const bool whole_fits = (s_prev_sent == 0) && (s_preview_len <= kMaxSingleFrameSamples);
    const uint16_t count =
        whole_fits
            ? static_cast<uint16_t>(s_preview_len)
            : static_cast<uint16_t>(std::min<uint32_t>(kChunkSamples, s_preview_len - s_prev_sent));

    WaveX::Protocol::WaveChunkMessage header{};
    header.offset = s_prev_sent;
    header.count = count;

    const size_t payload_bytes = sizeof(header) + static_cast<size_t>(count) * sizeof(int16_t);
    memcpy(s_preview_frame, &header, sizeof(header));
    memcpy(s_preview_frame + sizeof(header), s_preview + s_prev_sent, count * sizeof(int16_t));

    const int res = WaveX::Comm::UartLinkSend(
        WaveX::Protocol::MSG_WAVE_CHUNK, s_preview_frame, static_cast<uint16_t>(payload_bytes));
    if (res < 0) {
        return;  // TX queue full; retry the same chunk next pass, state unchanged
    }
    s_prev_sent += count;
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
    // Kept so a poisoned file object can be reopened in place. FatFS latches
    // a disk error into the FIL, after which every f_read fails immediately
    // and only a fresh f_open clears it - which is exactly why stopping and
    // re-triggering the audition by hand was the only way back.
    char path[128];

    // Non-destructive edit (MSG_SAMPLE_EDIT_SET). Byte offsets, not frames:
    // every consumer below works in bytes, and converting once here keeps the
    // per-pass arithmetic out of the refill path. Defaults are the whole file
    // with looping off, so an un-edited sample behaves exactly as before.
    uint32_t region_start;  // absolute file offset
    uint32_t region_end;    // absolute file offset, exclusive
    uint32_t loop_start;
    uint32_t loop_end;
    bool loop_enabled;
    q15_t gain_q15;  // 32767 = unity

    // Region fades (roadmap 1.5.6 item 3), in FRAMES at the file's own rate.
    // Held in frames rather than bytes because the fade position is compared
    // against a frame index inside the conversion loop, where the byte offsets
    // above have already been turned back into frames anyway.
    uint32_t region_start_frame;
    uint32_t region_end_frame;
    uint32_t fade_in_frames;
    uint32_t fade_out_frames;
};
static WavState s_wav = {};

// Loop gap (browser audition). Frames of silence still owed after a rewind.
static uint32_t s_loop_gap_frames = 0;  // configured length
static uint32_t s_loop_gap_remaining = 0;

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
// Sample-load staging buffer. Read size is the dominant factor in load time:
// FatFS and the SDMMC driver charge a largely FIXED cost per f_read (cluster
// walk, bookkeeping, IDMA setup), so cutting the call count cuts most of the
// overhead rather than a proportional slice. This was 1 KB while the streaming
// path next door used 8 KB (SD_BUFFER_SIZE) - loading issued eight times the
// calls to move the same bytes, for no reason anyone recorded.
//
// WHY 32 KB AND NOT MORE. The obvious ceiling would be main-loop blocking: a
// long f_read starves the WAV ring, which has only ~42 ms of headroom. It does
// not bind here, because OnSampleLoad calls CloseWav() before this loop -
// nothing is streaming during a load, so there is no ring to starve. What
// binds is the SD driver's own sector-count contract (below),
// and after that AXI SRAM - this is a permanent static allocation for a
// transient purpose. Beyond one cluster the read is a contiguous multi-block
// transfer running at the card's streaming rate anyway, so a larger buffer has
// little left to exploit even where the driver would allow it.
//
// HARD CEILING, learned the hard way: libDaisy's SD_read() (sd_diskio.c)
// documents "count: Number of sectors to read (1..128)", and FatFS passes the
// contiguous sector count straight through - ff.c clips it at the CLUSTER
// boundary, not at 128. A 64 KB request on a 64 KB-cluster card therefore
// becomes a single 128-sector disk_read, sitting exactly on the documented
// limit, against a driver this firmware had only ever run at 16 sectors (the
// 8 KB streaming path). That is what a first attempt at 64 KB did, and it
// crashed on load.
//
// 32 KB keeps the sector count at or below 64 whatever the cluster size:
// FatFS's clip means cc <= min(request, cluster), so bounding the request
// bounds cc. That is half the documented ceiling and four times the size this
// driver is proven at, which is the right side of a limit to sit on.
//
// The static_assert below is the point: this is enforced rather than
// remembered, so raising the constant fails the build instead of the card.
static constexpr UINT kSdSectorBytes = 512;
static constexpr UINT kSdMaxSectorsPerRead = 128;  // sd_diskio.c SD_read() contract
static constexpr UINT kSampleLoadChunkMax = 32768;
static_assert(kSampleLoadChunkMax / kSdSectorBytes <= kSdMaxSectorsPerRead / 2,
              "Sample-load reads must stay well inside SD_read()'s 1..128 sector contract; "
              "FatFS clips only at the cluster boundary, so the request size is the bound.");
alignas(32) static uint8_t s_sample_io[kSampleLoadChunkMax];

// Picks the read size from the mounted filesystem's actual geometry rather than
// a constant that guesses at it. FatFS is most efficient reading whole
// clusters: a read that ends mid-cluster leaves the next one straddling a
// boundary, which costs an extra FAT walk on every pass.
//
// Cluster size comes free from the open file (FIL::obj.fs->csize) - no
// f_getfree(), which would scan the entire FAT to count free clusters and can
// take seconds on a large card. Sector size is a compile-time 512 here
// (_MAX_SS == _MIN_SS == 512), so FATFS::ssize does not even exist to read.
//
// Refinement not taken: the first read starts at the WAV's data offset, which
// is not cluster-aligned (typically 44 B in), so every read straddles a
// boundary by that much. Sizing the first read to reach the next boundary would
// align all the rest. Worth doing only if a measurement says the boundary
// crossings cost more than the extra branch - for a contiguous file FatFS
// already issues one multi-sector transfer across it.
static UINT pick_sample_load_chunk(const FIL& file) {
    if (!file.obj.fs || file.obj.fs->csize == 0) {
        return kSampleLoadChunkMax;
    }
    const UINT cluster_bytes = static_cast<UINT>(file.obj.fs->csize) * kSdSectorBytes;
    if (cluster_bytes == 0 || cluster_bytes > kSampleLoadChunkMax) {
        // One cluster is bigger than the buffer: read the whole buffer, which
        // is still a whole number of sectors.
        return kSampleLoadChunkMax;
    }
    return (kSampleLoadChunkMax / cluster_bytes) * cluster_bytes;
}

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
    // The authoritative record. Every playback and display path reads its
    // markers, gain and channel mode from here, so streaming audition, RAM
    // voices and the preview generator cannot disagree about the same sample.
    WaveX::Protocol::SampleMetadata meta = {};
};
static constexpr size_t kLoadedSampleCapacity = kMaxZones;
static LoadedSampleInfo s_loaded_samples[kLoadedSampleCapacity];
static size_t s_loaded_sample_count = 0;

// Which sample MSG_NOTE_ON addresses. 0 means "most recently loaded playable
// one", which is what the engine did before there was any way to choose.
static uint16_t s_selected_sample_id = 0;

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
// Sends one sample's record. Called on load, on edit, and on request - the
// frontend never derives these values, it is told them.
static void PushSampleMeta(const LoadedSampleInfo& info) {
    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_META, &info.meta, sizeof(info.meta));
}

// Position of the streaming audition within its region, in frames.
//
// This is the READ position, which runs ahead of what is audible by the ring
// contents (2048 frames, ~42 ms) plus any filled SD slots. That is under a
// tenth of a second and invisible on a progress bar, but it is not a playhead
// - do not use it to drive anything sample-accurate.
bool GetPlaybackPosition(uint32_t& frames_played, uint32_t& region_frames) {
    frames_played = 0;
    region_frames = 0;
    if (!s_wav.open) {
        return false;
    }
    const uint32_t bytes_per_sample = (s_wav.bits_per_sample == 24) ? 3u : 2u;
    const uint32_t file_bpf = (uint32_t)s_wav.num_channels * bytes_per_sample;
    if (file_bpf == 0) {
        return false;
    }
    const uint32_t start = s_wav.region_start;
    const uint32_t end = s_wav.region_end;
    if (end <= start) {
        return false;
    }
    region_frames = (end - start) / file_bpf;

    const uint32_t pos = f_tell(&s_wav.file);
    frames_played = (pos > start) ? ((pos - start) / file_bpf) : 0u;
    if (frames_played > region_frames) {
        frames_played = region_frames;
    }
    return true;
}

void SetLoopGapMs(uint16_t gap_ms) {
    s_loop_gap_frames = (static_cast<uint32_t>(gap_ms) * s_sample_rate) / 1000u;
    s_loop_gap_remaining = 0;  // never start an audition mid-gap
}

void PushAllSampleMeta(uint16_t sample_id) {
    for (size_t i = 0; i < s_loaded_sample_count; ++i) {
        if (sample_id == 0 || s_loaded_samples[i].sample_id == sample_id) {
            PushSampleMeta(s_loaded_samples[i]);
        }
    }
}

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

void SelectSample(uint16_t sample_id) {
    s_selected_sample_id = sample_id;
    WaveX::Log::PrintLine("SAMPLE_SELECT: id=%u", (unsigned)sample_id);
}

uint16_t SelectedSample() {
    return s_selected_sample_id;
}

bool UnloadSample(uint16_t sample_id) {
    if (sample_id == 0) {
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=0 rejected (not a wildcard)");
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < s_loaded_sample_count; ++i) {
        if (s_loaded_samples[i].sample_id == sample_id) {
            found = true;
            break;
        }
    }
    if (!found) {
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=%u not loaded", (unsigned)sample_id);
        return false;
    }

    // Stop every voice before releasing the memory. Voices hold a non-owning
    // pointer into the sample's block, so freeing it under a sounding voice is
    // a use-after-free in the audio path.
    //
    // Ask the CALLBACK to do the stopping rather than calling StopAll() from
    // here. This function runs on the main loop, and a direct call would leave
    // a window where the callback is already inside Render() holding a voice's
    // sample pointer - stopping a voice it has finished reading for this block
    // does nothing about the block it is in the middle of. drain_note_queue()
    // consumes this flag at the top of the callback, so after one block period
    // no voice can still be reading. Same barrier OnSampleLoad takes before it
    // evicts; the 10 ms is its margin over the 1 ms block, kept identical
    // rather than tuned, since nothing here is latency-sensitive.
    __atomic_store_n(&s_voice_stop_all, true, __ATOMIC_RELEASE);
    System::Delay(10);

    remove_loaded_sample(sample_id);

    // A selection pointing at what we just freed would otherwise silently fall
    // back to "most recent", which is a different sample than the user asked
    // for. Clearing it makes the fallback explicit instead.
    if (s_selected_sample_id == sample_id) {
        s_selected_sample_id = 0;
    }

    WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=%u freed (%u still loaded)",
                          (unsigned)sample_id,
                          (unsigned)s_loaded_sample_count);
    return true;
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

// Defined with the envelope job below; declared here because loading is the
// one event that can pull the audio out from under a scan in flight.
static void CancelEnvelopeJob();

static bool upsert_loaded_sample(const SampleLoadMessage& sl, const wxsamp_t& handle) {
    CancelEnvelopeJob();

    LoadedSampleInfo info;
    info.sample_id = sl.sample_id;
    info.handle = handle;
    info.allocated_bytes = handle.len ? handle.len : sl.sample_size;
    info.loaded_bytes = 0;
    info.sample_rate = sl.sample_rate;
    info.channels = sl.channels;
    info.bit_depth = sl.bit_depth;

    // Seed the record. Markers default to the whole sample and gain to unity,
    // so an unedited sample behaves as it always has; every later change goes
    // through SetEditParams, which re-pushes.
    const uint32_t bpf = (sl.bit_depth / 8u) * (sl.channels ? sl.channels : 1u);
    //
    // generation is the cache-invalidation hook (roadmap 1.5.5 item 4).
    // Loading a different file into an id that is already in use IS a content
    // change, even though nothing renders destructively yet: a frontend
    // holding an envelope for the old audio must not keep drawing it. Marker
    // and gain edits deliberately do not bump it - they change what plays,
    // not what the sample contains, so the cached envelope stays valid.
    const LoadedSampleInfo* previous = find_loaded_sample(sl.sample_id);
    const uint16_t next_generation =
        previous ? static_cast<uint16_t>(previous->meta.generation + 1) : 0;

    info.meta = WaveX::Protocol::SampleMetadata();
    info.meta.generation = next_generation;
    info.meta.sample_id = sl.sample_id;
    info.meta.sample_rate = sl.sample_rate;
    info.meta.total_frames = bpf ? (info.allocated_bytes / bpf) : 0;
    info.meta.end_frame = info.meta.total_frames;
    info.meta.loop_end = info.meta.total_frames;
    info.meta.channels = sl.channels;
    info.meta.bits_per_sample = sl.bit_depth;
    info.meta.channel_mode = WaveX::Protocol::SAMPLE_CH_AS_RECORDED;
    WaveX::Protocol::detail::CopyWireString(info.meta.name, sizeof(info.meta.name), sl.path);

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
    PushSampleMeta(s_loaded_samples[s_loaded_sample_count - 1]);
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
// Lock-free by construction on Cortex-M7 (aligned 32-bit load/store is a single
// instruction); std::atomic<uint32_t>::is_always_lock_free isn't usable here because this
// target's <atomic> predates the C++17 addition of that member despite compiling as C++17.
static uint32_t s_rb_head = 0;
static uint32_t s_rb_tail = 0;
// Gates whether rb_pop_stereo_batch() (audio ISR, sole consumer) may touch
// s_rb_head/s_rb_tail at all. OpenWav()/CloseWav() run on the main loop
// (producer context) and are the only callers that reset BOTH indices
// together; a direct write to s_rb_tail from there raced the ISR's own
// tail update if an audio block landed mid-reset (same-core IRQ
// preemption, not a multi-core race), which could lose one of the two
// writes and leave (head - tail) briefly huge, playing stale ring
// contents. Clearing this first makes the consumer bail out before it
// reads or writes either index, so the producer-side reset below is
// never observed half-applied.
static bool s_rb_live = false;
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
    // Absolute file offset this slot's first frame was read from. The fade
    // needs to know WHERE in the region a block sits, and by the time the
    // block is converted the file handle has already moved on - so the
    // position has to be captured at read time, not derived at use time.
    uint32_t file_offset = 0;
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
// Failed f_read attempts. Kept separate from s_io_count, which only counts
// SUCCESSFUL reads: the failure path recorded s_io_duration and returned
// before incrementing the count, so a card that had stopped responding
// presented as "count frozen, last still changing" with no error anywhere -
// the read error itself was compiled out behind WAVEX_DAISY_SD_DEBUG.
static uint32_t s_io_errors = 0;
static uint32_t s_io_last_err = 0;
static uint32_t s_io_recoveries = 0;
// Set when playback stops for a STORAGE reason rather than a user request, so
// the frontend can be told. Without it the Daisy goes quiet while the ESP32
// still believes it is auditioning - the UI sits on "Playing" forever.
static bool s_playback_aborted = false;
// Retries are held off after a failure. Without this the pump spins as fast
// as the loop runs - measured at ~40,000 failed reads per second, each
// returning in ~8 us - which burns the main loop and floods the log to no
// purpose, since a poisoned FIL cannot succeed until it is reopened.
static uint32_t s_io_backoff_until_ms = 0;
// Per-interval throughput/latency accumulators, reset each time they are
// read. Kept separate from the since-boot totals so a report describes the
// interval it covers rather than the whole run, which is what makes a
// degradation visible.
static uint32_t s_iv_bytes = 0;
static uint32_t s_iv_reads = 0;
static uint32_t s_iv_ticks = 0;
static uint32_t s_iv_min_ticks = 0xFFFFFFFFu;
static uint32_t s_iv_max_ticks = 0;
constexpr uint32_t kIoBackoffMs = 20;
// A couple of failures can be a transient card hiccup; a run of them means
// the FIL is poisoned and only a reopen will clear it. Recoveries are capped
// so a genuinely dead card ends playback instead of reopening forever.
constexpr uint32_t kIoErrorsBeforeRecover = 3;
constexpr uint32_t kMaxIoRecoveries = 5;
// Streaming telemetry counters (WAVEX_DAISY_STREAM_DEBUG in
// hardware_config.h). Record why the streaming path discarded its last pass
// without consuming. Always updated - the writes are a few registers and
// keeping them unconditional avoids a second code path that only exists in
// debug builds; only the reporting in main.cpp is gated.
static uint32_t s_dbg_free = 0;       // rb_free_frames() at the top of the pass
static uint32_t s_dbg_want = 0;       // frames_to_transfer after all caps
static uint32_t s_dbg_resampled = 0;  // LinearResampleFrames() result
static uint32_t s_dbg_pushes = 0;     // passes that actually reached rb_push_frames

// Interval counters for MSG_DIAG_PUSH. Deltas, reset on read - a since-boot
// total cannot show that discards started thirty seconds ago, which is exactly
// the shape both playback stalls had.
static uint32_t s_diag_pushes = 0;
static uint32_t s_diag_discards = 0;   // passes that produced frames and then
                                       // skipped without consuming the slot
static uint32_t s_diag_underruns = 0;  // underrun episodes
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

// Playback gain, applied once on the converted block. Written out rather than
// calling arm_scale_q15: CMSIS-DSP's BasicMathFunctions are not in the linked
// set for this target, and this is a two-line multiply.
//
// Saturating on purpose. A wrapping multiply turns a hot sample into
// full-scale noise at the exact moment the user pushes gain up, which is the
// worst possible failure mode for a gain control - clipping is merely loud.
static void ApplyWavGain(q15_t* buf, uint32_t samples) {
    if (s_wav.gain_q15 == 32767 || samples == 0) {
        return;  // unity: skip the pass entirely
    }
    const int32_t g = s_wav.gain_q15;
    for (uint32_t i = 0; i < samples; ++i) {
        int32_t v = (static_cast<int32_t>(buf[i]) * g) >> 15;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        buf[i] = static_cast<q15_t>(v);
    }
}

// Region fade / de-click, applied to the converted block before resampling
// (roadmap 1.5.6 item 3). Pre-resample because the fade position is a SOURCE
// frame index: after resampling the block no longer maps one-to-one onto file
// frames, and the ramp would drift against the region boundary it exists to
// cover.
//
// `first_frame` is the region-relative index of the block's first frame.
// Costs nothing on the common path: with no fade in range the whole call is
// two comparisons.
static void ApplyWavFade(q15_t* buf, uint32_t frames, uint32_t first_frame) {
    if (frames == 0) {
        return;
    }
    const uint32_t fade_in = s_wav.fade_in_frames;
    const uint32_t fade_out = s_wav.fade_out_frames;
    if (fade_in == 0 && fade_out == 0) {
        return;
    }
    const uint32_t start = s_wav.region_start_frame;
    const uint32_t end = s_wav.region_end_frame;
    if (end <= start) {
        return;
    }

    const uint32_t block_start = start + first_frame;
    const uint32_t block_end = block_start + frames;
    // Skip the pass entirely unless the block actually overlaps a ramp. A
    // three-minute file is thousands of blocks and two of them are fades.
    const bool touches_in = (fade_in > 0) && (block_start < start + fade_in);
    const bool touches_out = (fade_out > 0) && (block_end > end - std::min(fade_out, end - start));
    if (!touches_in && !touches_out) {
        return;
    }

    for (uint32_t i = 0; i < frames; ++i) {
        const float g =
            WaveX::AudioEngine::RegionFadeGain(block_start + i, start, end, fade_in, fade_out);
        if (g >= 0.999999f) {
            continue;
        }
        const int32_t gq = static_cast<int32_t>(g * 32768.0f);
        q15_t* frame_ptr = buf + i * s_output_channels;
        for (uint32_t ch = 0; ch < s_output_channels; ++ch) {
            frame_ptr[ch] = static_cast<q15_t>((static_cast<int32_t>(frame_ptr[ch]) * gq) >> 15);
        }
    }
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
// One state for the whole stream: prebuffer_audio() fills the head of a file
// and PumpWavIO() takes over from there, so they are consecutive chunks of the
// SAME stream and must share the phase. Reset when a file is opened or closed.
static StreamResamplerState s_resampler;

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
            WaveX::Log::PrintLine("Pre-buffer complete: %u frames ready",
                                  (unsigned)PREBUFFER_FRAMES);
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
            WaveX::Log::PrintLine("Pre-buffer complete: %u frames (end of file)",
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

    if (fr != FR_OK || br == 0) {
#if WAVEX_DAISY_SD_DEBUG
        if (s_hw)
            WaveX::Log::PrintLine("Pre-buffer read error: fr=%d, br=%u", (int)fr, (unsigned)br);
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
    ApplyWavGain(conversion_output, frames_read * s_output_channels);

    q15_t* to_push = conversion_output;
    uint32_t output_frames = frames_read;
    if (resample_ratio != 1.0f) {
        uint32_t max_out_frames =
            static_cast<uint32_t>(std::ceil(frames_read * resample_ratio)) + 1;
        q15_t* resample_buffer = AcquireScratch(max_out_frames * s_output_channels);
        uint32_t resampled = 0;
        // Same snapshot reasoning as the streaming path: the drop below
        // discards this pass's output, so the phase must not stay advanced.
        const StreamResamplerState resampler_before = s_resampler;
        if (resample_buffer != nullptr) {
            resampled = ResampleStreamInterleaved(s_resampler,
                                                  conversion_output,
                                                  frames_read,
                                                  resample_buffer,
                                                  max_out_frames,
                                                  s_output_channels,
                                                  resample_ratio);
        }
        if (resampled == 0) {
            // Same reasoning as the null-scratch case above: drop, don't
            // push unresampled (wrong-pitch) audio (review Finding 6).
            s_resampler = resampler_before;
            return true;
        }
        to_push = resample_buffer;
        output_frames = resampled;
    }

    if (output_frames > free_prebuffer_frames) {
        // Truncating here would discard output whose input the resampler has
        // already consumed and phase-advanced past - a silent gap. The
        // pre-buffer is latency headroom, so stop short and let the streaming
        // path continue from the correct phase instead.
        output_frames = free_prebuffer_frames;
    }

    // Push into the pre-buffer
    q15_t* dst = &s_prebuffer[s_prebuffer_filled * s_output_channels];
    arm_copy_q15(to_push, dst, output_frames * s_output_channels);
    s_prebuffer_filled += output_frames;

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

    // Hold off after a failure rather than hammering a handle that cannot
    // succeed. 20 ms is well inside the ring's ~42 ms of headroom, so a
    // transient error costs no audio if the next attempt works.
    if (s_io_backoff_until_ms != 0) {
        if (System::GetNow() < s_io_backoff_until_ms) {
            return false;
        }
        s_io_backoff_until_ms = 0;
    }

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

        // Cap the request at the region (or loop) end. Without this the reader
        // runs to the end of the file and the markers have no audible effect.
        const uint32_t stop_at = s_wav.loop_enabled ? s_wav.loop_end : s_wav.region_end;
        const uint32_t pos = f_tell(&s_wav.file);
        if (pos < stop_at) {
            const uint32_t to_stop = stop_at - pos;
            if (req_bytes > to_stop) {
                req_bytes = (to_stop / file_bpf) * file_bpf;
                req_frames = req_bytes / file_bpf;
            }
        } else {
            req_bytes = 0;  // at or past the boundary: rewind or stop below
        }

        if (req_bytes == 0) {
            // Region end reached. Loop back to the loop point if looping, to
            // the region start otherwise. Logged unconditionally
            // (it happens once per pass through the file, so it cannot spam)
            // because a periodic audible artefact with every other metric
            // healthy points straight here, and this event was previously
            // invisible - it sat behind WAVEX_DAISY_SD_DEBUG, and f_lseek is
            // outside the s_io_duration timer that only wraps f_read, so the
            // rewind cost never appeared in I/O Stats either.
            // Owe the gap before the next pass starts.
            s_loop_gap_remaining = s_loop_gap_frames;
            const uint32_t rewind_to = s_wav.loop_enabled ? s_wav.loop_start : s_wav.region_start;
            const uint32_t seek_start = System::GetTick();
            f_lseek(&s_wav.file, rewind_to);
            const uint32_t seek_ticks = System::GetTick() - seek_start;
            const uint32_t ticks_per_us = System::GetTickFreq() / 1000000u;
            const uint32_t now_ms = System::GetNow();
            static uint32_t s_last_loop_ms = 0;
            WaveX::Log::PrintLine("WAV loop: rewind at %lu ms (period %lu ms, lseek %lu us)",
                                  (unsigned long)now_ms,
                                  (unsigned long)(now_ms - s_last_loop_ms),
                                  (unsigned long)(seek_ticks / (ticks_per_us ? ticks_per_us : 1u)));
            s_last_loop_ms = now_ms;

            const uint32_t rewind_stop = s_wav.loop_enabled ? s_wav.loop_end : s_wav.region_end;
            s_wav.bytes_remaining = (rewind_stop > rewind_to) ? (rewind_stop - rewind_to) : 0u;
            req_frames = std::min(max_frames, s_wav.bytes_remaining / file_bpf);
            req_bytes = req_frames * file_bpf;
#if WAVEX_DAISY_SD_DEBUG
            if (s_hw)
                WaveX::Log::PrintLine("WAV loop: rewinding to data start");
#endif
        }

        if (req_bytes == 0)
            return false;

        UINT br = 0;
        const uint32_t read_at = f_tell(&s_wav.file);
        s_io_start_time = System::GetTick();
        FRESULT fr = f_read(&s_wav.file, slot.data, req_bytes, &br);
        s_io_duration = System::GetTick() - s_io_start_time;

        if (fr != FR_OK || br == 0) {
            // Logged unconditionally and rate-limited. A failing card is the
            // difference between "playing" and "silent", so it must never be
            // a compile-time option; but it can fail on every pump, so it
            // cannot log per occurrence either.
            s_io_errors++;
            s_io_last_err = static_cast<uint32_t>(fr);
            s_io_backoff_until_ms = System::GetNow() + kIoBackoffMs;
            static uint32_t last_err_log_ms = 0;
            const uint32_t now = System::GetNow();
            if (last_err_log_ms == 0 || (now - last_err_log_ms) >= 1000u) {
                last_err_log_ms = now;
                // hal_err is the SDMMC layer's own reason for the failure;
                // FR_DISK_ERR (1) only says "disk_read said no".
                WaveX::Log::PrintLine(
                    "WAV read FAILED: fr=%d br=%u hal_err=0x%08lX state=%u (%lu errors, %lu "
                    "bytes left)",
                    (int)fr,
                    (unsigned)br,
                    (unsigned long)HAL_SD_GetError(&hsd1),
                    (unsigned)HAL_SD_GetCardState(&hsd1),
                    (unsigned long)s_io_errors,
                    (unsigned long)s_wav.bytes_remaining);
            }
            // Recover in place. FatFS latches the disk error into the FIL,
            // so nothing short of a reopen clears it - which is why the only
            // working remedy was stopping the audition and re-triggering it
            // by hand. Do that automatically, from the current position, so
            // playback continues instead of dying silently.
            if (s_io_errors >= kIoErrorsBeforeRecover && s_io_recoveries < kMaxIoRecoveries) {
                s_io_recoveries++;

                // A data CRC failure is bit corruption on the wire, not a sick
                // card (the card reports TRANSFER state throughout), so
                // reopening at the same bus clock just fails again. Step the
                // clock down first and reopen at the slower rate. Only for
                // CRC: a timeout or a genuinely absent card is not fixed by
                // going slower, and downgrading on those would quietly cost
                // throughput for no reason.
                if ((HAL_SD_GetError(&hsd1) & SDMMC_ERROR_DATA_CRC_FAIL) != 0u) {
                    WaveX::Storage::SdSdio::DowngradeSpeed();
                }
                const uint32_t resume_at =
                    s_wav.data_start + (s_wav.data_size - s_wav.bytes_remaining);
                f_close(&s_wav.file);
                FRESULT reopen = f_open(&s_wav.file, s_wav.path, FA_READ);
                if (reopen == FR_OK) {
                    reopen = f_lseek(&s_wav.file, resume_at);
                }
                WaveX::Log::PrintLine("WAV recovery %lu/%lu: reopen '%s' at %lu -> %s",
                                      (unsigned long)s_io_recoveries,
                                      (unsigned long)kMaxIoRecoveries,
                                      s_wav.path,
                                      (unsigned long)resume_at,
                                      reopen == FR_OK ? "ok" : "FAILED");
                if (reopen == FR_OK) {
                    s_io_errors = 0;  // fresh budget for the next incident
                } else {
                    // Unrecoverable: stop pretending to play. CloseWav()
                    // clears the ring, so the callback reports silence rather
                    // than looping stale audio forever.
                    WaveX::Log::PrintLine("WAV: playback aborted - SD unreadable");
                    s_playback_aborted = true;
                    CloseWav();
                }
            } else if (s_io_recoveries >= kMaxIoRecoveries) {
                WaveX::Log::PrintLine("WAV: playback aborted - SD unreadable after %lu recoveries",
                                      (unsigned long)s_io_recoveries);
                s_playback_aborted = true;
                CloseWav();
            }
            return false;
        }

        slot.bytes = br;
        slot.frames = br / file_bpf;
        slot.consumed = 0;
        slot.file_offset = read_at;
        slot.ready = true;
        s_sd_fill_index = (idx + 1) % kSdBufferCount;
        s_wav.bytes_remaining -= br;

        s_io_count++;
        if (s_io_duration > s_max_io_duration) {
            s_max_io_duration = s_io_duration;
        }
        s_iv_bytes += br;
        s_iv_reads++;
        s_iv_ticks += s_io_duration;
        if (s_io_duration < s_iv_min_ticks) {
            s_iv_min_ticks = s_io_duration;
        }
        if (s_io_duration > s_iv_max_ticks) {
            s_iv_max_ticks = s_io_duration;
        }

        return true;
    }

    return true;
}

// Thread-safe ring buffer operations.
//
// These use the __atomic_* builtins with literal memory orders rather than
// std::atomic<>, matching the note queue above. That is not a style choice: the
// Daisy image is currently built with no -O flag, and at -O0 std::atomic<T>'s
// load/store do not inline and the memory_order argument is not constant
// folded, so every access became a chain of out-of-line libstdc++ calls plus a
// full seq_cst dmb - even where the source said relaxed. In this per-sample
// function that measured ~5x the instruction count of the volatile code it
// replaced, and was the bulk of the streaming-playback CPU regression seen on
// the bench (field-findings-20260829.md §1). The builtins inline correctly at
// every optimization level and carry identical semantics.
//
// s_rb_head/s_rb_tail are plain uint32_t accessed only through those builtins;
// acquire/release on the cross-context handoffs give the same ordering the
// old __DMB() pairs were reaching for, but tied to the actual publish
// (data-then-head, head-then-data-read-then-tail) instead of the index
// reads/writes in isolation.
static inline uint32_t rb_count_frames() {
    uint32_t head = __atomic_load_n(&s_rb_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&s_rb_tail, __ATOMIC_ACQUIRE);
    return (head - tail) & (RB_CAP_FRAMES - 1u);
}

static inline uint32_t rb_free_frames() {
    return (RB_CAP_FRAMES - 1u) - rb_count_frames();
}

static inline void rb_push_frames(const q15_t* samples, uint32_t frames) {
    if (frames == 0 || samples == nullptr)
        return;

    const uint32_t mask = RB_CAP_FRAMES - 1u;
    // Producer-owned index; only this function and OpenWav/CloseWav (same
    // main-loop context, never concurrent with this call) ever write it.
    uint32_t head = __atomic_load_n(&s_rb_head, __ATOMIC_RELAXED);
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

    // Release: publish the sample writes above before the consumer can see
    // the new head and read them.
    __atomic_store_n(&s_rb_head, head + frames, __ATOMIC_RELEASE);
}

// Consumer-side batch pop, called once per Callback() rather than once per
// sample (review D2). Both s_rb_live and s_rb_head are written only from
// main-loop context, which cannot run concurrently with this ISR - they are
// therefore constant for the full duration of one Callback() invocation, so
// reading each of them once here and computing the whole block's worth of
// available frames up front is exactly equivalent to the old per-sample
// rb_pop_stereo() (one acquire-load of s_rb_live, one relaxed-load of tail,
// one acquire-load of head, per sample: ~190 atomics/barriers per 48-frame
// block that this removes), not an approximation of it.
//
// Pops min(size, available) frames into out_l/out_r starting at out index 0
// and returns that count; the caller silences and handles underrun/startup
// accounting for any remaining samples itself, exactly as the old per-sample
// loop did on a failed pop.
static inline size_t rb_pop_stereo_batch(float* out_l, float* out_r, size_t size) {
    if (!__atomic_load_n(&s_rb_live, __ATOMIC_ACQUIRE)) {
        // Main loop is mid-reset (OpenWav/CloseWav) - treat the ring as
        // empty and touch neither index, matching rb_push_frames' contract.
        return 0;
    }

    uint32_t tail = __atomic_load_n(&s_rb_tail, __ATOMIC_RELAXED);
    // Acquire: synchronizes with rb_push_frames' release store, so the
    // sample data below is guaranteed visible once head has advanced past it.
    const uint32_t head = __atomic_load_n(&s_rb_head, __ATOMIC_ACQUIRE);
    const uint32_t mask = RB_CAP_FRAMES - 1u;
    const uint32_t available = (head - tail) & mask;
    const uint32_t stride = s_output_channels;  // Must match writer stride
    constexpr float kInt16ToFloat = 1.0f / 32768.0f;

    const size_t popped = std::min<size_t>(size, available);
    for (size_t i = 0; i < popped; ++i) {
        const uint32_t idx = (tail & mask) * stride;
        const int16_t l16 = s_rb[idx + 0];
        const int16_t r16 = (stride > 1) ? s_rb[idx + 1] : s_rb[idx + 0];
        out_l[i] = static_cast<float>(l16) * kInt16ToFloat;
        out_r[i] = static_cast<float>(r16) * kInt16ToFloat;
        ++tail;
    }

    if (popped > 0) {
        // Release: the data reads above are complete before the freed slots
        // are republished to the producer via the advanced tail.
        __atomic_store_n(&s_rb_tail, tail, __ATOMIC_RELEASE);
    }
    return popped;
}

// Resampling temporarily disabled - using direct playback

void Init(DaisySeed& hw, float sample_rate, bool sdram_available) {
    s_hw = &hw;
    s_sample_rate = sample_rate;

    // Give the DTCM-placed state its intended values.
    //
    // Objects in .dtcmram_bss get no static initialization at all: the section
    // is (NOLOAD) and nothing runs their constructors, so the default member
    // initializers written on these types never execute. MemorySections::
    // InitDtcmBss() now zeroes the section, which makes the state deterministic
    // - but zero is not the same as correct, and for two of these it is
    // actively wrong:
    //
    //   VoiceLiveParams zeroed means filter_cutoff_hz == 0 Hz (filter shut) and
    //   sustain_level == 0, so every triggered voice is silent. That is not a
    //   theoretical concern: it is what a zeroed section produced on the bench.
    //
    //   s_rb_low_water only ever latches downward, so starting at 0 pins the
    //   ring low-water diagnostic at "hit empty" forever.
    //
    // Assigning a default-constructed temporary runs the member initializers on
    // the stack and copies them in, which is the cheapest way to get the values
    // the type declares. Anything added to DTCM that has non-zero defaults
    // belongs in this block too.
    s_voice_live_params = WaveX::AudioEngine::VoiceLiveParams{};
    s_voice_live_dirty = false;
    s_rb_low_water = 0xFFFFFFFFu;

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
        WaveX::Log::PrintLine("AUDIO_ENGINE: Sample RAM %s (arena=%lu, render scratch=%lu)",
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
        WaveX::Log::PrintLine("AUDIO_ENGINE: Testing Sample RAM allocation...");
    }
    wxsamp_t test_handle = {};
    bool test_alloc = s_sample_mem_mgr.alloc(1024, &test_handle);  // Try to allocate 1KB
    if (test_alloc) {
        if (s_hw) {
            WaveX::Log::PrintLine(
                "AUDIO_ENGINE: Sample RAM test allocation successful (handle: cls=%u page=%u "
                "slot=%u)",
                (unsigned)test_handle.cls,
                (unsigned)test_handle.page,
                (unsigned)test_handle.slot);
        }
        s_sample_mem_mgr.release(&test_handle);  // Clean up test allocation
        if (s_hw) {
            WaveX::Log::PrintLine("AUDIO_ENGINE: Sample RAM test completed successfully");
        }
    } else {
        if (s_hw) {
            WaveX::Log::PrintLine(
                "AUDIO_ENGINE: Sample RAM test allocation FAILED - SDRAM may not be initialized");
        }
    }

    // Initialize CPU load meter for audio processing performance monitoring.
    // The third argument is a smoothing-filter CUTOFF IN HZ, not a block
    // count - libDaisy's CpuLoadMeter::Init() defaults it to 1.0f for a
    // ~1 Hz-smoothed average. Passing 200 (Hz) against a 1 kHz block rate
    // gave a smoothing constant of ~0.56, i.e. GetAvgCpuLoad() was reporting
    // essentially per-block instantaneous load, not an average.
    s_cpu_load_meter.Init(sample_rate, 48);
}

void Callback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    PROFILE_SCOPE(audio_callback);
    uint32_t callback_cycles_start = WaveX::Profiling::GetCycles();
    // Start CPU load measurement for this audio block
    s_cpu_load_meter.OnBlockStart();
    ++s_callback_blocks;
    {
        const uint32_t occupancy = rb_count_frames();
        if (occupancy < s_rb_low_water) {
            s_rb_low_water = occupancy;
        }
    }

    (void)in;
    // Batched pop (review D2): one head/tail/live read for the whole block
    // instead of one rb_pop_stereo() call per sample. s_rb_live and s_wav.open
    // are both main-loop-only fields that cannot change while this ISR is
    // running (same-core preemption, not a multi-core race), so reading them
    // once up front is exactly equivalent to the old per-sample re-checks.
    const bool ring_live = __atomic_load_n(&s_rb_live, __ATOMIC_ACQUIRE);
    const size_t popped = ring_live ? rb_pop_stereo_batch(out[0], out[1], size) : 0;
    if (popped < size) {
        // !ring_live also covers CloseWav()'s teardown window: it clears
        // s_rb_live before clearing s_wav.open, so a callback landing in
        // that gap sees open==true with a dead ring. That is a stop in
        // progress, not a genuine buffer-starved underrun - counting it
        // pollutes the "zero underruns" diagnostic on every stop.
        const bool genuine_underrun = ring_live && s_wav.open;
        for (size_t i = popped; i < size; ++i) {
            // No audio should play on startup - output silence until audition
            // commands. Requirement: "When daisy starts, no audio plays (no
            // oscillator, no .wavs)". Also covers a mid-block ring-empty:
            // output silence to prevent glitches.
            out[0][i] = 0.0f;
            out[1][i] = 0.0f;
        }
        if (genuine_underrun) {
            // Signal underrun detection (logging handled in main loop)
            __atomic_store_n(&s_underrun_detected, true, __ATOMIC_RELEASE);
        }
    }

    // MIDI note path (roadmap Phase 1 item 8): apply pending note events,
    // then mix the RAM-resident voices on top of the streaming/ring
    // content above. Render() only runs when a voice is active, so the
    // startup silence requirement is preserved. All callback-safe: fixed
    // buffers, no allocation, no I/O, no logging.
    const bool any_note_on = drain_note_queue();

    // Push live parameter edits onto sounding voices before rendering them,
    // so a filter sweep is heard on the notes already playing and not only on
    // the next trigger. Consume-and-clear: a write landing after the exchange
    // is picked up by the next block (1 ms later), which is far below the
    // resolution of a knob gesture.
    if (__atomic_exchange_n(&s_voice_live_dirty, false, __ATOMIC_ACQUIRE)) {
        s_voice_manager.ApplyLiveParams(s_voice_live_params);
    }

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
// Each parameter now has TWO destinations, deliberately: the Stage A analog
// path (s_para_params, one shared VCF/VCA) and the digital per-voice path
// (s_voice_live_params). They are not alternatives - the analog board is
// optional hardware and the digital voices always render - so a knob has to
// reach both or it would do nothing on whichever configuration is in use.
// Per-slot (kit) scoping of these values remains Phase 2.5 instrument-model
// work; see s_voice_live_params for why engine-global is the honest interim.
void OnControlChange(const ControlChangeMessage& ctrl_msg) {
    const float norm = static_cast<float>(ctrl_msg.value) / 65535.0f;
    switch (ctrl_msg.parameter) {
        case PARAM_FILTER_CUTOFF:
            s_para_params.cutoff_base = norm;
            // Digital path: the analog side takes `norm` straight through as a
            // CV, but a digital cutoff is a frequency and has to be mapped.
            // Exponential over 20 Hz .. 20 kHz, because pitch perception is
            // logarithmic - a linear map spends most of its travel above
            // 10 kHz, where almost nothing audible happens, and crosses the
            // entire musically useful range in the first few percent.
            s_voice_live_params.filter_cutoff_hz = 20.0f * std::pow(1000.0f, norm);
            // Release-store the flag: a plain/volatile write to the flag alone
            // does not stop the compiler reordering the plain field stores
            // above it past this one, which would let the callback's acquire-
            // exchange observe dirty=true with a stale field (guide §6 - not a
            // theoretical concern once this builds at -O2 instead of today's
            // -O0). Same pattern as s_cv_dirty below.
            __atomic_store_n(&s_voice_live_dirty, true, __ATOMIC_RELEASE);
            break;
        case PARAM_PAN:
            // Linear 0..1 across the wire's full range. Voice::pan is applied
            // as a gain pair per block, so this is click-free without smoothing.
            s_voice_live_params.pan = norm;
            __atomic_store_n(&s_voice_live_dirty, true, __ATOMIC_RELEASE);
            break;

        case PARAM_PITCH: {
            // +/- 24 semitones around centre. Two octaves each way is enough to
            // play a sample as an instrument without the resampler running so
            // far from unity that the interpolation artefacts dominate.
            constexpr float kPitchRangeSemis = 24.0f;
            s_voice_live_params.pitch_semitones = (norm * 2.0f - 1.0f) * kPitchRangeSemis;
            __atomic_store_n(&s_voice_live_dirty, true, __ATOMIC_RELEASE);
            break;
        }

        case PARAM_FILTER_RESONANCE:
            s_para_params.resonance = norm;
            s_voice_live_params.filter_resonance = norm;  // svf_filter.hpp maps 0..1 onto Q
            __atomic_store_n(&s_voice_live_dirty, true, __ATOMIC_RELEASE);
            break;
        case PARAM_ENVELOPE_ATTACK:
        case PARAM_ENVELOPE_DECAY:
        case PARAM_ENVELOPE_SUSTAIN:
        case PARAM_ENVELOPE_RELEASE: {
            // Times span 1 ms .. 2 s; sustain is the raw 0..1 level.
            const float seconds = 0.001f + norm * 2.0f;
            if (ctrl_msg.parameter == PARAM_ENVELOPE_ATTACK) {
                s_para_params.attack_s = seconds;
                s_voice_live_params.attack_s = seconds;
            } else if (ctrl_msg.parameter == PARAM_ENVELOPE_DECAY) {
                s_para_params.decay_s = seconds;
                s_voice_live_params.decay_s = seconds;
            } else if (ctrl_msg.parameter == PARAM_ENVELOPE_SUSTAIN) {
                s_para_params.sustain = norm;
                s_voice_live_params.sustain_level = norm;
            } else {
                s_para_params.release_s = seconds;
                s_voice_live_params.release_s = seconds;
            }
            s_para_env.SetParams(s_para_params.attack_s,
                                 s_para_params.decay_s,
                                 s_para_params.sustain,
                                 s_para_params.release_s);
            __atomic_store_n(&s_voice_live_dirty, true, __ATOMIC_RELEASE);
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
            WaveX::Log::PrintLine("CV CAL: group %u out of range", (unsigned)m.group);
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
            WaveX::Log::PrintLine(
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
        WaveX::Log::PrintLine("CV TEST: %s (cut=%d res=%d vca=%d x1000)",
                              m.enable ? "ON" : "off",
                              (int)(m.cutoff * 1000),
                              (int)(m.resonance * 1000),
                              (int)(m.vca * 1000));
}

void LoadCvCalFromSd() {
    CvCal table[WAVEX_ANALOG_CV_GROUPS_MAX];
    if (!WaveX::Cv::LoadCvCalTable(s_cvcal_file, table)) {
        if (s_hw)
            WaveX::Log::PrintLine("CV CAL: no stored table (using defaults)");
        return;
    }
    for (uint8_t g = 0; g < WAVEX_ANALOG_CV_GROUPS_MAX; ++g) {
        s_cv_backend.SetGroupCal(g, table[g]);
    }
    if (s_hw)
        WaveX::Log::PrintLine("CV CAL: table loaded from SD");
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
        WaveX::Log::PrintLine("RX SEQ_TRANSPORT: cmd=%u src=%u bpm=%u",
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
static bool sample_is_playable(const LoadedSampleInfo& e) {
    return e.bit_depth == 16 && (e.channels == 1 || e.channels == 2);
}

static const LoadedSampleInfo* find_playable_sample() {
    // An explicit selection wins, but only if it is still loaded and playable -
    // otherwise a stale id (its sample unloaded, or a 24-bit file selected)
    // would silence the keyboard with no way to tell why from the outside.
    if (s_selected_sample_id != 0) {
        for (size_t i = 0; i < s_loaded_sample_count; ++i) {
            const auto& entry = s_loaded_samples[i];
            if (entry.sample_id == s_selected_sample_id && sample_is_playable(entry)) {
                return &entry;
            }
        }
        WaveX::Log::PrintLine(
            "  -> selected sample %u is not loaded or not playable; "
            "falling back to most recent",
            (unsigned)s_selected_sample_id);
    }
    for (size_t i = s_loaded_sample_count; i > 0; --i) {
        const auto& entry = s_loaded_samples[i - 1];
        if (sample_is_playable(entry)) {
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
        //
        // No s_hw guard: this is the one line that explains why the instrument
        // is silent, and gating it behind a pointer that may be null is how a
        // whole bench session went to working out whether notes were even
        // arriving. It runs on the main loop, well after init.
        WaveX::Log::PrintLine(
            "  -> dropped: no playable sample (need a RAM-resident 16-bit mono/stereo WAV; "
            "%u loaded)",
            (unsigned)s_loaded_sample_count);
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

    // Filter and envelope come from the live base params, so a note triggered
    // after a knob move sounds like the sweep the user just heard. Without
    // this the trigger would reset every voice to the struct defaults and an
    // edit would survive only until the next note.
    ev.params.filter_cutoff_hz = s_voice_live_params.filter_cutoff_hz;
    ev.params.filter_resonance = s_voice_live_params.filter_resonance;
    ev.params.attack_s = s_voice_live_params.attack_s;
    ev.params.decay_s = s_voice_live_params.decay_s;
    ev.params.sustain_level = s_voice_live_params.sustain_level;
    ev.params.release_s = s_voice_live_params.release_s;

    // Markers and gain come from the sample's record, so a note-triggered
    // voice plays exactly the region the editor auditioned. Previously
    // VoiceManager ignored both and the same file sounded different depending
    // on how it was triggered.
    {
        WaveX::Protocol::SampleMetadata m = src->meta;
        if (m.total_frames == 0) {
            m.total_frames = ev.params.sample_frames;
        }
        m.Resolve();
        ev.params.start_frame = m.start_frame;
        ev.params.end_frame = m.end_frame;
        ev.params.loop = m.loop_enabled != 0;
        ev.params.loop_start = m.loop_start;
        ev.params.loop_end = m.loop_end;
        ev.params.fade_in_ms = m.fade_in_ms;
        ev.params.fade_out_ms = m.fade_out_ms;
        // gain_mul is linear and multiplies the velocity gain, so the dB
        // figure has to be converted here rather than passed through.
        ev.params.gain_mul = (m.gain_db_x10 == 0)
                                 ? 1.0f
                                 : std::pow(10.0f, static_cast<float>(m.gain_db_x10) / 200.0f);
    }

    const bool queued = note_queue_push(ev);
    if (!queued && s_hw)
        WaveX::Log::PrintLine("RX NOTE_ON: note=%u DROPPED - note queue full",
                              (unsigned)note_msg.note);
#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (queued && s_hw)
        WaveX::Log::PrintLine("RX NOTE_ON: note=%u vel=%u ch=%u -> sample_id=%u (%lu frames)",
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
        WaveX::Log::PrintLine(
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
        WaveX::Log::PrintLine("SAMPLE_CTRL cmd=%u ignored (recording not implemented - review C2)",
                              (unsigned)sc.cmd);
}

// How a sample's channels map onto what the display asks for. Shared by the
// legacy decimated preview and the envelope job so the two cannot disagree
// about which channel the user is looking at.
struct DisplayChannelPlan {
    uint8_t src_channels;  // interleave stride in the stored data: 1 or 2
    uint8_t out_channels;  // traces to produce: 1 or 2
    bool sum;              // out_channels == 1 and both source channels averaged
    uint8_t pick[2];       // source channel index feeding each output trace
};

// allow_stereo lets a caller opt into two traces. SAMPLE_CH_AS_RECORDED on a
// stereo file is the only case that produces two: an explicit LEFT/RIGHT/SUM
// choice is a request for one specific trace, and honouring it as two would
// contradict what the same record tells the playback paths.
static DisplayChannelPlan ResolveDisplayChannels(const LoadedSampleInfo& src, bool allow_stereo) {
    DisplayChannelPlan plan{};
    plan.src_channels = src.channels ? src.channels : 1;
    plan.out_channels = 1;
    plan.sum = false;
    plan.pick[0] = 0;
    plan.pick[1] = 0;

    switch (src.meta.channel_mode) {
        case WaveX::Protocol::SAMPLE_CH_RIGHT:
            plan.pick[0] = (plan.src_channels > 1) ? 1 : 0;
            break;
        case WaveX::Protocol::SAMPLE_CH_MONO_SUM:
            plan.sum = (plan.src_channels > 1);
            break;
        case WaveX::Protocol::SAMPLE_CH_LEFT:
            plan.pick[0] = 0;
            break;
        case WaveX::Protocol::SAMPLE_CH_AS_RECORDED:
        default:
            if (allow_stereo && plan.src_channels > 1) {
                plan.out_channels = 2;
                plan.pick[0] = 0;
                plan.pick[1] = 1;
            }
            break;
    }
    return plan;
}

void OnPreviewReq(const PreviewReqMessage& pr) {
    s_prev_sent = 0;
    s_preview_len = 0;

    // Pick the most recently loaded sample; fall back to empty if none.
    if (s_loaded_sample_count == 0) {
        if (s_hw) {
            WaveX::Log::PrintLine("PREVIEW: No loaded samples; skipping preview");
        }
        return;
    }

    const LoadedSampleInfo& src = s_loaded_samples[s_loaded_sample_count - 1];
    void* sample_ptr = nullptr;
    if (!s_sample_mem_mgr.ptr(src.handle, &sample_ptr) || !sample_ptr) {
        if (s_hw) {
            WaveX::Log::PrintLine("PREVIEW: Failed to get pointer for sample_id=%u",
                                  (unsigned)src.sample_id);
        }
        return;
    }

    const uint32_t bytes_total = src.loaded_bytes ? src.loaded_bytes : src.handle.len;
    const uint32_t bytes_per_frame = (src.bit_depth / 8) * src.channels;
    if (bytes_per_frame == 0) {
        if (s_hw) {
            WaveX::Log::PrintLine("PREVIEW: Invalid bytes_per_frame=0 for sample_id=%u",
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

    // Channel selection comes from the record, not a hard-coded "left".
    // Silently previewing only the left channel drew a misleading trace for
    // anything panned, and a near-flat line for a hard-panned sample that is
    // plainly audible - with nothing on screen saying so.
    const DisplayChannelPlan plan = ResolveDisplayChannels(src, /*allow_stereo=*/false);
    const uint8_t ch_count = plan.src_channels;
    const uint8_t pick = plan.pick[0];
    const bool sum = plan.sum;

    for (uint32_t i = start; i < end; i += decim) {
        int16_t v = 0;
        if (src.bit_depth == 16) {
            if (ch_count == 1) {
                v = samples16[i];
            } else if (sum) {
                const int32_t l = samples16[i * ch_count];
                const int32_t r = samples16[i * ch_count + 1];
                v = static_cast<int16_t>((l + r) / 2);
            } else {
                v = samples16[i * ch_count + pick];
            }
        } else if (src.bit_depth == 24) {
            auto read24 = [&](uint32_t frame, uint8_t ch) -> int32_t {
                const uint32_t bi = frame * bytes_per_frame + ch * 3u;
                int32_t x =
                    (int32_t)(samples24[bi] | (samples24[bi + 1] << 8) | (samples24[bi + 2] << 16));
                if (x & 0x00800000) {
                    x |= 0xFF000000;  // sign-extend 24 -> 32
                }
                return x >> 8;  // scale to 16-bit for display
            };
            if (ch_count == 1) {
                v = (int16_t)read24(i, 0);
            } else if (sum) {
                v = (int16_t)((read24(i, 0) + read24(i, 1)) / 2);
            } else {
                v = (int16_t)read24(i, pick);
            }
        }
        s_preview[s_preview_len++] = v;
        if (s_preview_len >= kMaxPreviewPoints) {
            break;  // defensive; the decim widening above should prevent this
        }
    }

    if (s_hw) {
        WaveX::Log::PrintLine(
            "PREVIEW: Built preview for sample_id=%u frames=%lu decim=%u preview_len=%u",
            (unsigned)src.sample_id,
            (unsigned long)total_frames,
            (unsigned)decim,
            (unsigned)s_preview_len);
    }

    // s_prev_sent is already 0 (reset at the top of this function); the main
    // loop's PumpPreviewSend() picks the job up starting next pass.
}

// ============================
// Waveform envelope job (roadmap 1.5.5 item 2)
// ============================
//
// A true min/max envelope has to touch EVERY sample in the window - that is
// the whole difference from decimation, which is why it does not alias. For a
// three-minute stereo file that is ~16 M reads, and doing them in one go from
// a message handler would stall the main loop for ~100 ms: four times the
// ring's ~42 ms of headroom, i.e. an audible dropout every time the user
// zooms out. So the scan is a job, budgeted per main-loop pass, and each
// chunk goes out as soon as it is measured rather than at the end.
//
// The chunk is also the staging buffer: at 1920 payload bytes a chunk holds
// 480 mono or 240 stereo columns, and nothing larger than one packet is ever
// held. That is 10 KB of internal RAM this does NOT take (review H1 reclaimed
// ~254 KB; spending 4% of it on a display buffer would be a poor trade).
static constexpr uint32_t kEnvChunkPayloadBytes = 1920;
static constexpr uint32_t kEnvMaxChunkColumns =
    kEnvChunkPayloadBytes / sizeof(WaveX::Protocol::EnvelopeColumn);  // 480, mono

// Frames scanned per pass. 24576 frames is ~0.25 ms of SDRAM reads on this
// part - well inside a main-loop pass, and small enough that a full-file
// envelope of a long sample interleaves with SD refill instead of displacing
// it. A whole 8 M-frame file therefore takes ~340 passes, which is well under
// a second of wall clock at the loop rate.
static constexpr uint32_t kEnvFramesPerPass = 24576;

struct EnvelopeJob {
    bool active = false;
    uint16_t sample_id = 0;
    uint16_t generation = 0;
    uint32_t start_frame = 0;
    uint32_t end_frame = 0;
    uint16_t total_columns = 0;
    uint16_t next_column = 0;  // first column not yet measured
    uint8_t channels = 1;      // traces per column on the wire
};
static EnvelopeJob s_env_job;

alignas(4) static uint8_t
    s_env_frame[sizeof(WaveX::Protocol::EnvelopeChunkMessage) + kEnvChunkPayloadBytes];

// Cancels any envelope in flight. Called when the sample it is measuring is
// about to be replaced or released: the job holds a sample_id, not a pointer,
// but finishing a scan against rewritten memory would send a waveform of the
// wrong audio under the old generation, which the frontend would then cache.
static void CancelEnvelopeJob() {
    s_env_job.active = false;
}

// Reads one display sample of one source channel, scaled to int16.
static inline int32_t EnvReadSample(const LoadedSampleInfo& src,
                                    const void* base,
                                    uint32_t frame,
                                    uint8_t channel) {
    if (src.bit_depth == 24) {
        const uint8_t* p =
            static_cast<const uint8_t*>(base) + (frame * src.channels + channel) * 3u;
        int32_t x = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
        if (x & 0x00800000) {
            x |= static_cast<int32_t>(0xFF000000u);  // sign-extend 24 -> 32
        }
        return x >> 8;  // scale to 16-bit for display
    }
    return static_cast<const int16_t*>(base)[frame * src.channels + channel];
}

void OnEnvelopeReq(const WaveX::Protocol::EnvelopeReqMessage& req) {
    CancelEnvelopeJob();

    // sample_id 0 means "the most recently loaded", matching how
    // MSG_SAMPLE_EDIT_SET addresses a sample the edit page did not load.
    LoadedSampleInfo* info = req.sample_id ? find_loaded_sample(req.sample_id) : nullptr;
    if (!info && req.sample_id == 0 && s_loaded_sample_count > 0) {
        info = &s_loaded_samples[s_loaded_sample_count - 1];
    }
    if (!info) {
        if (s_hw) {
            WaveX::Log::PrintLine("ENVELOPE: no sample for id=%u", (unsigned)req.sample_id);
        }
        return;
    }

    const uint32_t total_frames = info->meta.total_frames;
    if (total_frames == 0) {
        return;
    }

    uint32_t start = std::min<uint32_t>(req.start_frame, total_frames);
    uint32_t end =
        (req.end_frame == 0) ? total_frames : std::min<uint32_t>(req.end_frame, total_frames);
    if (end <= start) {
        return;
    }

    uint32_t columns = req.columns ? req.columns : 1;
    if (columns > WaveX::Protocol::MAX_ENVELOPE_COLUMNS) {
        columns = WaveX::Protocol::MAX_ENVELOPE_COLUMNS;
    }
    // Never more columns than frames: an empty column has no min/max to
    // report, and padding one would draw audio that is not there.
    const uint32_t span = end - start;
    if (columns > span) {
        columns = span;
    }

    const DisplayChannelPlan plan = ResolveDisplayChannels(*info, /*allow_stereo=*/true);

    s_env_job.active = true;
    s_env_job.sample_id = info->sample_id;
    s_env_job.generation = info->meta.generation;
    s_env_job.start_frame = start;
    s_env_job.end_frame = end;
    s_env_job.total_columns = static_cast<uint16_t>(columns);
    s_env_job.next_column = 0;
    s_env_job.channels = plan.out_channels;
}

void PumpEnvelopeJob() {
    if (!s_env_job.active) {
        return;
    }

    // Re-resolve every pass. The sample can be unloaded or reloaded while a
    // scan is in flight, and a generation bump means the audio under us
    // changed - both make the rest of this envelope a description of
    // something that no longer exists.
    LoadedSampleInfo* info = find_loaded_sample(s_env_job.sample_id);
    void* base = nullptr;
    if (!info || info->meta.generation != s_env_job.generation ||
        !s_sample_mem_mgr.ptr(info->handle, &base) || base == nullptr) {
        CancelEnvelopeJob();
        return;
    }

    const DisplayChannelPlan plan = ResolveDisplayChannels(*info, /*allow_stereo=*/true);
    if (plan.out_channels != s_env_job.channels) {
        CancelEnvelopeJob();  // channel_mode changed mid-scan; the frontend will re-ask
        return;
    }

    const uint32_t columns_per_chunk = std::max<uint32_t>(
        1u, kEnvChunkPayloadBytes / (s_env_job.channels * sizeof(WaveX::Protocol::EnvelopeColumn)));

    auto* header = reinterpret_cast<WaveX::Protocol::EnvelopeChunkMessage*>(s_env_frame);
    auto* out = reinterpret_cast<WaveX::Protocol::EnvelopeColumn*>(
        s_env_frame + sizeof(WaveX::Protocol::EnvelopeChunkMessage));

    const uint16_t first_column = s_env_job.next_column;
    const uint64_t span = s_env_job.end_frame - s_env_job.start_frame;
    const uint32_t total_columns = s_env_job.total_columns;

    uint32_t columns_done = 0;
    uint32_t frames_scanned = 0;
    while (s_env_job.next_column < total_columns && columns_done < columns_per_chunk &&
           frames_scanned < kEnvFramesPerPass) {
        const uint32_t c = s_env_job.next_column;
        // 64-bit throughout: span * column index overflows 32 bits for any
        // file past ~3.3 M frames at 1280 columns, which is under 80 seconds.
        uint32_t f0 = s_env_job.start_frame + static_cast<uint32_t>((span * c) / total_columns);
        uint32_t f1 =
            s_env_job.start_frame + static_cast<uint32_t>((span * (c + 1)) / total_columns);
        if (f1 <= f0) {
            f1 = f0 + 1;  // sub-frame column: report the one frame it lands on
        }
        if (f1 > s_env_job.end_frame) {
            f1 = s_env_job.end_frame;
        }

        for (uint8_t ch = 0; ch < s_env_job.channels; ++ch) {
            const uint8_t src_ch = plan.pick[ch];
            int32_t lo = 32767;
            int32_t hi = -32768;
            for (uint32_t f = f0; f < f1; ++f) {
                int32_t v;
                if (plan.sum) {
                    v = (EnvReadSample(*info, base, f, 0) + EnvReadSample(*info, base, f, 1)) / 2;
                } else {
                    v = EnvReadSample(*info, base, f, src_ch);
                }
                if (v < lo) {
                    lo = v;
                }
                if (v > hi) {
                    hi = v;
                }
            }
            if (hi < lo) {  // empty column, cannot happen after the f1 fixups
                lo = hi = 0;
            }
            out[columns_done * s_env_job.channels + ch] =
                WaveX::Protocol::EnvelopeColumn(static_cast<int16_t>(lo), static_cast<int16_t>(hi));
        }

        frames_scanned += (f1 - f0) * s_env_job.channels;
        ++columns_done;
        ++s_env_job.next_column;
    }

    if (columns_done == 0) {
        CancelEnvelopeJob();
        return;
    }

    header->sample_id = s_env_job.sample_id;
    header->generation = s_env_job.generation;
    header->start_frame = s_env_job.start_frame;
    header->end_frame = s_env_job.end_frame;
    header->total_columns = static_cast<uint16_t>(total_columns);
    header->first_column = first_column;
    header->columns = static_cast<uint16_t>(columns_done);
    header->channels = s_env_job.channels;
    header->reserved = 0;

    const size_t payload_bytes =
        sizeof(WaveX::Protocol::EnvelopeChunkMessage) +
        columns_done * s_env_job.channels * sizeof(WaveX::Protocol::EnvelopeColumn);
    const int res = WaveX::Comm::UartLinkSend(
        WaveX::Protocol::MSG_ENVELOPE_CHUNK, s_env_frame, static_cast<uint16_t>(payload_bytes));
    if (res < 0) {
        // Queue full. Unlike the preview sender - which runs inside a message
        // handler and has to pump the TX queue itself - this is already on the
        // main loop, so the honest move is to rewind and let the next pass
        // retry. No blocking, and the columns are re-measured rather than
        // punching a hole in the envelope.
        s_env_job.next_column = first_column;
        return;
    }

    if (s_env_job.next_column >= total_columns) {
        s_env_job.active = false;
    }
}

void OnSampleLoad(const SampleLoadMessage& sl) {
    if (!s_sample_memory_available) {
        if (s_hw)
            WaveX::Log::PrintLine("SAMPLE_LOAD: rejected because SDRAM is unavailable");
        return;
    }
    if (s_hw) {
        WaveX::Log::PrintLine("SAMPLE_LOAD: path='%s' id=%u", sl.path, (unsigned)sl.sample_id);
    }
    // CRITICAL: Stop ALL SD activity (playback) and ensure PumpWavIO is not running.
    // FatFS + SDMMC are NOT thread-safe or re-entrant. The main loop calls PumpWavIO() which
    // will conflict with f_open/f_read calls here if s_wav.open is true.
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
            WaveX::Log::PrintLine("SAMPLE_LOAD: f_open failed (%d) for '%s' and alt '%s'",
                                  (int)fr,
                                  sl.path,
                                  alt_path);
        }
    } else if (fr != FR_OK && s_hw) {
        WaveX::Log::PrintLine("SAMPLE_LOAD: f_open failed (%d) for '%s'", (int)fr, sl.path);
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
            WaveX::Log::PrintLine("SAMPLE_LOAD: invalid WAV header (parse result %d)",
                                  (int)parse_result);
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
            WaveX::Log::PrintLine("SAMPLE_LOAD: unsupported format fmt=%u bits=%u ch=%u",
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
                WaveX::Log::PrintLine(
                    "SAMPLE_LOAD: alloc failed for %lu bytes (largest_free=%lu, free_total=%lu)",
                    (unsigned long)data_size,
                    (unsigned long)st.largest_free_bytes,
                    (unsigned long)st.large_free_bytes + (unsigned long)st.small_free_bytes);
            }
            f_close(&file);
            return;
        }
        if (s_hw) {
            WaveX::Log::PrintLine("SAMPLE_LOAD: evicted oldest sample to fit %lu bytes",
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
    // Wall-clock for the read loop, so load throughput is a number rather than
    // an impression. AGENTS.md wants a measurement before a performance claim,
    // and this is the one place a stopwatch is both cheap and meaningful - the
    // loop runs on the main loop for seconds, so a millisecond timer is ample
    // and there is no callback budget to protect.
    const uint32_t load_start_ms = System::GetNow();
    // Use a 32-byte-aligned AXI-SRAM staging buffer; stack/DTCM is not
    // accessible to SDMMC IDMA.
    uint8_t* temp = s_sample_io;
    const UINT kIoChunk = pick_sample_load_chunk(file);
    // Captured before the loop because f_close() below invalidates obj.fs, and
    // the completion log (after the close) reports it.
    const unsigned cluster_bytes =
        file.obj.fs ? static_cast<unsigned>(file.obj.fs->csize) * kSdSectorBytes : 0u;

    while (remaining > 0) {
        UINT to_read = (remaining > kIoChunk) ? kIoChunk : remaining;

        fr = f_read(&file, temp, to_read, &br);

        if (fr != FR_OK || br == 0) {
            if (s_hw) {
                WaveX::Log::PrintLine(
                    "SAMPLE_LOAD: read error %d after %lu bytes", (int)fr, (unsigned long)written);
            }
            s_sample_mem_mgr.release(&handle);
            f_close(&file);
            return;
        }
        memcpy(static_cast<uint8_t*>(sample_ptr) + written, temp, br);
        written += br;
        remaining -= br;

        // Progress, rate-limited to whole percent. A large sample off a slow
        // card takes seconds; without this the frontend has nothing to show
        // but an indeterminate spinner. state 0x11 is progress, distinct from
        // 0x10 (complete), so an existing frontend ignores it.
        if (data_size > 0) {
            const uint8_t pct =
                static_cast<uint8_t>((static_cast<uint64_t>(written) * 100ull) / data_size);
            static uint8_t s_last_pct = 0xFF;
            if (pct != s_last_pct) {
                s_last_pct = pct;
                SampleStatusMessage progress{};
                progress.sample_id = sl.sample_id;
                progress.state = 0x11;  // loading, frames_played carries percent
                progress.channels = num_ch;
                progress.sample_rate = sample_rate;
                progress.frames_played = pct;
                WaveX::Comm::UartLinkSend(
                    WaveX::Protocol::MSG_SAMPLE_STATUS, &progress, sizeof(progress));
                // The link is not pumped from here, so drain one frame or the
                // 4-deep TX queue fills and later progress is silently lost.
                WaveX::Comm::UartLinkPumpTx();
            }
        }
    }

    f_close(&file);

    if (!upsert_loaded_sample(sl, handle)) {
        if (s_hw)
            WaveX::Log::PrintLine("SAMPLE_LOAD: registry full (%u entries)",
                                  (unsigned)kLoadedSampleCapacity);
        s_sample_mem_mgr.release(&handle);
        return;
    }
    update_loaded_sample_progress(sl.sample_id, data_size);

    if (s_hw) {
        // Report throughput and the geometry it was achieved with, not just the
        // size. "3.1 MB in 900 ms (3444 KB/s, 65536 B reads, 32768 B clusters)"
        // is directly comparable across cards and buffer sizes, and shows
        // whether the read size actually tracked the filesystem; "loaded 3.1 MB"
        // is comparable to nothing.
        const uint32_t elapsed_ms = System::GetNow() - load_start_ms;
        const unsigned long kbps =
            elapsed_ms > 0 ? (unsigned long)((uint64_t)data_size / elapsed_ms) : 0;
        WaveX::Log::PrintLine(
            "SAMPLE_LOAD: Loaded %lu bytes for sample %u in %lu ms "
            "(%lu KB/s, %u B reads, %u B clusters)",
            (unsigned long)data_size,
            (unsigned)sl.sample_id,
            (unsigned long)elapsed_ms,
            kbps,
            (unsigned)kIoChunk,
            cluster_bytes);
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

    // Atomic consume-and-clear: a plain read-then-clear here can race the
    // callback's plain write to s_underrun_detected (Callback() runs in ISR
    // context and can preempt this function between the read and the
    // clear), letting a fresh episode's flag be silently overwritten back to
    // false before this function ever saw it as true.
    const bool detected_now = __atomic_exchange_n(&s_underrun_detected, false, __ATOMIC_ACQUIRE);
    if (detected_now && !s_underrun_logged) {
        episodes++;
        s_underrun_logged = true;
        ++s_diag_underruns;
    } else if (!detected_now && s_underrun_logged) {
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
        WaveX::Log::PrintLine("AUDIO: Ring buffer underrun - outputting silence (%u in last ~1s)",
                              (unsigned)episodes);
    }
    episodes = 0;
}

// Main-loop only: performs the blocking CV DAC transaction (~225 us
// MCP4728 fast-write) for values staged at the control tick - never in the
// callback (§7.1.4 / analog-voice-board.md §0 timing rules). If the DAC is
// absent (bench without the Stage A breadboard) or wedged, eight consecutive
// I2C failures pause the flush so the ~1 ms transaction timeout is not paid
// forever; a periodic long-backoff retry (rather than a permanent latch)
// means a transient wedge on real hardware recovers on its own instead of
// requiring a reboot to get the analog path back.
void FlushCv() {
    static uint32_t consecutive_failures = 0;
    static bool disabled_logged = false;
    static uint32_t disabled_since_ms = 0;
    constexpr uint32_t kMaxConsecutiveFailures = 8;
    constexpr uint32_t kRetryBackoffMs = 10000;

    if (consecutive_failures >= kMaxConsecutiveFailures) {
        if (System::GetNow() - disabled_since_ms < kRetryBackoffMs) {
            if (!disabled_logged) {
                disabled_logged = true;
                if (s_hw)
                    WaveX::Log::PrintLine(
                        "CV: MCP4728 not responding after %u attempts - CV flush paused, "
                        "retrying every %u ms",
                        (unsigned)kMaxConsecutiveFailures,
                        (unsigned)kRetryBackoffMs);
            }
            return;
        }
        // Backoff elapsed: give the DAC another chance rather than staying
        // disabled for the rest of the session.
        consecutive_failures = 0;
        disabled_logged = false;
    }
    if (!__atomic_exchange_n(&s_cv_dirty, false, __ATOMIC_ACQUIRE)) {
        return;  // nothing staged since the last flush
    }
    if (s_cv_router.Flush()) {
        consecutive_failures = 0;
    } else {
        ++consecutive_failures;
        if (consecutive_failures >= kMaxConsecutiveFailures) {
            disabled_since_ms = System::GetNow();
        }
    }
}

uint32_t GetCallbackBlocks() {
    return s_callback_blocks;
}

bool TakePlaybackAborted() {
    const bool aborted = s_playback_aborted;
    s_playback_aborted = false;
    return aborted;
}

void MarkPlaybackAborted() {
    s_playback_aborted = true;
}

uint32_t TakeRingLowWater() {
    // Atomic consume-and-clear, for the same reason CheckAndLogUnderruns()
    // uses one on s_underrun_detected: Callback() runs in ISR context and can
    // preempt this function between the read and the reset. A plain
    // read-then-reset therefore discards any dip the callback latches in that
    // window - and a rare dip toward empty is precisely what this diagnostic
    // exists to catch, so the losses are concentrated on the samples that
    // matter. The callback's own compare-and-latch needs no atomic: main-loop
    // code cannot run partway through an ISR, so its RMW is already indivisible
    // with respect to this function.
    const uint32_t low = __atomic_exchange_n(&s_rb_low_water, 0xFFFFFFFFu, __ATOMIC_ACQUIRE);
    return (low == 0xFFFFFFFFu) ? 0u : low;
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
    // CloseWav() zeroes the whole s_wav struct (s_wav = {}), including
    // gain_q15 - which defeats the "gain survives the open" contract below:
    // every open, first or not, saw gain_q15 == 0 and reset to unity. Capture
    // it before the close so a real prior value (set via SetEditParams) makes
    // it across.
    const q15_t prev_gain_q15 = s_wav.gain_q15;
    CloseWav();

    FRESULT fr = f_open(&s_wav.file, path, FA_READ);
    if (fr != FR_OK) {
        if (s_hw)
            WaveX::Log::PrintLine("WAV open failed: f_open error %d for path %s", (int)fr, path);
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
            WaveX::Log::PrintLine(
                "WAV open failed: header parse error %d for %s", (int)parse_result, path);
        f_close(&s_wav.file);
        return false;
    }

    // Support PCM format (fmt=1), 16-bit or 24-bit, mono or stereo
    if (wav_info.audio_format != 1 ||
        (wav_info.bits_per_sample != 16 && wav_info.bits_per_sample != 24) ||
        (wav_info.num_channels != 1 && wav_info.num_channels != 2)) {
        if (s_hw)
            WaveX::Log::PrintLine("WAV open failed: unsupported format fmt=%u bits=%u ch=%u",
                                  (unsigned)wav_info.audio_format,
                                  (unsigned)wav_info.bits_per_sample,
                                  (unsigned)wav_info.num_channels);
        f_close(&s_wav.file);
        return false;  // only PCM16/24 mono/stereo supported
    }

    // Leave the file positioned at the data payload for streaming.
    f_lseek(&s_wav.file, wav_info.data_offset);

    s_io_errors = 0;
    s_io_recoveries = 0;
    s_io_backoff_until_ms = 0;
    s_resampler.Reset();
    s_wav.open = true;
    std::strncpy(s_wav.path, path, sizeof(s_wav.path) - 1);
    s_wav.path[sizeof(s_wav.path) - 1] = '\0';
    s_wav.data_start = wav_info.data_offset;
    s_wav.data_size = wav_info.data_size;
    s_wav.bytes_remaining = wav_info.data_size;
    s_wav.num_channels = wav_info.num_channels;
    s_wav.bits_per_sample = wav_info.bits_per_sample;
    s_wav.sample_rate = wav_info.sample_rate;
    // Whole file, no loop, by default - an un-edited sample behaves exactly
    // as it did before edits existed. Gain deliberately survives the open:
    // it is a property of the sample being auditioned, and re-opening the
    // same file to hear a marker change should not silently reset it.
    s_wav.region_start = wav_info.data_offset;
    s_wav.region_end = wav_info.data_offset + wav_info.data_size;
    s_wav.loop_start = s_wav.region_start;
    s_wav.loop_end = s_wav.region_end;
    s_wav.loop_enabled = false;
    s_wav.gain_q15 = (prev_gain_q15 != 0)
                         ? prev_gain_q15
                         : static_cast<q15_t>(32767);  // 32767 = unity, first open of the session

    // Reset buffers. CloseWav() above already cleared s_rb_live and no
    // producer call (rb_push_frames) runs between here and there, so the
    // consumer is guaranteed to still be treating the ring as empty - these
    // stores can't race rb_pop_stereo_batch(). Publish head/tail before flipping
    // s_rb_live back on so the ISR never observes "live" with stale indices.
    __atomic_store_n(&s_rb_head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_rb_tail, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_rb_live, true, __ATOMIC_RELEASE);

    // Logged unconditionally: once per file open, so it cannot spam, and it
    // is the only place the per-file variables are visible. When some files
    // play cleanly and others of the SAME format do not, the difference has
    // to be here.
    //
    // data_start%4 != 0 means the data chunk is not frame-aligned for 16-bit
    // stereo, so every read starts mid-frame and the channels are read
    // swapped and shifted. data_start%512 != 0 means reads never land on a
    // sector boundary, forcing FatFS through its window buffer for the head
    // and tail of every transfer. Odd offsets are legal in RIFF - a LIST or
    // fact chunk of odd length before `data` produces them - and this player
    // does not compensate for either.
    const uint32_t frame_bytes =
        (uint32_t)wav_info.num_channels * ((wav_info.bits_per_sample == 24) ? 3u : 2u);
    WaveX::Log::PrintLine(
        "WAV open: '%s' %luHz ch=%u bits=%u data_start=%lu (frame%%=%lu sector%%=%lu) size=%lu",
        path,
        (unsigned long)wav_info.sample_rate,
        (unsigned)wav_info.num_channels,
        (unsigned)wav_info.bits_per_sample,
        (unsigned long)wav_info.data_offset,
        (unsigned long)(frame_bytes ? (wav_info.data_offset % frame_bytes) : 0u),
        (unsigned long)(wav_info.data_offset % 512u),
        (unsigned long)wav_info.data_size);

    // Reset pre-buffer state and start pre-buffering
    s_prebuffer_filled = 0;
    s_prebuffer_ready = false;
    s_prebuffering = false;

    return true;
}

void CloseWav() {
    // First: tell rb_pop_stereo_batch() (audio ISR) to stop touching the ring
    // indices at all. Until this is observed, the ISR may still be
    // advancing s_rb_tail; the stores below must not race that.
    __atomic_store_n(&s_rb_live, false, __ATOMIC_RELEASE);

    if (s_wav.open) {
        if (s_hw)
            WaveX::Log::PrintLine("CloseWav: closing WAV file and clearing state");
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

    // Clear ring buffer to stop any remaining audio immediately. Safe: the
    // ISR bailed out on s_rb_live above before touching either index, so
    // there is no writer left to race here.
    __atomic_store_n(&s_rb_head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_rb_tail, 0u, __ATOMIC_RELEASE);
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

void TakeIOThroughput(
    uint32_t& bytes, uint32_t& reads, uint32_t& avg_us, uint32_t& min_us, uint32_t& max_us) {
    const uint32_t per_us = System::GetTickFreq() / 1000000u;
    const uint32_t ticks_per_us = per_us ? per_us : 1u;
    bytes = s_iv_bytes;
    reads = s_iv_reads;
    avg_us = s_iv_reads ? (s_iv_ticks / s_iv_reads / ticks_per_us) : 0u;
    min_us = (s_iv_min_ticks == 0xFFFFFFFFu) ? 0u : (s_iv_min_ticks / ticks_per_us);
    max_us = s_iv_max_ticks / ticks_per_us;
    s_iv_bytes = 0;
    s_iv_reads = 0;
    s_iv_ticks = 0;
    s_iv_min_ticks = 0xFFFFFFFFu;
    s_iv_max_ticks = 0;
}

void GetIOErrors(uint32_t& errors, uint32_t& last_result) {
    errors = s_io_errors;
    last_result = s_io_last_err;
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
void TakeStreamCounters(uint32_t& pushes, uint32_t& discards, uint32_t& underruns) {
    pushes = s_diag_pushes;
    discards = s_diag_discards;
    underruns = s_diag_underruns;
    s_diag_pushes = 0;
    s_diag_discards = 0;
    s_diag_underruns = 0;
}

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

        s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
        s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
        return;
    }

    // Owed silence after a loop rewind (browser audition). Pushed into the
    // ring rather than skipped, so the gap is genuine silence instead of an
    // underrun - and it costs no SD read, which makes it free.
    if (s_loop_gap_remaining > 0) {
        const uint32_t room = rb_free_frames();
        uint32_t silent = std::min(s_loop_gap_remaining, room);
        if (silent > 0) {
            ResetScratchPool();
            q15_t* zeros = AcquireScratch(silent * s_output_channels);
            if (zeros != nullptr) {
                std::memset(zeros, 0, silent * s_output_channels * sizeof(q15_t));
                rb_push_frames(zeros, silent);
                s_loop_gap_remaining -= silent;
                ++s_diag_pushes;
            }
        }
        return;  // no file reading while the gap is owed
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
    ApplyWavGain(conversion_output, frames_to_transfer * s_output_channels);
    // Region-relative index of this block's first frame. slot.file_offset is
    // where the read started; consumed is how far into the slot we are.
    const uint32_t block_file_pos = slot.file_offset + slot.consumed * file_bpf;
    const uint32_t block_first_frame = (block_file_pos > s_wav.region_start)
                                           ? ((block_file_pos - s_wav.region_start) / file_bpf)
                                           : 0u;
    ApplyWavFade(conversion_output, frames_to_transfer, block_first_frame);

    q15_t* final_buffer = conversion_output;
    uint32_t final_frames = frames_to_transfer;
    // The resampler carries phase and history across calls, but the skip
    // paths below return WITHOUT consuming the slot, so this same input is
    // retried on a later pump. Retrying against advanced state resamples the
    // same audio at the wrong phase - duplicated, discontinuous output, i.e.
    // a stutter. Snapshot at function scope so every skip can rewind it.
    const StreamResamplerState resampler_before = s_resampler;
    if (resample_ratio != 1.0f) {
        uint32_t max_out_frames =
            static_cast<uint32_t>(std::ceil(frames_to_transfer * resample_ratio)) + 1;
        q15_t* resample_buffer = AcquireScratch(max_out_frames * s_output_channels);
        uint32_t resampled = 0;
        if (resample_buffer != nullptr) {
            resampled = ResampleStreamInterleaved(s_resampler,
                                                  conversion_output,
                                                  frames_to_transfer,
                                                  resample_buffer,
                                                  max_out_frames,
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
            s_resampler = resampler_before;
            ++s_diag_discards;
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
        if (resample_ratio != 1.0f) {
            s_resampler = resampler_before;  // see the snapshot above
        }
        ++s_diag_discards;
        s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
        s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
        return;
    }

    ++s_dbg_pushes;
    ++s_diag_pushes;
    rb_push_frames(final_buffer, final_frames);
    slot.consumed += frames_to_transfer;
    if (slot.consumed >= slot.frames) {
        slot.ready = false;
        slot.consumed = 0;
        s_sd_consume_index = (s_sd_consume_index + 1) % kSdBufferCount;
    }

    s_dwt_io_cycles = WaveX::Profiling::GetCycles() - block_cycles_start;
    s_dwt_io_max = std::max(s_dwt_io_max, s_dwt_io_cycles);
}

// ============================================================================
// Sample edit params (gain/loop/fade) for the Sample Load/Save page
// ============================================================================

// Shortest loop the streaming refill can sustain without re-seeking every
// pass and starving the ring.
static constexpr uint32_t kMinLoopFrames = 256;

struct LoadedSampleInfo;
static void ApplyMetaToStreaming(const LoadedSampleInfo* info);

// dB -> q15 linear, clamped. Table-free: this runs once per edit message, not
// per sample, so powf is affordable and exact beats fast here.
static q15_t GainDbToQ15(int16_t db_x10) {
    if (db_x10 <= -240) {
        return 0;  // -24 dB and below reads as silence on this control
    }
    if (db_x10 > 120) {
        db_x10 = 120;
    }
    const float lin = std::pow(10.0f, static_cast<float>(db_x10) / 200.0f);
    const float scaled = lin * 32767.0f;
    if (scaled >= 32767.0f) {
        return 32767;
    }
    return static_cast<q15_t>(scaled);
}

// Applies an edit to the sample's record, then pushes the result back. The
// backend clamps and is the authority; the frontend is told what was applied
// rather than assuming its request was taken verbatim.
void SetEditParams(uint8_t slot,
                   bool loop_enabled,
                   int16_t gain_db_x10,
                   uint32_t start_frame,
                   uint32_t end_frame,
                   uint32_t loop_start_frame,
                   uint32_t loop_end_frame,
                   uint16_t fade_in_ms,
                   uint16_t fade_out_ms) {
    // slot is the sample id. 0 means "whatever the audition is playing",
    // which is how the edit page addresses a sample it did not load itself.
    LoadedSampleInfo* info = slot ? find_loaded_sample(slot) : nullptr;
    if (!info && s_loaded_sample_count > 0) {
        info = &s_loaded_samples[s_loaded_sample_count - 1];  // most recent
    }

    if (info) {
        auto& m = info->meta;
        if (gain_db_x10 < -240) {
            gain_db_x10 = -240;
        } else if (gain_db_x10 > 120) {
            gain_db_x10 = 120;
        }
        m.gain_db_x10 = gain_db_x10;
        m.start_frame = start_frame;
        m.end_frame = end_frame;
        m.loop_start = loop_start_frame;
        m.loop_end = loop_end_frame;
        m.Resolve();
        // A loop shorter than one SD slot would re-seek on every refill pass
        // and starve the ring. The frontend cannot know this limit, so it is
        // enforced here and reported back rather than silently obeyed.
        m.loop_enabled = (loop_enabled && (m.loop_end - m.loop_start) >= kMinLoopFrames) ? 1 : 0;
        // Clamp fades to the region. A fade longer than the audio it shapes
        // never reaches unity, which reads as "the sample got quieter" rather
        // than as a fade - and the frontend cannot clamp it, because the
        // backend is the one that just decided what the region is.
        const uint32_t rate = m.sample_rate ? m.sample_rate : s_sample_rate;
        const uint32_t span_ms =
            rate ? static_cast<uint32_t>(
                       (static_cast<uint64_t>(m.end_frame - m.start_frame) * 1000u) / rate)
                 : 0u;
        m.fade_in_ms = static_cast<uint16_t>(std::min<uint32_t>(fade_in_ms, span_ms));
        m.fade_out_ms = static_cast<uint16_t>(std::min<uint32_t>(fade_out_ms, span_ms));
        PushSampleMeta(*info);
    }

    ApplyMetaToStreaming(info);
}

// Mirrors a record onto the streaming reader's byte offsets. Called whenever
// either the record or the open file changes, so the two cannot drift.
static void ApplyMetaToStreaming(const LoadedSampleInfo* info) {
    if (!s_wav.open) {
        return;
    }
    const uint32_t bytes_per_sample = (s_wav.bits_per_sample == 24) ? 3u : 2u;
    const uint32_t file_bpf = (uint32_t)s_wav.num_channels * bytes_per_sample;
    if (file_bpf == 0) {
        return;
    }
    const uint32_t total_frames = s_wav.data_size / file_bpf;

    WaveX::Protocol::SampleMetadata m;
    if (info) {
        m = info->meta;
    }
    // The streaming file is the authority on its own length: a record seeded
    // from a load request can describe a different (or not yet complete)
    // buffer.
    m.total_frames = total_frames;
    m.Resolve();

    s_wav.region_start = s_wav.data_start + m.start_frame * file_bpf;
    s_wav.region_end = s_wav.data_start + m.end_frame * file_bpf;
    s_wav.loop_start = s_wav.data_start + m.loop_start * file_bpf;
    s_wav.loop_end = s_wav.data_start + m.loop_end * file_bpf;
    s_wav.loop_enabled = m.loop_enabled != 0;
    s_wav.gain_q15 = GainDbToQ15(m.gain_db_x10);
    s_wav.region_start_frame = m.start_frame;
    s_wav.region_end_frame = m.end_frame;
    // Fades are specified against the FILE's rate, which is what m carries and
    // what the region frames above are counted in. Converting against the
    // engine rate here would make a 1 ms de-click come out 1.09 ms long on a
    // 44.1 kHz file - inaudible, but wrong in a way that compounds if a later
    // change reuses the figure.
    const uint32_t file_rate = s_wav.sample_rate ? s_wav.sample_rate : s_sample_rate;
    s_wav.fade_in_frames = WaveX::AudioEngine::FadeFrames(m.fade_in_ms, file_rate);
    s_wav.fade_out_frames = WaveX::AudioEngine::FadeFrames(m.fade_out_ms, file_rate);

    WaveX::Log::PrintLine("WAV edit: region %lu..%lu loop %lu..%lu %s gain %d.%ddB fade %u/%u ms",
                          (unsigned long)m.start_frame,
                          (unsigned long)m.end_frame,
                          (unsigned long)m.loop_start,
                          (unsigned long)m.loop_end,
                          s_wav.loop_enabled ? "on" : "off",
                          m.gain_db_x10 / 10,
                          (m.gain_db_x10 < 0 ? -m.gain_db_x10 : m.gain_db_x10) % 10,
                          (unsigned)m.fade_in_ms,
                          (unsigned)m.fade_out_ms);
}

}  // namespace AudioEngine
}  // namespace WaveX

#endif  // WAVEX_AUDIO_ENGINE_ENABLED
