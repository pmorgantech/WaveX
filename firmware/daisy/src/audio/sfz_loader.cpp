#include "sfz_loader.hpp"

#include <strings.h>  // for strcasecmp

#include "comm/log_ring.h"
#include "comm/mcu_link.h"
#include "config/hardware_config.h"
#include "ff.h"
#include "memory.h"
#include "memory_sections.h"

#include "bss_static.hpp"
#include "instrument_map.hpp"
#include "instrument_sound_undo.hpp"
#include "kit_edit.hpp"
#include "sample_load_info.hpp"
#include "sfz_import.hpp"
#include "snapshot_mailbox.hpp"
#include "storage/card_space.hpp"
#include "storage/fatfs_wav_reader.hpp"
#include "storage/sample_file_job.hpp"
#include "wav/wav_header_parser.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace WaveX {
namespace AudioEngine {
namespace SfzLoader {
namespace {

using namespace WaveX::Protocol;

// One per plan entry (one per distinct sample file the import references).
// A file already in the Pool is a `hit`: nothing is allocated or read for
// it, the Track just takes a ref. Otherwise the entry is admitted to the
// Pool (`pool_id`), allocated (`handle`) and read, and only committed - made
// visible, pinned to the Track - when the whole import succeeds.
struct LoadedSample {
    wxsamp_t handle = {};
    ResidentSampleInfo resident = {};
    uint32_t data_offset = 0;
    uint16_t pool_id = 0;
    bool hit = false;       // already resident before this load
    bool admitted = false;  // a fresh Pool record this load must clean up on failure
};

enum class Phase : uint8_t {
    Idle,
    OpenSfz,
    ParseLine,
    FinishParse,
    // The .wxi front half. One phase, not three: an Instrument document is a
    // few KB read sequentially, so it is one pass rather than the SFZ path's
    // line-at-a-time walk. Main-loop context, comparable to PumpWavIO's
    // existing blocking read - the audio callback is untouched either way.
    ReadWxi,
    ProbeSample,
    AwaitVoiceStop,
    AllocateSample,
    OpenSample,
    ReadSample,
    Commit,
    PrepareSave,
    ProbeSaveSample,
    SaveCopy,
};

// Tracks are foreground-owned. Construct raw cacheable D2 storage only
// from Reset(), after System initialization has enabled that RAM. The SRAM
// debug layout instead uses its spare AXI range. Keeping
// these expanded Instruments out of AXI SRAM preserves SD buffers and heap.
alignas(Tracks) static uint8_t s_bank_storage[sizeof(Tracks)] WAVEX_BACKGROUND_DATA;
static Tracks* s_bank = nullptr;
static Tracks* s_project_bank = nullptr;
static bool s_in_project_step = false;
static bool s_project_samples_only = false;
static bool s_project_close_failed = false;
static bool s_project_failed = false;
static InstrumentSoundUndo s_sound_undo[kNumTracks];
static InstEditSyncMessage s_action_reply;
static uint32_t s_action_completed[kNumTracks]{};
static uint8_t s_action_error[kNumTracks]{};
static bool s_action_pending = false;
static AllocationSyncMessage s_allocation_reply;
static bool s_allocation_pending = false;
static uint32_t s_allocation_completed[2][kNumTracks]{};
static uint8_t s_allocation_error[2][kNumTracks]{};
static Allocation::Override s_allocation_undo[kNumTracks];
static bool s_allocation_dirty[kNumTracks]{};
using ModTable = std::array<ModSlot, kMaxModSlots>;
static SnapshotMailbox<ModTable> s_mod_mailboxes[kNumTracks];
static ModTable s_mod_active[kNumTracks];  // callback-owned after Reset()

static void PublishModSlots(uint8_t track) {
    if (s_in_project_step)
        return;
    ModTable slots;
    const ModSlot* stored = s_bank->At(track).instrument.mod_slots;
    std::copy(stored, stored + kMaxModSlots, slots.begin());
    s_mod_mailboxes[track].Publish(slots);
}
// The one resolver, over the Pool. Registered by the engine because the
// allocator that turns a handle into a pointer lives there; a
// default-constructed one resolves nothing, so an unregistered engine drops
// rather than crashes.
static SampleResolver s_loaded_resolver;
static LoadedSample s_loaded_samples[kMaxInstrumentZones];

// Two oscillators of zone paths, so this is resident like
// the bank and the mapped instrument rather than a stack local (wxi.hpp says
// as much where the struct is declared).
static WaveX::BssStatic<Wxi::InstrumentFile> s_doc_storage;

// FatFs behind the WXCF container's read callback. Write is null: the loader
// only reads, and a null write is a clearer failure than a stub that lies.
static bool WxiReadCb(void* user, void* dest, size_t len) {
    auto* file = static_cast<FIL*>(user);
    UINT read = 0;
    if (f_read(file, dest, static_cast<UINT>(len), &read) != FR_OK)
        return false;
    return read == len;  // a short read is EOF, which the container expects
}

static bool WxiEofCb(void* user) {
    return f_eof(static_cast<FIL*>(user));
}

static InstOpMessage s_request;
static InstStatusMessage s_status;
static Phase s_phase = Phase::Idle;
static Sfz::Parser s_parser;
static WaveX::BssStatic<Sfz::MappedInstrument> s_mapped_storage;
static Sfz::MappedInstrument& s_mapped = s_mapped_storage.Get();
static Sfz::SamplePlan s_plan;
static Sfz::SampleProbe s_probes[kMaxInstrumentZones];
static char s_line[Sfz::kMaxLine];
static FIL s_file;
static bool s_file_open = false;
static bool s_project_snapshot = false;
static bool s_bank_snapshot = false;
static uint8_t s_snapshot_error = INST_ERROR_NONE;
static char s_snapshot_path[Protocol::BROWSE_PATH_MAX]{};
static uint32_t s_line_number = 0;
static uint8_t s_index = 0;
static uint8_t s_allocated = 0;
static uint32_t s_total_bytes = 0;
static uint32_t s_loaded_bytes = 0;
static uint32_t s_current_written = 0;
static uint8_t s_last_percent = 0xFF;

// Depth-limited fallback search (docs/roadmap.md "SFZ import does not search
// subfolders for samples"). Sfz::detail::ResolvePath is SFZ-spec-correct -
// sample= relative to the .sfz, default_path= prepended - but real-world
// packs are routinely rearranged, moving the .sfz next to, or above, its own
// Samples/ folder. Only used when the resolved path does not open.
static constexpr uint8_t kFallbackSearchDepth = 2;  // levels of subfolders below the .sfz's own
static constexpr uint8_t kMaxFallbackSubdirs = 12;  // per directory level

// The directory the last fallback hit came from, tried first on the next
// probe: a multi-sample instrument's files are almost always all in one
// place, so this is what keeps a 32-sample instrument from walking the tree
// 32 times. Cleared per Begin(); empty means "nothing cached yet".
static char s_fallback_dir[Sfz::kMaxPath] = {};

// Recursion needs one subdirectory-name buffer per depth level, sized like
// fs_browse.cpp's directory listing (kept out of the main-loop stack for the
// same reason - see its own comment on all_entries).
static char s_fallback_subdirs[kFallbackSearchDepth + 1][kMaxFallbackSubdirs][Sfz::kMaxPath];

const char* Basename(const char* path) {
    if (!path)
        return "";
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* last = (!slash || (backslash && backslash > slash)) ? backslash : slash;
    return last ? last + 1 : path;
}

void SendStatus(uint8_t state, uint8_t error = INST_ERROR_NONE) {
    s_status.state = state;
    s_status.error = error;
    if (s_request.request_id != 0) {
        WaveX::Comm::LinkSend(MSG_INST_STATUS, &s_status, sizeof(s_status));
    }
}

void CloseFile() {
    if (s_file_open) {
        if (f_close(&s_file) != FR_OK && s_in_project_step)
            s_project_close_failed = true;
        s_file_open = false;
    }
}

// Frees `sample_id` from the Pool: audio memory back to the arena, record
// gone. Callers guarantee no voice is reading it (a stopped Track, or a
// record this load admitted and never committed).
void FreeFromPool(SamplePool& pool, SampleMemMgr& memory, uint16_t sample_id) {
    SamplePool::Record* r = pool.Find(sample_id);
    if (!r)
        return;
    memory.release(&r->payload.handle);
    pool.Remove(sample_id);
}

static_assert(kMaxZones == INST_KEY_ZONE_COUNT, "Key Map covers every Instrument zone");
// Main-loop identity: every map mutation/replacement invalidates stale editors.
static uint32_t s_key_revision[kNumTracks]{};
void BumpKeyRevision(uint8_t track) {
    if (s_in_project_step)
        return;
    if (++s_key_revision[track] == 0)
        ++s_key_revision[track];
}
// Drops Track `track`'s refs on every Pool sample and frees whatever nobody
// else holds; then clears the Instrument. The caller stopped the Track's
// voices first.
void ReleaseTrack(SamplePool& pool,
                  SampleMemMgr& memory,
                  uint8_t track,
                  uint16_t keep_sample_id = 0) {
    if (!s_in_project_step)
        s_sound_undo[track].Apply();
    BumpKeyRevision(track);
    pool.ClearTrack(track, [&](uint16_t id) {
        if (id != keep_sample_id) {
            FreeFromPool(pool, memory, id);
        }
    });
    // Zones only: the mod slots are the user's, set through their own op,
    // and rebinding what plays is not a reason to lose them.
    Instrument& ins = s_bank->At(track).instrument;
    for (auto& oscillator: ins.osc)
        for (auto& zone: oscillator.zones) {
            zone = Zone{};
        }
    ins.origin = InstrumentOrigin::None;
    ins.name[0] = '\0';
}

// Undo everything this load admitted or allocated but did not commit.
void AbandonLoad(SamplePool* pool, SampleMemMgr* memory) {
    for (uint8_t i = 0; i < s_plan.count && i < kMaxInstrumentZones; ++i) {
        LoadedSample& ls = s_loaded_samples[i];
        if (ls.admitted && pool && memory) {
            if (ls.handle.len) {
                memory->release(&ls.handle);
            }
            pool->Remove(ls.pool_id);
        }
        ls = LoadedSample{};
    }
    s_allocated = 0;
}

void Fail(SamplePool* pool, SampleMemMgr* memory, uint8_t error) {
    if (s_in_project_step)
        s_project_failed = true;
    CloseFile();
    AbandonLoad(pool, memory);
    if (s_project_snapshot) {
        s_snapshot_error = error;
        s_project_snapshot = false;
        s_phase = Phase::Idle;
        return;
    }
    SendStatus(INST_STATUS_FAILED, error);
    s_phase = Phase::Idle;
}

uint32_t AvailableBytes(SamplePool& pool, SampleMemMgr& memory, uint8_t track) {
    wxsamp_stats_t stats{};
    memory.stats(&stats);
    uint64_t free_bytes = static_cast<uint64_t>(stats.large_free_bytes) + stats.small_free_bytes;
    // Replacing what this Track holds frees the samples only it references;
    // they become available after the callback's stop acknowledgement.
    if (!s_project_samples_only)
        free_bytes += ReclaimableBytes(pool, track);
    if (free_bytes <= WAVEX_INST_LOAD_RESERVE_BYTES)
        return 0;
    free_bytes -= WAVEX_INST_LOAD_RESERVE_BYTES;
    return free_bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(free_bytes);
}

enum class ProbeResult : uint8_t { Ok, Missing, Invalid };

// The .sfz's own directory, the search root: same slash logic as
// Sfz::detail::ResolvePath, kept local rather than shared since that one is
// embedded inline there and this module already owns all FatFs access.
bool SfzDirectory(const char* sfz_path, char* out, size_t capacity) {
    const char* slash = std::strrchr(sfz_path, '/');
    const char* backslash = std::strrchr(sfz_path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
    if (!slash) {
        return false;
    }
    const size_t length = static_cast<size_t>(slash - sfz_path);
    return length > 0 && Sfz::detail::Copy(out, capacity, sfz_path, length);
}

// Looks for a file named `basename` (case-insensitive - FAT is) directly in
// `dir`, then recurses into each of `dir`'s subfolders while
// `subfolder_depth_remaining` allows. Subfolder names are collected in a
// second pass per level (indexed by depth in s_fallback_subdirs) so this
// directory's handle closes before any recursion opens another. Main-loop
// FatFs only - never call this from the audio callback.
bool SearchForSample(const char* dir,
                     const char* basename,
                     uint8_t subfolder_depth_remaining,
                     char* out,
                     size_t capacity) {
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) {
        return false;
    }

    auto& subdirs = s_fallback_subdirs[subfolder_depth_remaining];
    uint8_t subdir_count = 0;
    bool found = false;

    FILINFO fno;
#if defined(FF_USE_LFN) && FF_USE_LFN
    char lfn_buf[Sfz::kMaxPath];
    fno.lfname = lfn_buf;
    fno.lfsize = sizeof(lfn_buf);
#endif

    for (;;) {
        if (f_readdir(&d, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }
#if defined(FF_USE_LFN) && FF_USE_LFN
        const char* name = (fno.lfname && fno.lfname[0]) ? fno.lfname : fno.fname;
#else
        const char* name = fno.fname;
#endif
        if (fno.fattrib & AM_DIR) {
            if (subfolder_depth_remaining > 0 && subdir_count < kMaxFallbackSubdirs &&
                Sfz::detail::Copy(subdirs[subdir_count], Sfz::kMaxPath, dir) &&
                Sfz::detail::AppendPath(subdirs[subdir_count], Sfz::kMaxPath, name, true)) {
                ++subdir_count;
            }
            continue;
        }
        if (strcasecmp(name, basename) == 0 && Sfz::detail::Copy(out, capacity, dir) &&
            Sfz::detail::AppendPath(out, capacity, name, true)) {
            found = true;
            break;
        }
    }
    f_closedir(&d);

    if (found) {
        return true;
    }
    for (uint8_t i = 0; i < subdir_count; ++i) {
        if (SearchForSample(subdirs[i],
                            basename,
                            static_cast<uint8_t>(subfolder_depth_remaining - 1),
                            out,
                            capacity)) {
            return true;
        }
    }
    return false;
}

// Tries the cached hit directory first (a single, non-recursive directory
// scan), then a fresh depth-limited search from the .sfz's own directory,
// caching where that one landed for the next sample.
bool FindSampleFallback(const char* missing_path, char* out, size_t capacity) {
    const char* base = Basename(missing_path);
    if (!base || *base == '\0') {
        return false;
    }
    if (s_fallback_dir[0] != '\0' && SearchForSample(s_fallback_dir, base, 0, out, capacity)) {
        return true;
    }

    char sfz_dir[Sfz::kMaxPath];
    if (!SfzDirectory(s_request.path, sfz_dir, sizeof(sfz_dir))) {
        return false;
    }
    if (!SearchForSample(sfz_dir, base, kFallbackSearchDepth, out, capacity)) {
        return false;
    }
    const char* found_base = Basename(out);
    const size_t dir_len = found_base ? static_cast<size_t>(found_base - out) : 0;
    if (dir_len > 0 && dir_len <= sizeof(s_fallback_dir)) {
        std::memcpy(s_fallback_dir, out, dir_len - 1);  // drop AppendPath's trailing '/'
        s_fallback_dir[dir_len - 1] = '\0';
    }
    return true;
}

// Both recall and save validate the card file, even when its PCM is already
// resident. Pool residency cannot make an otherwise unsupported file recallable.
bool ReadInstrumentSampleInfo(uint16_t sample_id,
                              ResidentSampleInfo& resident,
                              uint32_t& data_offset) {
    WaveX::Wav::WavInfo wav_info;
    WaveX::Storage::FatFsWavReader reader(s_file);
    const auto parsed = WaveX::Wav::ParseWavHeader(reader, wav_info);
    SampleLoadMessage hint;
    hint.sample_id = sample_id;
    if (parsed != WaveX::Wav::ParseResult::Ok ||
        !BuildResidentSampleInfo(hint,
                                 wav_info,
                                 static_cast<uint32_t>(f_size(&s_file)),
                                 WAVEX_INST_MAX_RAM_SAMPLE_BYTES,
                                 resident))
        return false;
    data_offset = wav_info.data_offset;
    return true;
}

ProbeResult ProbeCurrent(SamplePool& pool) {
    char* path = s_mapped.sample_paths[s_plan.entries[s_index].path_zone];
    FRESULT fr = f_open(&s_file, path, FA_READ);
    if (fr != FR_OK) {
        char candidate[Sfz::kMaxPath];
        if (FindSampleFallback(path, candidate, sizeof(candidate)) &&
            f_open(&s_file, candidate, FA_READ) == FR_OK) {
            WaveX::Log::PrintLine(
                "SFZ_PROBE: sample %u fallback resolved to '%s'", (unsigned)s_index, candidate);
            Sfz::detail::Copy(path, Sfz::kMaxPath, candidate);
        } else {
            WaveX::Log::PrintLine(
                "SFZ_PROBE: sample %u missing (%d): '%s'", (unsigned)s_index, (int)fr, path);
            return ProbeResult::Missing;
        }
    }
    s_file_open = true;
    ResidentSampleInfo resident;
    uint32_t data_offset = 0;
    const bool valid =
        ReadInstrumentSampleInfo(s_plan.entries[s_index].sample_id, resident, data_offset);
    CloseFile();
    if (!valid) {
        WaveX::Log::PrintLine("SFZ_PROBE: sample %u unsupported: '%s'", (unsigned)s_index, path);
        return ProbeResult::Invalid;
    }
    LoadedSample& ls = s_loaded_samples[s_index];
    ls.resident = resident;
    ls.data_offset = data_offset;
    // Already in the Pool (the user loaded it, or another import did): a
    // hit costs no memory and no SD read, and its bytes are not counted
    // against what this load needs.
    if (const SamplePool::Record* r = pool.FindByPath(path)) {
        if (s_in_project_step && (r->payload.meta.total_frames != resident.total_frames ||
                                  r->payload.sample_rate != resident.sample_rate ||
                                  r->payload.channels != resident.channels ||
                                  r->payload.bit_depth != resident.bit_depth))
            return ProbeResult::Invalid;
        ls.hit = true;
        ls.pool_id = r->sample_id;
        s_probes[s_index].bytes = 0;
        return ProbeResult::Ok;
    }
    s_probes[s_index].bytes = resident.data_size;
    s_total_bytes += resident.data_size;
    return ProbeResult::Ok;
}

UINT PickReadChunk(const FIL& file, uint32_t io_buffer_bytes) {
    const UINT limit = static_cast<UINT>(io_buffer_bytes);
    if (!file.obj.fs || file.obj.fs->csize == 0)
        return limit;
    static constexpr UINT kSectorBytes = 512;
    const UINT cluster_bytes = static_cast<UINT>(file.obj.fs->csize) * kSectorBytes;
    if (cluster_bytes == 0 || cluster_bytes > limit)
        return limit;
    return (limit / cluster_bytes) * cluster_bytes;
}

void FillCurrentStatus() {
    s_status.current_index = s_index;
    s_status.current_bytes = s_loaded_samples[s_index].resident.data_size;
    s_status.current_loaded_bytes = s_current_written;
    WaveX::Protocol::detail::CopyWireString(
        s_status.current_name,
        sizeof(s_status.current_name),
        Basename(s_mapped.sample_paths[s_plan.entries[s_index].path_zone]));
}

// Main-loop editor state. A read is disposable, a mutation result is retained
// per Track so a dropped UART reply cannot turn a failed save into success.
static uint32_t s_edit_completed[kNumTracks] = {};
static uint8_t s_edit_error[kNumTracks] = {};
static InstZoneSyncMessage s_zone_reply;
static bool s_zone_pending = false;
static InstPadSoundSyncMessage s_sound_reply;
static bool s_sound_pending = false;
static InstModSyncMessage s_mod_reply;
static bool s_mod_pending = false;
static InstArpSyncMessage s_arp_reply;
static bool s_arp_pending = false;
static InstLfoSyncMessage s_lfo_reply;
static bool s_lfo_pending = false;
static InstOscSyncMessage s_osc_reply;
static bool s_osc_pending = false;
static InstKeyMapSyncMessage s_key_reply;
static InstKeyMapOpMessage s_key_request;
static bool s_key_pending = false, s_key_assignment = false;
static TrackStateMessage s_track_reply;
static bool s_track_pending = false;
static uint32_t s_sound_completed[kNumTracks] = {};
static uint8_t s_sound_error[kNumTracks] = {};

void QueueZoneReply(uint32_t id, uint8_t track, uint8_t immediate_error = 0) {
    s_zone_reply = InstZoneSyncMessage{};
    s_zone_reply.request_id = id;
    s_zone_reply.track = track;
    if (track >= kNumTracks) {
        s_zone_reply.error = INST_ERROR_BAD_FILE;
    } else {
        const auto& ins = s_bank->At(track).instrument;
        s_zone_reply.completed_request_id = s_edit_completed[track];
        s_zone_reply.error = s_edit_error[track];
        s_zone_reply.loaded = ins.origin != InstrumentOrigin::None;
        s_zone_reply.mode = static_cast<uint8_t>(ins.mode);
        s_zone_reply.editable = KitEdit::Editable(ins);
        s_zone_reply.busy = s_phase != Phase::Idle;
        std::memcpy(s_zone_reply.name, ins.name, sizeof(ins.name));
        for (uint8_t i = 0; i < INST_PAD_COUNT; ++i) {
            const auto& z = ins.osc[0].zones[i];
            s_zone_reply.pads[i].sample_id = z.in_use ? z.sample_id : 0;
            s_zone_reply.pads[i].choke_group = z.choke_group;
            s_zone_reply.pads[i].note = static_cast<uint8_t>(INST_PAD_FIRST_NOTE + i);
        }
    }
    if (immediate_error) {
        s_zone_reply.completed_request_id = id;
        s_zone_reply.error = immediate_error;
    }
    s_zone_pending = true;
}
void FillOscReply(const InstOscOpMessage& request,
                  InstOscSyncMessage& out,
                  uint8_t immediate_error = 0) {
    out = InstOscSyncMessage{};
    out.request_id = request.request_id;
    out.track = request.track;
    out.oscillator = request.oscillator;
    if (request.track >= kNumTracks || request.oscillator >= kNumOscillators) {
        out.error = INST_ERROR_BAD_FILE;
        return;
    }
    const auto& ins = s_bank->At(request.track).instrument;
    const auto& osc = ins.osc[request.oscillator];
    out.valid = ins.origin != InstrumentOrigin::None;
    out.busy = s_phase != Phase::Idle;
    out.revision = s_key_revision[request.track];
    out.completed_request_id = s_edit_completed[request.track];
    out.error = s_edit_error[request.track];
    out.type = static_cast<uint8_t>(osc.type);
    for (const auto& zone: osc.zones)
        if (zone.in_use)
            ++out.zones;
    out.value = {osc.level,
                 ins.osc_mix,
                 osc.coarse_tune,
                 osc.fine_tune,
                 osc.keytrack,
                 static_cast<uint8_t>(osc.mono)};
    if (immediate_error) {
        out.completed_request_id = request.request_id;
        out.error = immediate_error;
    }
}
void FillArpReply(const InstArpOpMessage& request,
                  InstArpSyncMessage& out,
                  uint8_t immediate_error = 0) {
    out = {};
    out.request_id = request.request_id;
    out.track = request.track;
    if (request.track >= kNumTracks) {
        out.error = INST_ERROR_BAD_FILE;
        return;
    }
    const auto& ins = s_bank->At(request.track).instrument;
    out.valid = ins.origin != InstrumentOrigin::None;
    out.busy = Busy();
    out.revision = s_key_revision[request.track];
    out.completed_request_id = s_edit_completed[request.track];
    out.error = s_edit_error[request.track];
    out.value = ins.arp;
    if (immediate_error) {
        out.completed_request_id = request.request_id;
        out.error = immediate_error;
    }
}
void QueueArpReply(const InstArpOpMessage& request, uint8_t error = 0) {
    FillArpReply(request, s_arp_reply, error);
    s_arp_pending = true;
}
void FillLfoReply(const InstLfoOpMessage& request,
                  InstLfoSyncMessage& out,
                  uint8_t immediate_error = 0) {
    out = {};
    out.request_id = request.request_id;
    out.track = request.track;
    if (request.track >= kNumTracks) {
        out.error = INST_ERROR_BAD_FILE;
        return;
    }
    const auto& ins = s_bank->At(request.track).instrument;
    out.valid = ins.origin != InstrumentOrigin::None;
    out.busy = Busy();
    out.revision = s_key_revision[request.track];
    out.completed_request_id = s_edit_completed[request.track];
    out.error = s_edit_error[request.track];
    for (uint8_t i = 0; i < INST_LFO_COUNT; ++i) {
        const auto& lfo = ins.lfo[i];
        out.values[i] = {lfo.wave,
                         lfo.sync_div,
                         lfo.retrigger,
                         lfo.pitch_follow,
                         lfo.rate_hz,
                         lfo.delay_s,
                         lfo.fade_s};
    }
    if (immediate_error) {
        out.completed_request_id = request.request_id;
        out.error = immediate_error;
    }
}
void QueueLfoReply(const InstLfoOpMessage& request, uint8_t error = 0) {
    FillLfoReply(request, s_lfo_reply, error);
    s_lfo_pending = true;
}
void FillModReply(const InstModOpMessage& request,
                  InstModSyncMessage& out,
                  uint8_t immediate_error = 0) {
    out = InstModSyncMessage{};
    out.request_id = request.request_id;
    out.track = request.track;
    if (request.track >= kNumTracks) {
        out.error = INST_ERROR_BAD_FILE;
        return;
    }
    const auto& ins = s_bank->At(request.track).instrument;
    out.valid = ins.origin != InstrumentOrigin::None;
    out.busy = Busy();
    out.revision = s_key_revision[request.track];
    out.completed_request_id = s_edit_completed[request.track];
    out.error = s_edit_error[request.track];
    for (uint8_t i = 0; i < INST_ENV_COUNT; ++i) {
        const auto& e = ins.env[i];
        out.envelopes[i] = {e.attack_s, e.decay_s, e.sustain, e.release_s};
    }
    for (uint8_t i = 0; i < INST_MOD_SLOT_COUNT; ++i) {
        const auto& slot = ins.mod_slots[i];
        out.slots[i] = {slot.source, slot.dest, slot.depth, slot.curve, slot.flags};
    }
    if (immediate_error) {
        out.completed_request_id = request.request_id;
        out.error = immediate_error;
    }
}
void QueueModReply(const InstModOpMessage& request, uint8_t error = 0) {
    FillModReply(request, s_mod_reply, error);
    s_mod_pending = true;
}
void QueueOscReply(const InstOscOpMessage& request, uint8_t immediate_error = 0) {
    FillOscReply(request, s_osc_reply, immediate_error);
    s_osc_pending = true;
}
void QueueKeyReply(uint32_t id,
                   uint8_t track,
                   uint8_t immediate_error = 0,
                   uint8_t oscillator = 0) {
    s_key_reply = InstKeyMapSyncMessage{};
    auto& out = s_key_reply;
    out.request_id = id;
    out.track = track;
    s_key_pending = true;
    if (track >= kNumTracks || oscillator >= kNumOscillators) {
        out.error = INST_ERROR_BAD_FILE;
        return;
    }
    const auto& ins = s_bank->At(track).instrument;
    out.revision = s_key_revision[track];
    out.completed_request_id = s_edit_completed[track];
    out.error = s_edit_error[track];
    out.loaded = ins.origin != InstrumentOrigin::None;
    out.mode = static_cast<uint8_t>(ins.mode);
    out.busy = s_phase != Phase::Idle;
    Protocol::detail::CopyWireString(out.name, sizeof(out.name), ins.name);
    for (uint8_t i = 0; i < INST_KEY_ZONE_COUNT; ++i) {
        const auto& z = ins.osc[oscillator].zones[i];
        out.zones[i] = {static_cast<uint16_t>(z.in_use ? z.sample_id : 0),
                        z.key_lo,
                        z.key_hi,
                        z.vel_lo,
                        z.vel_hi,
                        z.root_note};
    }
    if (immediate_error) {
        out.completed_request_id = id;
        out.error = immediate_error;
    }
}
void FinishKey(uint8_t error = INST_ERROR_NONE) {
    s_edit_completed[s_key_request.track] = s_key_request.request_id;
    s_edit_error[s_key_request.track] = error;
    s_key_assignment = false;
    s_phase = Phase::Idle;
    QueueKeyReply(s_key_request.request_id, s_key_request.track, 0, s_key_request.oscillator);
}
void FinishEdit(uint8_t error = INST_ERROR_NONE) {
    if (s_project_snapshot) {
        s_snapshot_error = error;
        s_project_snapshot = false;
        s_phase = Phase::Idle;
        return;
    }
    s_edit_completed[s_request.slot] = s_request.request_id;
    s_edit_error[s_request.slot] = error;
    s_phase = Phase::Idle;
    QueueZoneReply(s_request.request_id, s_request.slot);
}
bool SaveSamplePath(const void* context, uint16_t id, char* out, size_t size) {
    const auto* pool = static_cast<const SamplePool*>(context);
    const auto* record = pool->Find(id);
    if (!record || !record->payload.path[0])
        return false;
    Protocol::detail::CopyWireString(out, size, record->payload.path);
    return std::strlen(record->payload.path) < size;
}
bool WxiWriteCb(void* user, const void* source, size_t bytes) {
    // The codec writes sub-sector chunks. FatFs stages these through the
    // resident FIL sector buffer, never DMA from the codec's stack scratch.
    if (bytes >= 512)
        return false;
    UINT written = 0;
    return f_write(static_cast<FIL*>(user), source, static_cast<UINT>(bytes), &written) == FR_OK &&
           written == bytes;
}
uint8_t PrepareSave(SamplePool& pool) {
    auto& ins = s_bank->At(s_request.slot).instrument;
    if (ins.origin == InstrumentOrigin::None ||
        (!s_project_snapshot && !IsValidInstrumentName(s_request.path)))
        return INST_ERROR_BAD_FILE;
    s_doc_storage.Reconstruct();
    auto& doc = s_doc_storage.Get();
    if (!InstrumentMap::ToFile(ins, {&pool, SaveSamplePath}, doc))
        return INST_ERROR_MISSING_SAMPLES;
    if (!s_project_snapshot)
        Protocol::detail::CopyWireString(doc.name, sizeof(doc.name), s_request.path);
    else if (s_bank_snapshot && !doc.name[0])
        std::snprintf(doc.name, sizeof(doc.name), "Track %u", s_request.slot + 1);
    s_index = 0;
    s_phase = Phase::ProbeSaveSample;
    return INST_ERROR_NONE;
}

void ProbeSaveSample(SamplePool& pool) {
    const auto& doc = s_doc_storage.Get();
    while (s_index < kMaxInstrumentZones) {
        const auto& osc = doc.osc[s_index / kMaxZones];
        const uint8_t zone = s_index++ % kMaxZones;
        if (zone >= osc.zone_count)
            continue;
        const char* path = osc.zones[zone].path;
        const FRESULT opened = f_open(&s_file, path, FA_READ);
        if (opened != FR_OK) {
            FinishEdit(opened == FR_NO_FILE || opened == FR_NO_PATH ? INST_ERROR_MISSING_SAMPLES
                                                                    : INST_ERROR_IO);
            return;
        }
        s_file_open = true;
        ResidentSampleInfo resident;
        uint32_t data_offset = 0;
        bool valid = ReadInstrumentSampleInfo(0, resident, data_offset);
        if (s_project_snapshot && valid) {
            const auto* live = pool.FindByPath(path);
            valid = live && live->payload.meta.total_frames == resident.total_frames &&
                    live->payload.sample_rate == resident.sample_rate &&
                    live->payload.channels == resident.channels &&
                    live->payload.bit_depth == resident.bit_depth;
        }
        const FRESULT closed = f_close(&s_file);
        s_file_open = false;
        if (closed != FR_OK) {
            FinishEdit(INST_ERROR_IO);
        } else if (!valid) {
            WaveX::Log::PrintLine("WXI_SAVE: unsupported sample: '%s'", path);
            FinishEdit(INST_ERROR_UNSUPPORTED_SAMPLE);
        }
        // One dependency per main-loop pass; never stop or release resident voices.
        return;
    }
    if (s_bank_snapshot)
        FinishEdit();
    else
        s_phase = Phase::SaveCopy;
}

uint8_t SaveCopy() {
    auto& ins = s_bank->At(s_request.slot).instrument;
    const auto& doc = s_doc_storage.Get();
    const auto space = Storage::CheckSaveSpace(Wxi::detail::TotalFileSize(doc));
    if (space != Storage::SaveSpace::Ready)
        return space == Storage::SaveSpace::Full ? INST_ERROR_NO_SPACE : INST_ERROR_IO;
    char destination[Protocol::BROWSE_PATH_MAX], temporary[Protocol::BROWSE_PATH_MAX + 16];
    if (s_project_snapshot) {
        Protocol::detail::CopyWireString(destination, sizeof(destination), s_snapshot_path);
        std::snprintf(temporary, sizeof(temporary), "%s.tmp", destination);
    } else {
        const FRESULT root = f_mkdir("0:/wavex");
        if (root != FR_OK && root != FR_EXIST)
            return INST_ERROR_IO;
        const FRESULT dir = f_mkdir("0:/wavex/instruments");
        if (dir != FR_OK && dir != FR_EXIST)
            return INST_ERROR_IO;
        std::snprintf(destination, sizeof(destination), "0:/wavex/instruments/%s.wxi", doc.name);
        // A per-request temp avoids truncating an earlier incomplete save.
        std::snprintf(temporary,
                      sizeof(temporary),
                      "0:/wavex/instruments/.%s-%08lx.tmp",
                      doc.name,
                      static_cast<unsigned long>(s_request.request_id));
    }
    FILINFO info{};
    const FRESULT found = f_stat(destination, &info);
    if (found == FR_OK)
        return INST_ERROR_EXISTS;
    if (found != FR_NO_FILE)
        return INST_ERROR_IO;
    if (f_open(&s_file, temporary, FA_WRITE | FA_CREATE_NEW) != FR_OK)
        return INST_ERROR_IO;
    s_file_open = true;
    const bool written =
        Wxi::Write({&s_file, nullptr, WxiWriteCb, nullptr}, doc) == Wxi::Result::Ok;
    // f_close flushes data and directory metadata; never rename an open FIL.
    const FRESULT closed = f_close(&s_file);
    s_file_open = false;
    if (!written || closed != FR_OK) {
        f_unlink(temporary);
        return INST_ERROR_IO;
    }
    const FRESULT renamed = f_rename(temporary, destination);
    if (renamed != FR_OK) {
        f_unlink(temporary);
        return renamed == FR_EXIST ? INST_ERROR_EXISTS : INST_ERROR_IO;
    }
    if (!s_project_snapshot) {
        Protocol::detail::CopyWireString(ins.name, sizeof(ins.name), doc.name);
        s_sound_undo[s_request.slot].Apply();
        BumpKeyRevision(s_request.slot);
    }
    return INST_ERROR_NONE;
}

}  // namespace

void Reset() {
    s_project_bank = nullptr;
    s_in_project_step = false;
    s_project_samples_only = false;
    s_project_close_failed = false;
    s_project_snapshot = false;
    s_bank_snapshot = false;
    s_snapshot_error = INST_ERROR_NONE;
    CloseFile();
    s_zone_pending = false;
    s_sound_pending = false;
    s_track_pending = false;
    s_key_pending = s_key_assignment = false;
    s_osc_pending = false;
    s_mod_pending = false;
    s_arp_pending = false;
    s_lfo_pending = false;
    s_action_pending = s_allocation_pending = false;
    if (s_bank)
        WaveX::ReconstructInPlace(*s_bank);
    else
        s_bank = new (s_bank_storage) Tracks();
    for (uint8_t track = 0; track < kNumTracks; ++track) {
        BumpKeyRevision(track);
        s_sound_undo[track].Apply();
        s_allocation_dirty[track] = false;
        for (uint8_t scope = 0; scope < 2; ++scope) {
            s_allocation_completed[scope][track] = 0;
            s_allocation_error[scope][track] = 0;
        }
        s_action_completed[track] = 0;
        s_action_error[track] = 0;
        s_sound_completed[track] = 0;
        s_sound_error[track] = 0;
        s_edit_completed[track] = 0;
        s_edit_error[track] = 0;
        s_mod_active[track] = ModTable{};
        s_mod_mailboxes[track].Init(s_mod_active[track]);
    }
    for (auto& ls: s_loaded_samples) {
        ls = LoadedSample{};
    }
    s_phase = Phase::Idle;
}

bool BeginProjectSnapshot(uint8_t track, const char* destination) {
    if (Busy() || track >= kNumTracks || !destination || !TrackLoaded(track))
        return false;
    constexpr char prefix[] = "0:/wavex/projects/";
    const size_t length = strnlen(destination, sizeof(s_snapshot_path));
    if (length == sizeof(s_snapshot_path) || length <= sizeof(prefix) ||
        std::strncmp(destination, prefix, sizeof(prefix) - 1) || std::strstr(destination, "..") ||
        std::strchr(destination, '\\'))
        return false;
    Protocol::detail::CopyWireString(s_snapshot_path, sizeof(s_snapshot_path), destination);
    s_request = InstOpMessage(0, track, INST_OP_SAVE, "");
    s_snapshot_error = INST_ERROR_NONE;
    s_project_snapshot = true;
    s_bank_snapshot = false;
    s_phase = Phase::PrepareSave;
    return true;
}
bool BeginBankSnapshot(uint8_t track) {
    if (Busy() || track >= kNumTracks || !TrackLoaded(track))
        return false;
    s_request = InstOpMessage(0, track, INST_OP_SAVE, "");
    s_snapshot_error = INST_ERROR_NONE;
    s_project_snapshot = s_bank_snapshot = true;
    s_phase = Phase::PrepareSave;
    return true;
}
const Wxi::InstrumentFile& BankSnapshot() {
    return s_doc_storage.Get();
}

uint8_t ProjectSnapshotError() {
    return s_snapshot_error;
}

bool Begin(const InstOpMessage& request) {
    if (!Busy())
        s_bank_snapshot = false;
    if (s_project_bank && !s_in_project_step)
        return false;
    if (request.op >= INST_OP_NEW && request.op <= INST_OP_NEW_KEYBOARD) {
        if (request.slot >= kNumTracks || request.request_id == 0) {
            QueueZoneReply(request.request_id, request.slot, INST_ERROR_BAD_FILE);
            return false;
        }
        if (request.op == INST_OP_GET_PAD_MAP) {
            QueueZoneReply(request.request_id, request.slot);
            return true;
        }
        if (s_edit_completed[request.slot] == request.request_id) {
            QueueZoneReply(request.request_id, request.slot);
            return false;  // duplicate mutation: replay its outcome, never its side effects
        }
        if (Busy()) {
            QueueZoneReply(request.request_id, request.slot, INST_ERROR_BUSY);
            return false;
        }
        s_request = request;
        // Never silently truncate an unterminated wire name.
        const bool named = request.op == INST_OP_NEW_KEYBOARD || request.op == INST_OP_NEW ||
                           request.op == INST_OP_SAVE || request.op == INST_OP_SET_NAME;
        if ((named && !IsValidInstrumentName(request.path)) ||
            (request.op == INST_OP_SET_PAD_SAMPLE &&
             (request.pad_index >= INST_PAD_COUNT || request.pad_choke > 15 ||
              !KitEdit::Editable(s_bank->At(request.slot).instrument)))) {
            FinishEdit(INST_ERROR_BAD_FILE);
            return false;
        }
        if (request.op == INST_OP_SET_NAME) {
            auto& ins = s_bank->At(request.slot).instrument;
            if (ins.origin == InstrumentOrigin::None) {
                FinishEdit(INST_ERROR_BAD_FILE);
                return false;
            }
            Protocol::detail::CopyWireString(ins.name, sizeof(ins.name), request.path);
            FinishEdit();
            return true;
        }
        s_phase = request.op == INST_OP_SAVE ? Phase::PrepareSave : Phase::AwaitVoiceStop;
        return true;
    }

    if (s_phase != Phase::Idle) {
        // Selection probes are disposable. Replace an in-flight probe with
        // the newest cursor selection so fast browsing cannot leave the final
        // file stuck on a BUSY result. A load is never cancelled this way.
        if (s_request.op == INST_OP_SFZ_PROBE && request.op == INST_OP_SFZ_PROBE) {
            CloseFile();
            s_phase = Phase::Idle;
        } else {
            InstStatusMessage busy;
            busy.request_id = request.request_id;
            busy.slot = request.slot;
            busy.op = request.op;
            busy.state = INST_STATUS_FAILED;
            busy.error = INST_ERROR_BUSY;
            if (request.request_id != 0) {
                WaveX::Comm::LinkSend(MSG_INST_STATUS, &busy, sizeof(busy));
            }
            return false;
        }
    }
    if (request.slot >= kNumTracks || request.path[0] == '\0' ||
        (request.op != INST_OP_SFZ_PROBE && request.op != INST_OP_SFZ_LOAD)) {
        s_request = request;
        s_status = InstStatusMessage{};
        s_status.request_id = request.request_id;
        s_status.slot = request.slot;
        s_status.op = request.op;
        SendStatus(INST_STATUS_FAILED, INST_ERROR_BAD_FILE);
        return false;
    }

    s_request = request;
    s_request.path[sizeof(s_request.path) - 1] = '\0';
    s_status = InstStatusMessage{};
    s_status.request_id = request.request_id;
    s_status.slot = request.slot;
    s_status.op = request.op;
    s_parser.Reset();
    s_mapped_storage.Reconstruct();
    s_doc_storage.Reconstruct();
    WaveX::ReconstructInPlace(s_plan);
    for (auto& probe: s_probes) {
        probe = Sfz::SampleProbe{};
    }
    for (auto& ls: s_loaded_samples) {
        ls = LoadedSample{};
    }
    s_line_number = 0;
    s_index = 0;
    s_allocated = 0;
    s_total_bytes = 0;
    s_loaded_bytes = 0;
    s_current_written = 0;
    s_last_percent = 0xFF;
    // Both front halves end at ProbeSample with the same MappedInstrument;
    // only the parse differs.
    s_phase = Phase::OpenSfz;
    SendStatus(INST_STATUS_PROBING);
    return true;
}

bool Busy() {
    return s_phase != Phase::Idle || (s_project_bank && !s_in_project_step);
}

bool TrackLoading(uint8_t slot) {
    return Busy() && s_request.slot == slot &&
           (s_key_assignment || s_request.op == INST_OP_NEW_KEYBOARD ||
            s_request.op == INST_OP_SFZ_LOAD || s_request.op == INST_OP_NEW ||
            s_request.op == INST_OP_SET_PAD_SAMPLE);
}

uint8_t VoiceStopTrack() {
    return s_phase == Phase::AwaitVoiceStop ? s_request.slot : 0xFF;
}

void ConfirmVoicesStopped(SamplePool& pool, SampleMemMgr& memory) {
    if (s_project_bank && !s_in_project_step)
        return;
    if (s_phase != Phase::AwaitVoiceStop)
        return;

    if (s_key_assignment) {
        auto& ins = s_bank->At(s_key_request.track).instrument;
        const auto& current = ins.osc[s_key_request.oscillator].zones[s_key_request.zone];
        if (s_key_revision[s_key_request.track] != s_key_request.revision ||
            (current.in_use ? current.sample_id : 0) != s_key_request.expected_sample) {
            FinishKey(INST_ERROR_BAD_FILE);
            return;
        }
        const auto sample = s_key_request.value.sample_id;
        const auto* record = pool.Find(sample);
        if (sample && (!record || !SampleIsPlayable(record->payload))) {
            FinishKey(INST_ERROR_MISSING_SAMPLES);
            return;
        }
        auto& zone = ins.osc[s_key_request.oscillator].zones[s_key_request.zone];
        const auto old = zone.in_use ? zone.sample_id : uint16_t{0};
        if (!sample)
            zone = Zone{};
        else {
            if (sample != old) {
                zone.start_frame = zone.end_frame = zone.loop_start = zone.loop_end = 0;
                zone.loop_mode = ZONE_LOOP_INHERIT;
            }
            ins.osc[s_key_request.oscillator].type = OscType::Sample;
            zone.sample_id = sample;
            zone.in_use = true;
            pool.SetUsedBy(sample, s_key_request.track, true);
        }
        if (old && old != sample && !KitEdit::Uses(ins, old)) {
            pool.SetUsedBy(old, s_key_request.track, false);
            const auto* unused = pool.Find(old);
            if (unused && !unused->pinned && !unused->used_by)
                FreeFromPool(pool, memory, old);
        }
        BumpKeyRevision(s_key_request.track);
        FinishKey();
        return;
    }
    if (s_request.op == INST_OP_NEW || s_request.op == INST_OP_NEW_KEYBOARD) {
        ReleaseTrack(pool, memory, s_request.slot);
        auto& ins = s_bank->At(s_request.slot).instrument;
        WaveX::ReconstructInPlace(ins);
        ins.origin = InstrumentOrigin::Built;
        ins.mode =
            s_request.op == INST_OP_NEW_KEYBOARD ? InstrumentMode::Keyboard : InstrumentMode::Drum;
        Protocol::detail::CopyWireString(ins.name, sizeof(ins.name), s_request.path);
        PublishModSlots(s_request.slot);
        FinishEdit();
        return;
    }
    if (s_request.op == INST_OP_SET_PAD_SAMPLE) {
        auto& ins = s_bank->At(s_request.slot).instrument;
        const auto sample = s_request.pad_sample_id;
        const auto* record = pool.Find(sample);
        if (sample && (!record || !SampleIsPlayable(record->payload))) {
            FinishEdit(INST_ERROR_MISSING_SAMPLES);
            return;
        }
        const auto old = ins.osc[0].zones[s_request.pad_index].sample_id;
        BumpKeyRevision(s_request.slot);
        KitEdit::Assign(ins, s_request.pad_index, sample, s_request.pad_choke);
        if (sample)
            pool.SetUsedBy(sample, s_request.slot, true);
        if (old && old != sample && !KitEdit::Uses(ins, old)) {
            pool.SetUsedBy(old, s_request.slot, false);
            const auto* unused = pool.Find(old);
            if (unused && !unused->pinned && !unused->used_by)
                FreeFromPool(pool, memory, old);
        }
        FinishEdit();
        return;
    }

    // Only this Track's holdings are released; every other Track's refs, and
    // the user's pinned samples, are untouched (they may be this import's
    // hits). Mod slots survive: a load is not an edit of them.
    if (!s_project_samples_only)
        ReleaseTrack(pool, memory, s_request.slot);
    s_allocated = 0;
    s_index = 0;
    s_phase = Phase::AllocateSample;
    SendStatus(INST_STATUS_LOAD_BEGIN);
}

Allocation::Override TrackAllocation(uint8_t track) {
    return track < kNumTracks ? s_bank->At(track).allocation : Allocation::Override{};
}
AllocationSyncMessage ReadAllocationState(uint8_t track, uint8_t scope) {
    AllocationSyncMessage out;
    out.track = track;
    out.scope = scope;
    out.busy = Busy();
    if (track >= kNumTracks || scope > ALLOC_TRACK)
        return out;
    const auto& t = s_bank->At(track);
    out.loaded = t.instrument.origin != InstrumentOrigin::None;
    out.valid = scope == ALLOC_TRACK || out.loaded;
    out.revision = s_key_revision[track];
    out.completed_request_id = s_allocation_completed[scope][track];
    out.error = s_allocation_error[scope][track];
    out.dirty = scope == ALLOC_SOUND ? s_sound_undo[track].Active() : s_allocation_dirty[track];
    out.inherited = t.allocation.inherit;
    out.sound = t.instrument.allocation;
    out.track_policy = t.allocation.policy;
    return out;
}
bool OnAllocationOp(const AllocationOpMessage& request) {
    s_allocation_reply = ReadAllocationState(request.track, request.scope);
    s_allocation_reply.request_id = request.request_id;
    s_allocation_pending = true;
    if (!IsValidAllocationOp(request)) {
        s_allocation_reply.completed_request_id = request.request_id;
        s_allocation_reply.error = INST_ERROR_BAD_FILE;
        return false;
    }
    const auto track = request.track, scope = request.scope;
    if (request.op == ALLOC_GET || s_allocation_completed[scope][track] == request.request_id)
        return false;
    if (Busy()) {
        s_allocation_reply.completed_request_id = request.request_id;
        s_allocation_reply.error = INST_ERROR_BUSY;
        return false;
    }
    auto& t = s_bank->At(track);
    uint8_t error = INST_ERROR_NONE;
    bool changed = false;
    if (request.revision != s_key_revision[track] ||
        (scope == ALLOC_SOUND && t.instrument.origin == InstrumentOrigin::None))
        error = INST_ERROR_BAD_FILE;
    else if (scope == ALLOC_SOUND) {
        if (request.op == ALLOC_APPLY)
            s_sound_undo[track].Apply();
        else if (request.op == ALLOC_REVERT) {
            changed = s_sound_undo[track].Revert(t.instrument);
            if (changed)
                PublishModSlots(track);
        } else if (t.instrument.allocation != request.policy) {
            s_sound_undo[track].Capture(t.instrument);
            t.instrument.allocation = request.policy;
            changed = true;
        }
    } else {
        if (request.op == ALLOC_APPLY)
            s_allocation_dirty[track] = false;
        else if (request.op == ALLOC_REVERT) {
            if (s_allocation_dirty[track]) {
                t.allocation = s_allocation_undo[track];
                s_allocation_dirty[track] = false;
                changed = true;
            }
        } else {
            const Allocation::Override value{request.policy, request.inherit != 0};
            if (t.allocation != value) {
                if (!s_allocation_dirty[track])
                    s_allocation_undo[track] = t.allocation;
                s_allocation_dirty[track] = true;
                t.allocation = value;
                changed = true;
            }
        }
    }
    if (!error)
        BumpKeyRevision(track);
    s_allocation_completed[scope][track] = request.request_id;
    s_allocation_error[scope][track] = error;
    s_allocation_reply = ReadAllocationState(track, scope);
    s_allocation_reply.request_id = request.request_id;
    return changed;
}

InstEditSyncMessage ReadEditState(uint8_t track) {
    InstEditSyncMessage out;
    out.track = track;
    out.busy = Busy();
    if (track >= kNumTracks)
        return out;
    const auto& ins = s_bank->At(track).instrument;
    out.valid = ins.origin != InstrumentOrigin::None;
    out.revision = s_key_revision[track];
    out.completed_request_id = s_action_completed[track];
    out.error = s_action_error[track];
    out.dirty = s_sound_undo[track].Active();
    out.filter_type = ins.filter.type;
    out.filter_topology = ins.filter.topology;
    out.filter_slope = ins.filter.slope;
    out.filter_drive = ins.filter.drive;
    out.sound = {ins.filter.cutoff_hz, ins.filter.resonance, ins.trim_gain, ins.trim_pan};
    return out;
}
bool OnEditOp(const InstEditOpMessage& request) {
    s_action_reply = ReadEditState(request.track);
    s_action_reply.request_id = request.request_id;
    s_action_pending = true;
    if (!IsValidInstEditOp(request)) {
        s_action_reply.completed_request_id = request.request_id;
        s_action_reply.error = INST_ERROR_BAD_FILE;
        return false;
    }
    if (request.op == INST_EDIT_GET || s_action_completed[request.track] == request.request_id)
        return false;
    if (Busy()) {
        s_action_reply.completed_request_id = request.request_id;
        s_action_reply.error = INST_ERROR_BUSY;
        return false;
    }
    auto& ins = s_bank->At(request.track).instrument;
    uint8_t error = INST_ERROR_NONE;
    bool changed = false;
    if (ins.origin == InstrumentOrigin::None || request.revision != s_key_revision[request.track])
        error = INST_ERROR_BAD_FILE;
    else if (request.op == INST_EDIT_APPLY) {
        s_sound_undo[request.track].Apply();
    } else if (request.op == INST_EDIT_REVERT) {
        changed = s_sound_undo[request.track].Revert(ins);
        if (changed)
            PublishModSlots(request.track);
    } else if (request.op == INST_EDIT_FILTER || request.op == INST_EDIT_FILTER_SETTINGS) {
        const bool settings = request.op == INST_EDIT_FILTER_SETTINGS;
        const uint8_t mode = settings ? request.filter_type : ins.filter.type;
        const uint8_t topology = settings ? request.filter_topology : ins.filter.topology;
        const uint8_t slope = settings ? request.filter_slope : ins.filter.slope;
        const float drive = settings ? request.filter_drive : ins.filter.drive;
        changed = ins.filter.type != mode || ins.filter.topology != topology ||
                  ins.filter.slope != slope || ins.filter.drive != drive ||
                  ins.filter.cutoff_hz != request.sound.cutoff_hz ||
                  ins.filter.resonance != request.sound.resonance;
        if (changed) {
            s_sound_undo[request.track].Capture(ins);
            ins.filter.type = mode;
            ins.filter.topology = topology;
            ins.filter.slope = slope;
            ins.filter.drive = drive;
            ins.filter.cutoff_hz = request.sound.cutoff_hz;
            ins.filter.resonance = request.sound.resonance;
        }
    } else {
        changed = ins.trim_gain != request.sound.gain || ins.trim_pan != request.sound.pan;
        if (changed) {
            s_sound_undo[request.track].Capture(ins);
            ins.trim_gain = request.sound.gain;
            ins.trim_pan = request.sound.pan;
        }
    }
    // Apply advances identity too: stale pre-Apply actions cannot consume the
    // next edit's undo point.
    if (!error)
        BumpKeyRevision(request.track);
    s_action_completed[request.track] = request.request_id;
    s_action_error[request.track] = error;
    s_action_reply = ReadEditState(request.track);
    s_action_reply.request_id = request.request_id;
    return changed;
}

InstArpSyncMessage ReadArpState(uint8_t track) {
    InstArpOpMessage request;
    request.track = track;
    InstArpSyncMessage out;
    FillArpReply(request, out);
    return out;
}
bool OnArpOp(const InstArpOpMessage& request) {
    if (!IsValidInstArpOp(request)) {
        QueueArpReply(request, INST_ERROR_BAD_FILE);
        return false;
    }
    if (request.op == INST_ARP_GET || s_edit_completed[request.track] == request.request_id) {
        QueueArpReply(request);
        return false;
    }
    if (Busy()) {
        QueueArpReply(request, INST_ERROR_BUSY);
        return false;
    }
    auto& ins = s_bank->At(request.track).instrument;
    uint8_t error = INST_ERROR_NONE;
    if (ins.origin == InstrumentOrigin::None || request.revision != s_key_revision[request.track])
        error = INST_ERROR_BAD_FILE;
    else {
        s_sound_undo[request.track].Capture(ins);
        ins.arp = request.value;
    }
    s_edit_completed[request.track] = request.request_id;
    s_edit_error[request.track] = error;
    if (!error)
        BumpKeyRevision(request.track);
    QueueArpReply(request);
    return error == INST_ERROR_NONE;
}
InstLfoSyncMessage ReadLfoState(uint8_t track) {
    InstLfoOpMessage request;
    request.track = track;
    InstLfoSyncMessage out;
    FillLfoReply(request, out);
    return out;
}
bool OnLfoOp(const InstLfoOpMessage& request) {
    if (!IsValidInstLfoOp(request)) {
        QueueLfoReply(request, INST_ERROR_BAD_FILE);
        return false;
    }
    if (request.op == INST_LFO_GET || s_edit_completed[request.track] == request.request_id) {
        QueueLfoReply(request);
        return false;
    }
    if (Busy()) {
        QueueLfoReply(request, INST_ERROR_BUSY);
        return false;
    }
    auto& ins = s_bank->At(request.track).instrument;
    uint8_t error = INST_ERROR_NONE;
    if (ins.origin == InstrumentOrigin::None || request.revision != s_key_revision[request.track])
        error = INST_ERROR_BAD_FILE;
    else {
        s_sound_undo[request.track].Capture(ins);
        const auto& v = request.value;
        ins.lfo[request.index] = {
            v.wave, v.rate_hz, v.sync_div, v.delay_s, v.fade_s, v.retrigger, v.pitch_follow};
    }
    s_edit_completed[request.track] = request.request_id;
    s_edit_error[request.track] = error;
    if (!error)
        BumpKeyRevision(request.track);
    QueueLfoReply(request);
    return error == INST_ERROR_NONE;
}
InstModSyncMessage ReadModState(uint8_t track) {
    InstModOpMessage request;
    request.track = track;
    InstModSyncMessage out;
    FillModReply(request, out);
    return out;
}
bool OnModOp(const InstModOpMessage& request) {
    if (!IsValidInstModOp(request)) {
        QueueModReply(request, INST_ERROR_BAD_FILE);
        return false;
    }
    if (request.op == INST_MOD_GET || s_edit_completed[request.track] == request.request_id) {
        QueueModReply(request);
        return false;
    }
    if (Busy()) {
        QueueModReply(request, INST_ERROR_BUSY);
        return false;
    }
    auto& ins = s_bank->At(request.track).instrument;
    uint8_t error = INST_ERROR_NONE;
    if (ins.origin == InstrumentOrigin::None || request.revision != s_key_revision[request.track])
        error = INST_ERROR_BAD_FILE;
    else if (request.op == INST_MOD_SET_ENV) {
        s_sound_undo[request.track].Capture(ins);
        const auto& e = request.envelope;
        ins.env[request.index] = {e.attack_s, e.decay_s, e.sustain, e.release_s};
    } else {
        s_sound_undo[request.track].Capture(ins);
        const auto& slot = request.slot;
        ins.mod_slots[request.index] = {
            slot.source, slot.destination, slot.depth, slot.curve, slot.flags};
        PublishModSlots(request.track);
    }
    s_edit_completed[request.track] = request.request_id;
    s_edit_error[request.track] = error;
    if (!error)
        BumpKeyRevision(request.track);
    QueueModReply(request);
    return error == INST_ERROR_NONE;
}
InstOscSyncMessage ReadOscState(uint8_t track, uint8_t oscillator) {
    InstOscOpMessage request;
    request.track = track;
    request.oscillator = oscillator;
    InstOscSyncMessage out;
    FillOscReply(request, out);
    return out;
}
bool OnOscOp(const InstOscOpMessage& request) {
    if (!IsValidInstOscOp(request)) {
        QueueOscReply(request, INST_ERROR_BAD_FILE);
        return false;
    }
    if (request.op == INST_OSC_GET || s_edit_completed[request.track] == request.request_id) {
        QueueOscReply(request);
        return false;
    }
    if (Busy()) {
        QueueOscReply(request, INST_ERROR_BUSY);
        return false;
    }
    auto& ins = s_bank->At(request.track).instrument;
    auto& osc = ins.osc[request.oscillator];
    uint8_t error = INST_ERROR_NONE;
    if (ins.origin == InstrumentOrigin::None || request.revision != s_key_revision[request.track]) {
        error = INST_ERROR_BAD_FILE;
    } else if (request.op == INST_OSC_COPY_EMPTY) {
        const auto& source = ins.osc[request.source];
        if (source.type != OscType::Sample)
            error = INST_ERROR_BAD_FILE;
        for (const auto& zone: osc.zones)
            if (zone.in_use)
                error = INST_ERROR_EXISTS;
        // No ownership transfer: every copied sample already has this Track's
        // Pool reference. No old zone or sounding voice is removed.
        if (!error)
            osc = source;
    } else {
        s_sound_undo[request.track].Capture(ins);
        osc.level = request.value.level;
        osc.coarse_tune = request.value.coarse;
        osc.fine_tune = request.value.fine;
        osc.keytrack = request.value.keytrack;
        osc.mono = request.value.mono != 0;
        ins.osc_mix = request.value.mix;
    }
    s_edit_completed[request.track] = request.request_id;
    s_edit_error[request.track] = error;
    if (!error)
        BumpKeyRevision(request.track);
    QueueOscReply(request);
    return error == INST_ERROR_NONE;
}

bool OnKeyMapOp(const InstKeyMapOpMessage& request) {
    if (!IsValidKeyMapOp(request)) {
        QueueKeyReply(request.request_id, request.track, INST_ERROR_BAD_FILE, request.oscillator);
        return false;
    }
    if (request.op == KEY_MAP_GET || s_edit_completed[request.track] == request.request_id) {
        QueueKeyReply(request.request_id, request.track, 0, request.oscillator);
        return false;
    }
    if (Busy()) {
        QueueKeyReply(request.request_id, request.track, INST_ERROR_BUSY, request.oscillator);
        return false;
    }
    s_key_request = request;
    auto& ins = s_bank->At(request.track).instrument;
    auto& z = ins.osc[request.oscillator].zones[request.zone];
    const auto sample = z.in_use ? z.sample_id : uint16_t{0};
    if (ins.origin == InstrumentOrigin::None || ins.mode != InstrumentMode::Keyboard ||
        ins.osc[request.oscillator].type == OscType::Wavetable ||
        request.revision != s_key_revision[request.track] || sample != request.expected_sample) {
        FinishKey(INST_ERROR_BAD_FILE);
        return false;
    }
    if (request.op == KEY_MAP_ASSIGN) {
        s_key_assignment = true;
        s_request = InstOpMessage(request.request_id, request.track, 0, "");
        s_phase = Phase::AwaitVoiceStop;
        QueueKeyReply(request.request_id, request.track, 0, request.oscillator);
        return false;
    }
    z.key_lo = request.value.key_lo;
    z.key_hi = request.value.key_hi;
    z.vel_lo = request.value.vel_lo;
    z.vel_hi = request.value.vel_hi;
    z.root_note = request.value.root_note;
    BumpKeyRevision(request.track);
    FinishKey();
    return true;
}
bool OnPadSoundOp(const InstPadSoundOpMessage& request) {
    s_sound_reply = InstPadSoundSyncMessage{};
    auto& reply = s_sound_reply;
    reply.request_id = request.request_id;
    reply.track = request.track;
    reply.pad = request.pad;
    reply.busy = Busy();
    s_sound_pending = true;
    if (request.track >= kNumTracks || request.pad >= INST_PAD_COUNT || !request.request_id) {
        reply.completed_request_id = request.request_id;
        reply.error = INST_ERROR_BAD_FILE;
        return false;
    }
    auto& ins = s_bank->At(request.track).instrument;
    bool changed = false;
    if (request.op != PAD_SOUND_GET && s_sound_completed[request.track] != request.request_id) {
        uint8_t error = INST_ERROR_NONE;
        if (!IsValidPadSoundOp(request))
            error = INST_ERROR_BAD_FILE;
        else if (Busy())
            error = INST_ERROR_BUSY;
        else if (!KitEdit::SetSound(ins, request))
            error = INST_ERROR_BAD_FILE;
        else
            changed = true;
        s_sound_completed[request.track] = request.request_id;
        s_sound_error[request.track] = error;
    }
    reply.completed_request_id = s_sound_completed[request.track];
    reply.error = s_sound_error[request.track];
    if (!IsValidPadSoundOp(request)) {
        reply.completed_request_id = request.request_id;
        reply.error = INST_ERROR_BAD_FILE;
    }
    KitEdit::ReadSound(ins, request.pad, reply);
    return changed;
}
void OnTrackStateRequest(const TrackStateRequest& request) {
    s_track_reply = TrackStateMessage{};
    auto& out = s_track_reply;
    out.request_id = request.request_id;
    out.track = request.track;
    s_track_pending = true;
    if (!request.request_id || request.track >= kNumTracks)
        return;
    const auto& track = s_bank->At(request.track);
    const auto& ins = track.instrument;
    out.valid = 1;
    out.busy = Busy();
    out.loaded = ins.origin != InstrumentOrigin::None;
    out.mode = static_cast<uint8_t>(ins.mode);
    out.midi_in = track.midi_in;
    out.poly_limit = track.poly_limit;
    out.priority = track.priority;
    out.program_change = track.program_change;
    out.sample_id = BoundSample(request.track);
    Protocol::detail::CopyWireString(out.name, sizeof(out.name), ins.name);
}
void PumpEditorReply() {
    if (s_allocation_pending &&
        WaveX::Comm::LinkSend(MSG_ALLOC_SYNC, &s_allocation_reply, sizeof(s_allocation_reply)) >= 0)
        s_allocation_pending = false;
    if (s_action_pending &&
        WaveX::Comm::LinkSend(MSG_INST_EDIT_SYNC, &s_action_reply, sizeof(s_action_reply)) >= 0)
        s_action_pending = false;
    if (s_arp_pending &&
        WaveX::Comm::LinkSend(MSG_INST_ARP_SYNC, &s_arp_reply, sizeof(s_arp_reply)) >= 0)
        s_arp_pending = false;
    if (s_lfo_pending &&
        WaveX::Comm::LinkSend(MSG_INST_LFO_SYNC, &s_lfo_reply, sizeof(s_lfo_reply)) >= 0)
        s_lfo_pending = false;
    if (s_mod_pending &&
        WaveX::Comm::LinkSend(MSG_INST_MOD_SYNC, &s_mod_reply, sizeof(s_mod_reply)) >= 0)
        s_mod_pending = false;
    if (s_osc_pending &&
        WaveX::Comm::LinkSend(MSG_INST_OSC_SYNC, &s_osc_reply, sizeof(s_osc_reply)) >= 0)
        s_osc_pending = false;
    if (s_key_pending &&
        WaveX::Comm::LinkSend(MSG_INST_KEY_MAP_SYNC, &s_key_reply, sizeof(s_key_reply)) >= 0)
        s_key_pending = false;
    if (s_track_pending &&
        WaveX::Comm::LinkSend(MSG_TRACK_STATE, &s_track_reply, sizeof(s_track_reply)) >= 0)
        s_track_pending = false;
    if (s_sound_pending &&
        WaveX::Comm::LinkSend(MSG_INST_PAD_SOUND_SYNC, &s_sound_reply, sizeof(s_sound_reply)) >= 0)
        s_sound_pending = false;
    if (s_zone_pending &&
        WaveX::Comm::LinkSend(MSG_INST_ZONE_SYNC, &s_zone_reply, sizeof(s_zone_reply)) >= 0)
        s_zone_pending = false;
}

void Pump(SamplePool& pool, SampleMemMgr& memory, uint8_t* io_buffer, uint32_t io_buffer_bytes) {
    if (s_project_bank && !s_in_project_step)
        return;
    if (s_phase == Phase::Idle || s_phase == Phase::AwaitVoiceStop)
        return;
    if (!memory.initialized() || !io_buffer || io_buffer_bytes < 512) {
        Fail(&pool, &memory, INST_ERROR_NO_MEMORY);
        return;
    }

    switch (s_phase) {
        case Phase::OpenSfz: {
            const FRESULT fr = f_open(&s_file, s_request.path, FA_READ);
            if (fr != FR_OK) {
                Fail(&pool, &memory, INST_ERROR_BAD_FILE);
                return;
            }
            s_file_open = true;
            s_phase = InstrumentMap::PathIsSfz(s_request.path) ? Phase::ParseLine : Phase::ReadWxi;
        } break;

        case Phase::ReadWxi: {
            Wxi::InstrumentFile& doc = s_doc_storage.Get();
            const WaveX::Wxcf::IoContext io{&s_file, &WxiReadCb, nullptr, &WxiEofCb};
            const Wxi::Result result = Wxi::Read(io, doc);
            CloseFile();
            if (result != Wxi::Result::Ok) {
                // A file that is not an Instrument at all, or is from a
                // future major, is a bad file to this build; too many zones
                // gets its own code so the UI can say which limit was hit.
                Fail(&pool,
                     &memory,
                     result == Wxi::Result::TooManyZones ? INST_ERROR_TOO_MANY_REGIONS
                                                         : INST_ERROR_BAD_FILE);
                return;
            }
            InstrumentMap::FromFile(doc, s_mapped);
            Sfz::Status status;
            if (!Sfz::BuildSamplePlan(s_mapped, s_plan, status)) {
                Fail(&pool, &memory, INST_ERROR_BAD_FILE);
                return;
            }
            s_status.zone_count = s_mapped.zone_count;
            s_status.sample_count = s_plan.count;
            s_index = 0;
            s_phase = Phase::ProbeSample;
        } break;

        case Phase::ParseLine: {
            if (f_gets(s_line, static_cast<int>(sizeof(s_line)), &s_file) != nullptr) {
                ++s_line_number;
                const size_t length = std::strlen(s_line);
                if ((length + 1 == sizeof(s_line) && s_line[length - 1] != '\n' &&
                     !f_eof(&s_file)) ||
                    !s_parser.FeedLine(s_line, s_line_number)) {
                    Fail(&pool,
                         &memory,
                         s_parser.GetStatus().error == Sfz::Error::TooManyRegions
                             ? INST_ERROR_TOO_MANY_REGIONS
                             : INST_ERROR_BAD_FILE);
                }
            } else {
                if (f_error(&s_file) != 0) {
                    Fail(&pool, &memory, INST_ERROR_IO);
                    return;
                }
                CloseFile();
                s_phase = Phase::FinishParse;
            }
        } break;

        case Phase::FinishParse: {
            Sfz::Status status;
            if (!s_parser.Finish() ||
                !Sfz::MapDocument(s_parser.GetDocument(), s_request.path, s_mapped, status) ||
                s_mapped.zone_count == 0 || !Sfz::BuildSamplePlan(s_mapped, s_plan, status)) {
                const Sfz::Error error = s_parser.GetStatus().error != Sfz::Error::None
                                             ? s_parser.GetStatus().error
                                             : status.error;
                Fail(&pool,
                     &memory,
                     error == Sfz::Error::TooManyRegions ? INST_ERROR_TOO_MANY_REGIONS
                                                         : INST_ERROR_BAD_FILE);
                return;
            }
            s_status.zone_count = s_mapped.zone_count;
            s_status.sample_count = s_plan.count;
            s_index = 0;
            s_phase = Phase::ProbeSample;
        } break;

        case Phase::ProbeSample: {
            if (s_index < s_plan.count) {
                const ProbeResult result = ProbeCurrent(pool);
                if (result == ProbeResult::Missing) {
                    ++s_status.missing_count;
                    s_status.flags |= INST_STATUS_MISSING_FILES;
                } else if (result == ProbeResult::Invalid) {
                    ++s_status.invalid_count;
                    s_status.flags |= INST_STATUS_INVALID_FILES;
                }
                ++s_index;
                return;
            }
            s_status.total_bytes = s_total_bytes;
            s_status.available_bytes = AvailableBytes(pool, memory, s_request.slot);
            if (s_total_bytes > s_status.available_bytes) {
                s_status.flags |= INST_STATUS_EXCEEDS_MEMORY;
            }
            if (s_request.op == INST_OP_SFZ_PROBE) {
                SendStatus(INST_STATUS_PROBE_COMPLETE);
                s_phase = Phase::Idle;
                return;
            }
            if (s_status.flags & INST_STATUS_MISSING_FILES) {
                Fail(&pool, &memory, INST_ERROR_MISSING_SAMPLES);
            } else if (s_status.flags & INST_STATUS_INVALID_FILES) {
                Fail(&pool, &memory, INST_ERROR_UNSUPPORTED_SAMPLE);
            } else if (s_status.flags & INST_STATUS_EXCEEDS_MEMORY) {
                Fail(&pool, &memory, INST_ERROR_TOO_LARGE);
            } else {
                s_phase = Phase::AwaitVoiceStop;
            }
        } break;

        case Phase::AllocateSample: {
            if (s_index >= s_plan.count) {
                s_index = 0;
                s_phase = Phase::OpenSample;
                return;
            }
            LoadedSample& ls = s_loaded_samples[s_index];
            if (ls.hit) {
                // Releasing this Track above may have freed the sample this
                // hit pointed at (it was only this Track's). Re-check; a
                // vanished hit is loaded like any other file.
                if (pool.Find(ls.pool_id)) {
                    ++s_index;
                    return;
                }
                ls.hit = false;
                ls.pool_id = 0;
                s_total_bytes += ls.resident.data_size;
            }
            // Admission first: an entry, then the bytes. Neither evicts.
            SamplePool::Record* record = nullptr;
            const auto admit =
                pool.AdmitPath(s_mapped.sample_paths[s_plan.entries[s_index].path_zone], &record);
            if (admit == SamplePool::Admit::AlreadyResident) {
                // Two plan entries can name one file; the second is a hit.
                ls.hit = true;
                ls.pool_id = record->sample_id;
                ++s_index;
                return;
            }
            if (admit != SamplePool::Admit::Ok) {
                Fail(&pool, &memory, INST_ERROR_NO_MEMORY);
                return;
            }
            ls.pool_id = record->sample_id;
            ls.admitted = true;
            if (!memory.alloc(ls.resident.data_size, &ls.handle)) {
                Fail(&pool, &memory, INST_ERROR_NO_MEMORY);
                return;
            }
            ++s_allocated;
            ++s_index;
        } break;

        case Phase::OpenSample: {
            // Hits have nothing to read.
            while (s_index < s_plan.count && s_loaded_samples[s_index].hit) {
                ++s_index;
            }
            if (s_index >= s_plan.count) {
                s_phase = Phase::Commit;
                return;
            }
            const char* path = s_mapped.sample_paths[s_plan.entries[s_index].path_zone];
            FRESULT fr = f_open(&s_file, path, FA_READ);
            if (fr == FR_OK) {
                s_file_open = true;
                fr = f_lseek(&s_file, s_loaded_samples[s_index].data_offset);
            }
            if (fr != FR_OK) {
                Fail(&pool, &memory, INST_ERROR_IO);
                return;
            }
            // Prepare this newly admitted record's edit defaults while yielding
            // between files. Explicit Project snapshots override defaults later.
            auto* record = pool.Find(s_loaded_samples[s_index].pool_id);
            SampleFile::Document saved;
            const auto& resident = s_loaded_samples[s_index].resident;
            saved.file_bytes = f_size(&s_file);
            saved.data_offset = s_loaded_samples[s_index].data_offset;
            saved.sample.sample_rate = resident.sample_rate;
            saved.sample.total_frames = resident.total_frames;
            saved.sample.channels = resident.channels;
            saved.sample.bits_per_sample = resident.bit_depth;
            saved.sample.Resolve();
            if (!record) {
                Fail(&pool, &memory, INST_ERROR_NO_MEMORY);
                return;
            }
            if (!s_project_bank) {
                const auto result = Storage::ReadSampleSidecar(path, saved, saved.sample);
                if (result == Storage::SampleSidecarResult::Invalid ||
                    result == Storage::SampleSidecarResult::IoError) {
                    Fail(&pool, &memory, INST_ERROR_IO);
                    return;
                }
            }
            record->payload.meta = saved.sample;
            s_current_written = 0;
            FillCurrentStatus();
            SendStatus(INST_STATUS_LOAD_PROGRESS);
            s_phase = Phase::ReadSample;
        } break;

        case Phase::ReadSample: {
            LoadedSample& loaded = s_loaded_samples[s_index];
            void* destination = nullptr;
            if (!memory.ptr(loaded.handle, &destination) || !destination) {
                Fail(&pool, &memory, INST_ERROR_NO_MEMORY);
                return;
            }
            const uint32_t remaining = loaded.resident.data_size - s_current_written;
            const UINT chunk = PickReadChunk(s_file, io_buffer_bytes);
            const UINT requested = remaining > chunk ? chunk : static_cast<UINT>(remaining);
            UINT bytes_read = 0;
            const FRESULT fr = f_read(&s_file, io_buffer, requested, &bytes_read);
            if (fr != FR_OK || bytes_read == 0) {
                Fail(&pool, &memory, INST_ERROR_IO);
                return;
            }
            std::memcpy(
                static_cast<uint8_t*>(destination) + s_current_written, io_buffer, bytes_read);
            s_current_written += bytes_read;
            s_loaded_bytes += bytes_read;
            s_status.loaded_bytes = s_loaded_bytes;
            FillCurrentStatus();
            const uint8_t pct =
                s_total_bytes == 0
                    ? 100
                    : static_cast<uint8_t>((static_cast<uint64_t>(s_loaded_bytes) * 100ull) /
                                           s_total_bytes);
            if (pct != s_last_percent) {
                s_last_percent = pct;
                SendStatus(INST_STATUS_LOAD_PROGRESS);
            }
            if (s_current_written == loaded.resident.data_size) {
                CloseFile();
                ++s_index;
                s_phase = Phase::OpenSample;
            }
        } break;

        case Phase::Commit: {
            // Every freshly read sample becomes a Pool record now, in one
            // pass, so a half-committed import never exists: before this
            // point nothing references them; after it the Track does.
            for (uint8_t i = 0; i < s_plan.count; ++i) {
                LoadedSample& loaded = s_loaded_samples[i];
                if (loaded.hit) {
                    continue;
                }
                SamplePool::Record* record = pool.Find(loaded.pool_id);
                if (!record) {
                    Fail(&pool, &memory, INST_ERROR_NO_MEMORY);
                    return;
                }
                SampleFile::Document saved;
                saved.sample = record->payload.meta;
                FillLoadedSample(record->payload,
                                 loaded.pool_id,
                                 s_mapped.sample_paths[s_plan.entries[i].path_zone],
                                 loaded.resident,
                                 loaded.handle);
                SampleFile::Apply(saved, record->payload.meta);
                loaded.admitted = false;  // committed: no longer this load's to abandon
                // Not pushed one by one: a burst of N records overruns the
                // 4-deep TX queue. The frontend pages the Pool
                // (MSG_SAMPLE_META_PAGE_REQ) and sees them there.
            }
            if (s_project_samples_only) {
                // Explicit preload pins every dependency in the private Pool.
                // Tracks and their ownership bits are never changed here.
                for (uint8_t i = 0; i < s_plan.count; ++i)
                    pool.SetPinned(s_loaded_samples[i].pool_id, true);
            } else {
                // The mapper numbered samples 1..N within this document; the
                // zones now name their Pool ids, and the Track takes its refs.
                Instrument& ins = s_bank->At(s_request.slot).instrument;
                ins = s_mapped.instrument;
                for (auto& oscillator: ins.osc)
                    for (auto& zone: oscillator.zones) {
                        if (!zone.in_use || zone.sample_id == 0 || zone.sample_id > s_plan.count) {
                            continue;
                        }
                        zone.sample_id = s_loaded_samples[zone.sample_id - 1].pool_id;
                        pool.SetUsedBy(zone.sample_id, s_request.slot, true);
                    }
                // The mapper knows zones, not where the document came from, so the
                // display name is stamped here - the one place still holding the
                // .sfz path. Truncation is fine; it is a label, not an identifier.
                if (InstrumentMap::PathIsSfz(s_request.path) || !ins.name[0])
                    std::snprintf(ins.name, sizeof(ins.name), "%s", Basename(s_request.path));
                PublishModSlots(s_request.slot);
            }
            s_status.loaded_bytes = s_total_bytes;
            s_status.current_loaded_bytes = s_status.current_bytes;
            SendStatus(INST_STATUS_LOAD_COMPLETE);
            WaveX::Log::PrintLine("SFZ_LOAD: %s '%s' Track %u (%u zones, %u samples, %lu B)",
                                  s_project_samples_only ? "preloaded" : "bound",
                                  s_request.path,
                                  (unsigned)s_request.slot,
                                  (unsigned)s_mapped.zone_count,
                                  (unsigned)s_plan.count,
                                  (unsigned long)s_total_bytes);
            s_phase = Phase::Idle;
        } break;

        case Phase::PrepareSave: {
            const uint8_t error = PrepareSave(pool);
            if (error != INST_ERROR_NONE)
                FinishEdit(error);
        } break;
        case Phase::ProbeSaveSample:
            ProbeSaveSample(pool);
            break;
        case Phase::SaveCopy:
            FinishEdit(SaveCopy());
            break;
        case Phase::Idle:
        case Phase::AwaitVoiceStop:
            break;
    }
}

bool Load(const char* path,
          uint8_t slot,
          SamplePool& pool,
          SampleMemMgr& memory,
          uint8_t* io_buffer,
          uint32_t io_buffer_bytes) {
    InstOpMessage request(0, slot, INST_OP_SFZ_LOAD, path);
    if (!Begin(request))
        return false;
    while (Busy()) {
        Pump(pool, memory, io_buffer, io_buffer_bytes);
        if (VoiceStopTrack() != 0xFF) {
            ConfirmVoicesStopped(pool, memory);  // audio has not started at boot
        }
    }
    // A failed preflight preserves the old Instrument. Its continued
    // presence must not turn this failed replacement into a successful load.
    return s_status.state == INST_STATUS_LOAD_COMPLETE && TrackLoaded(slot);
}

bool BeginProjectLoad(Tracks& candidate) {
    if (Busy())
        return false;
    for (uint8_t track = 0; track < kNumTracks; ++track)
        if (candidate.At(track).instrument.origin != InstrumentOrigin::None)
            return false;
    s_project_failed = false;
    s_project_bank = &candidate;
    return true;
}
bool ProjectLoadActive() {
    return s_project_bank != nullptr;
}
bool ProjectTrackBusy() {
    return s_project_bank && s_phase != Phase::Idle;
}
uint8_t ProjectTrackError() {
    return s_status.error;
}
bool BeginProjectTrack(uint8_t track, const char* path) {
    if (!s_project_bank || s_project_failed || s_phase != Phase::Idle || track >= kNumTracks ||
        !path || strnlen(path, sizeof(s_request.path)) == sizeof(s_request.path) ||
        s_project_bank->At(track).instrument.origin != InstrumentOrigin::None)
        return false;
    s_in_project_step = true;
    s_project_samples_only = false;
    s_project_close_failed = false;
    auto* live = s_bank;
    s_bank = s_project_bank;
    const bool accepted = Begin(InstOpMessage(0, track, INST_OP_SFZ_LOAD, path));
    if (!accepted)
        s_project_failed = true;
    s_bank = live;
    s_in_project_step = false;
    return accepted;
}
bool BeginProjectDocument(uint8_t track, const Wxi::InstrumentFile& document) {
    if (!BeginProjectTrack(track, "Bank.wxi"))
        return false;
    InstrumentMap::FromFile(document, s_mapped);
    Sfz::Status status;
    if (!Sfz::BuildSamplePlan(s_mapped, s_plan, status)) {
        s_project_failed = true;
        s_status.error = INST_ERROR_BAD_FILE;
        s_phase = Phase::Idle;
        return false;
    }
    s_status.zone_count = s_mapped.zone_count;
    s_status.sample_count = s_plan.count;
    s_index = 0;
    s_phase = Phase::ProbeSample;
    return true;
}
bool BeginProjectPreload(const Wxi::InstrumentFile& document) {
    if (!BeginProjectDocument(0, document))
        return false;
    s_project_samples_only = true;
    return true;
}
void PumpProjectLoad(SamplePool& pool, SampleMemMgr& memory, uint8_t* io, uint32_t bytes) {
    if (!s_project_bank || s_phase == Phase::Idle)
        return;
    auto* live = s_bank;
    s_bank = s_project_bank;
    s_in_project_step = true;
    // Candidate Tracks have never sounded; their stop acknowledgement is
    // immediate. The live bank and its callback publications are untouched.
    if (VoiceStopTrack() != 0xff)
        ConfirmVoicesStopped(pool, memory);
    else
        Pump(pool, memory, io, bytes);
    if (s_project_close_failed)
        Fail(&pool, &memory, INST_ERROR_IO);
    s_in_project_step = false;
    s_bank = live;
}
void CancelProjectTrack(SamplePool& pool, SampleMemMgr& memory) {
    if (!s_project_bank || s_phase == Phase::Idle)
        return;
    auto* live = s_bank;
    s_bank = s_project_bank;
    s_in_project_step = true;
    Fail(&pool, &memory, INST_ERROR_BAD_FILE);
    s_in_project_step = false;
    s_bank = live;
}
bool FinishProjectLoad(bool commit, int only_track, uint16_t recall_targets) {
    if (only_track < -1 || only_track >= kNumTracks)
        return false;
    if (!s_project_bank || s_phase != Phase::Idle || (commit && s_project_failed))
        return false;
    if (only_track >= 0) {
        if (!recall_targets)
            recall_targets = static_cast<uint16_t>(1u << only_track);
        if (!(recall_targets & (1u << only_track)))
            return false;
    } else if (recall_targets)
        return false;
    if (commit) {
        if (only_track < 0)
            *s_bank = *s_project_bank;
        else
            for (uint8_t track = 0; track < kNumTracks; ++track)
                if (recall_targets & (1u << track))
                    s_bank->At(track).instrument =
                        s_project_bank->At(static_cast<uint8_t>(only_track)).instrument;
    }
    s_project_bank = nullptr;
    s_project_samples_only = false;
    if (commit) {
        for (uint8_t track = 0; track < kNumTracks; ++track) {
            if (only_track >= 0 && !(recall_targets & (1u << track)))
                continue;
            s_sound_undo[track].Apply();
            if (only_track < 0)
                s_allocation_dirty[track] = false;
            BumpKeyRevision(track);
            PublishModSlots(track);
        }
    }
    return true;
}

bool TrackLoaded(uint8_t slot) {
    return slot < kNumTracks && s_bank->At(slot).instrument.origin != InstrumentOrigin::None;
}

// --- Track settings and MIDI routing (track-and-patch-model.md §2) ---------
//
// Main-loop only, like every other Tracks mutation in this file. The audio
// callback never reads midi_in: routing happens in the note handler before a
// NoteEvent is queued, so the callback still sees a resolved track index.

bool SetTrackMidiIn(uint8_t track, uint8_t midi_in) {
    if (track >= kNumTracks || !TrackMidiInValid(midi_in))
        return false;
    s_bank->At(track).midi_in = midi_in;
    return true;
}

uint8_t TrackMidiIn(uint8_t track) {
    return track < kNumTracks ? s_bank->At(track).midi_in : static_cast<uint8_t>(TRACK_MIDI_IN_OFF);
}

bool SetTrackPolyLimit(uint8_t track, uint8_t limit) {
    if (track >= kNumTracks || limit > WAVEX_NUM_VOICES)
        return false;
    s_bank->At(track).poly_limit = limit;
    return true;
}

uint8_t TrackPolyLimit(uint8_t track) {
    return track < kNumTracks ? s_bank->At(track).poly_limit : 0;
}

bool SetTrackPriority(uint8_t track, uint8_t priority) {
    if (track >= kNumTracks)
        return false;
    s_bank->At(track).priority = priority;
    return true;
}

uint8_t TrackPriority(uint8_t track) {
    return track < kNumTracks ? s_bank->At(track).priority : 0;
}

bool SetTrackProgramChange(uint8_t track, bool enabled) {
    if (track >= kNumTracks)
        return false;
    s_bank->At(track).program_change = enabled ? 1 : 0;
    return true;
}

bool TrackProgramChange(uint8_t track) {
    return track < kNumTracks && s_bank->At(track).program_change != 0;
}

uint8_t TracksForMidiChannel(uint8_t channel, uint8_t* out, uint8_t max) {
    return s_bank->TracksForMidiChannel(channel, out, max);
}

void SetLoadedSampleResolver(const SampleResolver& resolver) {
    s_loaded_resolver = resolver;
}

bool BindSample(
    SamplePool& pool, SampleMemMgr& memory, uint8_t slot, uint16_t sample_id, uint8_t root_note) {
    if (slot >= kNumTracks || Busy())
        return false;
    if (sample_id != 0 && !pool.Find(sample_id))
        return false;
    // Whatever the Track held goes: an import's samples that nobody else
    // holds are freed here (the caller stopped this Track's voices).
    // Assign may choose an unpinned sample from this Track's own import.
    // Transfer that sample into the new binding instead of freeing it with
    // the old zones; the main loop restores its Track reference below.
    ReleaseTrack(pool, memory, slot, sample_id);
    Instrument& ins = s_bank->At(slot).instrument;
    if (sample_id == 0) {
        return true;
    }
    pool.SetUsedBy(sample_id, slot, true);
    ins.osc[0].type = OscType::Sample;
    Zone& zone = ins.osc[0].zones[0];
    zone.sample_id = sample_id;
    zone.root_note = root_note;
    // No override: a Quick Instrument follows its Instrument's defaults, so
    // the Filter/Env pages edit it as a whole rather than one hidden zone.
    zone.flags = 0;
    zone.in_use = true;
    ins.mode = InstrumentMode::Keyboard;
    ins.origin = InstrumentOrigin::Built;
    return true;
}

const char* TrackName(uint8_t slot) {
    if (slot >= kNumTracks)
        return "";
    // A load in flight has not reached Commit, so the bank still holds the
    // PREVIOUS instrument for this slot - naming that would be actively
    // misleading. The request's own path is the truth until Commit runs.
    if (!s_project_bank && TrackLoading(slot) && !s_key_assignment &&
        s_request.op != INST_OP_SET_PAD_SAMPLE)
        return Basename(s_request.path);
    return s_bank->At(slot).instrument.name;
}

uint16_t BoundSample(uint8_t slot) {
    if (slot >= kNumTracks)
        return 0;
    const Instrument& ins = s_bank->At(slot).instrument;
    // Only the unnamed full-keyboard Quick Instrument represents a bare
    // sample binding. Named or edited maps must not masquerade as their
    // first sample in Track replacement/assignment flows.
    if (ins.origin != InstrumentOrigin::Built || ins.mode != InstrumentMode::Keyboard ||
        ins.name[0])
        return 0;
    for (const auto& extra: ins.osc[1].zones)
        if (extra.in_use)
            return 0;
    const auto& z = ins.osc[0].zones[0];
    if (!z.in_use || z.key_lo != 0 || z.key_hi != 127 || z.vel_lo != 1 || z.vel_hi != 127)
        return 0;
    for (uint8_t i = 1; i < kMaxZones; ++i)
        if (ins.osc[0].zones[i].in_use)
            return 0;
    return z.sample_id;
}

uint32_t ReclaimableBytes(SamplePool& pool, uint8_t track) {
    if (track >= kNumTracks)
        return 0;
    const uint16_t bit = static_cast<uint16_t>(1u << track);
    uint64_t bytes = 0;
    pool.ForEach([&](SamplePool::Record& r) {
        if (!r.pinned && r.used_by == bit) {
            bytes += r.payload.allocated_bytes;
        }
    });
    return bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(bytes);
}

void ForgetLoadedSample(uint16_t sample_id) {
    if (sample_id == 0)
        return;
    for (uint8_t slot = 0; slot < kNumTracks; ++slot) {
        Instrument& ins = s_bank->At(slot).instrument;
        if (ins.origin == InstrumentOrigin::None)
            continue;
        bool any_left = false;
        for (auto& oscillator: ins.osc)
            for (auto& zone: oscillator.zones) {
                if (zone.in_use && zone.sample_id == sample_id) {
                    BumpKeyRevision(slot);
                    zone = Zone{};
                }
                any_left = any_left || zone.in_use;
            }
        if (!any_left && ins.mode != InstrumentMode::Drum) {
            ins.origin = InstrumentOrigin::None;
        }
    }
}

uint8_t ResolveNote(
    uint8_t slot, uint8_t note, uint8_t velocity, VoiceTriggerParams* out, uint8_t max) {
    if (slot >= kNumTracks)
        return 0;
    if (s_bank->At(slot).instrument.origin == InstrumentOrigin::None)
        return 0;
    return s_bank->ResolveNote(slot, note, velocity, s_loaded_resolver, out, max);
}

void PrepareSequencerVoices(SequencerVoiceMap& map, uint16_t tracks) {
    for (uint8_t track = 0; track < kNumTracks; ++track) {
        if (!(tracks & (1u << track)))
            continue;
        if (TrackLoading(track))
            map.Revoke(static_cast<uint16_t>(1u << track));
        else {
            const auto& source = s_bank->At(track);
            map.PrepareTrack(track, source.instrument, s_loaded_resolver);
            map.tracks[track].policy = source.allocation.Resolve(source.instrument.allocation);
        }
    }
}

bool SetInstrumentFilter(uint8_t track, const InstrumentFilter& filter) {
    if (track >= kNumTracks)
        return false;
    auto& ins = s_bank->At(track).instrument;
    const auto& old = ins.filter;
    if (old.type != filter.type || old.topology != filter.topology || old.slope != filter.slope ||
        old.drive != filter.drive || old.cutoff_hz != filter.cutoff_hz ||
        old.resonance != filter.resonance || old.keytrack != filter.keytrack ||
        old.env2_amount != filter.env2_amount) {
        if (ins.origin != InstrumentOrigin::None)
            s_sound_undo[track].Capture(ins);
        BumpKeyRevision(track);
    }
    ins.filter = filter;
    return true;
}

bool SetInstrumentEnv(uint8_t track, const InstrumentEnv& env) {
    if (track >= kNumTracks)
        return false;
    const auto& old = s_bank->At(track).instrument.env[0];
    if (old.attack_s != env.attack_s || old.decay_s != env.decay_s || old.sustain != env.sustain ||
        old.release_s != env.release_s) {
        if (s_bank->At(track).instrument.origin != InstrumentOrigin::None)
            s_sound_undo[track].Capture(s_bank->At(track).instrument);
        BumpKeyRevision(track);
    }
    s_bank->At(track).instrument.env[0] = env;
    return true;
}

void PrepareLiveParams(uint8_t track, VoiceLiveParams& live) {
    if (track < kNumTracks)
        PrepareInstrumentLive(s_bank->At(track).instrument, live);
}

const InstrumentFilter* GetInstrumentFilter(uint8_t track) {
    return track < kNumTracks ? &s_bank->At(track).instrument.filter : nullptr;
}

const InstrumentEnv* GetInstrumentEnv(uint8_t track) {
    return track < kNumTracks ? &s_bank->At(track).instrument.env[0] : nullptr;
}

bool SetModSlot(uint8_t slot, uint8_t mod_slot_index, const ModSlot& value) {
    if (slot >= kNumTracks || mod_slot_index >= kMaxModSlots)
        return false;
    if (!IsValidInstModSlot({value.source, value.dest, value.depth, value.curve, value.flags}))
        return false;
    if (s_bank->At(slot).instrument.origin != InstrumentOrigin::None)
        s_sound_undo[slot].Capture(s_bank->At(slot).instrument);
    s_bank->At(slot).instrument.mod_slots[mod_slot_index] = value;
    BumpKeyRevision(slot);
    PublishModSlots(slot);
    return true;
}

const ModSlot* GetModSlots(uint8_t slot) {
    if (slot >= kNumTracks)
        return nullptr;
    s_mod_mailboxes[slot].ConsumeLatest(s_mod_active[slot]);
    return s_mod_active[slot].data();
}

}  // namespace SfzLoader
}  // namespace AudioEngine
}  // namespace WaveX
