// The Sample Pool's registry: every resident sample, whoever loaded it,
// refcounted by path (docs/features/track-and-patch-model.md §4).
//
// One registry replaces the two the Daisy carried - the WAV registry the
// browser loaded into and the SFZ loader's private table - so an import's
// samples are ordinary Pool entries the Sample Manager can list and Sample
// Edit can edit, two imports can share a sample by path, and a Track holding
// an import can take a bare sample (a deref, not a refusal).
//
// Shape, from §4's measurements at 1024 entries:
//
// - Indexed, never scanned, on the note path. A sample_id carries its own
//   slot: bits 0..9 are the slot, bits 10..15 a generation counter (1..63,
//   never 0, so no id is ever 0) that changes each time the slot is reused.
//   Find() is one masked index and one compare; a stale id fails instead of
//   resolving to whatever recycled its slot. Measured at ~330 ns for the
//   record read that has to happen anyway, against 188 us for the scan.
// - The index - one id per slot, 2 KB at 1024 - lives inside this object,
//   in internal RAM, and is all the note path reads before the record. The
//   records (used-by mask, path hash, generation, the caller's Payload -
//   ~130 B each on the Daisy) live wherever the caller puts them, SDRAM
//   there, and are touched only by slot. Path dedupe walks the records, but
//   only at load time.
// - Admission fails, it never evicts: Admit() reports Full and the caller
//   turns that into a reason the UI shows. "The user unloads; the engine
//   never guesses."
//
// Ownership: `used_by` is a 16-bit mask of the Tracks whose Instrument
// references the sample; `pinned` marks a sample the user loaded explicitly
// (Browse > Load), which only an explicit unload releases. A sample is
// releasable when it is neither pinned nor used. The registry never frees
// audio memory itself - it has no allocator - it tells the caller when.
//
// HAL-free and host-tested (firmware/shared/tests/audio/sample_registry_test).
// Main-loop only on the Daisy; nothing here is safe from the callback.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace Audio {

/// FNV-1a over the path bytes, case-sensitive: two spellings of one file are
/// two entries, which is the conservative failure (a duplicate load, not a
/// wrong sample).
inline uint32_t HashSamplePath(const char* path) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path ? path : ""); *p;
         ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;  // 0 means "no path" in the index
}

template <typename Payload, size_t Capacity>
class SampleRegistry {
   public:
    static_assert(Capacity > 0 && Capacity <= 1024, "slot bits are 0..9");
    static constexpr size_t kCapacity = Capacity;
    static constexpr uint16_t kSlotBits = 10;
    static constexpr uint16_t kSlotMask = (1u << kSlotBits) - 1u;
    static constexpr uint16_t kMaxGeneration = (0xFFFFu >> kSlotBits);  // 63

    struct Record {
        uint16_t sample_id = 0;   ///< as on the wire; 0 = slot free
        uint16_t used_by = 0;     ///< Track mask
        uint16_t generation = 0;  ///< last generation this slot handed out; survives Remove()
        bool pinned = false;
        uint32_t path_hash = 0;
        Payload payload{};
    };

    enum class Admit : uint8_t { Ok, Full, AlreadyResident };

    /// `records` must hold Capacity Records and outlive the registry. They
    /// are zeroed here, so SDRAM that came up with garbage is fine.
    explicit SampleRegistry(Record* records) : records_(records) {
        for (size_t i = 0; i < Capacity; ++i) {
            ids_[i] = 0;
            records_[i] = Record{};
        }
    }

    static uint16_t SlotOf(uint16_t sample_id) { return sample_id & kSlotMask; }

    /// O(1): the id names its slot; the slot's own id must agree.
    Record* Find(uint16_t sample_id) {
        if (sample_id == 0) {
            return nullptr;
        }
        const uint16_t slot = SlotOf(sample_id);
        if (slot >= Capacity || ids_[slot] != sample_id) {
            return nullptr;
        }
        return &records_[slot];
    }
    const Record* Find(uint16_t sample_id) const {
        return const_cast<SampleRegistry*>(this)->Find(sample_id);
    }

    /// Path dedupe: an index walk (not the note path), one compare per slot.
    Record* FindByPath(uint32_t path_hash) {
        if (path_hash == 0) {
            return nullptr;
        }
        for (size_t i = 0; i < Capacity; ++i) {
            if (ids_[i] != 0 && records_[i].path_hash == path_hash) {
                return &records_[i];
            }
        }
        return nullptr;
    }

    /// Claims a slot for `path_hash`. On Ok, `*out` is the new record with
    /// its id assigned, used_by 0, not pinned; the caller fills the payload.
    /// AlreadyResident hands back the existing record instead - the caller
    /// decides whether that is a hit (it usually is) or a mistake.
    Admit AdmitPath(uint32_t path_hash, Record** out) {
        if (Record* existing = FindByPath(path_hash)) {
            *out = existing;
            return Admit::AlreadyResident;
        }
        // Next free slot after the last one handed out, so freed slots are
        // not reused immediately: a stale id has to survive Capacity
        // admissions AND a generation wrap before it could alias.
        for (size_t n = 0; n < Capacity; ++n) {
            const size_t slot = (next_slot_ + n) % Capacity;
            if (ids_[slot] != 0) {
                continue;
            }
            Record& r = records_[slot];
            uint16_t gen = static_cast<uint16_t>(r.generation + 1u);
            if (gen == 0 || gen > kMaxGeneration) {
                gen = 1;
            }
            r = Record{};
            r.generation = gen;
            r.sample_id = static_cast<uint16_t>((gen << kSlotBits) | slot);
            r.path_hash = path_hash;
            ids_[slot] = r.sample_id;
            next_slot_ = (slot + 1) % Capacity;
            ++count_;
            *out = &r;
            return Admit::Ok;
        }
        *out = nullptr;
        return Admit::Full;
    }

    /// Forgets the entry. The caller releases the audio memory first (or
    /// after - the registry does not touch it). False if the id is stale.
    bool Remove(uint16_t sample_id) {
        Record* r = Find(sample_id);
        if (!r) {
            return false;
        }
        const uint16_t slot = SlotOf(sample_id);
        const uint16_t gen = r->generation;
        ids_[slot] = 0;
        *r = Record{};
        r->generation = gen;  // so the next admission here gets a fresh id
        --count_;
        return true;
    }

    // -- ownership ----------------------------------------------------------

    bool SetUsedBy(uint16_t sample_id, uint8_t track, bool used) {
        Record* r = Find(sample_id);
        if (!r || track >= 16) {
            return false;
        }
        const uint16_t bit = static_cast<uint16_t>(1u << track);
        r->used_by = used ? (r->used_by | bit) : (r->used_by & static_cast<uint16_t>(~bit));
        return true;
    }

    bool SetPinned(uint16_t sample_id, bool pinned) {
        Record* r = Find(sample_id);
        if (!r) {
            return false;
        }
        r->pinned = pinned;
        return true;
    }

    /// Neither the user nor any Track holds it: the caller may free it.
    bool Releasable(uint16_t sample_id) const {
        const Record* r = Find(sample_id);
        return r && !r->pinned && r->used_by == 0;
    }

    /// Clears Track `track` from every entry and reports, through
    /// `on_releasable`, each id that became releasable - the per-track unbind the model
    /// wants, in one pass over the index.
    template <typename Fn>
    void ClearTrack(uint8_t track, Fn on_releasable) {
        if (track >= 16) {
            return;
        }
        const uint16_t bit = static_cast<uint16_t>(1u << track);
        for (size_t i = 0; i < Capacity; ++i) {
            if (ids_[i] == 0 || (records_[i].used_by & bit) == 0) {
                continue;
            }
            records_[i].used_by &= static_cast<uint16_t>(~bit);
            if (records_[i].used_by == 0 && !records_[i].pinned) {
                on_releasable(ids_[i]);
            }
        }
    }

    // -- enumeration --------------------------------------------------------

    size_t Count() const { return count_; }

    /// Resident entries in slot order, `first` counted among resident entries
    /// (not slots), at most `max` of them. Returns how many were written.
    /// `total` is Count(). This is what a paged MSG_SAMPLE_META_PAGE reply
    /// walks, so the frontend's window is stable across pushes.
    size_t Page(size_t first, size_t max, Record** out, size_t* total) const {
        if (total) {
            *total = count_;
        }
        size_t seen = 0;
        size_t n = 0;
        for (size_t i = 0; i < Capacity && n < max; ++i) {
            if (ids_[i] == 0) {
                continue;
            }
            if (seen++ < first) {
                continue;
            }
            out[n++] = const_cast<Record*>(&records_[i]);
        }
        return n;
    }

    /// Visits every resident record in slot order.
    template <typename Fn>
    void ForEach(Fn fn) {
        for (size_t i = 0; i < Capacity; ++i) {
            if (ids_[i] != 0) {
                fn(records_[i]);
            }
        }
    }

    /// The most recently admitted id still resident, or 0. Some callers want
    /// "the sample that just loaded" without threading the id through.
    uint16_t Newest() const { return Find(newest_) ? newest_ : 0; }
    void NoteNewest(uint16_t sample_id) { newest_ = sample_id; }

   private:
    uint16_t ids_[Capacity];  ///< the SRAM index: 0 = free
    Record* records_;
    size_t next_slot_ = 0;
    size_t count_ = 0;
    uint16_t newest_ = 0;
};

}  // namespace Audio
}  // namespace WaveX
