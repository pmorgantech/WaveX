#include "bank_file_job.hpp"

#include "bss_static.hpp"
#include "card_space.hpp"
#include <cstdio>

namespace WaveX::Storage {
namespace {
using Codec = BankFile::Result;
bool Directory(const char* path) {
    const auto r = f_mkdir(path);
    return r == FR_OK || r == FR_EXIST;
}
}  // namespace
bool BankFileJob::Begin(const char* name) {
    if (Busy())
        return false;
    if (!BankFile::ValidName(name)) {
        result_ = Result::BadName;
        return false;
    }
    std::snprintf(name_, sizeof(name_), "%s", name);
    std::snprintf(destination_, sizeof(destination_), "0:/wavex/banks/%s.wxb", name);
    ReconstructInPlace(index_);
    document_ = nullptr;
    scratch_ = nullptr;
    bytes_ = 0;
    slot_ = 0;
    edit_slot_ = source_slot_ = destination_slot_ = -1;
    move_slot_ = false;
    io_failed_ = saving_ = false;
    result_ = Result::Working;
    return true;
}
bool BankFileJob::SaveCopy(const char* name,
                           uint32_t request_id,
                           const char* source,
                           int edit_slot,
                           const Wxi::InstrumentFile* document) {
    if (Busy())
        return false;
    if (!request_id || edit_slot < -1 || edit_slot >= BankFile::kSlots ||
        (document && (edit_slot < 0 || !BankFile::ValidInstrumentName(document->name)))) {
        result_ = Result::Invalid;
        return false;
    }
    if (source && !BankFile::ValidName(source)) {
        result_ = Result::BadName;
        return false;
    }
    // source may refer to Index().name, which Begin clears.
    char source_name[24]{};
    if (source)
        std::snprintf(source_name, sizeof(source_name), "%s", source);
    if (!Begin(name))
        return false;
    saving_ = true;
    edit_slot_ = edit_slot;
    document_ = document;
    std::snprintf(temporary_,
                  sizeof(temporary_),
                  "0:/wavex/banks/.%s-%08lx.tmp",
                  name_,
                  static_cast<unsigned long>(request_id));
    if (source) {
        std::snprintf(source_, sizeof(source_), "0:/wavex/banks/%s.wxb", source_name);
        phase_ = Phase::OpenSource;
    } else
        phase_ = Phase::Prepare;
    return true;
}
bool BankFileJob::TransferCopy(const char* name,
                               uint32_t request_id,
                               const char* source,
                               uint8_t source_slot,
                               uint8_t destination_slot,
                               bool move) {
    if (Busy())
        return false;
    if (!source || source_slot >= BankFile::kSlots || destination_slot >= BankFile::kSlots ||
        source_slot == destination_slot) {
        result_ = Result::Invalid;
        return false;
    }
    if (!SaveCopy(name, request_id, source))
        return false;
    source_slot_ = source_slot;
    destination_slot_ = destination_slot;
    move_slot_ = move;
    return true;
}
int BankFileJob::SourceSlot(uint8_t output_slot) const {
    if (output_slot == destination_slot_)
        return source_slot_;
    if (move_slot_ && output_slot == source_slot_)
        return -1;
    return output_slot;
}
bool BankFileJob::LoadIndex(const char* name) {
    if (!Begin(name))
        return false;
    std::snprintf(source_, sizeof(source_), "%s", destination_);
    phase_ = Phase::OpenSource;
    return true;
}
bool BankFileJob::ReadInstrument(const char* name, uint8_t slot, Wxi::InstrumentFile& scratch) {
    if (Busy())
        return false;
    if (slot >= BankFile::kSlots) {
        result_ = Result::Invalid;
        return false;
    }
    if (!LoadIndex(name))
        return false;
    slot_ = slot;
    scratch_ = &scratch;
    return true;
}
bool BankFileJob::Read(void* ctx, void* out, size_t bytes) {
    auto& j = *static_cast<BankFileJob*>(ctx);
    UINT n = 0;
    if (bytes >= 512 || f_read(&j.input_, out, static_cast<UINT>(bytes), &n) != FR_OK) {
        j.io_failed_ = true;
        return false;
    }
    return n == bytes;
}
bool BankFileJob::Write(void* ctx, const void* data, size_t bytes) {
    auto& j = *static_cast<BankFileJob*>(ctx);
    UINT n = 0;
    return bytes < 512 && f_write(&j.output_, data, static_cast<UINT>(bytes), &n) == FR_OK &&
           n == bytes;
}
bool BankFileJob::Eof(void* ctx) {
    return f_eof(&static_cast<BankFileJob*>(ctx)->input_);
}
bool BankFileJob::CloseFiles() {
    bool ok = true;
    if (input_open_) {
        ok = f_close(&input_) == FR_OK;
        input_open_ = false;
    }
    if (output_open_) {
        if (f_close(&output_) != FR_OK)
            ok = false;
        output_open_ = false;
    }
    return ok;
}
void BankFileJob::Finish(Result result) {
    decoder_.reset();
    encoder_.reset();
    if (!CloseFiles())
        result = Result::IoError;
    if (owned_temp_) {
        if (f_unlink(temporary_) != FR_OK)
            result = Result::IoError;
        owned_temp_ = false;
    }
    document_ = nullptr;
    scratch_ = nullptr;
    result_ = result;
}
void BankFileJob::Cancel() {
    if (Busy())
        Finish(Result::Cancelled);
}
void BankFileJob::Pump() {
    if (!Busy())
        return;
    switch (phase_) {
        case Phase::OpenSource: {
            const auto r = f_open(&input_, source_, FA_READ);
            if (r != FR_OK) {
                Finish(r == FR_NO_FILE || r == FR_NO_PATH ? Result::NotFound : Result::IoError);
                return;
            }
            input_open_ = true;
            bytes_ = f_size(&input_);
            if (bytes_ > BankFile::kMaxFileBytes) {
                Finish(Result::Invalid);
                return;
            }
            decoder_.emplace(Wxcf::IoContext{this, Read, nullptr, Eof}, index_);
            phase_ = Phase::Scan;
            break;
        }
        case Phase::Scan: {
            const auto r = decoder_->Advance();
            if (r == Codec::More)
                break;
            decoder_.reset();
            if (r != Codec::Done)
                Finish(io_failed_ ? Result::IoError : Result::Invalid);
            else if (saving_)
                phase_ = Phase::Prepare;
            else if (scratch_)
                phase_ = Phase::ReadSlot;
            else
                Finish(Result::Indexed);
            break;
        }
        case Phase::Prepare: {
            if (source_slot_ >= 0 && !index_.slots[source_slot_].used()) {
                Finish(Result::EmptySlot);
                return;
            }
            bytes_ = 12 + 8 + sizeof(index_.name);
            for (uint16_t slot = 0; slot < BankFile::kSlots; ++slot) {
                const int source = SourceSlot(static_cast<uint8_t>(slot));
                const auto size = slot == edit_slot_
                                      ? (document_ ? Wxi::detail::TotalFileSize(*document_) : 0)
                                      : (source >= 0 ? index_.slots[source].bytes : 0);
                if (!size)
                    continue;
                if (size > BankFile::kMaxDocumentBytes ||
                    bytes_ > BankFile::kMaxFileBytes - 8 - BankFile::kSlotPrefixBytes - size) {
                    Finish(Result::Invalid);
                    return;
                }
                bytes_ += 8 + BankFile::kSlotPrefixBytes + size;
            }
            phase_ = Phase::OpenSave;
            break;
        }
        case Phase::OpenSave: {
            const auto space = CheckSaveSpace(bytes_);
            if (space != SaveSpace::Ready) {
                Finish(space == SaveSpace::Full ? Result::NoSpace : Result::IoError);
                return;
            }
            if (!Directory("0:/wavex") || !Directory("0:/wavex/banks")) {
                Finish(Result::IoError);
                return;
            }
            FILINFO info{};
            const auto found = f_stat(destination_, &info);
            if (found != FR_NO_FILE) {
                Finish(found == FR_OK ? Result::Exists : Result::IoError);
                return;
            }
            const auto opened = f_open(&output_, temporary_, FA_WRITE | FA_CREATE_NEW);
            if (opened != FR_OK) {
                Finish(opened == FR_EXIST ? Result::Exists : Result::IoError);
                return;
            }
            output_open_ = owned_temp_ = true;
            encoder_.emplace(Wxcf::IoContext{this, nullptr, Write, nullptr});
            if (encoder_->Begin(name_) != Codec::More) {
                Finish(Result::IoError);
                return;
            }
            slot_ = 0;
            phase_ = Phase::Write;
            break;
        }
        case Phase::Write: {
            if (slot_ == BankFile::kSlots) {
                if (encoder_->Finish() != Codec::Done || !CloseFiles())
                    Finish(Result::IoError);
                else
                    phase_ = Phase::Publish;
                break;
            }
            const auto slot = static_cast<uint8_t>(slot_++);
            const int source = SourceSlot(slot);
            if (slot == edit_slot_) {
                if (document_ && encoder_->Append(slot, *document_) != Codec::More)
                    Finish(Result::IoError);
            } else if (source >= 0 && index_.slots[source].used()) {
                const auto& metadata = index_.slots[source];
                if (f_lseek(&input_, metadata.offset) != FR_OK ||
                    f_tell(&input_) != metadata.offset ||
                    encoder_->BeginCopy(slot, metadata) != Codec::More)
                    Finish(Result::IoError);
                else
                    phase_ = Phase::Copy;
            }
            break;
        }
        case Phase::Copy:
            if (encoder_->CopyNext({this, Read, nullptr, Eof}) != Codec::More)
                Finish(Result::IoError);
            else if (!encoder_->Copying())
                phase_ = Phase::Write;
            break;
        case Phase::Publish: {
            const auto r = f_rename(temporary_, destination_);
            if (r == FR_OK)
                owned_temp_ = false;
            Finish(r == FR_OK ? Result::Saved : r == FR_EXIST ? Result::Exists : Result::IoError);
            break;
        }
        case Phase::ReadSlot: {
            const auto& slot = index_.slots[slot_];
            if (!slot.used()) {
                Finish(Result::EmptySlot);
                break;
            }
            if (f_lseek(&input_, slot.offset) != FR_OK || f_tell(&input_) != slot.offset) {
                Finish(Result::IoError);
                break;
            }
            const auto r = BankFile::ReadSlot({this, Read, nullptr, Eof}, slot, *scratch_);
            Finish(r == Codec::Done ? Result::InstrumentRead
                   : io_failed_     ? Result::IoError
                                    : Result::Invalid);
            break;
        }
    }
}
}  // namespace WaveX::Storage
