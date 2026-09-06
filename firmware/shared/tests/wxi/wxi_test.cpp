#include "wxi/wxi.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using WaveX::Wxi::FilterType;
using WaveX::Wxi::InstrumentFile;
using WaveX::Wxi::Mode;
using WaveX::Wxi::OscType;
using WaveX::Wxi::Result;
using WaveX::Wxi::Zone;
namespace Wxcf = WaveX::Wxcf;
namespace Wxi = WaveX::Wxi;

namespace {

// Same in-memory IoContext the container's own tests use (wxcf_test.cpp): a
// short read is an I/O error, which is what makes "read past the last chunk"
// double as the EOF signal the reader stops on.
struct MemoryIo {
    std::vector<uint8_t> buf;
    size_t read_pos = 0;

    static bool Write(void* self, const void* src, size_t len) {
        auto* m = static_cast<MemoryIo*>(self);
        const uint8_t* p = static_cast<const uint8_t*>(src);
        m->buf.insert(m->buf.end(), p, p + len);
        return true;
    }

    static bool Read(void* self, void* dest, size_t len) {
        auto* m = static_cast<MemoryIo*>(self);
        if (m->read_pos + len > m->buf.size())
            return false;
        std::memcpy(dest, m->buf.data() + m->read_pos, len);
        m->read_pos += len;
        return true;
    }

    Wxcf::IoContext AsWriter() {
        Wxcf::IoContext io;
        io.user_data = this;
        io.write = &Write;
        return io;
    }
    Wxcf::IoContext AsReader() {
        Wxcf::IoContext io;
        io.user_data = this;
        io.read = &Read;
        return io;
    }
};

// A document with every field set to something distinguishable from its
// default, so a round-trip that silently drops a field fails rather than
// passing on coincidence.
InstrumentFile MakeFullDoc() {
    InstrumentFile d;
    std::strncpy(d.name, "Rhodes Mk I", sizeof(d.name) - 1);
    d.tags = 0x24;
    d.mode = Mode::Drum;
    d.transpose = -12;
    d.fine_tune = 7;
    d.trim_gain = 0.75f;
    d.trim_pan = 0.25f;
    d.output = 3;
    d.poly_mode = 2;
    d.osc_mix = 0.4f;

    for (uint8_t o = 0; o < Wxi::kNumOscillators; ++o) {
        Wxi::Oscillator& osc = d.osc[o];
        osc.type = OscType::Sample;
        osc.level = 0.8f - 0.1f * static_cast<float>(o);
        osc.pan = 0.3f + 0.2f * static_cast<float>(o);
        osc.coarse_tune = static_cast<int8_t>(-5 + o);
        osc.fine_tune = static_cast<int8_t>(11 + o);
        osc.keytrack = static_cast<uint8_t>(o == 0 ? 1 : 0);
        osc.zone_count = 3;
        for (uint8_t z = 0; z < osc.zone_count; ++z) {
            Zone& zn = osc.zones[z];
            zn.index = static_cast<uint8_t>(z * 4 + o);
            const std::string p =
                "0:/wavex/samples/kit/hit_" + std::to_string(o) + std::to_string(z) + ".wav";
            std::strncpy(zn.path, p.c_str(), sizeof(zn.path) - 1);
            zn.key_lo = static_cast<uint8_t>(36 + z * 8);
            zn.key_hi = static_cast<uint8_t>(43 + z * 8);
            zn.vel_lo = static_cast<uint8_t>(1 + z * 40);
            zn.vel_hi = static_cast<uint8_t>(40 + z * 40);
            zn.root_note = static_cast<uint8_t>(60 + z);
            zn.coarse_tune = static_cast<int8_t>(-3 + z);
            zn.fine_tune = static_cast<int8_t>(20 - z);
            zn.gain = 0.6f + 0.05f * static_cast<float>(z);
            zn.pan = 0.1f * static_cast<float>(z + 1);
            zn.start_frame = 100u + z;
            zn.end_frame = 200000u + z;
            zn.loop_start = 500u + z;
            zn.loop_end = 9000u + z;
            zn.loop_mode = static_cast<uint8_t>(z % 3);
            zn.choke_group = static_cast<uint8_t>(z + 1);
            zn.output_bus = static_cast<uint8_t>(z);
            zn.flags = static_cast<uint8_t>(1u << z);
            zn.cutoff_hz = 800.0f * static_cast<float>(z + 1);
            zn.attack_s = 0.01f * static_cast<float>(z + 1);
            zn.decay_s = 0.2f;
            zn.sustain = 0.5f;
            zn.release_s = 1.5f;
        }
    }

    d.filter.type = FilterType::SvfBp;
    d.filter.cutoff_hz = 3200.0f;
    d.filter.resonance = 0.6f;
    d.filter.keytrack = 0.5f;
    d.filter.env2_amount = -0.75f;
    d.amp.velocity_curve = 2;

    for (uint8_t i = 0; i < Wxi::kNumEnvelopes; ++i) {
        d.env[i].attack_s = 0.02f * static_cast<float>(i + 1);
        d.env[i].decay_s = 0.3f * static_cast<float>(i + 1);
        d.env[i].sustain = 0.25f * static_cast<float>(i + 1);
        d.env[i].release_s = 0.9f * static_cast<float>(i + 1);
    }
    for (uint8_t i = 0; i < Wxi::kNumVoiceLfos; ++i) {
        d.lfo[i].wave = static_cast<uint8_t>(i + 1);
        d.lfo[i].rate_hz = 5.5f + static_cast<float>(i);
        d.lfo[i].sync_div = static_cast<uint8_t>(i * 3);
        d.lfo[i].delay_s = 0.4f;
        d.lfo[i].fade_s = 0.2f;
        d.lfo[i].retrigger = static_cast<uint8_t>(i == 0 ? 1 : 0);
    }
    for (uint8_t i = 0; i < Wxi::kMaxModSlots; ++i) {
        d.mod_slots[i].source = static_cast<uint8_t>(i + 1);
        d.mod_slots[i].dest = static_cast<uint8_t>((i % 4) + 1);
        d.mod_slots[i].depth = static_cast<int16_t>(-20000 + i * 5000);
        d.mod_slots[i].curve = static_cast<uint8_t>(i % 3);
        d.mod_slots[i].flags = static_cast<uint8_t>(i & 1);
    }
    return d;
}

void ExpectZoneEq(const Zone& a, const Zone& b) {
    EXPECT_EQ(a.index, b.index);
    EXPECT_STREQ(a.path, b.path);
    EXPECT_EQ(a.key_lo, b.key_lo);
    EXPECT_EQ(a.key_hi, b.key_hi);
    EXPECT_EQ(a.vel_lo, b.vel_lo);
    EXPECT_EQ(a.vel_hi, b.vel_hi);
    EXPECT_EQ(a.root_note, b.root_note);
    EXPECT_EQ(a.coarse_tune, b.coarse_tune);
    EXPECT_EQ(a.fine_tune, b.fine_tune);
    EXPECT_FLOAT_EQ(a.gain, b.gain);
    EXPECT_FLOAT_EQ(a.pan, b.pan);
    EXPECT_EQ(a.start_frame, b.start_frame);
    EXPECT_EQ(a.end_frame, b.end_frame);
    EXPECT_EQ(a.loop_start, b.loop_start);
    EXPECT_EQ(a.loop_end, b.loop_end);
    EXPECT_EQ(a.loop_mode, b.loop_mode);
    EXPECT_EQ(a.choke_group, b.choke_group);
    EXPECT_EQ(a.output_bus, b.output_bus);
    EXPECT_EQ(a.flags, b.flags);
    EXPECT_FLOAT_EQ(a.cutoff_hz, b.cutoff_hz);
    EXPECT_FLOAT_EQ(a.attack_s, b.attack_s);
    EXPECT_FLOAT_EQ(a.decay_s, b.decay_s);
    EXPECT_FLOAT_EQ(a.sustain, b.sustain);
    EXPECT_FLOAT_EQ(a.release_s, b.release_s);
}

void ExpectDocEq(const InstrumentFile& a, const InstrumentFile& b) {
    EXPECT_STREQ(a.name, b.name);
    EXPECT_EQ(a.tags, b.tags);
    EXPECT_EQ(a.mode, b.mode);
    EXPECT_EQ(a.transpose, b.transpose);
    EXPECT_EQ(a.fine_tune, b.fine_tune);
    EXPECT_FLOAT_EQ(a.trim_gain, b.trim_gain);
    EXPECT_FLOAT_EQ(a.trim_pan, b.trim_pan);
    EXPECT_EQ(a.output, b.output);
    EXPECT_EQ(a.poly_mode, b.poly_mode);
    EXPECT_FLOAT_EQ(a.osc_mix, b.osc_mix);

    for (uint8_t o = 0; o < Wxi::kNumOscillators; ++o) {
        SCOPED_TRACE("osc " + std::to_string(o));
        EXPECT_EQ(a.osc[o].type, b.osc[o].type);
        EXPECT_FLOAT_EQ(a.osc[o].level, b.osc[o].level);
        EXPECT_FLOAT_EQ(a.osc[o].pan, b.osc[o].pan);
        EXPECT_EQ(a.osc[o].coarse_tune, b.osc[o].coarse_tune);
        EXPECT_EQ(a.osc[o].fine_tune, b.osc[o].fine_tune);
        EXPECT_EQ(a.osc[o].keytrack, b.osc[o].keytrack);
        ASSERT_EQ(a.osc[o].zone_count, b.osc[o].zone_count);
        for (uint8_t z = 0; z < a.osc[o].zone_count; ++z) {
            SCOPED_TRACE("zone " + std::to_string(z));
            ExpectZoneEq(a.osc[o].zones[z], b.osc[o].zones[z]);
        }
    }

    EXPECT_EQ(a.filter.type, b.filter.type);
    EXPECT_FLOAT_EQ(a.filter.cutoff_hz, b.filter.cutoff_hz);
    EXPECT_FLOAT_EQ(a.filter.resonance, b.filter.resonance);
    EXPECT_FLOAT_EQ(a.filter.keytrack, b.filter.keytrack);
    EXPECT_FLOAT_EQ(a.filter.env2_amount, b.filter.env2_amount);
    EXPECT_EQ(a.amp.velocity_curve, b.amp.velocity_curve);

    for (uint8_t i = 0; i < Wxi::kNumEnvelopes; ++i) {
        SCOPED_TRACE("env " + std::to_string(i));
        EXPECT_FLOAT_EQ(a.env[i].attack_s, b.env[i].attack_s);
        EXPECT_FLOAT_EQ(a.env[i].decay_s, b.env[i].decay_s);
        EXPECT_FLOAT_EQ(a.env[i].sustain, b.env[i].sustain);
        EXPECT_FLOAT_EQ(a.env[i].release_s, b.env[i].release_s);
    }
    for (uint8_t i = 0; i < Wxi::kNumVoiceLfos; ++i) {
        SCOPED_TRACE("lfo " + std::to_string(i));
        EXPECT_EQ(a.lfo[i].wave, b.lfo[i].wave);
        EXPECT_FLOAT_EQ(a.lfo[i].rate_hz, b.lfo[i].rate_hz);
        EXPECT_EQ(a.lfo[i].sync_div, b.lfo[i].sync_div);
        EXPECT_FLOAT_EQ(a.lfo[i].delay_s, b.lfo[i].delay_s);
        EXPECT_FLOAT_EQ(a.lfo[i].fade_s, b.lfo[i].fade_s);
        EXPECT_EQ(a.lfo[i].retrigger, b.lfo[i].retrigger);
    }
    for (uint8_t i = 0; i < Wxi::kMaxModSlots; ++i) {
        SCOPED_TRACE("mod " + std::to_string(i));
        EXPECT_EQ(a.mod_slots[i].source, b.mod_slots[i].source);
        EXPECT_EQ(a.mod_slots[i].dest, b.mod_slots[i].dest);
        EXPECT_EQ(a.mod_slots[i].depth, b.mod_slots[i].depth);
        EXPECT_EQ(a.mod_slots[i].curve, b.mod_slots[i].curve);
        EXPECT_EQ(a.mod_slots[i].flags, b.mod_slots[i].flags);
    }
}

// --- helpers for hand-built streams -----------------------------------------

void PutU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void PutFileHeader(std::vector<uint8_t>& v, uint16_t type, uint16_t version) {
    v.insert(v.end(), {'W', 'X', 'C', 'F'});
    PutU16(v, type);
    PutU16(v, version);
    PutU32(v, 0);
}
void PutChunk(std::vector<uint8_t>& v, uint16_t id, const std::vector<uint8_t>& payload) {
    PutU16(v, id);
    PutU16(v, Wxi::kChunkVersion);
    PutU32(v, static_cast<uint32_t>(payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
}
// A minimal valid HEAD payload: all-zero is in range for every field, so any
// test that only cares about "a HEAD is present" can use this.
std::vector<uint8_t> MinimalHead() {
    return std::vector<uint8_t>(Wxi::kHeadWireSize, 0);
}

Result ReadBytes(const std::vector<uint8_t>& bytes, InstrumentFile& out) {
    MemoryIo io;
    io.buf = bytes;
    return Wxi::Read(io.AsReader(), out);
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(WxiCodec, RoundTripsEveryField) {
    const InstrumentFile src = MakeFullDoc();
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    ExpectDocEq(src, dst);
}

TEST(WxiCodec, HeaderTotalLenMatchesBytesWritten) {
    const InstrumentFile src = MakeFullDoc();
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    Wxcf::Reader r(io.AsReader());
    uint16_t type = 0, version = 0;
    uint32_t total_len = 0;
    ASSERT_EQ(r.ReadHeader(type, version, total_len), Wxcf::Result::Ok);
    EXPECT_EQ(type, Wxi::kFileType);
    EXPECT_EQ(version, Wxi::kFileVersion);
    EXPECT_EQ(total_len, io.buf.size());
}

TEST(WxiCodec, RoundTripsAnEmptyInstrument) {
    const InstrumentFile src;  // all defaults, both oscillators Off with no zones
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    ExpectDocEq(src, dst);
    EXPECT_EQ(dst.osc[0].zone_count, 0);
    EXPECT_EQ(dst.osc[1].zone_count, 0);
}

TEST(WxiCodec, RoundTripsAFullZoneArray) {
    InstrumentFile src;
    src.osc[0].type = OscType::Sample;
    src.osc[0].zone_count = Wxi::kMaxZonesPerOsc;
    for (uint8_t z = 0; z < Wxi::kMaxZonesPerOsc; ++z) {
        src.osc[0].zones[z].index = z;
        src.osc[0].zones[z].root_note = static_cast<uint8_t>(z);
    }
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    ExpectDocEq(src, dst);
}

// The reason `index` is on the wire at all: an editor that addresses "pad 5"
// must still find it at 5 after a save that wrote only three zones.
TEST(WxiCodec, PreservesSparseZoneIndices) {
    InstrumentFile src;
    src.osc[0].type = OscType::Sample;
    src.osc[0].zone_count = 2;
    src.osc[0].zones[0].index = 5;
    src.osc[0].zones[1].index = 31;
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    EXPECT_EQ(dst.osc[0].zones[0].index, 5);
    EXPECT_EQ(dst.osc[0].zones[1].index, 31);
}

TEST(WxiCodec, TruncatesAnOverlongNameAndStillTerminates) {
    InstrumentFile src;
    // 40 characters into a 24-byte field.
    std::memcpy(src.name, "0123456789012345678901234567890123456789", sizeof(src.name));
    src.name[sizeof(src.name) - 1] = '\0';
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    EXPECT_EQ(std::strlen(dst.name), Wxi::kNameBytes - 1);
    EXPECT_STREQ(dst.name, "01234567890123456789012");
}

TEST(WxiCodec, WriteClampsAnOverlargeZoneCount) {
    InstrumentFile src;
    src.osc[0].type = OscType::Sample;
    src.osc[0].zone_count = 200;  // past kMaxZonesPerOsc; would read past the array
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    EXPECT_EQ(dst.osc[0].zone_count, Wxi::kMaxZonesPerOsc);
}

// --- container-level rejection ----------------------------------------------

TEST(WxiCodec, RejectsNonWxcfBytes) {
    std::vector<uint8_t> bytes(64, 0x5A);
    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadMagic);
}

TEST(WxiCodec, RejectsAnotherFileType) {
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, 7 /* not an Instrument */, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadFileType);
}

TEST(WxiCodec, RejectsAFutureMajorVersion) {
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxcf::MakeVersion(2, 0));
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadVersion);
}

// A minor bump is additive-only, so it must still load.
TEST(WxiCodec, AcceptsAFutureMinorVersion) {
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxcf::MakeVersion(1, 9));
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::Ok);
}

TEST(WxiCodec, RejectsAFileWithNoHeadChunk) {
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkAmp, std::vector<uint8_t>(Wxi::kAmpWireSize, 1));
    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadChunk);
}

TEST(WxiCodec, RejectsATruncatedKnownChunk) {
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, std::vector<uint8_t>(Wxi::kHeadWireSize - 1, 0));
    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadChunk);
}

// --- forward compatibility ---------------------------------------------------

TEST(WxiCodec, SkipsAnUnknownChunk) {
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, 0x7FFF /* from a much later firmware */, std::vector<uint8_t>(300, 0xAB));
    std::vector<uint8_t> head = MinimalHead();
    head[24] = 0x11;  // tags, so we can prove the HEAD after it was still found
    PutChunk(bytes, Wxi::kChunkHead, head);
    PutChunk(bytes, Wxi::kChunkFxch, std::vector<uint8_t>{});  // reserved, empty

    InstrumentFile dst;
    ASSERT_EQ(ReadBytes(bytes, dst), Result::Ok);
    EXPECT_EQ(dst.tags, 0x11);
}

TEST(WxiCodec, IgnoresTrailingFieldsInAKnownChunk) {
    std::vector<uint8_t> head = MinimalHead();
    head[24] = 0x33;                    // tags
    head.insert(head.end(), 16, 0xEE);  // a later version's extra fields
    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, head);

    InstrumentFile dst;
    ASSERT_EQ(ReadBytes(bytes, dst), Result::Ok);
    EXPECT_EQ(dst.tags, 0x33);
}

// The stride fields exist so a wider future Zone (or mod row) is walked, not
// misaligned. Build an OSC chunk with a longer header and a wider zone and
// check the fields we do understand survive.
TEST(WxiCodec, WalksAWiderZoneStride) {
    const uint32_t future_header = Wxi::kOscHeaderWireSize + 6;
    const uint32_t future_stride = Wxi::kZoneWireSize + 24;

    std::vector<uint8_t> osc;
    PutU16(osc, static_cast<uint16_t>(future_header));
    PutU16(osc, static_cast<uint16_t>(future_stride));
    osc.push_back(2);  // zone_count
    osc.push_back(static_cast<uint8_t>(OscType::Sample));
    PutU32(osc, 0x3F800000u);                                      // level = 1.0f
    PutU32(osc, 0x3F000000u);                                      // pan = 0.5f
    osc.push_back(static_cast<uint8_t>(static_cast<int8_t>(-2)));  // coarse
    osc.push_back(3);                                              // fine
    osc.push_back(1);                                              // keytrack
    osc.insert(osc.end(), future_header - Wxi::kOscHeaderWireSize, 0xCD);

    for (uint8_t z = 0; z < 2; ++z) {
        std::vector<uint8_t> zone(Wxi::kZoneWireSize, 0);
        zone[0] = static_cast<uint8_t>(z);  // index
        const char* p = "0:/wavex/samples/a.wav";
        std::memcpy(zone.data() + 1, p, std::strlen(p));
        // Derived, not a literal: root_note sits after index, the path and
        // the four key/velocity bounds, so it moves whenever kPathBytes does.
        constexpr size_t kRootNoteOffset = 1 + Wxi::kPathBytes + 4;
        zone[kRootNoteOffset] = static_cast<uint8_t>(48 + z);
        zone.insert(zone.end(), future_stride - Wxi::kZoneWireSize, 0xCD);
        osc.insert(osc.end(), zone.begin(), zone.end());
    }

    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    PutChunk(bytes, Wxi::kChunkOsc1, osc);

    InstrumentFile dst;
    ASSERT_EQ(ReadBytes(bytes, dst), Result::Ok);
    EXPECT_EQ(dst.osc[0].type, OscType::Sample);
    EXPECT_EQ(dst.osc[0].coarse_tune, -2);
    ASSERT_EQ(dst.osc[0].zone_count, 2);
    EXPECT_STREQ(dst.osc[0].zones[0].path, "0:/wavex/samples/a.wav");
    EXPECT_EQ(dst.osc[0].zones[0].root_note, 48);
    EXPECT_EQ(dst.osc[0].zones[1].root_note, 49);
}

TEST(WxiCodec, LoadsAnUnrenderableOscillatorTypeAsOff) {
    InstrumentFile src;
    src.osc[0].type = OscType::Sample;
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);
    // Patch the OSC1 chunk's type byte to a value no build defines. Layout:
    // 12 B file header + 8 B chunk header + kHeadWireSize + 8 B chunk header,
    // then the oscillator payload, whose type sits at offset 5.
    const size_t osc1_payload = 12 + 8 + Wxi::kHeadWireSize + 8;
    io.buf[osc1_payload + 5] = 9;

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    EXPECT_EQ(dst.osc[0].type, OscType::Off);
}

// --- malformed bodies --------------------------------------------------------

TEST(WxiCodec, RejectsAnOscChunkWhoseZoneCountDoesNotMatchItsLength) {
    std::vector<uint8_t> osc;
    PutU16(osc, static_cast<uint16_t>(Wxi::kOscHeaderWireSize));
    PutU16(osc, static_cast<uint16_t>(Wxi::kZoneWireSize));
    osc.push_back(3);  // claims 3 zones ...
    osc.resize(Wxi::kOscHeaderWireSize, 0);
    osc.insert(osc.end(), Wxi::kZoneWireSize, 0);  // ... but carries 1

    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    PutChunk(bytes, Wxi::kChunkOsc1, osc);

    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadChunk);
}

TEST(WxiCodec, RejectsMoreZonesThanThisBuildCanHold) {
    const uint32_t count = Wxi::kMaxZonesPerOsc + 1;
    std::vector<uint8_t> osc;
    PutU16(osc, static_cast<uint16_t>(Wxi::kOscHeaderWireSize));
    PutU16(osc, static_cast<uint16_t>(Wxi::kZoneWireSize));
    osc.push_back(static_cast<uint8_t>(count));
    osc.resize(Wxi::kOscHeaderWireSize, 0);
    osc.insert(osc.end(), count * Wxi::kZoneWireSize, 0);

    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    PutChunk(bytes, Wxi::kChunkOsc1, osc);

    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::TooManyZones);
}

// A zone index this build cannot address is rejected rather than clamped -
// clamping would silently collapse two zones onto one engine slot.
TEST(WxiCodec, RejectsAnOutOfRangeZoneIndex) {
    std::vector<uint8_t> osc;
    PutU16(osc, static_cast<uint16_t>(Wxi::kOscHeaderWireSize));
    PutU16(osc, static_cast<uint16_t>(Wxi::kZoneWireSize));
    osc.push_back(1);
    osc.resize(Wxi::kOscHeaderWireSize, 0);
    std::vector<uint8_t> zone(Wxi::kZoneWireSize, 0);
    zone[0] = Wxi::kMaxZonesPerOsc;  // one past the last addressable slot
    osc.insert(osc.end(), zone.begin(), zone.end());

    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    PutChunk(bytes, Wxi::kChunkOsc1, osc);

    InstrumentFile dst;
    EXPECT_EQ(ReadBytes(bytes, dst), Result::BadChunk);
}

TEST(WxiCodec, DropsModRowsPastThisBuildsMatrix) {
    const uint32_t count = Wxi::kMaxModSlots + 2;
    std::vector<uint8_t> modm;
    modm.push_back(static_cast<uint8_t>(count));
    modm.push_back(static_cast<uint8_t>(Wxi::kModSlotWireSize));
    for (uint32_t i = 0; i < count; ++i) {
        modm.push_back(static_cast<uint8_t>(i + 1));  // source
        modm.push_back(1);                            // dest
        PutU16(modm, 1000);                           // depth
        modm.push_back(0);                            // curve
        modm.push_back(0);                            // flags
    }

    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, MinimalHead());
    PutChunk(bytes, Wxi::kChunkModm, modm);

    InstrumentFile dst;
    ASSERT_EQ(ReadBytes(bytes, dst), Result::Ok);
    for (uint8_t i = 0; i < Wxi::kMaxModSlots; ++i)
        EXPECT_EQ(dst.mod_slots[i].source, i + 1);
}

// --- float sanitisation ------------------------------------------------------

TEST(WxiCodec, ReplacesNonFiniteAndOutOfRangeFloatsWithDefaults) {
    // trim_gain (offset 28) = NaN, trim_pan (offset 32) = 40.0f (out of 0..1).
    std::vector<uint8_t> head = MinimalHead();
    const uint32_t nan_bits = 0x7FC00000u;
    const uint32_t forty_bits = 0x42200000u;
    for (int i = 0; i < 4; ++i) {
        head[28 + static_cast<size_t>(i)] = static_cast<uint8_t>((nan_bits >> (8 * i)) & 0xFF);
        head[32 + static_cast<size_t>(i)] = static_cast<uint8_t>((forty_bits >> (8 * i)) & 0xFF);
    }

    std::vector<uint8_t> bytes;
    PutFileHeader(bytes, Wxi::kFileType, Wxi::kFileVersion);
    PutChunk(bytes, Wxi::kChunkHead, head);

    InstrumentFile dst;
    ASSERT_EQ(ReadBytes(bytes, dst), Result::Ok);
    EXPECT_FLOAT_EQ(dst.trim_gain, 1.0f);  // the field's own default
    EXPECT_FLOAT_EQ(dst.trim_pan, 0.5f);
}

TEST(WxiCodec, ReplacesAnInfiniteZoneGainWithUnity) {
    InstrumentFile src;
    src.osc[0].type = OscType::Sample;
    src.osc[0].zone_count = 1;
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);
    // Zone gain sits at offset 104 of the first zone record.
    const size_t zone0 = 12 + 8 + Wxi::kHeadWireSize + 8 + Wxi::kOscHeaderWireSize;
    const uint32_t inf_bits = 0x7F800000u;
    for (int i = 0; i < 4; ++i)
        io.buf[zone0 + 104 + static_cast<size_t>(i)] =
            static_cast<uint8_t>((inf_bits >> (8 * i)) & 0xFF);

    InstrumentFile dst;
    ASSERT_EQ(Wxi::Read(io.AsReader(), dst), Result::Ok);
    EXPECT_FLOAT_EQ(dst.osc[0].zones[0].gain, 1.0f);
}

// --- I/O failure -------------------------------------------------------------

TEST(WxiCodec, ReportsAWriteFailure) {
    struct Failing {
        int allowed = 0;
        static bool Write(void* self, const void*, size_t) {
            auto* f = static_cast<Failing*>(self);
            return f->allowed-- > 0;
        }
    } failing;
    failing.allowed = 2;  // the file header and one chunk header, then fail
    Wxcf::IoContext io;
    io.user_data = &failing;
    io.write = &Failing::Write;

    EXPECT_EQ(Wxi::Write(io, MakeFullDoc()), Result::IoError);
}

TEST(WxiCodec, ReportsATruncatedStreamMidChunk) {
    const InstrumentFile src = MakeFullDoc();
    MemoryIo io;
    ASSERT_EQ(Wxi::Write(io.AsWriter(), src), Result::Ok);
    // Cut the file inside the first oscillator's zone records.
    io.buf.resize(12 + 8 + Wxi::kHeadWireSize + 8 + Wxi::kOscHeaderWireSize + 40);

    InstrumentFile dst;
    EXPECT_EQ(Wxi::Read(io.AsReader(), dst), Result::IoError);
}
