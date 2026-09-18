#pragma once

#include "ff.h"

#include "wxcf/bank_file.hpp"
#include <optional>

namespace WaveX::Storage {
// Foreground-only SD transactions, not a runtime Bank/Track owner. Place this
// FIL-bearing object in AXI SRAM. The caller serializes card jobs and preserves
// the source file and borrowed WXI document until Busy() becomes false.
// ReadInstrument needs private scratch; never decode into a live Track.
class BankFileJob {
   public:
    enum class Result {
        Idle,
        Working,
        Saved,
        Indexed,
        InstrumentRead,
        Cancelled,
        BadName,
        Exists,
        NoSpace,
        NotFound,
        EmptySlot,
        Invalid,
        IoError
    };
    // No source creates an empty Bank. edit_slot -1 copies unchanged; a selected
    // slot with nullptr document clears it. A document must be validated WXI.
    bool SaveCopy(const char* name,
                  uint32_t request_id,
                  const char* source = nullptr,
                  int edit_slot = -1,
                  const Wxi::InstrumentFile* document = nullptr);
    // Transfers the serialized document into a new Bank; never edits the source file.
    bool TransferCopy(const char* name,
                      uint32_t request_id,
                      const char* source,
                      uint8_t source_slot,
                      uint8_t destination_slot,
                      bool move);
    bool LoadIndex(const char* name);
    bool ReadInstrument(const char* name, uint8_t slot, Wxi::InstrumentFile& scratch);
    void Pump();
    void Cancel();
    bool Busy() const { return result_ == Result::Working; }
    Result Status() const { return result_; }
    // Only usable after Indexed or InstrumentRead. Working/failure never
    // authorizes publication; the session owner keeps its old index separately.
    const BankFile::Index& Index() const { return index_; }
    uint32_t FileBytes() const { return bytes_; }
    BankFileJob() = default;
    BankFileJob(const BankFileJob&) = delete;
    BankFileJob& operator=(const BankFileJob&) = delete;

   private:
    enum class Phase { OpenSource, Scan, Prepare, OpenSave, Write, Copy, Publish, ReadSlot };
    bool Begin(const char* name);
    int SourceSlot(uint8_t output_slot) const;
    void Finish(Result result);
    bool CloseFiles();
    static bool Read(void*, void*, size_t);
    static bool Write(void*, const void*, size_t);
    static bool Eof(void*);
    FIL input_{}, output_{};
    BankFile::Index index_{};
    std::optional<BankFile::IndexDecoder> decoder_;
    std::optional<BankFile::Encoder> encoder_;
    const Wxi::InstrumentFile* document_ = nullptr;
    Wxi::InstrumentFile* scratch_ = nullptr;
    char name_[24]{}, source_[96]{}, destination_[96]{}, temporary_[112]{};
    uint32_t bytes_ = 0;
    int edit_slot_ = -1, source_slot_ = -1, destination_slot_ = -1;
    bool move_slot_ = false;
    uint16_t slot_ = 0;
    bool saving_ = false, input_open_ = false, output_open_ = false;
    bool owned_temp_ = false, io_failed_ = false;
    Phase phase_ = Phase::OpenSource;
    Result result_ = Result::Idle;
};
}  // namespace WaveX::Storage
