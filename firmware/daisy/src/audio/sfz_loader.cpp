#include "sfz_loader.hpp"

#include "comm/daisy_uart_link.h"
#include "comm/log_ring.h"
#include "config/hardware_config.h"
#include "ff.h"
#include "memory.h"

#include "sample_load_info.hpp"
#include "sfz_import.hpp"
#include "storage/fatfs_wav_reader.hpp"
#include "wav/wav_header_parser.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace AudioEngine {
namespace SfzLoader {
namespace {

using namespace WaveX::Protocol;

struct LoadedSample {
    wxsamp_t handle = {};
    ResidentSampleInfo resident = {};
    uint32_t data_offset = 0;
};

enum class Phase : uint8_t {
    Idle,
    OpenSfz,
    ParseLine,
    FinishParse,
    ProbeSample,
    AwaitVoiceStop,
    AllocateSample,
    OpenSample,
    ReadSample,
    Commit,
};

static InstrumentBank s_bank;
static Sfz::SampleTable s_sample_table;
static int8_t s_bound_slot = -1;
static uint32_t s_active_bytes = 0;
static uint8_t s_active_count = 0;
static LoadedSample s_loaded_samples[kMaxZones];

static InstOpMessage s_request;
static InstStatusMessage s_status;
static Phase s_phase = Phase::Idle;
static Sfz::Parser s_parser;
static Sfz::MappedInstrument s_mapped;
static Sfz::SamplePlan s_plan;
static Sfz::SampleProbe s_probes[kMaxZones];
static char s_line[Sfz::kMaxLine];
static FIL s_file;
static bool s_file_open = false;
static uint32_t s_line_number = 0;
static uint8_t s_index = 0;
static uint8_t s_allocated = 0;
static uint32_t s_total_bytes = 0;
static uint32_t s_loaded_bytes = 0;
static uint32_t s_current_written = 0;
static uint8_t s_last_percent = 0xFF;

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
        WaveX::Comm::UartLinkSend(MSG_INST_STATUS, &s_status, sizeof(s_status));
    }
}

void CloseFile() {
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = false;
    }
}

void ReleaseRange(SampleMemMgr& memory, LoadedSample* samples, uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        memory.release(&samples[i].handle);
        samples[i] = LoadedSample{};
    }
}

void Fail(SampleMemMgr* memory, uint8_t error) {
    CloseFile();
    if (memory && s_allocated > 0) {
        ReleaseRange(*memory, s_loaded_samples, s_allocated);
    }
    s_allocated = 0;
    SendStatus(INST_STATUS_FAILED, error);
    s_phase = Phase::Idle;
}

uint32_t AvailableBytes(SampleMemMgr& memory) {
    wxsamp_stats_t stats{};
    memory.stats(&stats);
    uint64_t free_bytes = static_cast<uint64_t>(stats.large_free_bytes) + stats.small_free_bytes;
    // A runtime load replaces the one v1 resident bank, so its allocations
    // become available after the callback stop acknowledgement.
    free_bytes += s_active_bytes;
    if (free_bytes <= WAVEX_INST_LOAD_RESERVE_BYTES)
        return 0;
    free_bytes -= WAVEX_INST_LOAD_RESERVE_BYTES;
    return free_bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(free_bytes);
}

enum class ProbeResult : uint8_t { Ok, Missing, Invalid };

ProbeResult ProbeCurrent() {
    const char* path = s_mapped.sample_paths[s_plan.entries[s_index].path_zone];
    FRESULT fr = f_open(&s_file, path, FA_READ);
    if (fr != FR_OK) {
        WaveX::Log::PrintLine(
            "SFZ_PROBE: sample %u missing (%d): '%s'", (unsigned)s_index, (int)fr, path);
        return ProbeResult::Missing;
    }
    s_file_open = true;
    WaveX::Wav::WavInfo wav_info;
    WaveX::Storage::FatFsWavReader reader(s_file);
    const auto parsed = WaveX::Wav::ParseWavHeader(reader, wav_info);
    SampleLoadMessage hint;
    hint.sample_id = s_plan.entries[s_index].sample_id;
    ResidentSampleInfo resident;
    const bool valid = parsed == WaveX::Wav::ParseResult::Ok &&
                       BuildResidentSampleInfo(hint,
                                               wav_info,
                                               static_cast<uint32_t>(f_size(&s_file)),
                                               WAVEX_INST_MAX_RAM_SAMPLE_BYTES,
                                               resident);
    CloseFile();
    if (!valid) {
        WaveX::Log::PrintLine("SFZ_PROBE: sample %u unsupported: '%s'", (unsigned)s_index, path);
        return ProbeResult::Invalid;
    }
    s_loaded_samples[s_index].resident = resident;
    s_loaded_samples[s_index].data_offset = wav_info.data_offset;
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

}  // namespace

void Reset() {
    CloseFile();
    s_bank = InstrumentBank{};
    s_sample_table.Clear();
    s_bound_slot = -1;
    s_active_bytes = 0;
    s_active_count = 0;
    s_phase = Phase::Idle;
}

bool Begin(const InstOpMessage& request) {
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
                WaveX::Comm::UartLinkSend(MSG_INST_STATUS, &busy, sizeof(busy));
            }
            return false;
        }
    }
    if (request.slot >= kNumInstrumentSlots || request.path[0] == '\0' ||
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
    s_mapped = Sfz::MappedInstrument{};
    s_plan = Sfz::SamplePlan{};
    for (auto& probe: s_probes) {
        probe = Sfz::SampleProbe{};
    }
    s_line_number = 0;
    s_index = 0;
    s_allocated = 0;
    s_total_bytes = 0;
    s_loaded_bytes = 0;
    s_current_written = 0;
    s_last_percent = 0xFF;
    s_phase = Phase::OpenSfz;
    SendStatus(INST_STATUS_PROBING);
    return true;
}

bool Busy() {
    return s_phase != Phase::Idle;
}

bool SlotLoading(uint8_t slot) {
    return Busy() && s_request.op == INST_OP_SFZ_LOAD && s_request.slot == slot;
}

bool NeedsVoiceStop() {
    return s_phase == Phase::AwaitVoiceStop;
}

void ConfirmVoicesStopped(SampleMemMgr& memory) {
    if (s_phase != Phase::AwaitVoiceStop)
        return;
    if (s_active_count > 0) {
        ReleaseRange(memory, s_loaded_samples, s_active_count);
    }
    s_bank = InstrumentBank{};
    s_sample_table.Clear();
    s_bound_slot = -1;
    s_active_count = 0;
    s_active_bytes = 0;
    s_allocated = 0;
    s_index = 0;
    s_phase = Phase::AllocateSample;
    SendStatus(INST_STATUS_LOAD_BEGIN);
}

void Pump(SampleMemMgr& memory, uint8_t* io_buffer, uint32_t io_buffer_bytes) {
    if (s_phase == Phase::Idle || s_phase == Phase::AwaitVoiceStop)
        return;
    if (!memory.initialized() || !io_buffer || io_buffer_bytes < 512) {
        Fail(&memory, INST_ERROR_NO_MEMORY);
        return;
    }

    switch (s_phase) {
        case Phase::OpenSfz: {
            const FRESULT fr = f_open(&s_file, s_request.path, FA_READ);
            if (fr != FR_OK) {
                Fail(&memory, INST_ERROR_BAD_FILE);
                return;
            }
            s_file_open = true;
            s_phase = Phase::ParseLine;
        } break;

        case Phase::ParseLine: {
            if (f_gets(s_line, static_cast<int>(sizeof(s_line)), &s_file) != nullptr) {
                ++s_line_number;
                const size_t length = std::strlen(s_line);
                if ((length + 1 == sizeof(s_line) && s_line[length - 1] != '\n' &&
                     !f_eof(&s_file)) ||
                    !s_parser.FeedLine(s_line, s_line_number)) {
                    Fail(&memory,
                         s_parser.GetStatus().error == Sfz::Error::TooManyRegions
                             ? INST_ERROR_TOO_MANY_REGIONS
                             : INST_ERROR_BAD_FILE);
                }
            } else {
                if (f_error(&s_file) != 0) {
                    Fail(&memory, INST_ERROR_IO);
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
                Fail(&memory,
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
                const ProbeResult result = ProbeCurrent();
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
            s_status.available_bytes = AvailableBytes(memory);
            if (s_total_bytes > s_status.available_bytes) {
                s_status.flags |= INST_STATUS_EXCEEDS_MEMORY;
            }
            if (s_request.op == INST_OP_SFZ_PROBE) {
                SendStatus(INST_STATUS_PROBE_COMPLETE);
                s_phase = Phase::Idle;
                return;
            }
            if (s_status.flags & INST_STATUS_MISSING_FILES) {
                Fail(&memory, INST_ERROR_MISSING_SAMPLES);
            } else if (s_status.flags & INST_STATUS_INVALID_FILES) {
                Fail(&memory, INST_ERROR_UNSUPPORTED_SAMPLE);
            } else if (s_status.flags & INST_STATUS_EXCEEDS_MEMORY) {
                Fail(&memory, INST_ERROR_TOO_LARGE);
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
            if (!memory.alloc(s_loaded_samples[s_index].resident.data_size,
                              &s_loaded_samples[s_index].handle)) {
                Fail(&memory, INST_ERROR_NO_MEMORY);
                return;
            }
            ++s_allocated;
            ++s_index;
        } break;

        case Phase::OpenSample: {
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
                Fail(&memory, INST_ERROR_IO);
                return;
            }
            s_current_written = 0;
            FillCurrentStatus();
            SendStatus(INST_STATUS_LOAD_PROGRESS);
            s_phase = Phase::ReadSample;
        } break;

        case Phase::ReadSample: {
            LoadedSample& loaded = s_loaded_samples[s_index];
            void* destination = nullptr;
            if (!memory.ptr(loaded.handle, &destination) || !destination) {
                Fail(&memory, INST_ERROR_NO_MEMORY);
                return;
            }
            const uint32_t remaining = loaded.resident.data_size - s_current_written;
            const UINT chunk = PickReadChunk(s_file, io_buffer_bytes);
            const UINT requested = remaining > chunk ? chunk : static_cast<UINT>(remaining);
            UINT bytes_read = 0;
            const FRESULT fr = f_read(&s_file, io_buffer, requested, &bytes_read);
            if (fr != FR_OK || bytes_read == 0) {
                Fail(&memory, INST_ERROR_IO);
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
            s_sample_table.Clear();
            for (uint8_t i = 0; i < s_plan.count; ++i) {
                void* sample_ptr = nullptr;
                const LoadedSample& loaded = s_loaded_samples[i];
                if (!memory.ptr(loaded.handle, &sample_ptr) || !sample_ptr ||
                    !s_sample_table.Bind(s_plan.entries[i].sample_id,
                                         SampleRef{static_cast<const int16_t*>(sample_ptr),
                                                   loaded.resident.total_frames,
                                                   loaded.resident.channels,
                                                   loaded.resident.sample_rate})) {
                    s_sample_table.Clear();
                    Fail(&memory, INST_ERROR_NO_MEMORY);
                    return;
                }
            }
            s_bank.Slot(s_request.slot) = s_mapped.instrument;
            s_bound_slot = static_cast<int8_t>(s_request.slot);
            s_active_count = s_plan.count;
            s_active_bytes = s_total_bytes;
            s_status.loaded_bytes = s_total_bytes;
            s_status.current_loaded_bytes = s_status.current_bytes;
            SendStatus(INST_STATUS_LOAD_COMPLETE);
            WaveX::Log::PrintLine("SFZ_LOAD: bound '%s' to slot %u (%u zones, %u samples, %lu B)",
                                  s_request.path,
                                  (unsigned)s_request.slot,
                                  (unsigned)s_mapped.zone_count,
                                  (unsigned)s_plan.count,
                                  (unsigned long)s_total_bytes);
            s_phase = Phase::Idle;
        } break;

        case Phase::Idle:
        case Phase::AwaitVoiceStop:
            break;
    }
}

bool Load(const char* path,
          uint8_t slot,
          SampleMemMgr& memory,
          uint8_t* io_buffer,
          uint32_t io_buffer_bytes) {
    InstOpMessage request(0, slot, INST_OP_SFZ_LOAD, path);
    if (!Begin(request))
        return false;
    while (Busy()) {
        Pump(memory, io_buffer, io_buffer_bytes);
        if (NeedsVoiceStop()) {
            ConfirmVoicesStopped(memory);  // audio has not started at boot
        }
    }
    return SlotLoaded(slot);
}

bool SlotLoaded(uint8_t slot) {
    return s_bound_slot >= 0 && slot == static_cast<uint8_t>(s_bound_slot);
}

uint8_t ResolveNote(
    uint8_t slot, uint8_t note, uint8_t velocity, VoiceTriggerParams* out, uint8_t max) {
    if (!SlotLoaded(slot))
        return 0;
    return s_bank.ResolveNote(slot, note, velocity, s_sample_table.Resolver(), out, max);
}

bool SetModSlot(uint8_t slot, uint8_t mod_slot_index, const ModSlot& value) {
    if (slot >= kNumInstrumentSlots || mod_slot_index >= kMaxModSlots)
        return false;
    s_bank.Slot(slot).mod_slots[mod_slot_index] = value;
    return true;
}

const ModSlot* GetModSlots(uint8_t slot) {
    if (slot >= kNumInstrumentSlots)
        return nullptr;
    return s_bank.Slot(slot).mod_slots;
}

}  // namespace SfzLoader
}  // namespace AudioEngine
}  // namespace WaveX
