#include "comm/log_ring.h"

#include "../config.hpp"
#include "audio/global_lfo.hpp"
#include "audio/master_gain.hpp"
#include "audio/mix_meter_window.hpp"
#include "audio/parameter_locks.hpp"
#include "audio/recording_session.hpp"
#include "audio/sample_channels.hpp"
#include "audio/sample_gain.hpp"
#include "audio/sample_loop.hpp"
#include "storage/sample_load_job.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include <daisy.h>  // For CpuLoadMeter

extern "C" SD_HandleTypeDef hsd1;  // libDaisy per/sdmmc.cpp

#include "../memory.h"
#include "../memory_sections.h"  // For WAVEX_DTCM_DATA
#include "../sdram_layout.h"
#include "audio_engine.h"
#include "comm/mcu_link.h"
#include "config/hardware_config.h"
#include "config/link_config.h"
#include "daisy_core.h"  // For memory sections
#include "ff.h"
#include "profiling/profiler.h"
#include "stm32h7xx_ll_cortex.h"  // For ARM atomic operations
#include "storage/sd_sdio.h"
#include "sys/dma.h"  // For cache management

#include "../bss_static.hpp"
#include "../cv/cv_cal_store.hpp"
#include "../cv/cv_group_router.hpp"
#include "../sequencer/sequencer_transport.hpp"
#include "../storage/fatfs_wav_reader.hpp"
#include "../timebase.hpp"
#include "arp_runtime.hpp"
#include "audio/sample_pool.hpp"
#include "callback_stop_fence.hpp"
#include "envelope_scan.hpp"
#include "fade.hpp"
#include "instrument.hpp"
#include "lfo.hpp"
#include "linear_resampler.hpp"
#include "live_note_runtime.hpp"
#include "mixer_control_handoff.hpp"
#include "mod_matrix.hpp"
#include "output_sink.hpp"
#include "paraphonic_envelope.hpp"
#include "playback_cursor.hpp"
#include "profiling/callback_detail.hpp"
#include "sample_load_info.hpp"
#include "sequencer/pattern_exchange.hpp"
#include "sequencer/sequencer_command_queue.hpp"
#include "sequencer_voice_map.hpp"
#include "sfz_loader.hpp"
#include "snapshot_mailbox.hpp"
#include "storage/bank_session.hpp"
#include "storage/card_service.hpp"
#include "storage/pattern_store.hpp"
#include "storage/project_session.hpp"
#include "storage/sample_file_job.hpp"
#include "track_live_updates.hpp"
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
PROFILE_DEFINE_ZONE(voice_events);
PROFILE_DEFINE_ZONE(voice_modulation);
PROFILE_DEFINE_ZONE(voice_render);
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
// object and both sinks stay in the build; see docs/roadmap.md.
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
static MixerControlHandoff s_mixer_controls;

static MasterGain s_master_gain;
// Foreground owns the lease, callback owns the window; snapshots cross back.
static MixMeterSubscription s_mix_meter_subscription;
static std::atomic<bool> s_mix_meters_active{false};
static MixMeterWindow s_mix_meter_window;
static bool s_callback_metering = false;
static uint32_t s_mix_meter_sequence = 0, s_mix_meter_sent = 0;
static SnapshotMailbox<MixMeterSnapshot> s_mix_meter_mailbox;
static MixMeterSnapshot s_mix_meter_main;

// Main-loop instrument/extras edits publish complete per-Track values. The
// callback alone applies them to sounding voices; different Tracks retain
// independent pending updates.
static TrackLiveUpdates s_track_live_updates;
static MidiModulation s_midi_modulation;

// Per-Track staging for the live values the Instrument does NOT own yet.
// Filter and envelope moved onto Instrument in stage 4 and the filter
// topology, slope and drive followed; pan and pitch become its
// trim_pan/transpose in stage 5, so those two stay here until then.
// Deliberately not a second copy of anything Instrument owns - one field,
// one owner.
struct TrackLiveExtras {
    float pan = 0.5f;
    float pitch_semitones = 0.0f;
};
static TrackLiveExtras s_track_live[WaveX::AudioEngine::kNumTracks];

// The message the callback applies, composed from the Instrument (the store)
// plus the extras above. Nothing else may build a VoiceLiveParams for a
// Track: composing in one place is what keeps the Instrument authoritative.
static WaveX::AudioEngine::VoiceLiveParams ComposeTrackLive(
    uint8_t track,
    const WaveX::AudioEngine::InstrumentFilter& filter,
    const WaveX::AudioEngine::InstrumentEnv& env) {
    WaveX::AudioEngine::VoiceLiveParams live;
    const TrackLiveExtras& extras =
        s_track_live[track < WaveX::AudioEngine::kNumTracks ? track : 0];
    live.track = track;
    live.filter_cutoff_hz = filter.cutoff_hz;
    live.filter_resonance = filter.resonance;
    live.attack_s = env.attack_s;
    live.decay_s = env.decay_s;
    live.sustain_level = env.sustain;
    live.release_s = env.release_s;
    WaveX::AudioEngine::SfzLoader::PrepareLiveParams(track, live);
    live.pan = extras.pan;
    live.pitch_semitones = extras.pitch_semitones;
    return live;
}

// The callback resolves each voice's own Track through a callback-private
// modulation snapshot. SfzLoader publishes complete tables from its main-loop
// Instrument bank, so slot edits and instrument commits cannot tear a route.
static const WaveX::AudioEngine::ModSlot* ResolveModSlots(const void*, uint8_t slot) {
    return SfzLoader::GetModSlots(slot);
}

// One performance-owned global LFO. The other two LFOs belong to each
// Instrument voice. Foreground settings and callback phase have separate owners.
static WaveX::AudioEngine::GlobalLfo s_global_lfo;
static SnapshotMailbox<SeqLockNoticeMessage> s_lock_notice;

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
// Callback publishes the Pattern identity for foreground motion-capture commands.
static std::atomic<uint32_t> s_seq_capture_epoch{0};
static constexpr uint32_t kSequencerCommandQueueSize = 32;
static WaveX::Sequencer::SequencerCommandQueue<kSequencerCommandQueueSize> s_seq_command_queue;

// Callback produces complete readbacks; foreground alone serializes them.
static SnapshotMailbox<SeqNotesMessage> s_seq_notes_mailbox;
struct SeqPageSnapshot {
    SeqSlotPageMessage value;
    bool scoped = false;
};
static WaveX::BssStatic<SnapshotMailbox<SeqPageSnapshot>> s_seq_page_mailbox_storage;
static auto& s_seq_page_mailbox = s_seq_page_mailbox_storage.Get();
static WaveX::BssStatic<SnapshotMailbox<SeqPlayheadMessage>> s_seq_head_mailbox_storage;
static auto& s_seq_head_mailbox = s_seq_head_mailbox_storage.Get();
static WaveX::BssStatic<SeqPageSnapshot> s_seq_page_pending_storage;
static WaveX::BssStatic<SeqPlayheadMessage> s_seq_head_pending_storage;
static bool s_seq_page_pending = false;
static bool s_seq_head_pending = false;
static uint32_t s_seq_telemetry_frames = 0;
static uint32_t s_seq_telemetry_interval = 1;

// Pattern row N addresses Track N's Instrument. Resolution stays on the
// main loop; the callback only reads complete prepared bindings.
// One fixed allocation from the existing SDRAM allocator at startup. Its
// lifetime is the engine's; sample reset releases Pool records only.
// Larger snapshots exchange ownership without a callback-sized bulk copy.
struct SequencerVoiceState {
    SequencerVoiceMap pending;
    SnapshotMailbox<SequencerVoiceMap> mailbox;
};
static SequencerVoiceState* s_seq_voices = nullptr;
// One AXI SRAM buffer whose ownership crosses only on release/acquire handoff.
static WaveX::BssStatic<WaveX::Sequencer::PatternExchange> s_pattern_exchange_storage
    WAVEX_BACKGROUND_DATA;

static wxsamp_t s_seq_voice_storage{};

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

// Explicit initialization before audio starts; CPU-only, no DMA access.
static LiveNoteRuntime s_live_notes WAVEX_DTCM_DATA;
// Foreground-owned received MIDI events; UI Track notes are excluded.
static uint32_t s_midi_notes = 0, s_midi_ccs = 0, s_midi_clocks = 0;
// CPU-only SRAM state; preserve the linker-enforced DTCM stack reserve.
static BssStatic<ArpRuntime> s_arps;
static void RecordArp(
    uint8_t source, uint8_t note, uint32_t serial, uint8_t velocity, uint16_t tracks) {
    s_seq_transport.RecordInput(source, note, serial, velocity, tracks);
}

// One main-loop producer and one callback consumer. Select/unload and the
// cooperative Instrument loader await their own generation before releasing
// sample storage; elapsed time alone never grants ownership of that memory.
static CallbackStopFence s_voice_stop_fence;

static bool StopTracksAndWait(uint16_t tracks) {
    s_midi_modulation.Reset(tracks);
    const uint32_t generation = s_voice_stop_fence.RequestStop(tracks);
    const uint32_t started = System::GetNow();
    while (!s_voice_stop_fence.Complete(generation)) {
        if (System::GetNow() - started >= 10u) {
            return false;
        }
        System::Delay(1);
    }
    return true;
}

// Audio-callback side: apply every pending note event, then honor a
// pending hard-stop. Order matters - a stop request must also kill
// triggers queued before it (they reference the memory being released).
// Returns true if any trigger was applied - the paraphonic envelope's
// note-on edge (item 5).
static bool drain_note_queue() {
    if (s_seq_voices)
        s_seq_voices->mailbox.AcquireLatest();
    s_arps.Get().Sync(s_seq_voices ? &s_seq_voices->mailbox.ConsumerValue() : nullptr,
                      s_voice_manager,
                      RecordArp);
    const bool triggered = s_live_notes.Drain(
        s_seq_voices ? &s_seq_voices->mailbox.ConsumerValue() : nullptr,
        s_voice_manager,
        [](LiveNoteEvent& event) {
            s_arps.Get().Input(event);
            s_seq_transport.RecordInput(
                event.id.source, event.id.note, event.id.serial, event.velocity, event.tracks);
        });
    if (s_seq_transport.InputMode())
        s_seq_transport.PruneRecording([](uint8_t source, uint8_t note, uint32_t serial) {
            return source < 32 && s_live_notes.OverflowReleased({serial, source, note});
        });
    s_arps.Get().Prune([](LiveNoteId id) { return s_live_notes.OverflowReleased(id); });
    s_voice_stop_fence.ConsumeAndStop([](uint16_t tracks) {
        s_live_notes.StopTracks(tracks, s_voice_manager);
        s_arps.Get().Stop(tracks, s_voice_manager, RecordArp);
    });
    return triggered;
}

// Callback-only. drain_note_queue has already acquired the latest voice map.
// The main loop publishes that map before enqueuing PLAY, so step 0 sees the
// complete matching binding.
template <typename Render, typename Controls>
static WAVEX_ITCM_CODE_NAMED("sequencer") bool drain_sequencer(uint16_t block_size,
                                                               Render render,
                                                               Controls controls) {
    SeqPatternRequestMessage read_request;
    bool read_requested = false, scoped_read = false, notes_requested = false;
    SeqPatternRequestMessage notes_request;
    WaveX::Sequencer::SequencerCommand command;
    {
        CALLBACK_DETAIL_SCOPE(SeqCommands);
        while (s_seq_command_queue.Pop(command)) {
            switch (command.type) {
                case WaveX::Sequencer::SequencerCommandType::StopOnly:
                    s_seq_transport.StopForProject();
                    break;
                case WaveX::Sequencer::SequencerCommandType::Transport:
                    s_seq_transport.ApplyTransport(command.transport);
                    break;
                case WaveX::Sequencer::SequencerCommandType::PatternOp:
                    s_seq_transport.ApplyPatternOp(command.pattern_op);
                    break;
                case WaveX::Sequencer::SequencerCommandType::SlotEdit:
                    s_seq_transport.ApplySlotEdit(command.slot_edit);
                    break;
                case WaveX::Sequencer::SequencerCommandType::NotesRequest:
                    notes_request = command.pattern_request;
                    notes_requested = true;
                    break;
                case WaveX::Sequencer::SequencerCommandType::SlotPage:
                    read_request = command.pattern_request;
                    read_requested = scoped_read = true;
                    break;
                case WaveX::Sequencer::SequencerCommandType::PatternRequest:
                    scoped_read = false;
                    read_request = command.pattern_request;
                    read_requested = true;
                    break;
                case WaveX::Sequencer::SequencerCommandType::RecordControl:
                    s_seq_transport.RecordControl(command.control, command.pattern_epoch);
                    break;
                case WaveX::Sequencer::SequencerCommandType::MidiClock:
                    s_seq_transport.OnMidiClock(command.midi_clock);
                    break;
            }
        }
        if (notes_requested) {
            SeqNotesMessage reply;
            s_seq_transport.BuildNotes(notes_request, reply);
            s_seq_notes_mailbox.Publish(reply);
        }
        // At most one bounded page copy per callback, regardless of request bursts.
        if (read_requested) {
            SeqPageSnapshot page;
            page.scoped = scoped_read;
            s_seq_transport.BuildSlotPage(read_request, page.value);
            s_seq_page_mailbox.Publish(page);
        }
    }
    const uint64_t block_start_frame = s_seq_transport.scheduler().CurrentFrame();
    static uint32_t run_epoch = 0, pattern_epoch = 0;
    const auto current_run = s_seq_transport.scheduler().RunEpoch();
    if (run_epoch != current_run || pattern_epoch != s_seq_transport.PatternEpoch() ||
        !s_seq_transport.IsPlaying()) {
        s_voice_manager.EndSequence();
        run_epoch = current_run;
        pattern_epoch = s_seq_transport.PatternEpoch();
    }
    uint16_t muted = 0;
    const auto& playing_pattern = s_seq_transport.PlaybackPattern();
    for (uint8_t t = 0; t < 16; ++t)
        if (!playing_pattern.tracks[t].enabled || !playing_pattern.tracks[t].melodic)
            muted |= static_cast<uint16_t>(1u << t);
    s_voice_manager.EndSequence(muted);

    constexpr size_t kPlaybackEvents = WaveX::Sequencer::kMaxEventsPerTick + kNumTracks;
    WaveX::Sequencer::TriggerEvent events[kPlaybackEvents];
    const double arp_start_tick = s_seq_transport.scheduler().PositionTicks();
    const auto boundary_frame = s_seq_transport.scheduler().HasQueuedPattern()
                                    ? s_seq_transport.scheduler().QueuedBoundaryFrame()
                                    : block_start_frame;
    uint16_t cleanup_offset = UINT16_MAX;
    size_t event_count;
    {
        CALLBACK_DETAIL_SCOPE(SeqTick);
        event_count = s_seq_transport.Tick(events, WaveX::Sequencer::kMaxEventsPerTick);
        if (s_pattern_exchange_storage.Get().Process(s_seq_transport, event_count == 0))
            event_count = 0;  // a validated replacement discards this block's old-pattern triggers
        if (pattern_epoch != s_seq_transport.PatternEpoch() ||
            run_epoch != s_seq_transport.scheduler().RunEpoch()) {
            cleanup_offset =
                static_cast<uint16_t>(boundary_frame > block_start_frame &&
                                              boundary_frame < block_start_frame + block_size
                                          ? boundary_frame - block_start_frame
                                          : 0);
            s_voice_manager.EndSequence(0xffff, cleanup_offset);
            pattern_epoch = s_seq_transport.PatternEpoch();
            run_epoch = s_seq_transport.scheduler().RunEpoch();
        }
        s_seq_telemetry_frames += block_size;
        if (s_seq_telemetry_frames >= s_seq_telemetry_interval) {
            s_seq_telemetry_frames = 0;
            s_seq_head_mailbox.Publish(s_seq_transport.BuildPlayhead());
        }
        s_voice_manager.SetTempo(s_seq_transport.scheduler().Tempo());
    }
    const auto arp_count = s_arps.Get().Events(s_seq_transport.scheduler(),
                                               block_start_frame,
                                               arp_start_tick,
                                               block_size,
                                               s_seq_transport.IsPlaying(),
                                               current_run,
                                               events + event_count,
                                               s_voice_manager,
                                               RecordArp);
    event_count += arp_count;
    if (arp_count)
        std::sort(events, events + event_count, [](const auto& a, const auto& b) {
            return a.frame < b.frame || (a.frame == b.frame && a.track < b.track);
        });
    static uint32_t lock_notice_count = 0;
    const auto& notice = s_seq_transport.LockNotice();
    if (notice.count != lock_notice_count) {
        lock_notice_count = notice.count;
        s_lock_notice.Publish(notice);
    }
    controls(event_count != 0);
    s_seq_capture_epoch.store(s_seq_transport.PatternEpoch(), std::memory_order_release);
    bool any_trigger = false;
    if (!s_seq_voices) {
        render(0, block_size, block_start_frame);
        return false;
    }
    const auto& voice_map = s_seq_voices->mailbox.ConsumerValue();
    static_assert(WaveX::Sequencer::kMaxEventsPerTick <= 64);
    SequencerVoiceMap::Selection selected[kPlaybackEvents];
    uint16_t rendered = 0;
    for (size_t begin = 0; begin < event_count;) {
        size_t end = begin + 1;
        while (end < event_count && events[end].frame == events[begin].frame)
            ++end;
        const uint64_t frame = events[begin].frame;
        if (frame >= block_start_frame && frame - block_start_frame < block_size) {
            const auto offset = static_cast<uint16_t>(frame - block_start_frame);
            if (offset > rendered)
                render(rendered, offset - rendered, block_start_frame + rendered);
            rendered = offset;
            for (size_t i = begin; i < end; ++i)
                if (events[i].lane != 0xff)
                    s_voice_manager.EndSequenceLane(events[i].track, events[i].lane, 0);
            const auto admitted = s_voice_manager.TriggerBatch(
                static_cast<uint16_t>(end - begin),
                [&](uint16_t request) {
                    CALLBACK_DETAIL_SCOPE(SeqResolve);
                    const auto& event = events[begin + request];
                    selected[request] = voice_map.Select(event.track, event.note, event.velocity);
                    return voice_map.Describe(selected[request]);
                },
                [&](uint16_t request, uint8_t layer, VoiceTriggerParams& params) {
                    const auto& event = events[begin + request];
                    {
                        CALLBACK_DETAIL_SCOPE(SeqResolve);
                        voice_map.Materialize(selected[request], layer, params);
                    }
                    {
                        CALLBACK_DETAIL_SCOPE(SeqLocks);
                        ApplyParamLocks(params, event.param_locks, event.param_lock_count);
                        params.start_offset_frames = 0;
                        params.sequence_lane = event.lane;
                        if (!event.arp && cleanup_offset != UINT16_MAX && offset < cleanup_offset)
                            params.sequence_release_offset =
                                static_cast<uint16_t>(cleanup_offset - offset);
                        params.sequence_gate_tick =
                            !event.arp && event.gate_ticks ? event.tick + event.gate_ticks : 0;
                    }
                },
                [&](uint16_t request, uint64_t group) {
                    const auto& event = events[begin + request];
                    if (event.arp)
                        s_arps.Get().Admit(event, group, s_voice_manager, RecordArp);
                });
            any_trigger = admitted != 0 || any_trigger;
        }
        begin = end;
    }
    if (rendered < block_size)
        render(rendered, block_size - rendered, block_start_frame + rendered);
    return any_trigger;
}

// Sample RAM Manager (for loaded samples)
static SampleMemMgr s_sample_mem_mgr;

// The four statics below are all written every audio block from Callback()
// itself (CPU-only, never DMA'd) and are individually tiny - DTCM per the
// same reasoning as s_voice_manager/s_para_env above, extended to the
// per-block performance-stat counters rather than just DSP state.
static BlockMeters s_last_block_meters WAVEX_DTCM_DATA = {0, 0, 0, 0};
struct CallbackTelemetry {
    BlockMeters meters{};
    float cpu_avg = 0;
    float cpu_min = 0;
    float cpu_max = 0;
    uint32_t blocks = 0;
    uint32_t cycles = 0;
};
static SnapshotMailbox<CallbackTelemetry> s_telemetry_mailbox;
static CallbackTelemetry s_telemetry_main;

// Main-loop only: callback publication never modifies this consumer's copy.
static const CallbackTelemetry& ReadCallbackTelemetry() {
    s_telemetry_mailbox.ConsumeLatest(s_telemetry_main);
    return s_telemetry_main;
}

// CPU Load Meter for audio processing performance monitoring
static CpuLoadMeter s_cpu_load_meter WAVEX_DTCM_DATA;
// Counts audio callbacks. The callback is driven by SAI DMA interrupts, not
// the main loop, so comparing its rate against the expected 1 kHz separates
// "the ring starved" from "the callback stopped running" - the latter reports
// no underrun at all, because underruns are only detected inside it.
static uint32_t s_callback_blocks WAVEX_DTCM_DATA = 0;
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
    uint8_t channel_mode = SAMPLE_CH_AS_RECORDED;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
    // Kept so a poisoned file object can be reopened in place. FatFS latches
    // a disk error into the FIL, after which every f_read fails immediately
    // and only a fresh f_open clears it - which is exactly why stopping and
    // re-triggering the audition by hand was the only way back.
    char path[WaveX::Protocol::BROWSE_PATH_MAX];

    // Non-destructive edit (MSG_SAMPLE_EDIT_SET). Byte offsets, not frames:
    // every consumer below works in bytes, and converting once here keeps the
    // per-pass arithmetic out of the refill path. Defaults are the whole file
    // with looping off, so an un-edited sample behaves exactly as before.
    uint32_t region_start;  // absolute file offset
    uint32_t region_end;    // absolute file offset, exclusive
    uint32_t loop_start;
    uint32_t loop_end;
    bool loop_enabled;
#if WAVEX_DEBUG_HARNESS_ENABLED
    uint32_t rewinds;
#endif
    int16_t gain_q13;  // 8192 = unity; supports the metadata range through +12 dB

    // Region fades (roadmap 1.5.6 item 3), in FRAMES at the file's own rate.
    // Held in frames rather than bytes because the fade position is compared
    // against a frame index inside the conversion loop, where the byte offsets
    // above have already been turned back into frames anyway.
    uint32_t region_start_frame;
    uint32_t region_end_frame;
    uint32_t fade_in_frames;
    uint32_t fade_out_frames;
    uint32_t crossfade_frames, crossfade_step;
};
static WavState s_wav = {};
// Foreground converted loop head. No pointers into the Pool survive an unload,
// and nonresident sidecar auditions use the same bounded cache. Never DMA here.
WAVEX_BACKGROUND_DATA static q15_t s_loop_head[SampleLoop::kMaxCrossfadeFrames * kMaxMixChannels];

// Loop gap (browser audition). Frames of silence still owed after a rewind.
static uint32_t s_loop_gap_frames = 0;  // configured length
static uint32_t s_loop_gap_remaining = 0;

// ============================
// Sample loading state
// ============================
// Shared foreground scratch for resident loading and Instrument imports.
// AXI SRAM is SDMMC-reachable; each borrower returns it before yielding.
// The resident job limits payload reads to 4 KiB; imports retain their budget.
alignas(32) static uint8_t s_sample_io[32768];
static BssStatic<Storage::SampleLoadJob> s_sample_load;
static uint32_t s_sample_load_started_ms = 0;

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
static BssStatic<RecordingSession> s_recording;
static BssStatic<std::optional<Storage::ProjectSession>> s_project_session;
static BssStatic<std::optional<Storage::BankSession>> s_bank_session;
static BssStatic<Storage::SampleFileJob> s_sample_file_job;
static SampleFileStatusMessage s_sample_file_status;
static SampleFileOpMessage s_sample_file_request;
static bool s_sample_file_send = false;
static bool StopBankTrack(uint16_t mask);
static bool StopProjectVoices();
static void PublishProject();

// O(1): the id names its registry slot (SampleRegistry::Find).
static LoadedSampleInfo* find_loaded_sample(uint16_t sample_id) {
    if (!s_pool) {
        return nullptr;
    }
    SamplePool::Record* r = s_pool->Find(sample_id);
    return r ? &r->payload : nullptr;
}

// "The sample that just loaded": what the envelope job means by id 0.
static LoadedSampleInfo* newest_loaded_sample() {
    return s_pool ? find_loaded_sample(s_pool->Newest()) : nullptr;
}

static size_t loaded_sample_count() {
    return s_pool ? s_pool->Count() : 0;
}

// Names a resident sample by its card path, for saving an Instrument.
// A Pool id means nothing in a file (sample_pool.hpp), so this is the one
// way a .wxi can refer to a sample at all - and false here fails the save
// rather than writing a zone nothing can load.
bool SamplePathForId(uint16_t sample_id, char* out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    const LoadedSampleInfo* info = find_loaded_sample(sample_id);
    if (!info || info->path[0] == '\0')
        return false;
    if (std::strlen(info->path) >= out_len)
        return false;  // truncating a path silently is how it becomes unopenable
    WaveX::Protocol::detail::CopyWireString(out, out_len, info->path);
    return true;
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
    ref.loop_crossfade_ms = m.loop_crossfade_ms;
    ref.channel_mode = m.channel_mode;
    // gain_mul is linear and multiplies the velocity gain, so the dB figure
    // has to be converted here rather than passed through.
    ref.gain_mul = SampleGainLinear(m.gain_db_x10);
    return ref;
}

// Foreground-owned pending map and mailbox storage have engine lifetime.
static void PublishSequencerVoiceMap(uint16_t tracks = 0xFFFFu) {
    if (!s_seq_voices)
        return;
    SfzLoader::PrepareSequencerVoices(s_seq_voices->pending, tracks);
    s_seq_voices->mailbox.ProducerValue().CopyLiveFrom(s_seq_voices->pending);
    s_seq_voices->mailbox.PublishPrepared();
}

static void ClearSequencerVoiceMap(uint16_t tracks = 0xFFFFu) {
    if (!s_seq_voices)
        return;
    s_seq_voices->pending.Revoke(tracks);
    s_seq_voices->mailbox.ProducerValue().CopyLiveFrom(s_seq_voices->pending);
    s_seq_voices->mailbox.PublishPrepared();
}

// Drops `sample_id` from the registry and returns its memory to the arena.
// Entries stay in load order (oldest first), so removal closes the gap by
// shifting rather than swapping with the tail: newest_loaded_sample() reads
// the last entry as "most recently loaded", and a swap would quietly hand it
// an older sample.
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
    WaveX::Comm::LinkSend(WaveX::Protocol::MSG_SAMPLE_META, &wire, sizeof(wire));
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
    if (!s_pool) {
        WaveX::Log::PrintLine("SAMPLE_SELECT: Sample Pool unavailable");
        return;
    }
    if (slot >= kNumTracks) {
        WaveX::Log::PrintLine("SAMPLE_SELECT: track=%u out of range, ignored", (unsigned)slot);
        return;
    }
    if (SfzLoader::Busy()) {
        PushTrackBinding(slot);
        WaveX::Log::PrintLine("SAMPLE_SELECT: Instrument loader busy");
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
    // A rebind may free samples held only by this Track's old Instrument.
    // Preserve both the binding and the samples if the callback cannot confirm
    // its stop within the bounded main-loop wait.
    if (SfzLoader::TrackLoaded(slot)) {
        ClearSequencerVoiceMap(static_cast<uint16_t>(1u << slot));
        if (!StopTracksAndWait(static_cast<uint16_t>(1u << slot))) {
            PublishSequencerVoiceMap();
            PushTrackBinding(slot);
            WaveX::Log::PrintLine("SAMPLE_SELECT: track=%u callback stop timed out",
                                  (unsigned)slot);
            return;
        }
    }
    if (!SfzLoader::BindSample(*s_pool, s_sample_mem_mgr, slot, sample_id)) {
        PublishSequencerVoiceMap();
        WaveX::Log::PrintLine(
            "SAMPLE_SELECT: track=%u id=%u refused", (unsigned)slot, (unsigned)sample_id);
        return;
    }
    PublishSequencerVoiceMap();
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
// so it needs no synchronisation - see LinkSend's single-context
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

StreamDebugState DebugStreamState() {
    StreamDebugState state;
    state.open = s_wav.open;
    if (!state.open) {
        return state;
    }
    auto* record = s_pool ? s_pool->FindByPath(s_wav.path) : nullptr;
    state.sample_id = record ? record->sample_id : 0;
    const uint32_t bpf = s_wav.num_channels * (s_wav.bits_per_sample / 8u);
    state.start = (s_wav.region_start - s_wav.data_start) / bpf;
    state.end = (s_wav.region_end - s_wav.data_start) / bpf;
    state.loop_start = (s_wav.loop_start - s_wav.data_start) / bpf;
    state.loop_end = (s_wav.loop_end - s_wav.data_start) / bpf;
    state.loop = s_wav.loop_enabled;
    state.rewinds = s_wav.rewinds;
    // Keep the console's legacy unity=32767 scale, widened for positive gain.
    state.gain_q15 = (static_cast<int32_t>(s_wav.gain_q13) * 32767 + 4096) / 8192;
    return state;
}

bool DebugSampleMeta(uint16_t sample_id, SampleMetadata& out) {
    const auto* info = find_loaded_sample(sample_id);
    if (!info)
        return false;
    out = info->meta;
    return true;
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
    static_assert(sizeof(s_meta_page_buf) <= UINT16_MAX, "metadata page fits link length");
    if (WaveX::Comm::LinkSend(MSG_SAMPLE_META_PAGE, s_meta_page_buf, static_cast<uint16_t>(bytes)) <
        0) {
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
        if (WaveX::Comm::LinkSend(MSG_TRACK_BINDING, &msg, sizeof(msg)) < 0) {
            return;
        }
        s_track_binding_pending &= static_cast<uint16_t>(~(1u << track));
    }
}

bool UnloadSample(uint16_t sample_id) {
    if (s_recording.Get().Owns(sample_id))
        return false;
    if (SfzLoader::Busy()) {
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: Instrument loader busy");
        return false;
    }
    if (sample_id == 0) {
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=0 rejected (not a wildcard)");
        return false;
    }
    if (!find_loaded_sample(sample_id)) {
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=%u not loaded", (unsigned)sample_id);
        return false;
    }

    // Clear callback-owned trigger pointers, then wait for a stop of every
    // Track. A stopped/missing callback never authorizes freeing its samples.
    ClearSequencerVoiceMap();
    if (!StopTracksAndWait(0xFFFFu)) {
        PublishSequencerVoiceMap();
        WaveX::Log::PrintLine("SAMPLE_UNLOAD: id=%u callback stop timed out", (unsigned)sample_id);
        return false;
    }

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
        WaveX::Comm::LinkSend(WaveX::Protocol::MSG_SAMPLE_META, &bye, sizeof(bye));
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
// CPU-only provenance shares the PCM ring's head/tail publication. No DMA.
static uint32_t s_rb_source_frames[RB_CAP_FRAMES];
static uint32_t s_stream_epoch = 0;  // main writes only with s_rb_live false
static uint32_t s_consumed_source_frame = SourceFrameWalk::kSilent;  // callback-owned
struct PlayheadQuery {
    SamplePlayheadRequest request;
    const int16_t* pcm = nullptr;  // comparison only; never dereferenced
    uint32_t stream_epoch = 0;
};
static SnapshotMailbox<PlayheadQuery> s_playhead_query;
static SnapshotMailbox<SamplePlayheadMessage> s_playhead_reply;
static PlayheadQuery s_playhead_latest;  // foreground owner

// Pre-buffering system for smooth playback start
static const uint32_t PREBUFFER_FRAMES =
    1024;  // ~21ms at 48kHz (much more responsive for auditioning)
static q15_t s_prebuffer[PREBUFFER_FRAMES * kMaxMixChannels];  // ~23ms of interleaved audio
static uint32_t s_prebuffer_source_frames[PREBUFFER_FRAMES];
static uint32_t s_prebuffer_filled = 0;  // Number of frames pre-buffered
static bool s_prebuffer_ready = false;   // Whether pre-buffer is ready for playback
static bool s_prebuffering = false;      // Whether we're currently pre-buffering

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

static uint32_t ConvertFramesToOutput(const uint8_t*, q15_t*, uint32_t, uint16_t, uint8_t);
static bool PrepareWavCrossfade() {
    const uint32_t n = s_wav.crossfade_frames;
    if (!n)
        return true;
    const uint32_t bpf = s_wav.num_channels * (s_wav.bits_per_sample / 8u);
    if (f_lseek(&s_wav.file, s_wav.loop_start) != FR_OK)
        return false;
    uint32_t copied = 0;
    while (copied < n) {  // <=2 reads, independent of sample length
        const uint32_t frames = std::min(n - copied, uint32_t(SD_BUFFER_SIZE / bpf));
        UINT bytes = 0;
        if (f_read(&s_wav.file, s_prebuffer_sd, frames * bpf, &bytes) != FR_OK ||
            bytes != frames * bpf)
            return false;
        ConvertFramesToOutput(s_prebuffer_sd,
                              s_loop_head + copied * s_output_channels,
                              frames,
                              s_wav.num_channels,
                              static_cast<uint8_t>(s_wav.bits_per_sample));
        copied += frames;
    }
    return true;
}
static void ApplyWavCrossfade(q15_t* buf, uint32_t frames, uint32_t first) {
    if (!s_wav.crossfade_frames)
        return;
    const uint32_t bpf = s_wav.num_channels * (s_wav.bits_per_sample / 8u);
    const uint32_t end = (s_wav.loop_end - s_wav.data_start) / bpf;
    SampleLoop::ApplyBlock(buf,
                           frames,
                           first,
                           s_loop_head,
                           end,
                           s_wav.crossfade_frames,
                           s_output_channels,
                           s_wav.crossfade_step);
}

// Playback gain is applied on the foreground's converted block, before refill.
static void ApplyWavGain(q15_t* buf, uint32_t samples) {
    ApplySampleGain(buf, samples, s_wav.gain_q13);
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

        SampleChannels::Map(sample_vals[0], sample_vals[1], src_channels, s_wav.channel_mode);
        for (uint32_t ch = 0; ch < s_output_channels; ++ch) {
            q15_t value = ch < 2 ? sample_vals[ch] : 0;
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
static uint32_t s_resampler_history_frame = SourceFrameWalk::kSilent;

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
    const uint32_t source_first = (f_tell(&s_wav.file) - s_wav.data_start) / file_bpf;
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
    ApplyWavCrossfade(conversion_output, frames_read, source_first);
    ApplyWavGain(conversion_output, frames_read * s_output_channels);
    ApplyWavFade(conversion_output, frames_read, source_first - s_wav.region_start_frame);

    q15_t* to_push = conversion_output;
    uint32_t output_frames = frames_read;
    const StreamResamplerState resampler_before = s_resampler;
    if (resample_ratio != 1.0f) {
        uint32_t max_out_frames =
            static_cast<uint32_t>(std::ceil(static_cast<float>(frames_read) * resample_ratio)) + 1;
        q15_t* resample_buffer = AcquireScratch(max_out_frames * s_output_channels);
        uint32_t resampled = 0;
        // Same snapshot reasoning as the streaming path: the drop below
        // discards this pass's output, so the phase must not stay advanced.
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
    SourceFrameWalk walk{
        source_first,
        s_resampler_history_frame,
        resample_ratio != 1.0f && resampler_before.has_history ? resampler_before.phase : 0.0f,
        1.0f / resample_ratio,
        false};
    for (uint32_t i = 0; i < output_frames; ++i)
        s_prebuffer_source_frames[s_prebuffer_filled + i] = walk.Next();
    s_resampler_history_frame = source_first + frames_read - 1;
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
            const uint32_t rewind_to =
                (s_wav.loop_enabled ? s_wav.loop_start + s_wav.crossfade_frames * file_bpf
                                    : s_wav.region_start);
            const uint32_t seek_start = System::GetTick();
            if (f_lseek(&s_wav.file, rewind_to) == FR_OK) {
#if WAVEX_DEBUG_HARNESS_ENABLED
                ++s_wav.rewinds;
#endif
            }
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
                // Retry the failed read itself. bytes_remaining is relative
                // to the current region/loop end, not the full data chunk;
                // deriving an absolute offset from data_size skips elsewhere
                // in the file after a trimmed region or loop read fails.
                const uint32_t resume_at = read_at;
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

static inline void rb_push_frames(const q15_t* samples,
                                  uint32_t frames,
                                  SourceFrameWalk walk = {},
                                  const uint32_t* tags = nullptr) {
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

    for (uint32_t i = 0; i < frames; ++i)
        s_rb_source_frames[(head + i) & mask] = tags ? tags[i] : walk.Next();

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
    s_consumed_source_frame = SourceFrameWalk::kSilent;
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
        s_consumed_source_frame = s_rb_source_frames[(tail - 1) & mask];
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
    s_mixer_controls.Init();
    s_master_gain.Init(sample_rate);
    s_mix_meter_subscription.Unsubscribe();
    s_mix_meters_active.store(false, std::memory_order_relaxed);
    s_mix_meter_window.Init(static_cast<uint32_t>(sample_rate));
    s_callback_metering = false;
    s_mix_meter_sequence = s_mix_meter_sent = 0;
    s_mix_meter_main = {};
    s_mix_meter_mailbox.Init(s_mix_meter_main);
    s_playhead_query.Init({});
    s_playhead_reply.Init({});
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
    s_track_live_updates.Init();
    s_midi_modulation.Init();
    for (auto& extras: s_track_live) {
        extras = TrackLiveExtras{};
    }
    s_para_pending = ParaphonicParams{};
    s_para_active = s_para_pending;
    s_para_mailbox.Init(s_para_pending);
    s_cv_test_pending = CvTestParams{};
    s_cv_test_active = s_cv_test_pending;
    s_cv_test_mailbox.Init(s_cv_test_pending);
    s_live_notes.Init();
    s_voice_stop_fence.Init();
    s_seq_command_queue.Init();
    s_seq_page_pending_storage.Reconstruct();
    s_seq_head_pending_storage.Reconstruct();
    s_seq_notes_mailbox.Init(SeqNotesMessage{});
    s_seq_page_mailbox.Init(s_seq_page_pending_storage.Get());
    s_seq_head_mailbox.Init(s_seq_head_pending_storage.Get());
    s_seq_page_pending = false;
    s_seq_head_pending = false;
    s_seq_telemetry_frames = 0;
    s_seq_telemetry_interval = static_cast<uint32_t>(sample_rate / 25.0f);
    if (s_seq_telemetry_interval == 0)
        s_seq_telemetry_interval = 1;

    s_seq_voices = nullptr;
    s_seq_voice_storage = {};
    s_rb_low_water = 0xFFFFFFFFu;

    SfzLoader::Reset();
    SfzLoader::SetLoadedSampleResolver(SampleResolver{nullptr, &ResolveLoadedSample});

    WaveX::Profiling::InitHardware();
    PROFILE_REGISTER_ZONE(audio_callback);
    PROFILE_REGISTER_ZONE(voice_events);
    PROFILE_REGISTER_ZONE(voice_modulation);
    PROFILE_REGISTER_ZONE(voice_render);
    PROFILE_REGISTER_ZONE(wav_pump_io);
    PROFILE_REGISTER_ZONE(format_conversion);
    PROFILE_REGISTER_ZONE(ring_buffer_push);
    PROFILE_REGISTER_ZONE(prebuffer_audio);
    PROFILE_REGISTER_ZONE(sd_refill);

#if WAVEX_ANALOG_CV_ENABLED
#if WAVEX_CV_BACKEND == WAVEX_CV_BACKEND_MCP4728
    s_cv_backend.Init(0x60);
#else
    s_cv_backend.Init();
#endif
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
        s_bank_session.Get().emplace(s_sample_mem_mgr,
                                     *s_pool,
                                     s_sample_io,
                                     sizeof(s_sample_io),
                                     Storage::BankSession::Boundary{StopBankTrack, PublishProject});
        s_project_session.Get().emplace(
            s_sample_mem_mgr,
            *s_pool,
            *s_bank_session.Get(),
            s_pattern_exchange_storage.Get(),
            s_mixer_controls,
            s_sample_io,
            sizeof(s_sample_io),
            Storage::ProjectSession::Boundary{StopProjectVoices, PublishProject});
        void* memory = nullptr;
        if (s_sample_mem_mgr.alloc(sizeof(SequencerVoiceState), &s_seq_voice_storage) &&
            s_sample_mem_mgr.ptr(s_seq_voice_storage, &memory)) {
            s_seq_voices = new (memory) SequencerVoiceState{};
            s_seq_voices->mailbox.Init(s_seq_voices->pending);
        } else {
            s_sample_mem_mgr.release(&s_seq_voice_storage);
            WaveX::Log::PrintLine("SEQ: prepared voice storage unavailable");
        }
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

    s_recording.Get().Init(s_pool, &s_sample_mem_mgr, PushSampleMeta, Comm::LinkSend);
    s_voice_manager.Init(static_cast<uint32_t>(sample_rate));

    // The global LFO advances once per 1 kHz audio/control block.
    s_global_lfo.Init();
    s_lock_notice.Init(SeqLockNoticeMessage{});

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
    s_telemetry_main = CallbackTelemetry{};
    s_telemetry_mailbox.Init(s_telemetry_main);
    s_callback_blocks = 0;
}

WAVEX_ITCM_CODE_NAMED("audio.Callback")
void Callback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    PROFILE_SCOPE(audio_callback);
#if WAVEX_PROFILE_CALLBACK_DETAIL
    const uint32_t detail_start = WaveX::Profiling::GetCycles();
    WaveX::Profiling::callback_detail_window.Begin();
#endif
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
    s_recording.Get().Commands(s_voice_manager);
    PROFILE_BEGIN(voice_events);
    bool any_note_on;
    {
        CALLBACK_DETAIL_SCOPE(Queue);
        any_note_on = drain_note_queue();
    }

    // Publish control-plane changes only at a block boundary. A callback that
    // preempts the producer mid-copy keeps the previous complete snapshot and
    // picks up the new generation one block later.
    {
        CALLBACK_DETAIL_SCOPE(Controls);
        s_track_live_updates.ApplyTo(s_voice_manager);
        s_mixer_controls.ApplyTo(s_track_mixer);
    }

    PROFILE_END(voice_events);
    const bool metering = s_mix_meters_active.load(std::memory_order_relaxed);
    if (metering != s_callback_metering) {
        s_mix_meter_window.Reset();
        s_callback_metering = metering;
    }
    const auto& midi_sources = s_midi_modulation.Acquire();
    ModSources global_sources;
    const ModSlotResolver resolver{nullptr, &ResolveModSlots};
    s_track_mixer.Tick(static_cast<uint32_t>(size));
    // Render chronologically up to each trigger frame before admitting its
    // groups. A later steal must never erase the earlier part of this block.
    const auto render = [&](uint16_t offset, uint16_t frames, uint64_t start) {
        s_arps.Get().Gates(offset, frames, s_voice_manager, RecordArp);
        s_voice_manager.PrepareSequenceGates(start, frames, [&](double tick) {
            return s_seq_transport.scheduler().FrameAtTick(tick);
        });
        {
            PROFILE_SCOPE(voice_modulation);
            CALLBACK_DETAIL_SCOPE(Modulation);
            s_voice_manager.TickModulation(resolver, global_sources, frames, &midi_sources);
        }
        if (!s_voice_manager.ActiveVoiceCount() || size > Timebase::kBlockSize)
            return;
        static float vm_l[Timebase::kBlockSize], vm_r[Timebase::kBlockSize];
        {
            PROFILE_SCOPE(voice_render);
            CALLBACK_DETAIL_SCOPE(Render);
            s_voice_manager.Render(
                vm_l, vm_r, frames, metering ? s_mix_meter_window.Peaks() : nullptr);
        }
        for (size_t i = 0; i < frames; ++i) {
            out[0][offset + i] += vm_l[i];
            out[1][offset + i] += vm_r[i];
        }
    };
    any_note_on = drain_sequencer(static_cast<uint16_t>(size),
                                  render,
                                  [&](bool scheduled) {
                                      global_sources.lfo1 =
                                          s_global_lfo.Tick(s_seq_transport.scheduler().Tempo(),
                                                            s_seq_transport.IsPlaying(),
                                                            s_seq_transport.scheduler().RunEpoch(),
                                                            any_note_on || scheduled);
                                  }) ||
                  any_note_on;
    if (s_para_mailbox.ConsumeLatest(s_para_active)) {
        s_para_env.SetParams(s_para_active.attack_s,
                             s_para_active.decay_s,
                             s_para_active.sustain,
                             s_para_active.release_s);
    }
    s_cv_test_mailbox.ConsumeLatest(s_cv_test_active);
#if WAVEX_DEBUG_HARNESS_ENABLED
    __atomic_store_n(&s_dbg_active_voices, s_voice_manager.ActiveVoiceCount(), __ATOMIC_RELAXED);
#endif

    if (metering && s_mix_meter_window.Advance(static_cast<uint32_t>(size))) {
        MixMeterSnapshot snapshot;
        snapshot.peaks = s_mix_meter_window.Values();
        snapshot.sequence = ++s_mix_meter_sequence;
        s_mix_meter_mailbox.Publish(snapshot);
        s_mix_meter_window.Reset();
    }
    if (s_playhead_query.AcquireLatest()) {
        const auto& query = s_playhead_query.ConsumerValue();
        SamplePlayheadMessage reply;
        reply.request_id = query.request.request_id;
        reply.sample_id = query.request.sample_id;
        reply.generation = query.request.generation;
        // An active matching audition owns the cursor even during its silent
        // loop gap; don't jump to a resident voice during that gap.
        if (query.stream_epoch && __atomic_load_n(&s_rb_live, __ATOMIC_ACQUIRE) &&
            query.stream_epoch == s_stream_epoch) {
            if (s_consumed_source_frame != SourceFrameWalk::kSilent) {
                reply.source = PLAYHEAD_STREAM;
                reply.frame = s_consumed_source_frame;
            }
        } else {
            uint32_t frame = 0;
            if (FindVoicePlayhead(s_voice_manager, query.pcm, frame)) {
                reply.source = PLAYHEAD_VOICE;
                reply.frame = frame;
            }
        }
        s_playhead_reply.Publish(reply);
    }
    s_master_gain.SetTarget(s_track_mixer.MasterGain());

    s_recording.Get().Monitor(in[0], in[1], out[0], out[1], static_cast<uint32_t>(size));
    // Compute post-master per-block meters
    float sumL = 0.f, sumR = 0.f;
    float pkL = 0.f, pkR = 0.f;
    for (size_t i = 0; i < size; ++i) {
        const float gain = s_master_gain.Next();
        float l = out[0][i] *= gain;
        float r = out[1][i] *= gain;
        sumL += l * l;
        sumR += r * r;
        float al = fabsf(l);
        float ar = fabsf(r);
        if (al > pkL)
            pkL = al;
        if (ar > pkR)
            pkR = ar;
    }
    s_recording.Get().Process(in[0], in[1], out[0], out[1], static_cast<uint32_t>(size));
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
    CallbackTelemetry telemetry;
    telemetry.meters = s_last_block_meters;
    telemetry.cpu_avg = s_cpu_load_meter.GetAvgCpuLoad();
    telemetry.cpu_min = s_cpu_load_meter.GetMinCpuLoad();
    telemetry.cpu_max = s_cpu_load_meter.GetMaxCpuLoad();
    telemetry.blocks = s_callback_blocks;
    telemetry.cycles = s_dwt_callback_cycles;
    s_telemetry_mailbox.Publish(telemetry);
#if WAVEX_PROFILE_CALLBACK_DETAIL
    // Excludes the peak-selection/mailbox copy and the outer profiler epilogue.
    // Diagnostic scopes have overhead; compare gate images with detail OFF.
    if (WaveX::Profiling::callback_detail_window.End(detail_start, WaveX::Profiling::GetCycles()))
        WaveX::Profiling::callback_detail_mailbox.Publish(
            WaveX::Profiling::callback_detail_window.Peak());
#endif
}

// MSG_CONTROL_CHANGE -> Stage A paraphonic path (item 5 stage 3). Main-loop
// handlers update pending structs and publish complete snapshots. The callback
// applies them at the next block boundary, including shared-envelope rates.
// Each parameter now has TWO destinations, deliberately: the Stage A analog
// path (s_para_pending, one shared VCF/VCA) and the digital per-voice path
// (s_track_live_updates). The analog board is
// optional hardware and the digital voices always render - so a knob has to
// reach both or it would do nothing on whichever configuration is in use.
void OnMixStateRequest(const MixStateRequest& request) {
    const auto state = s_mixer_controls.Read(request);
    if (state.valid)
        WaveX::Comm::LinkSend(MSG_MIX_STATE, &state, sizeof(state));
}

void OnMixOp(const MixOpMessage& m) {
    if (m.op == MIX_OP_SUB_METERS || m.op == MIX_OP_UNSUB_METERS) {
        if (m.op == MIX_OP_SUB_METERS)
            s_mix_meter_subscription.Subscribe(System::GetNow());
        else
            s_mix_meter_subscription.Unsubscribe();
        s_mix_meters_active.store(m.op == MIX_OP_SUB_METERS, std::memory_order_relaxed);
        return;
    }
    s_mixer_controls.Update(m);
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
    if (!IsValidTrackOp(m))
        return;
    bool ok = false;
    switch (m.op) {
        case TRACK_OP_SET_MIDI_IN:
            ok = SfzLoader::SetTrackMidiIn(m.track, static_cast<uint8_t>(m.value));
            if (ok)
                s_midi_modulation.Reset(static_cast<uint16_t>(1u << m.track));
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

/**
 * One parameter change, addressed to a Track (track-and-patch-model.md §3.2).
 *
 * `channel` is the Track index. Filter and envelope belong to that Track's
 * **Instrument**, which is the authority: a note triggered later reads them
 * from there, and they travel with the Instrument between Tracks and Banks.
 * The VoiceLiveParams published below are *derived* from the Instrument for
 * the sounding-voice push - they are a message, not a second store, which is
 * why they are composed fresh each time rather than accumulated.
 *
 * Pan, pitch and the filter topology are not Instrument fields yet (they
 * become trim_pan/transpose and FilterType in stage 5), so they are still
 * held per Track here, in s_track_live.
 */
void OnControlChange(const ControlChangeMessage& ctrl_msg) {
    const float norm = static_cast<float>(ctrl_msg.value) / 65535.0f;
    if (ctrl_msg.parameter == PARAM_VOLUME) {
        s_mixer_controls.Update(
            MixOpMessage{MIX_OP_SET_MASTER, 0, Mix::GainDbToWire(Mix::LinearToDb(norm))});
        return;
    }
    const uint8_t track = ctrl_msg.channel & 0x0Fu;
    bool para_changed = false;
    bool voice_changed = false;

    // Start from what this Track's Instrument already says, so a knob moves
    // one field and leaves the rest of the Instrument's sound alone.
    InstrumentFilter filter;
    InstrumentEnv env;
    if (const InstrumentFilter* f = SfzLoader::GetInstrumentFilter(track))
        filter = *f;
    if (const InstrumentEnv* e = SfzLoader::GetInstrumentEnv(track))
        env = *e;

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
            filter.cutoff_hz = 20.0f * std::pow(1000.0f, norm);
            voice_changed = true;
            break;
        case PARAM_PAN:
            // Linear 0..1 across the wire's full range. Voice::pan is applied
            // as a gain pair per block, so this is click-free without smoothing.
            s_track_live[track].pan = norm;
            voice_changed = true;
            break;

        case PARAM_PITCH: {
            // +/- 24 semitones around centre. Two octaves each way is enough to
            // play a sample as an instrument without the resampler running so
            // far from unity that the interpolation artefacts dominate.
            constexpr float kPitchRangeSemis = 24.0f;
            s_track_live[track].pitch_semitones = (norm * 2.0f - 1.0f) * kPitchRangeSemis;
            voice_changed = true;
            break;
        }

        case PARAM_FILTER_RESONANCE:
            s_para_pending.resonance = norm;
            filter.resonance = norm;  // svf_filter.hpp maps 0..1 onto Q
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
                env.attack_s = seconds;
            } else if (ctrl_msg.parameter == PARAM_ENVELOPE_DECAY) {
                s_para_pending.decay_s = seconds;
                env.decay_s = seconds;
            } else if (ctrl_msg.parameter == PARAM_ENVELOPE_SUSTAIN) {
                s_para_pending.sustain = norm;
                env.sustain = norm;
            } else {
                s_para_pending.release_s = seconds;
                env.release_s = seconds;
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
            // is deferred hardware anyway. LFO_*: no Stage A
            // consumer either (the analog VCA is the level control; a global
            // LFO is future work).
            break;
    }
    if (para_changed) {
        s_para_mailbox.Publish(s_para_pending);
    }
    if (voice_changed) {
        // The Instrument is the store; publish what it now says, for this
        // Track only, so a knob on Track 3 cannot move Track 5's held notes.
        SfzLoader::SetInstrumentFilter(track, filter);
        SfzLoader::SetInstrumentEnv(track, env);
        s_track_live_updates.Publish(ComposeTrackLive(track, filter, env));
        PublishSequencerVoiceMap(static_cast<uint16_t>(1u << track));
        if (!WaveX::PatternStore::BlocksEdits()) {
            WaveX::Sequencer::SequencerCommand command;
            command.type = WaveX::Sequencer::SequencerCommandType::RecordControl;
            command.control = ctrl_msg;
            command.control.channel = track;
            command.pattern_epoch = s_seq_capture_epoch.load(std::memory_order_acquire);
            if (!s_seq_command_queue.Push(command))
                WaveX::Log::PrintLine("SEQ: motion capture queue full; control not recorded");
        }
    }
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
    WaveX::Comm::LinkSend(WaveX::Protocol::MSG_CV_CAL_RESP, &resp, sizeof(resp));
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
        CloseWav();  // free-space discovery and writes own the foreground SD path
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
    if (ProjectBusy()) {
        if (m.command == SEQ_TRANSPORT_STOP) {
            WaveX::Sequencer::SequencerCommand command;
            command.type = WaveX::Sequencer::SequencerCommandType::StopOnly;
            EnqueueSequencerCommand(command);
        }
        return;
    }
    if (WaveX::PatternStore::BlocksEdits() && m.command != SEQ_TRANSPORT_STOP &&
        m.command != SEQ_TRANSPORT_CONFIGURE)
        return;
    // Publish before enqueuing PLAY: its first downbeat is due in the same
    // callback that consumes this command, so its immutable sample pointers
    // must be available before Tick() starts the scheduler.
    if (m.command == SEQ_TRANSPORT_PLAY || m.command == SEQ_TRANSPORT_CONTINUE)
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

void OnSeqSlotEdit(const SeqSlotEditMessage& message) {
    if (ProjectBusy() || PatternStore::BlocksEdits() || !IsValidSeqSlotEdit(message))
        return;
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::SlotEdit;
    command.slot_edit = message;
    EnqueueSequencerCommand(command);
}
void OnSeqNotesRequest(const SeqPatternRequestMessage& request) {
    if (!IsValidSeqNotesRequest(request))
        return;
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::NotesRequest;
    command.pattern_request = request;
    EnqueueSequencerCommand(command);
}
void OnSeqSlotPageRequest(const SeqPatternRequestMessage& request) {
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::SlotPage;
    command.pattern_request = request;
    EnqueueSequencerCommand(command);
}
void OnSeqPatternRequest(const SeqPatternRequestMessage& request) {
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::PatternRequest;
    command.pattern_request = request;
    EnqueueSequencerCommand(command);
}

void OnSeqFileOp(const SeqFileOpMessage& request) {
    const bool accepted = WaveX::PatternStore::Request(
        request, s_pattern_exchange_storage.Get(), s_recording.Get().Busy() || SampleLoadBusy());
    if (accepted && (request.op == SEQ_FILE_SAVE_COPY || request.op == SEQ_FILE_LOAD))
        CloseWav();  // file jobs own SD bandwidth; resident Track voices continue
}

bool ProjectBusy() {
    return s_project_session.Get() && s_project_session.Get()->BlocksEdits();
}
void OnSongOp(const SeqSongOpMessage& request) {
    if (!IsValidSeqSongOp(request))
        return;
    if (s_project_session.Get()) {
        if (s_project_session.Get()->RequestSong(
                request,
                BankBusy() || SampleFileBusy() || SampleLoadBusy() || SfzLoader::Busy() ||
                    PatternStore::Busy() || Storage::CardService::Busy()) &&
            request.op == SEQ_SONG_PLAY)
            PublishSequencerVoiceMap();
    } else {
        SeqSongStatusMessage status;
        status.request_id = request.request_id;
        status.song = request.song;
        status.completed_request_id = request.op == SEQ_SONG_GET ? 0 : request.request_id;
        status.completed_op = request.op;
        status.error = SEQ_SONG_NO_MEMORY;
        Comm::LinkSend(MSG_SEQ_SONG_STATUS, &status, sizeof(status));
    }
}
void OnPatternSlotOp(const SeqSlotOpMessage& request) {
    if (!IsValidSeqSlotOp(request))
        return;
    if (s_project_session.Get()) {
        s_project_session.Get()->RequestPattern(
            request,
            BankBusy() || SampleFileBusy() || SampleLoadBusy() || SfzLoader::Busy() ||
                PatternStore::Busy() || Storage::CardService::Busy());
    } else {
        SeqSlotStatusMessage status;
        status.request_id = request.request_id;
        status.slot = request.slot;
        status.completed_request_id = request.op == SEQ_SLOT_GET ? 0 : request.request_id;
        status.completed_op = request.op;
        status.error = SEQ_SLOT_NO_MEMORY;
        Comm::LinkSend(MSG_SEQ_SLOT_STATUS, &status, sizeof(status));
    }
}
void OnProjectOp(const ProjectOpMessage& request) {
    if (!IsValidProjectOp(request))
        return;
    if (!s_project_session.Get()) {
        ProjectStatusMessage status;
        status.request_id = request.request_id;
        status.completed_request_id = request.op == PROJECT_GET ? 0 : request.request_id;
        status.completed_op = request.op;
        status.error = PROJECT_NO_MEMORY;
        Comm::LinkSend(MSG_PROJECT_STATUS, &status, sizeof(status));
        return;
    }
    const bool accepted = s_project_session.Get()->Request(
        request,
        s_recording.Get().Busy() || SampleFileBusy() || SampleLoadBusy() || BankBusy() ||
            SfzLoader::Busy() || PatternStore::Busy() || Storage::CardService::Busy());
    if (accepted) {
        CancelEnvelopeJob();
        CloseWav();
    }
}
static bool StopProjectVoices() {
    ClearSequencerVoiceMap();
    return StopTracksAndWait(0xffff);
}
static void PublishProject() {
    PublishSequencerVoiceMap();
    PushTrackBinding(0xff);
}
#if WAVEX_DEBUG_HARNESS_ENABLED
static BankJobMetrics s_bank_job_metrics;
BankJobMetrics DebugBankJobMetrics() {
    return s_bank_job_metrics;
}
#endif
bool BankBusy() {
    return s_bank_session.Get() && s_bank_session.Get()->Busy();
}
static bool BankExternalBusy() {
    return s_recording.Get().Busy() || SampleLoadBusy() ||
           (s_project_session.Get() && s_project_session.Get()->Busy()) || PatternStore::Busy() ||
           SampleFileBusy() || Storage::CardService::Busy() || (!BankBusy() && SfzLoader::Busy());
}
static void BankAccepted() {
#if WAVEX_DEBUG_HARNESS_ENABLED
    s_bank_job_metrics = BankJobMetrics{};
    s_bank_job_metrics.request_id = s_bank_session.Get()->Status().active_request_id;
    s_bank_job_metrics.op = s_bank_session.Get()->Status().active_op;
    s_bank_job_metrics.busy = true;
#endif
    CancelEnvelopeJob();
    CloseWav();
}
void OnMidiProgram(const MidiProgramMessage& message) {
    if (s_bank_session.Get() && s_bank_session.Get()->ProgramChange(message, BankExternalBusy()))
        BankAccepted();
}
void OnBankSlotOp(const BankSlotOpMessage& request) {
    if (!IsValidBankSlotOp(request))
        return;
    if (!s_bank_session.Get()) {
        BankStatusMessage status;
        status.request_id = status.completed_request_id = request.request_id;
        status.completed_op = request.op;
        status.error = BANK_NO_MEMORY;
        status.slot = request.destination_slot;
        Comm::LinkSend(MSG_BANK_STATUS, &status, sizeof(status));
        return;
    }
    if (s_bank_session.Get()->RequestSlotOperation(request, BankExternalBusy()))
        BankAccepted();
}
void OnBankOp(const BankOpMessage& request) {
    if (!IsValidBankOp(request))
        return;
    if (!s_bank_session.Get()) {
        BankStatusMessage status;
        status.request_id = request.request_id;
        status.completed_request_id = request.op == BANK_GET ? 0 : request.request_id;
        status.completed_op = request.op;
        status.error = BANK_NO_MEMORY;
        status.slot = request.slot;
        Comm::LinkSend(MSG_BANK_STATUS, &status, sizeof(status));
        return;
    }
    if (s_bank_session.Get()->Request(request, BankExternalBusy()))
        BankAccepted();
}

static bool StopBankTrack(uint16_t mask) {
    ClearSequencerVoiceMap(mask);
    return StopTracksAndWait(mask);
}
void PumpProjectSession() {
    if (auto& bank = s_bank_session.Get(); bank) {
#if WAVEX_DEBUG_HARNESS_ENABLED
        if (bank->Busy()) {
            // TIM2 raw ticks wrap as uint32_t; GetUs() does not. Each bounded
            // pump is far shorter than one timer wrap. No callback instrumentation.
            static const uint32_t ticks_per_us = daisy::System::GetTickFreq() / 1000000u;
            const uint32_t started = daisy::System::GetTick();
            bank->Pump();
            const uint32_t us = (daisy::System::GetTick() - started) / ticks_per_us;
            ++s_bank_job_metrics.pumps;
            s_bank_job_metrics.max_pump_us = std::max(s_bank_job_metrics.max_pump_us, us);
            s_bank_job_metrics.work_us += std::min(us, UINT32_MAX - s_bank_job_metrics.work_us);
            s_bank_job_metrics.busy = bank->Busy();
            s_bank_job_metrics.error = bank->Status().error;
        }
#else
        bank->Pump();
#endif
        if (bank->ReplyPending() &&
            Comm::LinkSend(MSG_BANK_STATUS, &bank->Status(), sizeof(BankStatusMessage)) >= 0)
            bank->ReplySent();
    }
    auto& session = s_project_session.Get();
    if (!session)
        return;
    session->Pump();
    auto& songs = session->Songs();
    if (songs.ReplyPending() &&
        Comm::LinkSend(MSG_SEQ_SONG_STATUS, &songs.Status(), sizeof(SeqSongStatusMessage)) >= 0)
        songs.ReplySent();
    auto& patterns = session->Patterns();
    if (patterns.ReplyPending() &&
        Comm::LinkSend(MSG_SEQ_SLOT_STATUS, &patterns.Status(), sizeof(SeqSlotStatusMessage)) >= 0)
        patterns.ReplySent();
    if (session->ReplyPending() &&
        Comm::LinkSend(MSG_PROJECT_STATUS, &session->Status(), sizeof(ProjectStatusMessage)) >= 0)
        session->ReplySent();
}
bool StorageJobBusy() {
    return s_recording.Get().Busy() || SampleFileBusy() || SampleLoadBusy() || BankBusy() ||
           (s_project_session.Get() && s_project_session.Get()->Busy()) || SfzLoader::Busy() ||
           WaveX::PatternStore::Busy();
}
void OnRecordOp(const RecordOpMessage& request) {
    const bool other_busy = SampleFileBusy() || SampleLoadBusy() || BankBusy() ||
                            (s_project_session.Get() && s_project_session.Get()->Busy()) ||
                            SfzLoader::Busy() || WaveX::PatternStore::Busy() ||
                            Storage::CardService::Busy();
    s_recording.Get().Request(request, other_busy);
}
void PumpRecording() {
    s_recording.Get().Pump(System::GetNow());
}
bool SampleFileBusy() {
    return s_sample_file_job.Get().Busy();
}
void OnSampleFileOp(const SampleFileOpMessage& request) {
    if (!IsValidSampleFileOp(request))
        return;
    auto& status = s_sample_file_status;
    status.request_id = request.request_id;
    s_sample_file_send = true;
    if (request.op == SAMPLE_FILE_GET || request.request_id == status.active_request_id ||
        request.request_id == status.completed_request_id)
        return;
    if (StorageJobBusy() || Storage::CardService::Busy()) {
        // Do not replace the active operation or its eventual result.
        status.completed_request_id = request.request_id;
        status.completed_op = request.op;
        status.error = SAMPLE_FILE_BUSY;
        return;
    }
    const auto* info = find_loaded_sample(request.sample_id);
    status.sample_id = request.sample_id;
    status.progress = 0;
    status.path[0] = 0;
    if (!info) {
        status.completed_request_id = request.request_id;
        status.completed_op = request.op;
        status.error = SAMPLE_FILE_BAD_SAMPLE;
        return;
    }
    if (!s_sample_file_job.Get().Begin(request, info->path, info->meta)) {
        status.completed_request_id = request.request_id;
        status.completed_op = request.op;
        status.error = s_sample_file_job.Get().Error();
        return;
    }
    CloseWav();
    CancelEnvelopeJob();
    s_sample_file_request = request;
    status.busy = 1;
    status.active_request_id = request.request_id;
}
void PumpSampleFile() {
    auto& job = s_sample_file_job.Get();
    auto& status = s_sample_file_status;
    if (job.Busy()) {
        job.Pump();
        status.progress = job.Progress();
        if (!job.Busy()) {
            // All job handles are closed and competing storage jobs were
            // excluded at admission. A remount invalidates handles, so only
            // now may a write-side bus error negotiate a lower clock. Keep
            // the failed result: never replay a mutation automatically.
#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
            if (job.CardIoFailed())
                Storage::SdSdio::DowngradeSpeed();
#endif
            status.busy = 0;
            status.active_request_id = 0;
            status.completed_request_id = s_sample_file_request.request_id;
            status.completed_op = s_sample_file_request.op;
            status.sample_id = s_sample_file_request.sample_id;
            status.error = job.Error();
            status.progress = status.error == SAMPLE_FILE_OK ? 100 : status.progress;
            Protocol::detail::CopyWireString(status.path, sizeof(status.path), job.Destination());
            s_sample_file_send = true;
        }
    }
    if (s_sample_file_send && Comm::LinkSend(MSG_SAMPLE_FILE_STATUS, &status, sizeof(status)) >= 0)
        s_sample_file_send = false;
}
bool PrepareCardFormat() {
    if (StorageJobBusy())
        return false;
    CloseWav();
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::Transport;
    command.transport.command = SEQ_TRANSPORT_STOP;
    if (!s_seq_command_queue.Push(command))
        return false;
    ClearSequencerVoiceMap();
    if (!StopTracksAndWait(0xFFFFu)) {
        PublishSequencerVoiceMap();
        return false;
    }
    return true;
}
void FinishCardFormat() {
    // Resident PCM remains owned by its Tracks, just as on card removal.
    // Save admission will reject the now-missing on-card dependencies.
    PublishSequencerVoiceMap();
}

void OnSamplePlayheadRequest(const SamplePlayheadRequest& request) {
    PlayheadQuery query;
    query.request = request;
    const auto* sample = find_loaded_sample(request.sample_id);
    if (sample && sample->meta.generation == request.generation) {
        query.pcm = ResolveLoadedSample(nullptr, request.sample_id).data;
        if (s_wav.open && sample->path[0] && std::strcmp(sample->path, s_wav.path) == 0)
            query.stream_epoch = s_stream_epoch;
    }
    s_playhead_latest = query;
    s_playhead_query.Publish(query);
}

void PumpSamplePlayhead() {
    SamplePlayheadMessage reply;
    if (!s_playhead_reply.ConsumeLatest(reply) ||
        reply.request_id != s_playhead_latest.request.request_id)
        return;
    const auto* sample = find_loaded_sample(reply.sample_id);
    if (!sample || sample->meta.generation != reply.generation ||
        (reply.source == PLAYHEAD_STREAM &&
         (!s_wav.open || s_playhead_latest.stream_epoch != s_stream_epoch))) {
        reply.source = PLAYHEAD_IDLE;
        reply.frame = 0;
    }
    // Display telemetry is expendable. A full TX queue drops this reply;
    // the panel times out and asks again instead of delaying control traffic.
    Comm::LinkSend(MSG_SAMPLE_PLAYHEAD, &reply, sizeof(reply));
}

void PumpMixMeters() {
    const bool enabled = s_mix_meter_subscription.Enabled(System::GetNow());
    s_mix_meters_active.store(enabled, std::memory_order_relaxed);
    s_mix_meter_mailbox.ConsumeLatest(s_mix_meter_main);
    if (!enabled) {
        s_mix_meter_sent = s_mix_meter_main.sequence;
        return;
    }
    if (s_mix_meter_main.sequence == s_mix_meter_sent)
        return;
    MixMetersMessage message;
    for (uint8_t track = 0; track < Mix::kNumTracks; ++track)
        message.peak[track] = Mix::PeakToMeterByte(s_mix_meter_main.peaks[track]);
    if (WaveX::Comm::LinkSend(MSG_MIX_METERS, &message, sizeof(message)) >= 0)
        s_mix_meter_sent = s_mix_meter_main.sequence;
}

void OnGlobalLfoOp(const GlobalLfoOpMessage& request) {
    s_global_lfo.Request(request);
}
void PumpSequencerState() {
    s_global_lfo.Pump(Comm::LinkSend);
    static SeqLockNoticeMessage notice;
    static bool notice_pending = false;
    notice_pending |= s_lock_notice.ConsumeLatest(notice);
    if (notice_pending && Comm::LinkSend(MSG_SEQ_LOCK_NOTICE, &notice, sizeof(notice)) >= 0)
        notice_pending = false;
    SeqClockOutMessage clock;
    for (unsigned i = 0; i < 8 && s_seq_transport.PopClockOut(clock); ++i) {
        if (WaveX::Comm::LinkSend(MSG_SEQ_CLOCK_OUT, &clock, sizeof(clock)) < 0) {
            s_seq_transport.ClockOutFailed(clock);
            break;
        }
    }
    static SeqNotesMessage notes;
    static bool notes_pending = false;
    notes_pending |= s_seq_notes_mailbox.ConsumeLatest(notes);
    if (notes_pending && WaveX::Comm::LinkSend(MSG_SEQ_NOTES, &notes, sizeof(notes)) >= 0)
        notes_pending = false;
    WaveX::PatternStore::Pump(s_pattern_exchange_storage.Get());
    auto& page = s_seq_page_pending_storage.Get();
    auto& head = s_seq_head_pending_storage.Get();
    s_seq_page_pending |= s_seq_page_mailbox.ConsumeLatest(page);
    s_seq_head_pending |= s_seq_head_mailbox.ConsumeLatest(head);
    if (s_seq_page_pending) {
        const int sent =
            page.scoped ? WaveX::Comm::LinkSend(MSG_SEQ_SLOT_PAGE, &page.value, sizeof(page.value))
                        : WaveX::Comm::LinkSend(
                              MSG_SEQ_PATTERN_SYNC, &page.value.page, sizeof(page.value.page));
        if (sent < 0)
            return;
        s_seq_page_pending = false;
    }
    if (s_seq_head_pending && WaveX::Comm::LinkSend(MSG_SEQ_PLAYHEAD, &head, sizeof(head)) >= 0)
        s_seq_head_pending = false;
}

void OnSeqPatternOp(const SeqPatternOpMessage& m) {
    if (WaveX::PatternStore::BlocksEdits())
        return;
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::PatternOp;
    command.pattern_op = m;
    EnqueueSequencerCommand(command);
}

void TakeMidiDiagnostics(DiagPushMessage& out) {
    const auto sat = [](uint32_t n) {
        return static_cast<uint16_t>(std::min<uint32_t>(n, UINT16_MAX));
    };
    out.midi_notes = sat(s_midi_notes);
    out.midi_ccs = sat(s_midi_ccs);
    out.midi_clock_ticks = sat(s_midi_clocks);
    s_midi_notes = s_midi_ccs = s_midi_clocks = 0;
    const auto& head = s_seq_head_pending_storage.Get();
    out.measured_bpm_x100 = head.measured_bpm_x100;
    out.sync_state = head.sync_state;
    out.transport_playing = head.playing;
    out.pattern = head.pattern;
    out.step = head.step;
}
void OnMidiClockEvent(const MidiClockEventMessage& m) {
    if (!IsValidMidiClockEvent(m))
        return;
    if (m.event == MIDI_CLK_TICK)
        ++s_midi_clocks;
    if (ProjectBusy() && (s_project_session.Get()->Status().active_op != PROJECT_SAVE_COPY ||
                          (m.event != MIDI_CLK_TICK && m.event != MIDI_CLK_STOP)))
        return;
    if (WaveX::PatternStore::BlocksEdits() &&
        (m.event == MIDI_CLK_START || m.event == MIDI_CLK_CONTINUE))
        return;
    WaveX::Sequencer::SequencerCommand command;
    command.type = WaveX::Sequencer::SequencerCommandType::MidiClock;
    command.midi_clock = m;
    EnqueueSequencerCommand(command);
}

static uint16_t MidiDestinations(uint8_t channel) {
    uint8_t tracks[kNumTracks];
    const auto count = SfzLoader::TracksForMidiChannel(channel, tracks, kNumTracks);
    uint16_t mask = 0;
    for (uint8_t i = 0; i < count; ++i)
        mask |= static_cast<uint16_t>(1u << tracks[i]);
    return mask;
}
void OnMidiCc(const MidiCcMessage& m) {
    if (!IsValidMidiCc(m))
        return;
    ++s_midi_ccs;
    if (m.cc == 1)
        s_midi_modulation.Wheel(MidiDestinations(m.channel), m.value);
    else if (m.cc == 121)
        s_midi_modulation.Reset(MidiDestinations(m.channel));
}
void OnMidiPressure(const MidiPressureMessage& m) {
    if (IsValidMidiPressure(m))
        s_midi_modulation.Pressure(MidiDestinations(m.channel), m.value);
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

static uint8_t LiveSource(uint8_t address) {
    return static_cast<uint8_t>(NoteAddressIndex(address) + (NoteAddressesTrack(address) ? 16 : 0));
}

void OnNoteOn(const NoteMessage& note_msg) {
    if (note_msg.note > 127 || note_msg.velocity > 127 || (note_msg.channel & 0x70u) != 0)
        return;
    if (!note_msg.velocity) {
        OnNoteOff(note_msg);
        return;
    }
    if (!NoteAddressesTrack(note_msg.channel))
        ++s_midi_notes;
    uint8_t tracks[kNumTracks];
    const uint8_t n = RouteNote(note_msg, tracks, kNumTracks);
    uint16_t destinations = 0;
    for (uint8_t i = 0; i < n; ++i)
        if (!SfzLoader::TrackLoading(tracks[i]) && SfzLoader::TrackLoaded(tracks[i]))
            destinations |= static_cast<uint16_t>(1u << tracks[i]);
    if (!s_live_notes.Press(
            LiveSource(note_msg.channel), note_msg.note, note_msg.velocity, destinations))
        WaveX::Log::PrintLine("RX NOTE_ON: source=%u note=%u DROPPED - note queue full (%lu)",
                              unsigned(note_msg.channel),
                              unsigned(note_msg.note),
                              static_cast<unsigned long>(s_live_notes.Refused()));
}

void OnNoteOff(const NoteMessage& note_msg) {
    if (note_msg.note > 127 || note_msg.velocity > 127 || (note_msg.channel & 0x70u) != 0)
        return;
    if (!NoteAddressesTrack(note_msg.channel))
        ++s_midi_notes;
    // Original source and FIFO press identity survive routing/binding changes.
    // Never resolve destinations again and never release by pitch alone.
    s_live_notes.Release(LiveSource(note_msg.channel), note_msg.note);
}

// Retired legacy transport commands. Recording uses correlated MSG_REC_OP.
void OnSampleCtrl(const SampleCtrlMessage& sc) {
    if (s_hw)
        WaveX::Log::PrintLine("SAMPLE_CTRL cmd=%u retired; recording uses REC_OP",
                              (unsigned)sc.cmd);
}

// How a sample's channels map onto what the display asks for. Resolved in
// one place so the envelope job cannot disagree with the playback paths
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

// ============================
// Waveform envelope job (roadmap 1.5.5 item 2)
// ============================
//
// The scan owns one packet and a partial-column cursor. Each pump reads a
// bounded number of PCM values, even for a one-column whole-file request.
// Only the main loop touches it; sample retirement cancels before freeing PCM.
static EnvelopeScan s_env_scan;

static void CancelEnvelopeJob() {
    s_env_scan.Cancel();
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

    WaveX::Protocol::EnvelopeChunkMessage identity;
    identity.sample_id = info->sample_id;
    identity.generation = info->meta.generation;
    identity.start_frame = start;
    identity.end_frame = end;
    identity.total_columns = static_cast<uint16_t>(columns);
    identity.channels = plan.out_channels;
    s_env_scan.Begin(identity, req.end_frame);
}

void PumpEnvelopeJob() {
    if (SampleLoadBusy())
        return;
    if (!s_env_scan.Active()) {
        return;
    }

    // Re-resolve before either scanning OR sending a retained packet.
    const auto& identity = s_env_scan.Identity();
    LoadedSampleInfo* info = find_loaded_sample(identity.sample_id);
    void* base = nullptr;
    if (!info || info->meta.generation != identity.generation ||
        !s_sample_mem_mgr.ptr(info->handle, &base) || base == nullptr) {
        CancelEnvelopeJob();
        return;
    }
    const DisplayChannelPlan plan = ResolveDisplayChannels(*info, /*allow_stereo=*/true);
    if (plan.out_channels != identity.channels) {
        CancelEnvelopeJob();
        return;
    }

    s_env_scan.Pump(
        plan.sum ? 2 : plan.out_channels,
        [&](uint32_t frame, uint8_t channel) {
            const int32_t value = plan.sum ? (EnvReadSample(*info, base, frame, 0) +
                                              EnvReadSample(*info, base, frame, 1)) /
                                                 2
                                           : EnvReadSample(*info, base, frame, plan.pick[channel]);
            return static_cast<int16_t>(value);
        },
        [](const EnvelopeScan::Packet& packet, uint16_t bytes) {
            // Never stack waveform frames behind normal traffic or another
            // waveform frame. A queued packet includes the active DMA frame.
            if (!WaveX::Comm::LinkTxIdle()) {
                return false;
            }
            const bool sent =
                WaveX::Comm::LinkSend(WaveX::Protocol::MSG_ENVELOPE_CHUNK, &packet, bytes) >= 0;
            if (sent) {
                WaveX::Comm::LinkPumpTx();
            }
            return sent;
        });
}

bool LoadSfzInstrument(const char* path, uint8_t slot) {
    if (!s_pool || SampleLoadBusy()) {
        return false;
    }
    return SfzLoader::Load(path, slot, *s_pool, s_sample_mem_mgr, s_sample_io, sizeof(s_sample_io));
}

void OnTrackStateRequest(const TrackStateRequest& request) {
    SfzLoader::OnTrackStateRequest(request);
}
static void PublishInstrumentSound(uint8_t track) {
    PublishSequencerVoiceMap(static_cast<uint16_t>(1u << track));
    const auto* filter = SfzLoader::GetInstrumentFilter(track);
    const auto* env = SfzLoader::GetInstrumentEnv(track);
    if (filter && env)
        s_track_live_updates.Publish(ComposeTrackLive(track, *filter, *env));
}
void OnAllocationOp(const AllocationOpMessage& request) {
    if (SfzLoader::OnAllocationOp(request))
        PublishInstrumentSound(request.track);
}
void OnEditOp(const InstEditOpMessage& request) {
    if (SfzLoader::OnEditOp(request))
        PublishInstrumentSound(request.track);
}
void OnArpOp(const InstArpOpMessage& request) {
    if (SfzLoader::OnArpOp(request))
        PublishInstrumentSound(request.track);
}
void OnLfoOp(const InstLfoOpMessage& request) {
    if (SfzLoader::OnLfoOp(request))
        PublishInstrumentSound(request.track);
}
void OnModOp(const InstModOpMessage& request) {
    if (SfzLoader::OnModOp(request))
        PublishInstrumentSound(request.track);
}
void OnOscOp(const InstOscOpMessage& request) {
    if (SfzLoader::OnOscOp(request)) {
        PublishInstrumentSound(request.track);
        PushTrackBinding(request.track);
    }
}
void OnKeyMapOp(const InstKeyMapOpMessage& request) {
    if (SfzLoader::OnKeyMapOp(request)) {
        PublishSequencerVoiceMap(static_cast<uint16_t>(1u << request.track));
        PushTrackBinding(request.track);
    }
}
void OnPadSoundOp(const InstPadSoundOpMessage& request) {
    if (SfzLoader::OnPadSoundOp(request))
        PublishSequencerVoiceMap(static_cast<uint16_t>(1u << request.track));
}

void OnInstrumentOp(const InstOpMessage& request) {
    if (s_recording.Get().Busy() || SampleLoadBusy()) {
        InstStatusMessage status;
        status.request_id = request.request_id;
        status.slot = request.slot;
        status.op = request.op;
        status.state = INST_STATUS_FAILED;
        status.error = INST_ERROR_BUSY;
        Comm::LinkSend(MSG_INST_STATUS, &status, sizeof(status));
        return;
    }
    if (request.op == INST_OP_SAVE && !SfzLoader::Busy())
        CloseWav();
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
    if (SfzLoader::Begin(request) &&
        (request.op == INST_OP_NEW_KEYBOARD || request.op == INST_OP_SFZ_LOAD ||
         request.op == INST_OP_NEW || request.op == INST_OP_SET_PAD_SAMPLE)) {
        ClearSequencerVoiceMap(static_cast<uint16_t>(1u << request.slot));
        // Streaming audition and instrument import share FatFs/SD bandwidth.
        // A load owns storage until its cooperative state machine completes.
        CloseWav();
    }
    if (request.op == INST_OP_SET_NAME)
        PushTrackBinding(request.slot);
}

void PumpInstrumentLoad() {
    if (BankBusy() || ProjectBusy() || SfzLoader::ProjectLoadActive())
        return;
    SfzLoader::PumpEditorReply();
    if (!s_pool) {
        return;
    }
    static uint32_t stop_generation = 0;
    const uint8_t stop_track = SfzLoader::VoiceStopTrack();
    if (stop_track != 0xFF) {
        // Per-track barrier: only the Track being (re)loaded stops; what it
        // held is released on the callback's acknowledgement.
        if (stop_generation == 0) {
            ClearSequencerVoiceMap(static_cast<uint16_t>(1u << stop_track));
            stop_generation =
                s_voice_stop_fence.RequestStop(static_cast<uint16_t>(1u << stop_track));
            return;
        }
        if (s_voice_stop_fence.Complete(stop_generation)) {
            SfzLoader::ConfirmVoicesStopped(*s_pool, s_sample_mem_mgr);
            if (!SfzLoader::Busy()) {
                PublishSequencerVoiceMap();
                PushTrackBinding(stop_track);
            }
            stop_generation = 0;
        }
        return;
    }
    stop_generation = 0;
    const bool was_busy = SfzLoader::Busy();
    SfzLoader::Pump(*s_pool, s_sample_mem_mgr, s_sample_io, sizeof(s_sample_io));
    // The loader publishes its new slot binding only when its state machine
    // reaches Idle. Rebuild the callback-owned snapshot at that transition;
    // rebuilding during the load would expose incomplete sample pointers.
    // An import finishing (or failing) changes what its Track holds; push
    // every binding so the frontend's cache follows without asking.
    if (was_busy && !SfzLoader::Busy()) {
        PublishSequencerVoiceMap();
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
    WaveX::Comm::LinkSend(WaveX::Protocol::MSG_SAMPLE_STATUS, &status, sizeof(status));
}

bool SampleLoadBusy() {
    return s_sample_load.Get().Busy() || s_sample_load.Get().ReplyPending();
}

void OnSampleLoad(const SampleLoadMessage& sl) {
    if (!s_sample_memory_available || !s_pool) {
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_NO_SDRAM);
        return;
    }
    if (StorageJobBusy() || Storage::CardService::Busy() || !s_sample_load.Get().Begin(sl)) {
        ReportSampleLoadFailed(sl.sample_id, SAMPLE_LOAD_FAIL_BUSY);
        return;
    }
    if (!s_sample_load.Get().Busy())
        return;  // invalid path: the job retains its terminal failure reply
    // Admission owns the read-only file and private allocation, not live
    // voices. Return to dispatch immediately; no delay or nested link pump.
    if (!s_pool->FindByPath(sl.path)) {
        CloseWav();
        CancelEnvelopeJob();
    }
    s_sample_load_started_ms = System::GetNow();
}

void PumpSampleLoad() {
    auto& job = s_sample_load.Get();
    const bool was_busy = job.Busy();
    if (was_busy && s_pool)
        job.Pump(*s_pool, s_sample_mem_mgr, s_sample_io, sizeof(s_sample_io));
    if (was_busy && !job.Busy()) {
        const auto& status = job.Status();
        if (status.state == SAMPLE_STATUS_LOAD_COMPLETE) {
            if (const auto* info = find_loaded_sample(status.sample_id))
                PushSampleMeta(*info);
            WaveX::Log::PrintLine(
                "SAMPLE_LOAD: loaded id=%u bytes=%lu in %lu ms",
                static_cast<unsigned>(status.sample_id),
                static_cast<unsigned long>(job.BytesRead()),
                static_cast<unsigned long>(System::GetNow() - s_sample_load_started_ms));
        } else {
            WaveX::Log::PrintLine("SAMPLE_LOAD: failed request=%u reason=%lu after %lu bytes",
                                  static_cast<unsigned>(status.sample_id),
                                  static_cast<unsigned long>(status.frames_played),
                                  static_cast<unsigned long>(job.BytesRead()));
        }
    }
    // A full UART queue must not lose the terminal result and strand the UI.
    if (job.ReplyPending() &&
        Comm::LinkSend(MSG_SAMPLE_STATUS, &job.Status(), sizeof(job.Status())) >= 0)
        job.ReplySent();
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
    out = ReadCallbackTelemetry().meters;
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
    return ReadCallbackTelemetry().blocks;
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
    return ReadCallbackTelemetry().cpu_avg;
}

float GetMinCpuLoad() {
    return ReadCallbackTelemetry().cpu_min;
}

float GetMaxCpuLoad() {
    return ReadCallbackTelemetry().cpu_max;
}

float GetBlockPeriodMs() {
    return 1000.0f * (float)s_block_size / s_sample_rate;  // Block period in milliseconds
}

// ============================
// WAV playback implementation
// ============================

static void ApplyMetaToStreaming(const LoadedSampleInfo* info);

bool AuditionSample(uint16_t sample_id) {
    const LoadedSampleInfo* info = find_loaded_sample(sample_id);
    if (!info || info->path[0] == '\0') {
        return false;
    }
    SetLoopGapMs(0);
    return OpenWav(info->path);
}

bool OpenWav(const char* path) {
    if (SampleLoadBusy())
        return false;
    // Reject an unrepresentable identity before replacing a working stream.
    if (!path || path[0] == '\0' || std::strlen(path) >= sizeof(s_wav.path)) {
        return false;
    }
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
    // Every open starts with this file's own metadata. Gain, fades and
    // markers from the previous file must never bleed into another audition.
    SamplePool::Record* record = s_pool ? s_pool->FindByPath(path) : nullptr;
    LoadedSampleInfo standalone{};
    if (!record) {
        SampleFile::Document geometry;
        geometry.file_bytes = f_size(&s_wav.file);
        geometry.data_offset = wav_info.data_offset;
        auto& meta = geometry.sample;
        meta.sample_rate = wav_info.sample_rate;
        meta.total_frames =
            wav_info.data_size / (wav_info.num_channels * (wav_info.bits_per_sample / 8u));
        meta.channels = static_cast<uint8_t>(wav_info.num_channels);
        meta.bits_per_sample = static_cast<uint8_t>(wav_info.bits_per_sample);
        meta.Resolve();
        standalone.meta = meta;
        std::strcpy(standalone.path, path);
        const auto sidecar = Storage::ReadSampleSidecar(path, geometry, standalone.meta);
        if (sidecar == Storage::SampleSidecarResult::Invalid ||
            sidecar == Storage::SampleSidecarResult::IoError) {
            CloseWav();
            return false;
        }
    }
    ApplyMetaToStreaming(record ? &record->payload : &standalone);
    if (!PrepareWavCrossfade() || f_lseek(&s_wav.file, s_wav.region_start) != FR_OK) {
        CloseWav();
        return false;
    }
    const uint32_t stop = s_wav.loop_enabled ? s_wav.loop_end : s_wav.region_end;
    s_wav.bytes_remaining = stop - s_wav.region_start;

    // Reset buffers. CloseWav() above already cleared s_rb_live and no
    // producer call (rb_push_frames) runs between here and there, so the
    // consumer is guaranteed to still be treating the ring as empty - these
    // stores can't race rb_pop_stereo_batch(). Keep the ring unpublished until
    // PumpWavIO transfers the prebuffer: an empty ring is not ready to play.
    __atomic_store_n(&s_rb_head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_rb_tail, 0u, __ATOMIC_RELEASE);

    // Logged unconditionally: once per file open, so it cannot spam, and it
    // is the only place the per-file variables are visible. When some files
    // play cleanly and others of the SAME format do not, the difference has
    // to be here.
    //
    // Frame alignment is relative to data_start, not the file origin: a
    // legal word-aligned RIFF payload need not be aligned to a stereo frame.
    // Whole-frame read sizes preserve channel order from that exact offset.
    // Sector alignment can affect FatFs read cost, not PCM channel identity.
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
    if (++s_stream_epoch == 0)
        ++s_stream_epoch;
    s_resampler_history_frame = SourceFrameWalk::kSilent;

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
    callback_cycles = ReadCallbackTelemetry().cycles;
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

        rb_push_frames(s_prebuffer, frames_to_transfer, {}, s_prebuffer_source_frames);
        // Publish only after the initial audio is visible to the consumer.
        // OpenWav leaves the callback silent throughout SD prebuffering.
        __atomic_store_n(&s_rb_live, true, __ATOMIC_RELEASE);

        s_prebuffer_filled -= frames_to_transfer;
        if (s_prebuffer_filled > 0) {
            memmove(s_prebuffer_source_frames,
                    s_prebuffer_source_frames + frames_to_transfer,
                    s_prebuffer_filled * sizeof(uint32_t));
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
    ApplyWavCrossfade(conversion_output,
                      frames_to_transfer,
                      (slot.file_offset - s_wav.data_start) / file_bpf + slot.consumed);
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
    const uint32_t source_first = (block_file_pos - s_wav.data_start) / file_bpf;
    SourceFrameWalk walk{
        source_first,
        s_resampler_history_frame,
        resample_ratio != 1.0f && resampler_before.has_history ? resampler_before.phase : 0.0f,
        1.0f / resample_ratio,
        false};
    rb_push_frames(final_buffer, final_frames, walk);
    s_resampler_history_frame = source_first + frames_to_transfer - 1;
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

void OnSampleSeam(const SampleSeamRequest& request) {
    if (!IsValidSampleSeamRequest(request))
        return;
    SampleSeamStatus reply;
    reply.request_id = request.request_id;
    reply.sample_id = request.expected.sample_id;
    auto* info = find_loaded_sample(reply.sample_id);
    const auto ref = ResolveLoadedSample(nullptr, reply.sample_id);
    if (SampleFileBusy() || ProjectBusy() || BankBusy() || SfzLoader::Busy() ||
        s_recording.Get().Owns(reply.sample_id)) {
        reply.error = SAMPLE_SEAM_BUSY;
    } else if (!info || !ref.valid()) {
        reply.error = SAMPLE_SEAM_MISSING;
    } else {
        auto& m = info->meta;
        reply.generation = m.generation;
        reply.frame = SampleLoop::Marker(m, request.marker);
        if (!SampleLoop::Matches(m, request)) {
            reply.error = SAMPLE_SEAM_STALE;
        } else if (request.action == SAMPLE_SEAM_SNAP) {
            uint32_t frame = reply.frame;
            if (!SampleLoop::Snap(ref.data, m, request.marker, request.radius, frame)) {
                reply.error = SAMPLE_SEAM_NO_CROSSING;
            } else {
                reply.moved = frame != reply.frame;
                reply.frame = frame;
                auto candidate = m;
                SampleLoop::SetMarker(candidate, request.marker, frame);
                SetEditParams(m.sample_id,
                              candidate.loop_enabled,
                              candidate.gain_db_x10,
                              candidate.start_frame,
                              candidate.end_frame,
                              candidate.loop_start,
                              candidate.loop_end,
                              candidate.fade_in_ms,
                              candidate.fade_out_ms,
                              candidate.loop_crossfade_ms,
                              candidate.channel_mode);
            }
        }
        SampleLoop::Measure(ref.data, m, reply);
        PushSampleMeta(*info);
    }
    // Losing this reply is a timeout, never permission to repeat the snap.
    Comm::LinkSend(MSG_SAMPLE_SEAM_STATUS, &reply, sizeof(reply));
}

// Applies an edit to the sample's record, then pushes the result back. The
// backend clamps and is the authority; the frontend is told what was applied
// rather than assuming its request was taken verbatim.
void SetEditParams(uint16_t sample_id,
                   bool loop_enabled,
                   int16_t gain_db_x10,
                   uint32_t start_frame,
                   uint32_t end_frame,
                   uint32_t loop_start_frame,
                   uint32_t loop_end_frame,
                   uint16_t fade_in_ms,
                   uint16_t fade_out_ms,
                   uint8_t loop_crossfade_ms,
                   uint8_t channel_mode) {
    // The Pool id, and only that. This used to fall back to the newest
    // sample for id 0, which hid the frontend truncating every id to one
    // byte: each edit "worked", on the wrong sample. An unknown id is a
    // frontend bug or a sample unloaded under it; either way, touching some
    // other record would be worse than doing nothing.
    LoadedSampleInfo* info = sample_id ? find_loaded_sample(sample_id) : nullptr;
    if (!info || channel_mode > SAMPLE_CH_MONO_SUM) {
        if (s_hw) {
            WaveX::Log::PrintLine("SAMPLE_EDIT: no sample for id=%u", (unsigned)sample_id);
        }
        return;
    }

    if (s_recording.Get().Owns(sample_id)) {
        PushSampleMeta(*info);
        return;
    }
    auto& m = info->meta;
    if (gain_db_x10 < -240) {
        gain_db_x10 = -240;
    } else if (gain_db_x10 > 120) {
        gain_db_x10 = 120;
    }
    if (m.channel_mode != channel_mode) {
        m.channel_mode = channel_mode;
        ++m.generation;  // channel mapping changes the cached waveform
    }
    m.loop_crossfade_ms = std::min(loop_crossfade_ms, kMaxLoopCrossfadeMs);
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
        rate ? static_cast<uint32_t>((static_cast<uint64_t>(m.end_frame - m.start_frame) * 1000u) /
                                     rate)
             : 0u;
    m.fade_in_ms = static_cast<uint16_t>(std::min<uint32_t>(fade_in_ms, span_ms));
    m.fade_out_ms = static_cast<uint16_t>(std::min<uint32_t>(fade_out_ms, span_ms));
    PushSampleMeta(*info);

    // Discard queued PCM made from the old markers/head before adopting edits.
    // Re-open owns the complete stream transition and preserves resampler epoch.
    if (s_wav.open && info->path[0] && std::strcmp(info->path, s_wav.path) == 0) {
        char path[BROWSE_PATH_MAX];
        std::strcpy(path, info->path);
        OpenWav(path);
    }
    PublishSequencerVoiceMap();
}

// Mirrors a record onto the streaming reader's byte offsets. Called whenever
// either the record or the open file changes, so the two cannot drift.
static void ApplyMetaToStreaming(const LoadedSampleInfo* info) {
    if (!s_wav.open ||
        (info && (info->path[0] == '\0' || std::strcmp(info->path, s_wav.path) != 0))) {
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
    s_wav.gain_q13 = SampleGainQ13(m.gain_db_x10);
    s_wav.channel_mode = m.channel_mode;
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
    s_wav.crossfade_frames =
        m.loop_enabled
            ? SampleLoop::CrossfadeFrames(m.loop_crossfade_ms, file_rate, m.loop_end - m.loop_start)
            : 0;
    s_wav.crossfade_step = SampleLoop::RampStep(s_wav.crossfade_frames);
    if (s_wav.crossfade_frames)
        s_wav.fade_in_frames =
            std::min(s_wav.fade_in_frames, m.loop_start + s_wav.crossfade_frames - m.start_frame);
    s_wav.fade_out_frames =
        s_wav.crossfade_frames ? 0 : WaveX::AudioEngine::FadeFrames(m.fade_out_ms, file_rate);

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
