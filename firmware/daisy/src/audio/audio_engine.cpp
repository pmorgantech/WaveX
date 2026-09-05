#include "comm/log_ring.h"

#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include <daisy.h>  // For CpuLoadMeter

extern "C" SD_HandleTypeDef hsd1;  // libDaisy per/sdmmc.cpp

#include "../memory.h"
#include "../memory_sections.h"  // For WAVEX_DTCM_DATA
#include "../sdram_layout.h"

#include "../bss_static.hpp"
// q15_t was CMSIS-DSP's name for a 16-bit sample; the engine keeps the name
// (it says "audio sample, not a count") but no longer links the library - its
// one used routine, arm_copy_q15, was a plain copy loop, and memcpy is at
// least as fast. docs/daisy_rt_audio_coding_guide.md §8 says when the
// library IS worth linking.
using q15_t = int16_t;
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
#include "audio/sample_pool.hpp"
#include "fade.hpp"
#include "instrument.hpp"
#include "lfo.hpp"
#include "linear_resampler.hpp"
#include "mod_matrix.hpp"
#include "note_event_queue.hpp"
#include "output_sink.hpp"
#include "paraphonic_envelope.hpp"
#include "sample_load_info.hpp"
#include "sequencer/sequencer_command_queue.hpp"
#include "sfz_loader.hpp"
#include "snapshot_mailbox.hpp"
#include "voice_manager.hpp"
#include "wav/wav_header_parser.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

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
// Constructed but not yet driven: nothing calls into the sink today, so the
// StereoMix/TDM8 backend flag currently selects only which sink type must
// keep compiling (CI builds both flag sets). Kept so Stage B wiring has its
// object and both sinks stay in the build; see docs/backlog.md.
__attribute__((unused)) static OutputSinkType s_output_sink;
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

// Deliberately NOT in DTCM, unlike its neighbours. .dtcmram_bss is (NOLOAD)
// and nothing runs constructors for objects placed there, so this would come
// up all-zeroes - and a zeroed TrackMixer has every mute ramp at 0, i.e. all
// 16 tracks silent. That is the exact hazard Init() below documents for
// VoiceLiveParams. Init() also calls Reset() on it, so the correct state does
// not depend on where the linker put it.
static WaveX::Mix::TrackMixer s_track_mixer;

// Meter subscription (MSG_MIX_OP SUB/UNSUB_METERS). Honoured as a flag now;
// the MSG_MIX_METERS sender is stage 4 of output-routing-and-mixer.md §6, so
// subscribing currently records intent and sends nothing.
static bool s_mix_meters_subscribed = false;

// Digital voice base parameters - what MSG_CONTROL_CHANGE edits for the
// all-digital path (features/digital-voice-audition.md stage 1). The main loop
// owns pending; the callback owns active. A double-buffered generation mailbox
// publishes the complete struct at a block boundary, so the callback never
// observes a mixed old/new parameter set.
//
// ENGINE-GLOBAL, not per-slot, and deliberately so. param-locks-and-
// modulation.md scopes base values to an instrument slot, but nothing can
// address a slot differently yet - OnNoteOn does not even set one - so a
// 16-entry table would be 16 copies of the same values with no way to reach
// 15 of them. This mirrors s_para_pending, which is engine-global for the
// analog path for the same reason. It becomes per-slot with the instrument
// model (Phase 2.5), which is also when a slot becomes addressable.
//
// DTCM for the same reason as s_voice_manager: read from Callback(), CPU-only,
// tiny.
static WaveX::AudioEngine::VoiceLiveParams s_voice_live_active WAVEX_DTCM_DATA;
static WaveX::AudioEngine::VoiceLiveParams s_voice_live_pending;
static SnapshotMailbox<WaveX::AudioEngine::VoiceLiveParams> s_voice_live_mailbox;

// Modulation matrix (roadmap Phase 2.5 item 4; param-locks-and-modulation.md
// §3/§9 stage 4). Slots are instrument-scoped, stored on Instrument itself
// (SfzLoader's Tracks) rather than engine-global - unlike
// s_voice_live_pending above, an instrument slot IS now addressable
// (MSG_INST_OP's own `slot` field), so there is no more "N copies of the
// same array" problem to work around. ResolveModSlots below is the
// ModSlotResolver VoiceManager::TickModulation() uses to look up each
// voice's OWN instrument's slots (Voice::slot) - see SfzLoader::GetModSlots's
// own comment for why this reads SfzLoader's bank directly rather than
// through a mailbox.
static const WaveX::AudioEngine::ModSlot* ResolveModSlots(const void*, uint8_t slot) {
    return SfzLoader::GetModSlots(slot);
}

// Two engine-global LFOs (§5) - global by design regardless of the
// instrument model, so unlike the mod slots above they don't wait on it
// ("slot 3's wobble must not change because slot 5 loaded a new
// instrument"). Ticked once per control tick (one callback == one 1kHz tick,
// timebase.hpp) into the matrix's SRC_LFO1/SRC_LFO2 sources.
// DTCM for the same reason as s_para_env: ticked once per block from
// Callback() itself, CPU-only, tiny. Init() below MUST also set rate_hz_ -
// Lfo::Init()/Reset() deliberately leave it untouched (a running LFO
// shouldn't have its rate reset along with its phase), so on DTCM's zeroed
// memory it would otherwise stay 0 forever and the LFO would never advance -
// the exact hazard VoiceManager::Init()'s own comment warns about for
// live_pitch_scale_.
static WaveX::AudioEngine::Lfo s_mod_lfo1 WAVEX_DTCM_DATA;
static WaveX::AudioEngine::Lfo s_mod_lfo2 WAVEX_DTCM_DATA;

// Sequencer transport is callback-owned: its command queue gives the main
// loop an immutable, bounded hand-off and SequencerTransport itself keeps a
// pending/active Pattern pair swapped only between steps.
//
// BssStatic (bss_static.hpp): the transport is ~50 KB of patterns whose only
// non-zero defaults are velocity/probability/tempo, and as a plain static it
// was a 50 KB image in flash copied into SRAM at boot. Same for the voice
// map, its mailbox and the note queue below.
static WaveX::BssStatic<WaveX::Sequencer::SequencerTransport> s_seq_transport_storage;
static WaveX::Sequencer::SequencerTransport& s_seq_transport = s_seq_transport_storage.Get();
static constexpr uint32_t kSequencerCommandQueueSize = 32;
static WaveX::Sequencer::SequencerCommandQueue<kSequencerCommandQueueSize> s_seq_command_queue;

// Stage 5's deliberate interim voice map: one sample/Patch bound to Track 1
// plays chromatically across the 16 scheduler rows. It is built on the main
// loop with SfzLoader (which owns mutable instrument state) and published as
// a complete snapshot. The callback therefore never races an SFZ rebind or
// sample-registry mutation. A later Track->Patch implementation replaces this
// map with one immutable binding per Track; it must not make the callback read
// SfzLoader directly.
static constexpr uint8_t kSequencerPreviewTrack = 0;
static constexpr uint8_t kSequencerRootNote = 60;
struct SequencerVoiceMap {
    uint8_t layer_count[WaveX::Sequencer::kMaxTracks] = {};
    VoiceTriggerParams layers[WaveX::Sequencer::kMaxTracks][kMaxLayerTriggers] = {};
};
static WaveX::BssStatic<SequencerVoiceMap> s_seq_voice_map_active_storage;
static SequencerVoiceMap& s_seq_voice_map_active = s_seq_voice_map_active_storage.Get();
static WaveX::BssStatic<SnapshotMailbox<SequencerVoiceMap>> s_seq_voice_map_mailbox_storage;
static SnapshotMailbox<SequencerVoiceMap>& s_seq_voice_map_mailbox =
    s_seq_voice_map_mailbox_storage.Get();

// --- Stage A paraphonic analog path (roadmap item 5; analog-voice-board.md
// §0). One shared envelope drives the shared VCF/VCA CVs; values are
// STAGED at the 1 kHz control tick (audio context, callback-safe - the
// router/backend publishes one complete frame) and FLUSHED from the main loop
// (blocking I2C ~225 us, §7.1.4) via FlushCv() below.
// DTCM: ticked once per block from Callback() itself, CPU-only, small - same
// case as s_voice_manager above.
static ParaphonicEnvelope s_para_env WAVEX_DTCM_DATA;

// Shared-path control values. The main loop owns pending; the callback owns
// active and consumes only complete mailbox snapshots at block boundaries.
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
static ParaphonicParams s_para_active WAVEX_DTCM_DATA;
static ParaphonicParams s_para_pending;
static SnapshotMailbox<ParaphonicParams> s_para_mailbox;

// Set by the tick after staging fresh CV values; consumed by FlushCv().
static volatile bool s_cv_dirty = false;

// Calibration-procedure CV override (MSG_CV_TEST): while active the tick
// stages these fixed control values instead of the paraphonic law. Publish the
// whole override together so enable can never pair with stale coordinates.
struct CvTestParams {
    bool active = false;
    uint8_t group = 0;
    float cutoff = 0.0f;
    float resonance = 0.0f;
    float vca = 0.0f;
};
static CvTestParams s_cv_test_active WAVEX_DTCM_DATA;
static CvTestParams s_cv_test_pending;
static SnapshotMailbox<CvTestParams> s_cv_test_mailbox;

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
    bool scoped_release = false;
    uint8_t note = 0;
    uint8_t track = 0;
    WaveX::AudioEngine::VoiceTriggerParams params;
};
static constexpr uint32_t kNoteQueueSize = 16;  // power of two (index math wraps)
using NoteQueue = NoteEventQueue<NoteEvent, kNoteQueueSize>;
static WaveX::BssStatic<NoteQueue> s_note_queue_storage;
static NoteQueue& s_note_queue = s_note_queue_storage.Get();

// NoteEventQueue's legacy overflow bitmap is keyed only by note. Instrument
// note-offs also need the slot, or a full queue could release a same-pitch
// voice belonging to another MIDI channel. One bounded bitmap per slot keeps
// that information without allocation or locks. Producer: main loop; consumer:
// callback, same release/acquire discipline as NoteEventQueue.
static uint32_t
    s_scoped_release_overflow[kNumTracks]
                             [NoteEventQueue<NoteEvent, kNoteQueueSize>::kReleaseWordCount];
static uint32_t s_scoped_release_pending_slots = 0;

// Set by the main loop before releasing loaded-sample memory (an unload);
// consumed by Callback(), which drains the queue and then hard-stops every
// voice so nothing keeps reading freed SDRAM.
static bool s_voice_stop_all = false;
// The per-Track form of the same barrier (track-and-patch-model.md §4): a
// bit per Track whose voices must stop before that Track's Pool refs are
// released - replacing an import, binding a sample over one. Other Tracks
// keep sounding.
static uint16_t s_voice_stop_tracks = 0;
// Runtime SFZ replacement waits for this callback acknowledgement before it
// releases the old Track's non-owning sample pointers.
static bool s_voice_stop_ack = false;

// Audio-callback side: apply every pending note event, then honor a
// pending hard-stop. Order matters - a stop request must also kill
// triggers queued before it (they reference the memory being released).
// Returns true if any trigger was applied - the paraphonic envelope's
// note-on edge (item 5).
static bool drain_note_queue() {
    bool any_trigger = false;
    NoteEvent ev;
    while (s_note_queue.Pop(ev)) {
        if (ev.is_trigger) {
            s_voice_manager.Trigger(ev.params);
            any_trigger = true;
        } else if (ev.scoped_release) {
            s_voice_manager.ReleaseTrack(ev.note, ev.track);
        } else {
            s_voice_manager.Release(ev.note);
        }
    }
    // A full queue may drop note-ons, but never note-offs: releases that could
    // not enter the ring are coalesced by MIDI note and applied after all
    // older queued events, preserving their arrival order relative to them.
    for (uint32_t word = 0; word < NoteQueue::kReleaseWordCount; ++word) {
        uint32_t releases = s_note_queue.TakeOverflowReleaseWord(word);
        while (releases != 0) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(releases));
            s_voice_manager.Release(static_cast<uint8_t>(word * 32u + bit));
            releases &= releases - 1u;
        }
    }
    uint32_t pending_slots =
        __atomic_exchange_n(&s_scoped_release_pending_slots, 0u, __ATOMIC_ACQUIRE);
    while (pending_slots != 0) {
        const uint8_t slot = static_cast<uint8_t>(__builtin_ctz(pending_slots));
        for (uint32_t word = 0; word < NoteQueue::kReleaseWordCount; ++word) {
            uint32_t releases =
                __atomic_exchange_n(&s_scoped_release_overflow[slot][word], 0u, __ATOMIC_ACQUIRE);
            while (releases != 0) {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(releases));
                s_voice_manager.ReleaseTrack(static_cast<uint8_t>(word * 32u + bit), slot);
                releases &= releases - 1u;
            }
        }
        pending_slots &= pending_slots - 1u;
    }
    if (__atomic_exchange_n(&s_voice_stop_all, false, __ATOMIC_ACQUIRE)) {
        s_voice_manager.StopAll();
        __atomic_store_n(&s_voice_stop_ack, true, __ATOMIC_RELEASE);
    }
    uint16_t stop_tracks = __atomic_exchange_n(&s_voice_stop_tracks, 0u, __ATOMIC_ACQUIRE);
    if (stop_tracks != 0) {
        while (stop_tracks != 0) {
            const uint8_t track = static_cast<uint8_t>(__builtin_ctz(stop_tracks));
            s_voice_manager.StopTrack(track);
            stop_tracks &= static_cast<uint16_t>(stop_tracks - 1u);
        }
        __atomic_store_n(&s_voice_stop_ack, true, __ATOMIC_RELEASE);
    }
    return any_trigger;
}

static void ApplySequencerLiveParams(VoiceTriggerParams& params) {
    params.filter_cutoff_hz = s_voice_live_active.filter_cutoff_hz;
    params.filter_resonance = s_voice_live_active.filter_resonance;
    params.attack_s = s_voice_live_active.attack_s;
    params.decay_s = s_voice_live_active.decay_s;
    params.sustain_level = s_voice_live_active.sustain_level;
    params.release_s = s_voice_live_active.release_s;
}

// Callback-only. The main loop publishes complete command records and an
// independently complete voice map before enqueuing PLAY, so consuming commands
// before the map makes step 0's immediate trigger see the matching binding.
static bool drain_sequencer(uint16_t block_size) {
    WaveX::Sequencer::SequencerCommand command;
    while (s_seq_command_queue.Pop(command)) {
        switch (command.type) {
            case WaveX::Sequencer::SequencerCommandType::Transport:
                s_seq_transport.ApplyTransport(command.transport);
                break;
            case WaveX::Sequencer::SequencerCommandType::PatternOp:
                s_seq_transport.ApplyPatternOp(command.pattern_op);
                break;
            case WaveX::Sequencer::SequencerCommandType::MidiClock:
                s_seq_transport.OnMidiClock(command.midi_clock);
                break;
            case WaveX::Sequencer::SequencerCommandType::MidiCc:
                s_seq_transport.OnMidiCc(command.midi_cc);
                break;
        }
    }
    s_seq_voice_map_mailbox.ConsumeLatest(s_seq_voice_map_active);

    const uint64_t block_start_frame = s_seq_transport.scheduler().CurrentFrame();
    WaveX::Sequencer::TriggerEvent events[WaveX::Sequencer::kMaxEventsPerTick];
    const size_t event_count = s_seq_transport.Tick(events, WaveX::Sequencer::kMaxEventsPerTick);
    bool any_trigger = false;
    for (size_t event_index = 0; event_index < event_count; ++event_index) {
        const WaveX::Sequencer::TriggerEvent& event = events[event_index];
        if (event.track >= WaveX::Sequencer::kMaxTracks || event.frame < block_start_frame) {
            continue;
        }
        const uint64_t offset = event.frame - block_start_frame;
        if (offset >= block_size) {
            continue;
        }
        const uint8_t layer_count = s_seq_voice_map_active.layer_count[event.track];
        for (uint8_t layer = 0; layer < layer_count; ++layer) {
            VoiceTriggerParams params = s_seq_voice_map_active.layers[event.track][layer];
            params.velocity = event.velocity;
            params.start_offset_frames = static_cast<uint16_t>(offset);
            ApplySequencerLiveParams(params);
            s_voice_manager.Trigger(params);
            any_trigger = true;
        }
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
#if WAVEX_DEBUG_HARNESS_ENABLED
static uint8_t s_dbg_active_voices = 0;  // callback-published, see Callback()
#endif

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
// The Sample Pool (track-and-patch-model.md §4)
// ============================
// One registry for every resident sample, whoever loaded it. The record
// payload below is what the engine keeps per sample; the registry adds the
// id, the Track ownership mask, the pin and the path hash. Records live in
// their own SDRAM partition; the registry object (with its 2 KB id index)
// is SRAM. Main-loop only - the callback never touches it; voices hold
// non-owning pointers into the arena, which is why every release goes
// through the voice-stop barrier first.
// LoadedSampleInfo and SamplePool: audio/sample_pool.hpp.
static_assert(sizeof(SamplePool::Record) * WAVEX_SAMPLE_POOL_CAPACITY <=
                  WaveX::SdramLayout::kSampleRegistryBytes,
              "the Sample Pool's records must fit their SDRAM partition");
// Raw .bss storage, constructed in Init() once SDRAM is known good (the
// registry needs its record table's address, so BssStatic's default
// construction does not fit).
alignas(SamplePool) static uint8_t s_pool_bytes[sizeof(SamplePool)];
static SamplePool* s_pool = nullptr;

// O(1): the id names its registry slot (SampleRegistry::Find).
static LoadedSampleInfo* find_loaded_sample(uint16_t sample_id) {
    if (!s_pool) {
        return nullptr;
    }
    SamplePool::Record* r = s_pool->Find(sample_id);
    return r ? &r->payload : nullptr;
}

// "The sample that just loaded": what the preview, the envelope job and
// SetEditParams mean by id 0.
static LoadedSampleInfo* newest_loaded_sample() {
    return s_pool ? find_loaded_sample(s_pool->Newest()) : nullptr;
}

static size_t loaded_sample_count() {
    return s_pool ? s_pool->Count() : 0;
}

// The SampleResolver for instruments built on-device (SfzLoader::BindSample)
// - the bridge instrument-model.md §12.1 asks for, over this registry's ids.
// Beyond the audio itself it hands over the sample's own resolved markers
// and gain, so a zone that leaves its region/loop fields at 0 plays exactly
// the region the editor auditioned: the same record every other playback
// and display path reads (see LoadedSampleInfo::meta). Main-loop context
// (OnNoteOn), same as the registry's other readers.
static SampleRef ResolveLoadedSample(const void*, uint16_t sample_id) {
    SampleRef ref;
    const LoadedSampleInfo* src = find_loaded_sample(sample_id);
    void* sample_ptr = nullptr;
    if (!src || !SampleIsPlayable(*src) || !s_sample_mem_mgr.ptr(src->handle, &sample_ptr) ||
        !sample_ptr) {
        return ref;
    }
    const uint32_t bytes = src->loaded_bytes ? src->loaded_bytes : src->handle.len;
    ref.data = static_cast<const int16_t*>(sample_ptr);
    ref.frames = bytes / (2u * src->channels);
    ref.channels = src->channels;
    ref.sample_rate_hz = src->sample_rate;  // 44.1k content pitches correctly on 48k engine

    WaveX::Protocol::SampleMetadata m = src->meta;
    if (m.total_frames == 0) {
        m.total_frames = ref.frames;
    }
    m.Resolve();
    ref.start_frame = m.start_frame;
    ref.end_frame = m.end_frame;
    ref.loop_enabled = m.loop_enabled != 0;
    ref.loop_start = m.loop_start;
    ref.loop_end = m.loop_end;
    ref.fade_in_ms = m.fade_in_ms;
    ref.fade_out_ms = m.fade_out_ms;
    // gain_mul is linear and multiplies the velocity gain, so the dB figure
    // has to be converted here rather than passed through.
    ref.gain_mul =
        (m.gain_db_x10 == 0) ? 1.0f : std::pow(10.0f, static_cast<float>(m.gain_db_x10) / 200.0f);
    return ref;
}

// Scratch for building the next voice map on the main loop before it is
// copied into the mailbox. A static rather than a local because a
// SequencerVoiceMap is ~6 KB: as `SequencerVoiceMap map{}` this was a 6 KB
// stack frame against the DTCM stack budget (bss_static.hpp).
static WaveX::BssStatic<SequencerVoiceMap> s_seq_voice_map_scratch_storage;

static void PublishSequencerVoiceMap() {
    s_seq_voice_map_scratch_storage.Reconstruct();
    SequencerVoiceMap& map = s_seq_voice_map_scratch_storage.Get();
    if (!SfzLoader::TrackLoading(kSequencerPreviewTrack)) {
        for (uint8_t track = 0; track < WaveX::Sequencer::kMaxTracks; ++track) {
            const uint8_t note = static_cast<uint8_t>(kSequencerRootNote + track);
            map.layer_count[track] = SfzLoader::ResolveNote(
                kSequencerPreviewTrack, note, 127, nullptr, map.layers[track], kMaxLayerTriggers);
        }
    }
    s_seq_voice_map_mailbox.Publish(map);
}

static void ClearSequencerVoiceMap() {
    s_seq_voice_map_scratch_storage.Reconstruct();
    s_seq_voice_map_mailbox.Publish(s_seq_voice_map_scratch_storage.Get());
}

// Drops `sample_id` from the registry and returns its memory to the arena.
// Entries stay in load order (oldest first), so removal closes the gap by
// shifting rather than swapping with the tail: OnPreviewReq() reads the last
// entry as "most recently loaded", and a swap would quietly hand it an older
// sample.
// Sends one sample's record. Called on load, on edit, and on request - the
// frontend never derives these values, it is told them. Ownership (used_by,
// pinned) rides along from the Pool record when there is one.
static void PushSampleMeta(const LoadedSampleInfo& info) {
    SampleMetadata wire = info.meta;
    wire.flags = SAMPLE_META_RESIDENT;
    if (s_pool) {
        if (const SamplePool::Record* r = s_pool->Find(info.sample_id)) {
            wire.used_by = r->used_by;
            if (r->pinned) {
                wire.flags |= SAMPLE_META_PINNED;
            }
        }
    }
    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_META, &wire, sizeof(wire));
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
    s_loop_gap_frames =
        static_cast<uint32_t>((static_cast<float>(gap_ms) * s_sample_rate) / 1000.0f);
    s_loop_gap_remaining = 0;  // never start an audition mid-gap
}

void PushAllSampleMeta(uint16_t sample_id) {
    if (!s_pool) {
        return;
    }
    if (sample_id != 0) {
        if (const LoadedSampleInfo* info = find_loaded_sample(sample_id)) {
            PushSampleMeta(*info);
        }
        return;
    }
    // Everything, one message per record: at Pool scale this is what the
    // paged query replaces (stage 3d); until then bounded by the TX queue
    // draining between main-loop passes.
    s_pool->ForEach([](SamplePool::Record& r) { PushSampleMeta(r.payload); });
}

// Frees the audio memory and forgets the record. Callers MUST have passed
// the voice-stop barrier first: this releases SDRAM that a playing voice
// would otherwise still be reading through its non-owning Voice::sample.
static void remove_loaded_sample(uint16_t sample_id) {
    LoadedSampleInfo* info = find_loaded_sample(sample_id);
    if (!info) {
        return;
    }
    s_sample_mem_mgr.release(&info->handle);
    s_pool->Remove(sample_id);
}

void SelectSample(uint16_t sample_id, uint8_t slot) {
    if (slot >= kNumTracks) {
        WaveX::Log::PrintLine("SAMPLE_SELECT: track=%u out of range, ignored", (unsigned)slot);
        return;
    }
    if (sample_id != 0) {
        if (!find_loaded_sample(sample_id)) {
            WaveX::Log::PrintLine("SAMPLE_SELECT: track=%u id=%u is not resident, ignored",
                                  (unsigned)slot,
                                  (unsigned)sample_id);
            return;
        }
    }
    // Whatever the Track holds is released by the bind, and an import's
    // samples that nobody else holds are freed with it - so this Track's
    // voices stop first. Per-track: the other fifteen keep sounding. The
    // callback consumes the mask at the top of its next block; 10 ms is
    // the same margin over the 1 ms block that the unload barrier uses.
    if (SfzLoader::TrackLoaded(slot)) {
        if (slot == kSequencerPreviewTrack) {
            ClearSequencerVoiceMap();
        }
        __atomic_fetch_or(
            &s_voice_stop_tracks, static_cast<uint16_t>(1u << slot), __ATOMIC_RELEASE);
        System::Delay(10);
    }
    if (!SfzLoader::BindSample(*s_pool, s_sample_mem_mgr, slot, sample_id)) {
        WaveX::Log::PrintLine(
            "SAMPLE_SELECT: track=%u id=%u refused", (unsigned)slot, (unsigned)sample_id);
        return;
    }
    if (slot == kSequencerPreviewTrack) {
        PublishSequencerVoiceMap();
    }
    // The frontend caches bindings; tell it this one changed rather than
    // wait to be asked (the HIL suite found the cache going stale when a
    // Track changed behind the UI's back, 2026-09-04).
    PushTrackBinding(slot);
    WaveX::Log::PrintLine("SAMPLE_SELECT: track=%u id=%u", (unsigned)slot, (unsigned)sample_id);
}

uint16_t SelectedSample(uint8_t slot) {
    return SfzLoader::BoundSample(slot);
}

// Tracks whose binding has been requested but not yet sent, one bit each.
// Main-loop only (the message handler sets it, PumpTrackBinding drains it),
// so it needs no synchronisation - see UartLinkSend's single-context
// invariant. A broadcast marks all 16 rather than sending them: the UART TX
// queue is 4 deep (daisy_uart_link.cpp MSG_QUEUE_SIZE), so a 16-message
// burst would drop most of the replies as queue overflow.
static uint16_t s_track_binding_pending = 0;

// What a Track holds, as MSG_TRACK_BINDING reports it. Main loop only.
static void BuildTrackBinding(uint8_t track, TrackBindingMessage& out) {
    uint8_t state = TRACK_BINDING_EMPTY;
    uint16_t sample_id = 0;
    if (SfzLoader::TrackLoading(track)) {
        state = TRACK_BINDING_LOADING;
    } else if (SfzLoader::TrackLoaded(track)) {
        sample_id = SfzLoader::BoundSample(track);
        state = sample_id != 0 ? TRACK_BINDING_SAMPLE : TRACK_BINDING_PATCH;
    }
    out = TrackBindingMessage(track, state, sample_id);
    snprintf(out.name, sizeof(out.name), "%s", SfzLoader::TrackName(track));
}

#if WAVEX_DEBUG_HARNESS_ENABLED
void DebugTrackBinding(uint8_t track, TrackBindingMessage& out) {
    if (track >= kNumTracks) {
        out = TrackBindingMessage();
        return;
    }
    BuildTrackBinding(track, out);
}

uint8_t DebugActiveVoices() {
    return __atomic_load_n(&s_dbg_active_voices, __ATOMIC_RELAXED);
}

size_t DebugLoadedSamples(uint16_t* ids, size_t cap) {
    if (!s_pool) {
        return 0;
    }
    size_t n = 0;
    s_pool->ForEach([&](SamplePool::Record& r) {
        if (n < cap) {
            ids[n++] = r.sample_id;
        }
    });
    return cap == 0 ? s_pool->Count() : n;
}
#endif

// The pending page request (0xFFFF = none). One at a time: a newer request
// replaces an older unsent one, which is what a scrolling list wants.
static uint16_t s_meta_page_first = 0xFFFF;
static uint8_t s_meta_page_count = 0;
// Reply buffer: header + MAX_SAMPLE_META_PAGE records, one UART frame.
static uint8_t
    s_meta_page_buf[sizeof(SampleMetaPageHeader) + MAX_SAMPLE_META_PAGE * sizeof(SampleMetadata)];

void RequestSampleMetaPage(uint16_t first, uint8_t count) {
    s_meta_page_first = first;
    s_meta_page_count =
        count == 0 ? 1 : (count > MAX_SAMPLE_META_PAGE ? MAX_SAMPLE_META_PAGE : count);
}

// The record as the frontend should see it: the payload's meta plus the
// Pool's ownership, which lives on the Record, not in the meta.
static void MetaForWire(const SamplePool::Record& r, SampleMetadata& out) {
    out = r.payload.meta;
    out.used_by = r.used_by;
    out.flags = SAMPLE_META_RESIDENT | (r.pinned ? SAMPLE_META_PINNED : 0);
}

static void PumpSampleMetaPage() {
    if (s_meta_page_first == 0xFFFF) {
        return;
    }
    SamplePool::Record* page[MAX_SAMPLE_META_PAGE];
    size_t total = 0;
    const size_t n = s_pool ? s_pool->Page(s_meta_page_first, s_meta_page_count, page, &total) : 0;
    auto* header = reinterpret_cast<SampleMetaPageHeader*>(s_meta_page_buf);
    *header = SampleMetaPageHeader();
    header->total = static_cast<uint16_t>(total);
    header->first = s_meta_page_first;
    header->n = static_cast<uint8_t>(n);
    auto* records =
        reinterpret_cast<SampleMetadata*>(s_meta_page_buf + sizeof(SampleMetaPageHeader));
    for (size_t i = 0; i < n; ++i) {
        MetaForWire(*page[i], records[i]);
    }
    const size_t bytes = sizeof(SampleMetaPageHeader) + n * sizeof(SampleMetadata);
    if (WaveX::Comm::UartLinkSend(MSG_SAMPLE_META_PAGE, s_meta_page_buf, bytes) < 0) {
        return;  // queue full: try again next pass
    }
    s_meta_page_first = 0xFFFF;
}

void PushTrackBinding(uint8_t track) {
    if (track == 0xFF) {
        s_track_binding_pending = 0xFFFF;
    } else if (track < kNumTracks) {
        s_track_binding_pending |= static_cast<uint16_t>(1u << track);
    }
}

void PumpTrackBinding() {
    PumpSampleMetaPage();
    // Two per iteration leaves room in the 4-deep queue for the status and
    // meter traffic this same loop sends. A full queue keeps the bit set, so
    // the reply is delayed rather than lost.
    for (uint8_t sent = 0; sent < 2 && s_track_binding_pending != 0; ++sent) {
        const uint8_t track =
            static_cast<uint8_t>(__builtin_ctz(static_cast<unsigned>(s_track_binding_pending)));

        TrackBindingMessage msg;
        BuildTrackBinding(track, msg);
        if (WaveX::Comm::UartLinkSend(MSG_TRACK_BINDING, &msg, sizeof(msg)) < 0) {
            return;
        }
        s_track_binding_pending &= static_cast<uint16_t>(~(1u << track));
    }
}

bool UnloadSample(uint16_t sample_id) {
    if (sample_id == 0) {
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=0 rejected (not a wildcard)");
        return false;
    }
    if (!find_loaded_sample(sample_id)) {
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
    // Drop the sequencer's immutable pointer snapshot before the callback
    // acknowledgement that permits this sample's storage to be freed.
    ClearSequencerVoiceMap();
    __atomic_store_n(&s_voice_stop_all, true, __ATOMIC_RELEASE);
    System::Delay(10);

    // A zone bound to what we are about to free must not keep resolving to
    // a sample_id that no longer exists - drop the binding first rather than
    // leave one that OnNoteOn would silently fail to resolve.
    SfzLoader::ForgetLoadedSample(sample_id);
    // Tell the frontend the record is gone: the same message with the
    // resident flag clear, so its cache drops the entry instead of listing a
    // sample that no longer exists.
    if (LoadedSampleInfo* gone = find_loaded_sample(sample_id)) {
        WaveX::Protocol::SampleMetadata bye = gone->meta;
        bye.flags = 0;
        WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_META, &bye, sizeof(bye));
    }
    remove_loaded_sample(sample_id);
    PublishSequencerVoiceMap();
    // Any Track that was bound to it is empty now; the frontend cannot know
    // which, so refresh them all.
    PushTrackBinding(0xFF);

    WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=%u freed (%u still loaded)",
                          (unsigned)sample_id,
                          (unsigned)loaded_sample_count());
    return true;
}

// Defined with the envelope job below; declared here because loading is the
// one event that can pull the audio out from under a scan in flight.
static void CancelEnvelopeJob();

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
static uint32_t s_dbg_resampled = 0;  // ResampleStreamInterleaved() result
static uint32_t s_dbg_pushes = 0;     // passes that actually reached rb_push_frames

// Interval counters for MSG_DIAG_PUSH. Deltas, reset on read - a since-boot
// total cannot show that discards started thirty seconds ago, which is exactly
// the shape both playback stalls had.
static uint32_t s_diag_pushes = 0;
static uint32_t s_diag_discards = 0;   // passes that produced frames and then
                                       // skipped without consuming the slot
static uint32_t s_diag_underruns = 0;  // underrun episodes
static uint32_t s_last_io_time = 0;    // Last time we did SD I/O (for rate limiting)
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
// calling arm_scale_q15: CMSIS-DSP is not linked, and this is a two-line
// multiply.
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

    const WaveX::AudioEngine::RegionFade region_fade =
        WaveX::AudioEngine::RegionFade::Prepare(start, end, fade_in, fade_out);
    for (uint32_t i = 0; i < frames; ++i) {
        const float g = region_fade.Gain(block_start + i);
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

// Pre-buffering functions
static bool prebuffer_audio() {
    PROFILE_SCOPE(prebuffer_audio);

    if (!s_wav.open || s_prebuffer_ready) {
        return true;
    }

    s_prebuffering = true;
    ResetScratchPool();
    uint32_t bytes_per_sample = (s_wav.bits_per_sample == 24) ? 3u : 2u;
    uint32_t file_bpf = (uint32_t)s_wav.num_channels * bytes_per_sample;
    uint32_t free_prebuffer_frames = PREBUFFER_FRAMES - s_prebuffer_filled;
    float resample_ratio = 1.0f;
    if (static_cast<float>(s_wav.sample_rate) != s_sample_rate) {
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

        // ResampleStreamInterleaved() returns 0 for fewer than 2 input frames, and
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

    UINT br = 0;
    s_io_start_time = System::GetTick();
    FRESULT fr = f_read(&s_wav.file, s_prebuffer_sd, req_bytes, &br);
    s_io_duration = System::GetTick() - s_io_start_time;

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
    ConvertFramesToOutput(src,
                          conversion_output,
                          frames_read,
                          s_wav.num_channels,
                          static_cast<uint8_t>(s_wav.bits_per_sample));
    ApplyWavGain(conversion_output, frames_read * s_output_channels);

    q15_t* to_push = conversion_output;
    uint32_t output_frames = frames_read;
    if (resample_ratio != 1.0f) {
        uint32_t max_out_frames =
            static_cast<uint32_t>(std::ceil(static_cast<float>(frames_read) * resample_ratio)) + 1;
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
    memcpy(dst, to_push, output_frames * s_output_channels * sizeof(q15_t));
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
    memcpy(&s_rb[dst_idx], samples, chunk * samples_per_channel * sizeof(q15_t));
    if (frames > chunk) {
        memcpy(s_rb,
               samples + chunk * samples_per_channel,
               (frames - chunk) * samples_per_channel * sizeof(q15_t));
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

void Init(DaisySeed& hw, float sample_rate, bool sdram_available) {
    s_hw = &hw;
    s_sample_rate = sample_rate;

    // Explicit rather than relying on static initialization - see the note on
    // s_track_mixer's placement. Reset() opens every track and settles the
    // ramps, so nothing fades in at boot.
    s_track_mixer.Reset();
    s_track_mixer.SetSampleRate(sample_rate);
    s_voice_manager.SetTrackMixer(&s_track_mixer);

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
    s_voice_live_pending = WaveX::AudioEngine::VoiceLiveParams{};
    s_voice_live_active = s_voice_live_pending;
    s_voice_live_mailbox.Init(s_voice_live_pending);
    s_para_pending = ParaphonicParams{};
    s_para_active = s_para_pending;
    s_para_mailbox.Init(s_para_pending);
    s_cv_test_pending = CvTestParams{};
    s_cv_test_active = s_cv_test_pending;
    s_cv_test_mailbox.Init(s_cv_test_pending);
    s_note_queue.Init();
    s_seq_command_queue.Init();
    s_seq_voice_map_active_storage.Reconstruct();
    s_seq_voice_map_mailbox.Init(s_seq_voice_map_active);
    std::memset(s_scoped_release_overflow, 0, sizeof(s_scoped_release_overflow));
    __atomic_store_n(&s_scoped_release_pending_slots, 0u, __ATOMIC_RELAXED);
    s_rb_low_water = 0xFFFFFFFFu;

    SfzLoader::Reset();
    SfzLoader::SetLoadedSampleResolver(SampleResolver{nullptr, &ResolveLoadedSample});

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
    // The Pool's records live in SDRAM too, so the registry exists only on
    // boots where SDRAM came up - the same gate the loader has always had.
    if (s_sample_memory_available) {
        s_pool = new (s_pool_bytes) SamplePool(
            reinterpret_cast<SamplePool::Record*>(WaveX::SdramLayout::kSampleRegistryBase));
    }
    if (s_hw) {
        WaveX::Log::PrintLine(
            "AUDIO_ENGINE: Sample RAM %s (arena=%lu, pool=%u records in %lu, render scratch=%lu)",
            s_sample_memory_available ? "ready" : "disabled",
            (unsigned long)WaveX::SdramLayout::kSampleArenaBytes,
            (unsigned)WAVEX_SAMPLE_POOL_CAPACITY,
            (unsigned long)WaveX::SdramLayout::kSampleRegistryBytes,
            (unsigned long)WaveX::SdramLayout::kRenderScratchBytes);
    }

    s_voice_manager.Init(static_cast<uint32_t>(sample_rate));

    // Global LFOs run at the 1kHz control-tick rate regardless of the audio
    // sample rate. See s_mod_lfo1/2's own comment for why SetRateHz() must
    // be called explicitly here rather than trusted to a member initializer.
    s_mod_lfo1.Init(1000.0f);
    s_mod_lfo1.SetRateHz(1.0f);
    s_mod_lfo2.Init(1000.0f);
    s_mod_lfo2.SetRateHz(1.0f);

    // Sequencer transport uses the same sample-rate/block-size timebase as
    // the audio engine so its scheduler frames line up with the callback.
    s_seq_transport.Init(static_cast<uint32_t>(sample_rate), Timebase::kBlockSize);

    // Stage A paraphonic envelope runs at the control-tick rate (1 kHz).
    // Defaults are musical bring-up values; stage 3 maps ENVELOPE_* wire
    // parameters onto SetParams.
    s_para_env.Init(1000);
    s_para_env.SetParams(s_para_active.attack_s,
                         s_para_active.decay_s,
                         s_para_active.sustain,
                         s_para_active.release_s);

    // Test basic allocation to ensure SDRAM is working
    if (s_hw) {
        WaveX::Log::PrintLine("AUDIO_ENGINE: Testing Sample RAM allocation...");
    }
    wxsamp_t test_handle = {};
    bool test_alloc = s_sample_mem_mgr.alloc(1024, &test_handle);
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
    bool any_note_on = drain_note_queue();

    // Publish control-plane changes only at a block boundary. A callback that
    // preempts the producer mid-copy keeps the previous complete snapshot and
    // picks up the new generation one block later.
    if (s_voice_live_mailbox.ConsumeLatest(s_voice_live_active)) {
        s_voice_manager.ApplyLiveParams(s_voice_live_active);
    }

    // Sequencer transport and pattern edits are callback-owned through the
    // bounded command queue. Its TriggerEvents start voices at their exact
    // sample offset inside this block, after the latest live voice snapshot is
    // active for both existing and newly scheduled voices.
    any_note_on = drain_sequencer(static_cast<uint16_t>(size)) || any_note_on;
    if (s_para_mailbox.ConsumeLatest(s_para_active)) {
        s_para_env.SetParams(s_para_active.attack_s,
                             s_para_active.decay_s,
                             s_para_active.sustain,
                             s_para_active.release_s);
    }
    s_cv_test_mailbox.ConsumeLatest(s_cv_test_active);

    // Modulation matrix + global LFOs (roadmap Phase 2.5 item 4;
    // param-locks-and-modulation.md §3/§5). One callback IS one 1kHz control
    // tick (Timebase's own invariant - see its comment), so this runs once
    // per block: tick both global LFOs, then evaluate every sounding voice's
    // modulation destinations (against ITS OWN instrument's slots, via
    // ResolveModSlots) so Render() below picks up this tick's values rather
    // than the previous one's. Unconditional on WAVEX_ANALOG_CV_ENABLED -
    // this is the all-digital path, unrelated to the optional analog CV
    // stage further down.
    {
        WaveX::AudioEngine::ModSources mod_global_sources;
        mod_global_sources.lfo1 = s_mod_lfo1.Tick();
        mod_global_sources.lfo2 = s_mod_lfo2.Tick();
        const WaveX::AudioEngine::ModSlotResolver mod_slot_resolver{nullptr, &ResolveModSlots};
        s_voice_manager.TickModulation(
            mod_slot_resolver, mod_global_sources, static_cast<uint32_t>(size));
    }

    const uint8_t active_voices = s_voice_manager.ActiveVoiceCount();
#if WAVEX_DEBUG_HARNESS_ENABLED
    // One relaxed store per block for the console's STATE verb; the main
    // loop reads it, nothing synchronises on it.
    __atomic_store_n(&s_dbg_active_voices, active_voices, __ATOMIC_RELAXED);
#endif
    if (active_voices > 0 && size <= static_cast<size_t>(Timebase::kBlockSize)) {
        static float vm_l[Timebase::kBlockSize];
        static float vm_r[Timebase::kBlockSize];
        // Advance the mute ramps once per block, before the voices read them.
        s_track_mixer.Tick(static_cast<uint32_t>(size));
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

#if WAVEX_ANALOG_CV_ENABLED
    // Compiled out by default - see WAVEX_ANALOG_CV_ENABLED in
    // hardware_config.h. The filter and VCA you actually hear are the
    // per-voice digital SVF and gain in VoiceManager; this block drives an
    // external paraphonic analog stage that is not fitted, and it is not free:
    // a paraphonic envelope plus CvShapeCutoff()'s two expf() calls, every
    // millisecond, inside the audio callback.
    Timebase::Tick1kHz([&] {
        // Stage A paraphonic control law (item 5): the shared envelope
        // gates the analog VCA and modulates the shared VCF cutoff above
        // its base. Staging is callback-safe: QueueGroup publishes one
        // complete frame; the I2C transaction happens in FlushCv() on the
        // main loop.
        const bool any_held = s_voice_manager.HeldVoiceCount() > 0;
        const float env = s_para_env.Tick(any_note_on, any_held);
        if (s_cv_test_active.active) {
            // Calibration override: steady, user-commanded CVs.
            s_cv_router.QueueVoice(s_cv_test_active.group,
                                   s_cv_test_active.cutoff,
                                   s_cv_test_active.resonance,
                                   s_cv_test_active.vca);
        } else {
            const float cutoff =
                CvClamp01(s_para_active.cutoff_base + s_para_active.env_to_cutoff * env);
            s_cv_router.QueueVoice(0, cutoff, s_para_active.resonance, env);
        }
        __atomic_store_n(&s_cv_dirty, true, __ATOMIC_RELEASE);
    });
#else
    (void)any_note_on;
#endif

    s_cpu_load_meter.OnBlockEnd();
    s_dwt_callback_cycles = WaveX::Profiling::GetCycles() - callback_cycles_start;
    s_dwt_callback_max = std::max(s_dwt_callback_max, s_dwt_callback_cycles);
}

// MSG_CONTROL_CHANGE -> Stage A paraphonic path (item 5 stage 3). Main-loop
// handlers update pending structs and publish complete snapshots. The callback
// applies them at the next block boundary, including shared-envelope rates.
// Each parameter now has TWO destinations, deliberately: the Stage A analog
// path (s_para_pending, one shared VCF/VCA) and the digital per-voice path
// (s_voice_live_pending). They are not alternatives - the analog board is
// optional hardware and the digital voices always render - so a knob has to
// reach both or it would do nothing on whichever configuration is in use.
// Per-slot (kit) scoping of these values remains Phase 2.5 instrument-model
// work; see s_voice_live_pending for why engine-global is the honest interim.
void OnMixOp(const MixOpMessage& m) {
    switch (m.op) {
        case MIX_OP_SET_GAIN:
            s_track_mixer.SetGain(m.track,
                                  WaveX::Mix::DbToLinear(WaveX::Mix::WireToGainDb(m.value)));
            break;
        case MIX_OP_SET_PAN:
            s_track_mixer.SetPanOffset(m.track, WaveX::Mix::WireToPan(m.value));
            break;
        case MIX_OP_SET_MUTE:
            s_track_mixer.SetMute(m.track, m.value != 0);
            break;
        case MIX_OP_SET_MUTE_MASK:
            // One message for a whole solo change: sending 16 individual mutes
            // would walk the ramps through states where the wrong tracks are
            // down, and at 5 ms per ramp that is audible.
            s_track_mixer.SetMuteMask(m.value);
            break;
        case MIX_OP_SET_MASTER:
            // Stored, not yet applied. PARAM_VOLUME still owns the master gain;
            // having both drive it would mean two controls fighting over one
            // value with no defined winner. output-routing-and-mixer.md §1 says
            // PARAM_VOLUME becomes explicitly master-scoped - that
            // reconciliation belongs with the mixer page, not here.
            s_track_mixer.SetMasterGain(WaveX::Mix::DbToLinear(WaveX::Mix::WireToGainDb(m.value)));
            break;
        case MIX_OP_SUB_METERS:
        case MIX_OP_UNSUB_METERS:
            s_mix_meters_subscribed = (m.op == MIX_OP_SUB_METERS);
            break;
        default:
            break;
    }
}

/**
 * One Track setting (MSG_TRACK_OP; track-and-patch-model.md §2.1).
 *
 * Main-loop context, like every other Tracks mutation. Nothing here touches
 * a sounding voice: midi_in changes where the *next* note goes, so a Track
 * switched to Off while holding notes releases them normally rather than
 * cutting - and the note-off routes by the same rule the note-on used only
 * if midi_in did not change in between. That window is one held note and
 * resolves on the next note-off from the same source; a mid-note routing
 * change is a user action, not a real-time path, so it does not justify
 * per-voice routing state.
 */
void OnTrackOp(const TrackOpMessage& m) {
    bool ok = false;
    switch (m.op) {
        case TRACK_OP_SET_MIDI_IN:
            ok = SfzLoader::SetTrackMidiIn(m.track, static_cast<uint8_t>(m.value));
            break;
        case TRACK_OP_SET_POLY_LIMIT:
            ok = SfzLoader::SetTrackPolyLimit(m.track, static_cast<uint8_t>(m.value));
            break;
        case TRACK_OP_SET_PRIORITY:
            ok = SfzLoader::SetTrackPriority(m.track, static_cast<uint8_t>(m.value));
            break;
        case TRACK_OP_SET_PROGRAM_CHANGE:
            ok = SfzLoader::SetTrackProgramChange(m.track, m.value != 0);
            break;
        default:
            break;
    }
    if (!ok && s_hw) {
        // Rejected rather than clamped: a bad op/track/value means the two
        // ends disagree about the model, and silently landing it on Track 0
        // is how that stays hidden until a bench session.
        WaveX::Log::PrintLine("TRACK_OP rejected: op=%u track=%u value=%u",
                              (unsigned)m.op,
                              (unsigned)m.track,
                              (unsigned)m.value);
    }
}

void OnControlChange(const ControlChangeMessage& ctrl_msg) {
    const float norm = static_cast<float>(ctrl_msg.value) / 65535.0f;
    bool para_changed = false;
    bool voice_changed = false;
    switch (ctrl_msg.parameter) {
        case PARAM_FILTER_CUTOFF:
            s_para_pending.cutoff_base = norm;
            para_changed = true;
            // Digital path: the analog side takes `norm` straight through as a
            // CV, but a digital cutoff is a frequency and has to be mapped.
            // Exponential over 20 Hz .. 20 kHz, because pitch perception is
            // logarithmic - a linear map spends most of its travel above
            // 10 kHz, where almost nothing audible happens, and crosses the
            // entire musically useful range in the first few percent.
            s_voice_live_pending.filter_cutoff_hz = 20.0f * std::pow(1000.0f, norm);
            voice_changed = true;
            break;
        case PARAM_PAN:
            // Linear 0..1 across the wire's full range. Voice::pan is applied
            // as a gain pair per block, so this is click-free without smoothing.
            s_voice_live_pending.pan = norm;
            voice_changed = true;
            break;

        case PARAM_PITCH: {
            // +/- 24 semitones around centre. Two octaves each way is enough to
            // play a sample as an instrument without the resampler running so
            // far from unity that the interpolation artefacts dominate.
            constexpr float kPitchRangeSemis = 24.0f;
            s_voice_live_pending.pitch_semitones = (norm * 2.0f - 1.0f) * kPitchRangeSemis;
            voice_changed = true;
            break;
        }

        case PARAM_FILTER_RESONANCE:
            s_para_pending.resonance = norm;
            s_voice_live_pending.filter_resonance = norm;  // svf_filter.hpp maps 0..1 onto Q
            para_changed = true;
            voice_changed = true;
            break;
        case PARAM_ENVELOPE_ATTACK:
        case PARAM_ENVELOPE_DECAY:
        case PARAM_ENVELOPE_SUSTAIN:
        case PARAM_ENVELOPE_RELEASE: {
            // Times span 1 ms .. 2 s; sustain is the raw 0..1 level.
            const float seconds = 0.001f + norm * 2.0f;
            if (ctrl_msg.parameter == PARAM_ENVELOPE_ATTACK) {
                s_para_pending.attack_s = seconds;
                s_voice_live_pending.attack_s = seconds;
            } else if (ctrl_msg.parameter == PARAM_ENVELOPE_DECAY) {
                s_para_pending.decay_s = seconds;
                s_voice_live_pending.decay_s = seconds;
            } else if (ctrl_msg.parameter == PARAM_ENVELOPE_SUSTAIN) {
                s_para_pending.sustain = norm;
                s_voice_live_pending.sustain_level = norm;
            } else {
                s_para_pending.release_s = seconds;
                s_voice_live_pending.release_s = seconds;
            }
            para_changed = true;
            voice_changed = true;
            break;
        }
        default:
            // PARAM_MODULATION_MATRIX (0x0A): retired-but-reserved now that
            // real mod-matrix slots exist (SET_MOD_SLOT, MSG_INST_OP) -
            // param-locks-and-modulation.md's own note that this alias's
            // behavior is deleted in the same commit the real thing lands.
            // env_to_cutoff keeps its compiled-in default (0.8) since Stage A
            // is deferred hardware anyway. PARAM_VOLUME / LFO_*: no Stage A
            // consumer either (the analog VCA is the level control; a global
            // LFO is future work).
            break;
    }
    if (para_changed) {
        s_para_mailbox.Publish(s_para_pending);
    }
    if (voice_changed) {
        s_voice_live_mailbox.Publish(s_voice_live_pending);
    }
}

void SetFilterSelection(const FilterSelection& sel) {
    WaveX::AudioEngine::FilterConfig cfg;
    cfg.topology = sel.topology == 1 ? WaveX::AudioEngine::FilterTopology::DaisySpSvf
                                     : WaveX::AudioEngine::FilterTopology::WaveXSvf;
    cfg.slope = sel.slope_db == 24 ? WaveX::AudioEngine::SvfFilter::Slope::Db24
                                   : WaveX::AudioEngine::SvfFilter::Slope::Db12;
    cfg.drive = sel.drive < 0.0f ? 0.0f : (sel.drive > 1.0f ? 1.0f : sel.drive);
    s_voice_live_pending.filter = cfg;
    s_voice_live_mailbox.Publish(s_voice_live_pending);
}

FilterSelection GetFilterSelection() {
    const WaveX::AudioEngine::FilterConfig& cfg = s_voice_live_pending.filter;
    FilterSelection sel;
    sel.topology = cfg.topology == WaveX::AudioEngine::FilterTopology::DaisySpSvf ? 1 : 0;
    sel.slope_db = cfg.slope == WaveX::AudioEngine::SvfFilter::Slope::Db24 ? 24 : 12;
    sel.drive = cfg.drive;
    return sel;
}

// --- CV calibration workflow (item 5 stage 4; analog-voice-board.md §3).
// All main-loop message-handler context: SetGroupCal publishes the backend's
// complete calibration table for the next control tick; SD I/O is blocking
// FatFS on the main loop.

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
    s_cv_test_pending.group = m.group < WAVEX_ANALOG_CV_GROUPS_MAX ? m.group : 0;
    s_cv_test_pending.cutoff = m.cutoff;
    s_cv_test_pending.resonance = m.resonance;
    s_cv_test_pending.vca = m.vca;
    s_cv_test_pending.active = m.enable != 0;
    s_cv_test_mailbox.Publish(s_cv_test_pending);
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
// Main-loop handlers only enqueue complete records. The callback owns the
// transport and drains them at the start of each audio block, so foreground
// edits and MIDI timing never race audio scheduling.
static void EnqueueSequencerCommand(const WaveX::Sequencer::SequencerCommand& command) {
    if (!s_seq_command_queue.Push(command)) {
        WaveX::Log::PrintLine("SEQ: command queue full; command dropped");
    }
}

void OnSeqTransport(const SeqTransportMessage& m) {
    // Publish before enqueuing PLAY: its first downbeat is due in the same
    // callback that consumes this command, so its immutable sample pointers
    // must be available before Tick() starts the scheduler.
    PublishSequencerVoiceMap();
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::Transport;
    command.transport = m;
    EnqueueSequencerCommand(command);
#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (s_hw)
        WaveX::Log::PrintLine("RX SEQ_TRANSPORT: cmd=%u src=%u bpm=%u",
                              (unsigned)m.command,
                              (unsigned)m.clock_source,
                              (unsigned)m.tempo_bpm_x100);
#endif
}

void OnSeqPatternOp(const SeqPatternOpMessage& m) {
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::PatternOp;
    command.pattern_op = m;
    EnqueueSequencerCommand(command);
}

void OnMidiClockEvent(const MidiClockEventMessage& m) {
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::MidiClock;
    command.midi_clock = m;
    EnqueueSequencerCommand(command);
}

void OnMidiCc(const MidiCcMessage& m) {
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::MidiCc;
    command.midi_cc = m;
    EnqueueSequencerCommand(command);
}

// One Track's share of a note-on. Split out of OnNoteOn() because a MIDI
// note reaches every Track listening on its channel (track-and-patch-model.md
// §2.2) - the per-Track work is the same however the note was addressed.
static void TriggerTrackNoteOn(uint8_t slot, const NoteMessage& note_msg) {
    // A replacement has stopped the old voices and is about to release their
    // sample pointers. Do not queue a trigger resolved against that old table.
    if (SfzLoader::TrackLoading(slot)) {
        return;
    }

    // One resolution path for every kind of instrument (roadmap Phase 2.5
    // item 1): an .sfz import and a bare sample bound with MSG_SAMPLE_SELECT
    // are both Instruments in SfzLoader's bank, differing only in which
    // sample registry their zones' ids index. The bare case is a one-zone
    // Keyboard instrument whose zone inherits the sample's own markers/gain
    // (ResolveLoadedSample) and takes filter/ADSR from the live params - so
    // a note after a knob move still sounds like the sweep the user just
    // heard, and the editor's auditioned region is what a pad plays.
    VoiceTriggerParams params[kMaxLayerTriggers];
    const uint8_t count = SfzLoader::ResolveNote(
        slot, note_msg.note, note_msg.velocity, &s_voice_live_pending, params, kMaxLayerTriggers);
    if (count == 0) {
        // No s_hw guard: this is the one line that explains why the
        // instrument is silent, and gating it behind a pointer that may be
        // null is how a whole bench session went to working out whether
        // notes were even arriving. It runs on the main loop, well after
        // init. Two distinct reasons, named apart because they need
        // different fixes.
        if (!SfzLoader::TrackLoaded(slot)) {
            WaveX::Log::PrintLine(
                "  -> dropped: Track %u has no instrument loaded and no sample bound "
                "(MSG_SAMPLE_SELECT; %u loaded)",
                (unsigned)slot,
                (unsigned)loaded_sample_count());
        } else {
            WaveX::Log::PrintLine(
                "  -> dropped: Track %u has no zone for note=%u vel=%u with a resident sample",
                (unsigned)slot,
                (unsigned)note_msg.note,
                (unsigned)note_msg.velocity);
        }
        return;
    }

    for (uint8_t i = 0; i < count; ++i) {
        NoteEvent event;
        event.is_trigger = true;
        event.note = note_msg.note;
        event.track = slot;
        event.params = params[i];
        if (!s_note_queue.Push(event)) {
            WaveX::Log::PrintLine("RX NOTE_ON: track=%u note=%u layer=%u DROPPED - note queue full",
                                  (unsigned)slot,
                                  (unsigned)note_msg.note,
                                  (unsigned)i);
            break;
        }
    }
#if WAVEX_MCU_LINK_PACKET_DEBUG
    WaveX::Log::PrintLine("RX NOTE_ON: track=%u note=%u vel=%u -> %u layers (%lu frames)",
                          (unsigned)slot,
                          (unsigned)note_msg.note,
                          (unsigned)note_msg.velocity,
                          (unsigned)count,
                          (unsigned long)params[0].sample_frames);
#endif
}

/**
 * Route an incoming note to the Tracks that should hear it (§2.2).
 *
 * Track-addressed (NOTE_ADDR_TRACK set: Play grid, sequencer, arpeggiator)
 * reaches exactly that Track. Channel-addressed (the ESP32 MIDI task, which
 * forwards raw events) reaches every Track whose midi_in matches, which is
 * what makes layering free and Omni mean what it says.
 *
 * Main loop, not the callback: a 16-entry compare in the note handler, and
 * the NoteEvents it queues already name a resolved Track.
 */
static uint8_t RouteNote(const NoteMessage& note_msg, uint8_t* tracks, uint8_t max) {
    if (NoteAddressesTrack(note_msg.channel)) {
        if (max == 0)
            return 0;
        tracks[0] = NoteAddressIndex(note_msg.channel);
        return 1;
    }
    return SfzLoader::TracksForMidiChannel(NoteAddressIndex(note_msg.channel), tracks, max);
}

void OnNoteOn(const NoteMessage& note_msg) {
    uint8_t tracks[kNumTracks];
    const uint8_t n = RouteNote(note_msg, tracks, kNumTracks);
    for (uint8_t i = 0; i < n; ++i) {
        TriggerTrackNoteOn(tracks[i], note_msg);
    }
#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (n == 0 && s_hw) {
        WaveX::Log::PrintLine(
            "RX NOTE_ON: ch=%u reached no Track (all midi_in Off or set elsewhere)",
            (unsigned)NoteAddressIndex(note_msg.channel));
    }
#endif
}

// One Track's share of a note-off. Routed identically to note-on, so the
// three Tracks a layered note-on reached are the three it releases.
static void ReleaseTrackNoteOff(uint8_t slot, const NoteMessage& note_msg) {
    NoteEvent ev;
    ev.is_trigger = false;
    ev.note = note_msg.note;
    ev.track = slot;
    ev.scoped_release = SfzLoader::TrackLoaded(slot);
    const bool queued =
        ev.scoped_release ? s_note_queue.Push(ev) : s_note_queue.PushReleaseOrRemember(ev);
    if (!queued && ev.scoped_release) {
        const uint32_t word = static_cast<uint32_t>(note_msg.note) / 32u;
        const uint32_t bit = 1u << (static_cast<uint32_t>(note_msg.note) % 32u);
        __atomic_fetch_or(&s_scoped_release_overflow[slot][word], bit, __ATOMIC_RELEASE);
        __atomic_fetch_or(
            &s_scoped_release_pending_slots, 1u << static_cast<uint32_t>(slot), __ATOMIC_RELEASE);
    }
    if (!queued && s_hw) {
        WaveX::Log::PrintLine("RX NOTE_OFF: note=%u queue full - release preserved",
                              (unsigned)note_msg.note);
    }

#if WAVEX_MCU_LINK_PACKET_DEBUG
    if (s_hw)
        WaveX::Log::PrintLine(
            "RX NOTE_OFF: note=%u ch=%u", (unsigned)note_msg.note, (unsigned)note_msg.channel);
#endif
}

void OnNoteOff(const NoteMessage& note_msg) {
    uint8_t tracks[kNumTracks];
    const uint8_t n = RouteNote(note_msg, tracks, kNumTracks);
    for (uint8_t i = 0; i < n; ++i) {
        ReleaseTrackNoteOff(tracks[i], note_msg);
    }
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
    const LoadedSampleInfo* newest = newest_loaded_sample();
    if (!newest) {
        if (s_hw) {
            WaveX::Log::PrintLine("PREVIEW: No loaded samples; skipping preview");
        }
        return;
    }

    const LoadedSampleInfo& src = *newest;
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
    if (!info && req.sample_id == 0) {
        info = newest_loaded_sample();
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

bool LoadSfzInstrument(const char* path, uint8_t slot) {
    if (!s_pool) {
        return false;
    }
    return SfzLoader::Load(path, slot, *s_pool, s_sample_mem_mgr, s_sample_io, sizeof(s_sample_io));
}

void OnInstrumentOp(const InstOpMessage& request) {
    if (request.op == INST_OP_SET_MOD_SLOT) {
        // Not a probe/load request - Begin()'s state machine (below) would
        // reject it as a malformed one (empty path, unrecognised op) and send
        // a spurious INST_STATUS_FAILED back to the ESP32. Handle it and
        // return before Begin() ever sees it.
        ModSlot slot;
        slot.source = request.mod_source;
        slot.dest = request.mod_dest;
        slot.depth = request.mod_depth;
        slot.curve = request.mod_curve;
        slot.flags = request.mod_flags;
        SfzLoader::SetModSlot(request.slot, request.mod_slot_index, slot);
        return;
    }
    if (SfzLoader::Begin(request) && request.op == INST_OP_SFZ_LOAD) {
        if (request.slot == kSequencerPreviewTrack) {
            ClearSequencerVoiceMap();
        }
        // Streaming audition and instrument import share FatFs/SD bandwidth.
        // A load owns storage until its cooperative state machine completes.
        CloseWav();
    }
}

void PumpInstrumentLoad() {
    if (!s_pool) {
        return;
    }
    static bool stop_requested = false;
    const uint8_t stop_track = SfzLoader::VoiceStopTrack();
    if (stop_track != 0xFF) {
        // Per-track barrier: only the Track being (re)loaded stops; what it
        // held is released on the callback's acknowledgement.
        if (!stop_requested) {
            __atomic_store_n(&s_voice_stop_ack, false, __ATOMIC_RELAXED);
            __atomic_fetch_or(
                &s_voice_stop_tracks, static_cast<uint16_t>(1u << stop_track), __ATOMIC_RELEASE);
            stop_requested = true;
            return;
        }
        if (__atomic_exchange_n(&s_voice_stop_ack, false, __ATOMIC_ACQUIRE)) {
            SfzLoader::ConfirmVoicesStopped(*s_pool, s_sample_mem_mgr);
            stop_requested = false;
        }
        return;
    }
    stop_requested = false;
    const bool preview_was_loading = SfzLoader::TrackLoading(kSequencerPreviewTrack);
    const bool was_busy = SfzLoader::Busy();
    SfzLoader::Pump(*s_pool, s_sample_mem_mgr, s_sample_io, sizeof(s_sample_io));
    // The loader publishes its new slot binding only when its state machine
    // reaches Idle. Rebuild the callback-owned snapshot at that transition;
    // rebuilding during the load would expose incomplete sample pointers.
    if (preview_was_loading && !SfzLoader::TrackLoading(kSequencerPreviewTrack)) {
        PublishSequencerVoiceMap();
    }
    // An import finishing (or failing) changes what its Track holds; push
    // every binding so the frontend's cache follows without asking.
    if (was_busy && !SfzLoader::Busy()) {
        PushTrackBinding(0xFF);
    }
}

// Tells the frontend a MSG_SAMPLE_LOAD is not going to complete, and why. Every
// early return in OnSampleLoad() goes through here: a load that fails silently
// leaves the frontend's spinner to time out with nothing to say.
static void ReportSampleLoadFailed(uint16_t sample_id, SampleLoadFailReason reason) {
    SampleStatusMessage status{};
    status.sample_id = sample_id;
    status.state = SAMPLE_STATUS_LOAD_FAILED;
    status.frames_played = reason;
    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_STATUS, &status, sizeof(status));
}

void OnSampleLoad(const SampleLoadMessage& sl) {
    if (!s_sample_memory_available) {
        if (s_hw)
            WaveX::Log::PrintLine("SAMPLE_LOAD: rejected because SDRAM is unavailable");
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_NO_SDRAM);
        return;
    }
    if (s_hw) {
        WaveX::Log::PrintLine("SAMPLE_LOAD: path='%s' request=%u", sl.path, (unsigned)sl.sample_id);
    }
    // The Pool is refcounted by path: a file that is already resident is a
    // hit, not a second copy. The user's explicit load pins it, and the
    // frontend hears the id it already had.
    const uint32_t path_hash = WaveX::Audio::HashSamplePath(sl.path);
    if (SamplePool::Record* hit = s_pool->FindByPath(path_hash)) {
        s_pool->SetPinned(hit->sample_id, true);
        s_pool->NoteNewest(hit->sample_id);
        PushSampleMeta(hit->payload);
        SampleStatusMessage status{};
        status.sample_id = hit->sample_id;
        status.state = SAMPLE_STATUS_LOAD_COMPLETE;
        status.channels = hit->payload.channels;
        status.sample_rate = hit->payload.sample_rate;
        status.frames_played = hit->payload.meta.total_frames;
        WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_STATUS, &status, sizeof(status));
        if (s_hw) {
            WaveX::Log::PrintLine(
                "SAMPLE_LOAD: '%s' already resident as id=%u", sl.path, (unsigned)hit->sample_id);
        }
        return;
    }

    // CRITICAL: Stop ALL SD activity (playback) and ensure PumpWavIO is not running.
    // FatFS + SDMMC are NOT thread-safe or re-entrant. The main loop calls PumpWavIO() which
    // will conflict with f_open/f_read calls here if s_wav.open is true.
    CloseWav();

    // No voice-stop barrier here any more: a load into the Pool frees and
    // rewrites nothing (admission fails rather than evicting), so nothing a
    // sounding voice reads is touched. Loading no longer cuts notes off.
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
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_OPEN);
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
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_FORMAT);
        return;
    }
    ResidentSampleInfo resident;
    if (!BuildResidentSampleInfo(sl,
                                 wav_info,
                                 static_cast<uint32_t>(f_size(&file)),
                                 WaveX::SdramLayout::kLargeSamplePoolBytes,
                                 resident)) {
        if (s_hw) {
            WaveX::Log::PrintLine(
                "SAMPLE_LOAD: invalid or unsupported resident WAV rate=%lu bits=%u ch=%u "
                "data_off=%lu data_size=%lu file_size=%lu resident_max=%lu",
                (unsigned long)wav_info.sample_rate,
                (unsigned)wav_info.bits_per_sample,
                (unsigned)wav_info.num_channels,
                (unsigned long)wav_info.data_offset,
                (unsigned long)wav_info.data_size,
                (unsigned long)f_size(&file),
                (unsigned long)WaveX::SdramLayout::kLargeSamplePoolBytes);
        }
        f_close(&file);
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_FORMAT);
        return;
    }

    const uint16_t num_ch = resident.channels;
    const uint32_t sample_rate = resident.sample_rate;
    const uint16_t bits = resident.bit_depth;
    const uint32_t data_off = wav_info.data_offset;
    const uint32_t data_size = resident.data_size;

    // Admission: an entry and the bytes, or a reason. Nothing is evicted to
    // make room - the user unloads; the engine never guesses (§4).
    SamplePool::Record* record = nullptr;
    if (s_pool->AdmitPath(path_hash, &record) != SamplePool::Admit::Ok) {
        if (s_hw) {
            WaveX::Log::PrintLine("SAMPLE_LOAD: pool full (%u entries)",
                                  (unsigned)WAVEX_SAMPLE_POOL_CAPACITY);
        }
        f_close(&file);
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_REGISTRY_FULL);
        return;
    }
    const uint16_t sample_id = record->sample_id;
    wxsamp_t handle = {};
    if (!s_sample_mem_mgr.alloc(data_size, &handle)) {
        if (s_hw) {
            wxsamp_stats_t st{};
            s_sample_mem_mgr.stats(&st);
            WaveX::Log::PrintLine(
                "SAMPLE_LOAD: alloc failed for %lu bytes (largest_free=%lu, free_total=%lu)",
                (unsigned long)data_size,
                (unsigned long)st.largest_free_bytes,
                (unsigned long)st.large_free_bytes + (unsigned long)st.small_free_bytes);
        }
        s_pool->Remove(sample_id);
        f_close(&file);
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_RAM);
        return;
    }

    void* sample_ptr = nullptr;
    if (!s_sample_mem_mgr.ptr(handle, &sample_ptr)) {
        s_sample_mem_mgr.release(&handle);
        s_pool->Remove(sample_id);
        f_close(&file);
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_RAM);
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
            s_pool->Remove(sample_id);
            f_close(&file);
            ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_READ);
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
                progress.sample_id = sample_id;
                progress.state = SAMPLE_STATUS_LOAD_PROGRESS;  // frames_played = percent
                progress.channels = static_cast<uint8_t>(num_ch);
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

    CancelEnvelopeJob();
    FillLoadedSample(record->payload, sample_id, sl.path, resident, handle);
    // The user asked for it by name: only an explicit unload releases it.
    s_pool->SetPinned(sample_id, true);
    s_pool->NoteNewest(sample_id);
    PushSampleMeta(record->payload);

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
            "SAMPLE_LOAD: Loaded %lu bytes as sample %u in %lu ms "
            "(%lu KB/s, %u B reads, %u B clusters)",
            (unsigned long)data_size,
            (unsigned)sample_id,
            (unsigned long)elapsed_ms,
            kbps,
            (unsigned)kIoChunk,
            cluster_bytes);
    }

    // Notify host (ESP32) that sample load completed - with the Pool's id,
    // which is the one every later message must use.
    SampleStatusMessage status{};
    status.sample_id = sample_id;
    status.state = SAMPLE_STATUS_LOAD_COMPLETE;
    status.channels = static_cast<uint8_t>(num_ch);
    status.sample_rate = sample_rate;
    status.frames_played = data_size / ((bits / 8) * num_ch);  // total frames loaded
    WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_SAMPLE_STATUS, &status, sizeof(status));
}

void GetSampleMemStatus(SampleMemStatusMessage& out) {
    // Deliberate byte-wise zero of a wire struct: the default constructor
    // does not clear entries[], and every byte of sizeof(out) goes on the
    // UART. void* cast acknowledges the non-trivial type for GCC.
    memset(static_cast<void*>(&out), 0, sizeof(out));
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

    SamplePool::Record* page[WAVEX_SAMPLE_STATUS_MAX_ENTRIES];
    size_t total = 0;
    const size_t count =
        s_pool ? s_pool->Page(0, WAVEX_SAMPLE_STATUS_MAX_ENTRIES, page, &total) : 0;
    out.sample_count = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& src = page[i]->payload;
        auto& dst = out.entries[i];
        dst.sample_id = src.sample_id;
        dst.allocated_bytes = src.allocated_bytes;
        dst.loaded_bytes = src.loaded_bytes;
        dst.cls = src.handle.cls;
        dst.page = src.handle.page;
        dst.slot = src.handle.slot;
        // This legacy diagnostic field is only 16 bits on the wire. Keep
        // high-rate samples authoritative in SampleMetadata instead of
        // wrapping (for example, 96 kHz to 30464 Hz).
        dst.sample_rate =
            src.sample_rate <= UINT16_MAX ? static_cast<uint16_t>(src.sample_rate) : 0;
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

#if WAVEX_DEBUG_HARNESS_ENABLED
uint32_t DebugUnderruns() {
    return s_diag_underruns;
}
#endif

// Main-loop only: performs the blocking CV DAC transaction (~225 us
// MCP4728 fast-write) for values staged at the control tick - never in the
// callback (§7.1.4 / analog-voice-board.md §0 timing rules). If the DAC is
// absent (bench without the Stage A breadboard) or wedged, eight consecutive
// I2C failures pause the flush so the ~1 ms transaction timeout is not paid
// forever; a periodic long-backoff retry (rather than a permanent latch)
// means a transient wedge on real hardware recovers on its own instead of
// requiring a reboot to get the analog path back.
void FlushCv() {
#if !WAVEX_ANALOG_CV_ENABLED
    // Analog CV is compiled out (hardware_config.h). Returning here rather
    // than removing the main-loop call site keeps the caller unconditional and
    // stops the I2C retry/backoff machinery below from talking to a DAC that
    // is not fitted - which it would otherwise do forever, failing.
    return;
#else
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
#endif
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

    // Validate semantic geometry as well as the RIFF chunk walk. In
    // particular, a zero sample rate previously reached the resampler as an
    // infinite ratio and left audition permanently open but unable to pump.
    if (!ValidatePcmWavPayload(wav_info, static_cast<uint32_t>(f_size(&s_wav.file)), 0xFFFFFFFFu)) {
        if (s_hw)
            WaveX::Log::PrintLine(
                "WAV open failed: invalid geometry fmt=%u rate=%lu bits=%u ch=%u "
                "data_off=%lu data_size=%lu file_size=%lu",
                (unsigned)wav_info.audio_format,
                (unsigned long)wav_info.sample_rate,
                (unsigned)wav_info.bits_per_sample,
                (unsigned)wav_info.num_channels,
                (unsigned long)wav_info.data_offset,
                (unsigned long)wav_info.data_size,
                (unsigned long)f_size(&s_wav.file));
        f_close(&s_wav.file);
        return false;
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
    wav_channels = static_cast<uint8_t>(s_wav.num_channels);
    wav_bits = static_cast<uint8_t>(s_wav.bits_per_sample);
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
        (static_cast<float>(s_wav.sample_rate) != s_sample_rate)
            ? static_cast<float>(s_sample_rate) / static_cast<float>(s_wav.sample_rate)
            : 1.0f;

    // A 1-frame slot tail cannot be resampled: ResampleStreamInterleaved() needs
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
    // the pool together: conversion (f x out_ch) + ResampleStreamInterleaved's
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
    ConvertFramesToOutput(src,
                          conversion_output,
                          frames_to_transfer,
                          s_wav.num_channels,
                          static_cast<uint8_t>(s_wav.bits_per_sample));
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
        uint32_t max_out_frames = static_cast<uint32_t>(std::ceil(
                                      static_cast<float>(frames_to_transfer) * resample_ratio)) +
                                  1;
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
    if (!info) {
        info = newest_loaded_sample();
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
        const uint32_t rate = m.sample_rate ? m.sample_rate : static_cast<uint32_t>(s_sample_rate);
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
    const uint32_t file_rate =
        s_wav.sample_rate ? s_wav.sample_rate : static_cast<uint32_t>(s_sample_rate);
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
