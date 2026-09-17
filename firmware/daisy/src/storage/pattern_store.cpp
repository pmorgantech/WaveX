#include "pattern_store.hpp"

#include "comm/log_ring.h"
#include "comm/mcu_link.h"
#include "ff.h"

#include "bss_static.hpp"
#include "storage/card_space.hpp"
#include "wxcf/pattern_file.hpp"
#include <cstdio>
#include <cstring>
#include <optional>
#ifndef UNIT_TEST
#include "stm32h7xx_hal.h"
extern "C" SD_HandleTypeDef hsd1;
#endif

namespace WaveX {
namespace PatternStore {
using namespace Protocol;
namespace {
enum class Phase { Idle, Capture, OpenSave, Write, OpenLoad, Read, Install };
struct Job {
    Phase phase = Phase::Idle;
    SeqFileOpMessage request;
    SeqFileStatusMessage status;
    bool send = false, open = false, temp_owned = false;
    FRESULT first_io_error = FR_OK;
    FIL file{};  // AXI SRAM sector window; sub-sector adapters never DMA stack records.
    char destination[96]{}, temporary[112]{}, decoded_name[SEQ_FILE_NAME_BYTES]{};
    std::optional<PatternFile::Encoder> encoder;
    std::optional<PatternFile::Decoder> decoder;
};
BssStatic<Job> storage;
Job& job() {
    return storage.Get();
}
FRESULT IoResult(FRESULT result) {
    if (result != FR_OK && job().first_io_error == FR_OK) {
        job().first_io_error = result;
#ifndef UNIT_TEST
        if (result == FR_DISK_ERR || result == FR_INVALID_OBJECT)
            Log::PrintLine("PATTERN_SD: fr=%u hal_err=0x%08lX state=%u offset=%lu",
                           static_cast<unsigned>(result),
                           static_cast<unsigned long>(HAL_SD_GetError(&hsd1)),
                           static_cast<unsigned>(HAL_SD_GetCardState(&hsd1)),
                           static_cast<unsigned long>(f_tell(&job().file)));
#endif
    }
    return result;
}
bool Read(void* context, void* dest, size_t bytes) {
    if (bytes >= 512)
        return false;
    UINT read = 0;
    return IoResult(f_read(static_cast<FIL*>(context), dest, static_cast<UINT>(bytes), &read)) ==
               FR_OK &&
           read == bytes;
}
bool Write(void* context, const void* source, size_t bytes) {
    if (bytes >= 512)
        return false;
    UINT written = 0;
    return IoResult(f_write(
               static_cast<FIL*>(context), source, static_cast<UINT>(bytes), &written)) == FR_OK &&
           written == bytes;
}
bool Eof(void* context) {
    return f_eof(static_cast<FIL*>(context));
}
void Finish(uint8_t error, Sequencer::PatternExchange& exchange) {
    auto& j = job();
    j.encoder.reset();
    j.decoder.reset();
    if (j.open) {
        if (IoResult(f_close(&j.file)) != FR_OK)
            error = SEQ_FILE_IO;
        j.open = false;
    }
    if (j.temp_owned) {
        f_unlink(j.temporary);
        j.temp_owned = false;
    }
    exchange.Retire();
    if (error == SEQ_FILE_IO || error == SEQ_FILE_BAD_FILE)
        Log::PrintLine("PATTERN_FILE: op=%u phase=%u error=%u fatfs=%u name=%s",
                       static_cast<unsigned>(j.request.op),
                       static_cast<unsigned>(j.phase),
                       static_cast<unsigned>(error),
                       static_cast<unsigned>(j.first_io_error),
                       j.request.name);
    if (error == SEQ_FILE_OK)
        detail::CopyWireString(j.status.name,
                               sizeof(j.status.name),
                               j.request.op == SEQ_FILE_NEW ? "" : j.request.name);
    j.status.completed_request_id = j.request.request_id;
    j.status.completed_op = j.request.op;
    j.status.active_request_id = 0;
    j.status.busy = 0;
    j.status.error = error;
    j.phase = Phase::Idle;
    j.send = true;
}
bool MakeDirectory(const char* name) {
    const auto result = f_mkdir(name);
    if (result != FR_OK && result != FR_EXIST)
        IoResult(result);
    return result == FR_OK || result == FR_EXIST;
}
}  // namespace
const char* CurrentName() {
    return job().status.name;
}
void SetProjectPatternName(const char* name) {
    if (!Busy())
        detail::CopyWireString(job().status.name, sizeof(job().status.name), name);
}
bool BlocksEdits() {
    const auto& j = job();
    return j.phase != Phase::Idle && j.request.op != SEQ_FILE_SAVE_COPY;
}
bool Busy() {
    return job().phase != Phase::Idle;
}
bool Request(const SeqFileOpMessage& request, Sequencer::PatternExchange& exchange) {
    if (!IsValidSeqFileOp(request))
        return false;
    auto& j = job();
    j.status.request_id = request.request_id;
    j.send = true;
    if (request.op == SEQ_FILE_GET || request.request_id == j.status.active_request_id ||
        request.request_id == j.status.completed_request_id)
        return false;
    if (j.phase != Phase::Idle) {
        j.status.completed_request_id = request.request_id;
        j.status.completed_op = request.op;
        j.status.error = SEQ_FILE_BUSY;
        return false;
    }
    j.request = request;
    j.first_io_error = FR_OK;
    if (request.op == SEQ_FILE_NEW)
        j.request.name[0] = 0;
    if ((request.op == SEQ_FILE_SAVE_COPY || request.op == SEQ_FILE_LOAD) &&
        !PatternFile::ValidName(request.name)) {
        Finish(SEQ_FILE_BAD_NAME, exchange);
        return false;
    }
    j.status.busy = 1;
    j.status.active_request_id = request.request_id;
    std::snprintf(
        j.destination, sizeof(j.destination), "0:/wavex/patterns/%s.wxpat", j.request.name);
    std::snprintf(j.temporary,
                  sizeof(j.temporary),
                  "0:/wavex/patterns/.%s-%08lx.tmp",
                  j.request.name,
                  static_cast<unsigned long>(request.request_id));
    if (request.op == SEQ_FILE_SAVE_COPY) {
        if (!exchange.Capture()) {
            Finish(SEQ_FILE_CAPTURE_BUSY, exchange);
            return false;
        }
        j.phase = Phase::Capture;
    } else if (request.op == SEQ_FILE_LOAD)
        j.phase = Phase::OpenLoad;
    else {
        ReconstructInPlace(exchange.foreground());
        exchange.Install();
        j.phase = Phase::Install;
    }
    return true;
}
void Pump(Sequencer::PatternExchange& exchange) {
    auto& j = job();
    switch (j.phase) {
        case Phase::Idle:
            break;
        case Phase::Capture:
            if (exchange.state() == Sequencer::PatternExchange::State::Captured)
                j.phase = Phase::OpenSave;
            else if (exchange.state() == Sequencer::PatternExchange::State::Failed)
                Finish(SEQ_FILE_CAPTURE_BUSY, exchange);
            break;
        case Phase::OpenSave: {
            const auto space = Storage::CheckSaveSpace(PatternFile::kFileBytes);
            if (space != Storage::SaveSpace::Ready) {
                Finish(space == Storage::SaveSpace::Full ? SEQ_FILE_NO_SPACE : SEQ_FILE_IO,
                       exchange);
                break;
            }
            if (!MakeDirectory("0:/wavex") || !MakeDirectory("0:/wavex/patterns")) {
                Finish(SEQ_FILE_IO, exchange);
                break;
            }
            FILINFO info{};
            const auto found = f_stat(j.destination, &info);
            if (found != FR_NO_FILE) {
                if (found != FR_OK)
                    IoResult(found);
                Finish(found == FR_OK ? SEQ_FILE_EXISTS : SEQ_FILE_IO, exchange);
                break;
            }
            if (IoResult(f_open(&j.file, j.temporary, FA_WRITE | FA_CREATE_NEW)) != FR_OK) {
                Finish(SEQ_FILE_IO, exchange);
                break;
            }
            j.open = j.temp_owned = true;
            j.encoder.emplace(Wxcf::IoContext{&j.file, nullptr, Write, nullptr},
                              exchange.foreground(),
                              j.request.name);
            j.phase = Phase::Write;
            break;
        }
        case Phase::Write: {
            PatternFile::Result result = PatternFile::Result::More;
            for (unsigned i = 0; i < 8 && result == PatternFile::Result::More; ++i)
                result = j.encoder->Advance();
            if (result == PatternFile::Result::More)
                break;
            j.encoder.reset();
            if (result != PatternFile::Result::Done) {
                Finish(SEQ_FILE_IO, exchange);
                break;
            }
            const auto closed = IoResult(f_close(&j.file));
            j.open = false;
            if (closed != FR_OK) {
                Finish(SEQ_FILE_IO, exchange);
                break;
            }
            const auto renamed = IoResult(f_rename(j.temporary, j.destination));
            if (renamed == FR_OK)
                j.temp_owned = false;
            Finish(renamed == FR_OK      ? SEQ_FILE_OK
                   : renamed == FR_EXIST ? SEQ_FILE_EXISTS
                                         : SEQ_FILE_IO,
                   exchange);
            break;
        }
        case Phase::OpenLoad: {
            const auto opened = IoResult(f_open(&j.file, j.destination, FA_READ));
            if (opened != FR_OK) {
                Finish(
                    opened == FR_NO_FILE || opened == FR_NO_PATH ? SEQ_FILE_NOT_FOUND : SEQ_FILE_IO,
                    exchange);
                break;
            }
            j.open = true;
            if (f_size(&j.file) > PatternFile::kMaxFileBytes) {
                Finish(SEQ_FILE_BAD_FILE, exchange);
                break;
            }
            j.decoder.emplace(Wxcf::IoContext{&j.file, Read, nullptr, Eof},
                              exchange.foreground(),
                              j.decoded_name);
            j.phase = Phase::Read;
            break;
        }
        case Phase::Read: {
            PatternFile::Result result = PatternFile::Result::More;
            for (unsigned i = 0; i < 8 && result == PatternFile::Result::More; ++i)
                result = j.decoder->Advance();
            if (result == PatternFile::Result::More)
                break;
            j.decoder.reset();
            if (result != PatternFile::Result::Done) {
                Finish(j.first_io_error == FR_OK ? SEQ_FILE_BAD_FILE : SEQ_FILE_IO, exchange);
                break;
            }
            const auto closed = IoResult(f_close(&j.file));
            j.open = false;
            if (closed != FR_OK) {
                Finish(SEQ_FILE_IO, exchange);
                break;
            }
            exchange.Install();
            j.phase = Phase::Install;
            break;
        }
        case Phase::Install:
            if (exchange.state() == Sequencer::PatternExchange::State::Installed)
                Finish(SEQ_FILE_OK, exchange);
            break;
    }
    if (j.send && Comm::LinkSend(MSG_SEQ_FILE_STATUS, &j.status, sizeof(j.status)) >= 0)
        j.send = false;
}
}  // namespace PatternStore
}  // namespace WaveX
