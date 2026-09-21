#include "wxcf/sample_file.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {
struct Memory {
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    WaveX::Wxcf::IoContext Io() {
        return {this,
                [](void* p, void* out, size_t n) {
                    auto& m = *static_cast<Memory*>(p);
                    if (n > m.bytes.size() - m.pos)
                        return false;
                    std::memcpy(out, m.bytes.data() + m.pos, n);
                    m.pos += n;
                    return true;
                },
                [](void* p, const void* in, size_t n) {
                    auto& m = *static_cast<Memory*>(p);
                    const auto* b = static_cast<const uint8_t*>(in);
                    m.bytes.insert(m.bytes.end(), b, b + n);
                    return true;
                },
                [](void* p) {
                    const auto& m = *static_cast<Memory*>(p);
                    return m.pos == m.bytes.size();
                }};
    }
};
WaveX::SampleFile::Document Example() {
    WaveX::SampleFile::Document d;
    d.file_bytes = 192044;
    d.data_offset = 44;
    auto& m = d.sample;
    m.sample_rate = 48000;
    m.total_frames = 48000;
    m.channels = 2;
    m.bits_per_sample = 16;
    m.start_frame = 123;
    m.end_frame = 47000;
    m.loop_start = 456;
    m.loop_end = 45000;
    m.loop_enabled = 1;
    m.gain_db_x10 = -123;
    m.fade_in_ms = 17;
    m.fade_out_ms = 29;
    m.loop_crossfade_ms = 13;
    m.channel_mode = WaveX::Protocol::SAMPLE_CH_RIGHT;
    return d;
}
TEST(SampleFile, RoundTripEditsWithoutRuntimeIdentity) {
    auto d = Example();
    d.sample.sample_id = 1234;
    d.sample.generation = 9;
    d.sample.used_by = 5;
    Memory file;
    ASSERT_TRUE(WaveX::SampleFile::Write(file.Io(), d));
    ASSERT_EQ(file.bytes.size(), WaveX::SampleFile::kFileBytes);
    WaveX::SampleFile::Document out;
    ASSERT_TRUE(WaveX::SampleFile::Read(file.Io(), static_cast<uint32_t>(file.bytes.size()), out));
    EXPECT_TRUE(WaveX::SampleFile::Matches(out, d));
    auto live = d.sample;
    live.start_frame = 0;
    WaveX::SampleFile::Apply(out, live);
    EXPECT_EQ(live.sample_id, 1234);
    EXPECT_EQ(live.generation, 9);
    EXPECT_EQ(live.used_by, 5);
    EXPECT_EQ(live.start_frame, 123u);
    EXPECT_EQ(live.end_frame, 47000u);
    EXPECT_EQ(live.loop_start, 456u);
    EXPECT_EQ(live.loop_end, 45000u);
    EXPECT_EQ(live.loop_enabled, 1);
    EXPECT_EQ(live.gain_db_x10, -123);
    EXPECT_EQ(live.fade_in_ms, 17);
    EXPECT_EQ(live.fade_out_ms, 29);
    EXPECT_EQ(live.loop_crossfade_ms, 13);
    EXPECT_EQ(live.channel_mode, WaveX::Protocol::SAMPLE_CH_RIGHT);
    EXPECT_EQ(out.sample.sample_id, 0);
    EXPECT_EQ(out.sample.used_by, 0);
    ++d.sample.total_frames;
    EXPECT_FALSE(WaveX::SampleFile::Matches(out, d));
}
TEST(SampleFile, MalformedOrTruncatedFileNeverPublishesPartialEdits) {
    Memory original;
    ASSERT_TRUE(WaveX::SampleFile::Write(original.Io(), Example()));
    for (size_t n = 0; n < original.bytes.size(); ++n) {
        Memory file;
        file.bytes.assign(original.bytes.begin(), original.bytes.begin() + n);
        auto out = Example();
        out.sample.start_frame = 77;
        EXPECT_FALSE(
            WaveX::SampleFile::Read(file.Io(), static_cast<uint32_t>(file.bytes.size()), out));
        EXPECT_EQ(out.sample.start_frame, 77u);
    }
    for (size_t offset: {size_t(0), size_t(4), size_t(7), size_t(40 + 20), size_t(43 + 20)}) {
        Memory file = original;
        file.bytes[offset] = 255;
        WaveX::SampleFile::Document out;
        EXPECT_FALSE(
            WaveX::SampleFile::Read(file.Io(), static_cast<uint32_t>(file.bytes.size()), out));
    }
}
TEST(SampleFile, InvalidGeometryAndMarkersCannotBeWritten) {
    for (unsigned bad = 0; bad < 5; ++bad) {
        auto d = Example();
        if (bad == 0)
            d.sample.start_frame = d.sample.end_frame;
        if (bad == 1)
            d.sample.loop_start = d.sample.loop_end;
        if (bad == 2)
            d.sample.gain_db_x10 = 121;
        if (bad == 3)
            d.file_bytes = d.data_offset;
        if (bad == 4)
            d.sample.channel_mode = 99;
        Memory file;
        EXPECT_FALSE(WaveX::SampleFile::Write(file.Io(), d));
        EXPECT_TRUE(file.bytes.empty());
    }
}
}  // namespace

TEST(SampleFileCompatibility, Version10DefaultsCrossfadeOff) {
    auto doc = Example();
    doc.sample.loop_crossfade_ms = 0;
    Memory file;
    ASSERT_TRUE(WaveX::SampleFile::Write(file.Io(), doc));
    // WXCF header version and sole chunk version, little endian.
    WaveX::Wxcf::detail::WriteU16LE(file.bytes.data() + 6, 0x0100);
    WaveX::Wxcf::detail::WriteU16LE(file.bytes.data() + 14, 0x0100);
    WaveX::SampleFile::Document out;
    ASSERT_TRUE(WaveX::SampleFile::Read(file.Io(), uint32_t(file.bytes.size()), out));
    EXPECT_EQ(out.sample.loop_crossfade_ms, 0);
    file.pos = 0;
    file.bytes[62] = 1;
    EXPECT_FALSE(WaveX::SampleFile::Read(file.Io(), uint32_t(file.bytes.size()), out));
}
