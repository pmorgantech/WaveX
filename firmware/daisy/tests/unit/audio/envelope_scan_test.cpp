#include "audio/envelope_scan.hpp"

#include <gtest/gtest.h>

#include "uart_protocol/uart_protocol.h"

#include <cstring>
#include <vector>

using WaveX::AudioEngine::EnvelopeScan;
using WaveX::Protocol::EnvelopeChunkMessage;
using WaveX::Protocol::EnvelopeColumn8;

namespace {
EnvelopeChunkMessage Identity(uint32_t frames, uint16_t columns, uint8_t channels = 2) {
    EnvelopeChunkMessage h;
    h.sample_id = 513;
    h.generation = 17;
    h.end_frame = frames;
    h.total_columns = columns;
    h.channels = channels;
    return h;
}
}  // namespace

TEST(EnvelopeScanTest, HugeColumnYieldsWithinReadBudgetAndPreservesExtrema) {
    EnvelopeScan scan;
    constexpr uint32_t frames = 100000;
    ASSERT_TRUE(scan.Begin(Identity(frames, 1)));
    uint32_t reads = 0;
    uint32_t sent = 0;
    auto read = [&](uint32_t frame, uint8_t ch) -> int16_t {
        ++reads;
        return frame == 90000 ? (ch ? INT16_MIN : INT16_MAX) : 0;
    };
    auto send = [&](const EnvelopeScan::Packet& packet, uint16_t) {
        ++sent;
        EXPECT_EQ(packet.data[0].min_sample, 0);
        EXPECT_EQ(packet.data[0].max_sample, INT8_MAX);
        EXPECT_EQ(packet.data[1].min_sample, INT8_MIN);
        EXPECT_EQ(packet.data[1].max_sample, 0);
        return true;
    };
    EXPECT_EQ(scan.Pump(2, read, send), EnvelopeScan::kSampleReadsPerPass);
    EXPECT_EQ(sent, 0u);
    for (unsigned pass = 0; scan.Active() && pass < 100; ++pass) {
        EXPECT_LE(scan.Pump(2, read, send), EnvelopeScan::kSampleReadsPerPass);
    }
    EXPECT_FALSE(scan.Active());
    EXPECT_EQ(sent, 1u);
    EXPECT_EQ(reads, frames * 2);
}

TEST(EnvelopeScanTest, BackpressureRetriesIdenticalPacketWithoutReadingPcmAgain) {
    EnvelopeScan scan;
    ASSERT_TRUE(scan.Begin(Identity(1024, 128)));
    uint32_t reads = 0;
    auto read = [&](uint32_t f, uint8_t ch) {
        ++reads;
        return static_cast<int16_t>(ch ? -static_cast<int32_t>(f) : f);
    };
    std::vector<uint8_t> retained;
    auto refuse = [&](const EnvelopeScan::Packet& p, uint16_t bytes) {
        auto* begin = reinterpret_cast<const uint8_t*>(&p);
        if (retained.empty()) {
            retained.assign(begin, begin + bytes);
        } else {
            EXPECT_EQ(std::vector<uint8_t>(begin, begin + bytes), retained);
        }
        return false;
    };
    scan.Pump(2, read, refuse);
    const uint32_t first_reads = reads;
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(scan.Pump(2, read, refuse), 0u);
    }
    EXPECT_EQ(reads, first_reads);
    uint16_t expected_first = 0;
    auto accept = [&](const EnvelopeScan::Packet& p, uint16_t bytes) {
        EXPECT_EQ(p.header.first_column, expected_first);
        EXPECT_LE(bytes, sizeof(EnvelopeScan::Packet));
        expected_first = static_cast<uint16_t>(expected_first + p.header.columns);
        return true;
    };
    for (unsigned pass = 0; scan.Active() && pass < 100; ++pass) {
        scan.Pump(2, read, accept);
    }
    EXPECT_FALSE(scan.Active());
    EXPECT_EQ(expected_first, 128);
    EXPECT_EQ(reads, 2048u);
}

TEST(EnvelopeScanTest, ChunkedStereoMatchesReferenceAndRoundTripsTheLiveFraming) {
    EnvelopeScan scan;
    auto identity = Identity(17003, 203);
    identity.start_frame = 79;
    ASSERT_TRUE(scan.Begin(identity));
    auto read = [](uint32_t f, uint8_t ch) {
        return static_cast<int16_t>((f * 157u + ch * 19001u) % 65536u - 32768);
    };
    uint16_t first = 0;
    uint16_t sequence = 1;
    auto send = [&](const EnvelopeScan::Packet& p, uint16_t bytes) {
        EXPECT_EQ(p.header.first_column, first);
        EXPECT_EQ(p.header.sample_id, identity.sample_id);
        EXPECT_EQ(p.header.generation, identity.generation);
        EXPECT_EQ(p.header.start_frame, identity.start_frame);
        EXPECT_EQ(p.header.end_frame, identity.end_frame);
        EXPECT_EQ(p.header.total_columns, identity.total_columns);
        EXPECT_EQ(p.header.channels, 2);
        EXPECT_EQ(p.header.encoding, WaveX::Protocol::ENVELOPE_ENCODING_S8);
        for (uint16_t c = 0; c < p.header.columns; ++c) {
            const uint64_t span = identity.end_frame - identity.start_frame;
            const uint32_t f0 = identity.start_frame +
                                static_cast<uint32_t>(span * (first + c) / identity.total_columns);
            const uint32_t f1 =
                identity.start_frame +
                static_cast<uint32_t>(span * (first + c + 1) / identity.total_columns);
            for (uint8_t ch = 0; ch < 2; ++ch) {
                int16_t lo = INT16_MAX, hi = INT16_MIN;
                for (uint32_t f = f0; f < f1; ++f) {
                    lo = std::min(lo, read(f, ch));
                    hi = std::max(hi, read(f, ch));
                }
                const auto decoded_column = p.data[c * 2 + ch].Expand();
                EXPECT_LE(decoded_column.min_sample, lo);
                EXPECT_GE(decoded_column.max_sample, hi);
                EXPECT_LE(lo - decoded_column.min_sample, 258);
                EXPECT_LE(decoded_column.max_sample - hi, 258);
            }
        }
        uint8_t wire[sizeof(EnvelopeScan::Packet) + WaveX::UartProtocol::UART_FRAME_OVERHEAD];
        uint8_t decoded[sizeof(EnvelopeScan::Packet)];
        const size_t size = WaveX::UartProtocol::CreateUartPacket(
            wire, sizeof(wire), WaveX::Protocol::MSG_ENVELOPE_CHUNK, &p, bytes, sequence, 0);
        size_t capacity = sizeof(decoded);
        uint8_t type = 0, flags = 0;
        uint16_t seq = 0;
        EXPECT_TRUE(
            WaveX::UartProtocol::ParseUartPacket(wire, size, type, decoded, capacity, seq, flags));
        EXPECT_EQ(type, WaveX::Protocol::MSG_ENVELOPE_CHUNK);
        EXPECT_EQ(seq, sequence++);
        EXPECT_EQ(capacity, bytes);
        EXPECT_EQ(std::memcmp(decoded, &p, bytes), 0);
        first = static_cast<uint16_t>(first + p.header.columns);
        return true;
    };
    for (unsigned pass = 0; scan.Active() && pass < 100; ++pass) {
        scan.Pump(2, read, send);
    }
    EXPECT_EQ(first, identity.total_columns);
    EXPECT_FALSE(scan.Active());
}

TEST(EnvelopeScanTest, EofClipsOnlyLastBinWithoutShiftingTheRequestedGrid) {
    EnvelopeScan scan;
    ASSERT_TRUE(scan.Begin(Identity(10, 3, 1), 12));  // tier = 4 frames
    std::vector<EnvelopeColumn8> result;
    auto read = [](uint32_t f, uint8_t) {
        return EnvelopeColumn8::DecodeSample(static_cast<int8_t>(f));
    };
    scan.Pump(1, read, [&](const EnvelopeScan::Packet& p, uint16_t) {
        result.assign(p.data.begin(), p.data.begin() + p.header.columns);
        return true;
    });
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].min_sample, 0);
    EXPECT_EQ(result[0].max_sample, 3);
    EXPECT_EQ(result[1].min_sample, 4);
    EXPECT_EQ(result[1].max_sample, 7);
    EXPECT_EQ(result[2].min_sample, 8);
    EXPECT_EQ(result[2].max_sample, 9);
}

TEST(EnvelopeScanTest, CancelAndReplacementDiscardRetainedDataAndIdentity) {
    EnvelopeScan scan;
    ASSERT_TRUE(scan.Begin(Identity(100, 100)));
    auto read = [](uint32_t, uint8_t) -> int16_t { return 42; };
    auto refuse = [](const EnvelopeScan::Packet&, uint16_t) { return false; };
    scan.Pump(2, read, refuse);
    scan.Cancel();
    EXPECT_EQ(scan.Pump(2, read, refuse), 0u);
    auto next = Identity(7, 7, 1);
    next.sample_id = 900;
    next.generation = 18;
    ASSERT_TRUE(scan.Begin(next));
    unsigned sends = 0;
    scan.Pump(1, read, [&](const EnvelopeScan::Packet& p, uint16_t) {
        ++sends;
        EXPECT_EQ(p.header.sample_id, 900);
        EXPECT_EQ(p.header.generation, 18);
        EXPECT_EQ(p.header.first_column, 0);
        EXPECT_EQ(p.header.columns, 7);
        EXPECT_EQ(p.header.channels, 1);
        return true;
    });
    EXPECT_EQ(sends, 1u);
    EXPECT_FALSE(scan.Active());
}

TEST(EnvelopeScanTest, SummedStereoChargesBothReadsAndInvalidShapesAreRejected) {
    EnvelopeScan scan;
    ASSERT_TRUE(scan.Begin(Identity(100000, 1, 1)));
    unsigned frames = 0;
    scan.Pump(
        2,
        [&](uint32_t, uint8_t) -> int16_t {
            ++frames;
            return 0;
        },
        [](const EnvelopeScan::Packet&, uint16_t) { return false; });
    EXPECT_EQ(frames, EnvelopeScan::kSampleReadsPerPass / 2);
    EXPECT_FALSE(scan.Begin(Identity(0, 1)));
    EXPECT_FALSE(scan.Begin(Identity(10, 0)));
    EXPECT_FALSE(scan.Begin(Identity(10, 11)));
    EXPECT_FALSE(scan.Begin(Identity(10, 1, 0)));
    EXPECT_FALSE(scan.Begin(Identity(10, 1, 3)));
    EXPECT_FALSE(scan.Active());
}

TEST(EnvelopeScanTest, LongStereoScanPacksFullChunksAcrossPumpBudgets) {
    EnvelopeScan scan;
    ASSERT_TRUE(scan.Begin(Identity(8000000, 1280)));
    unsigned packets = 0;
    size_t wire_bytes = 0;
    auto read = [](uint32_t, uint8_t) -> int16_t { return 0; };
    for (unsigned pass = 0; scan.Active() && pass < 10000; ++pass) {
        EXPECT_LE(scan.Pump(2,
                            read,
                            [&](const EnvelopeScan::Packet&, uint16_t bytes) {
                                ++packets;
                                wire_bytes += bytes + WaveX::UartProtocol::UART_FRAME_OVERHEAD;
                                return true;
                            }),
                  EnvelopeScan::kSampleReadsPerPass);
    }
    EXPECT_FALSE(scan.Active());
    EXPECT_EQ(packets, 20u);
    EXPECT_EQ(wire_bytes, 5720u);
}

TEST(EnvelopeScanTest, Stereo1140ColumnsHave4560DataBytesAndBoundedFrames) {
    EnvelopeScan scan;
    ASSERT_TRUE(scan.Begin(Identity(11400, 1140)));
    uint32_t packets = 0;
    size_t bytes_total = 0;
    for (unsigned pass = 0; scan.Active() && pass < 100; ++pass) {
        scan.Pump(
            2,
            [](uint32_t, uint8_t) -> int16_t { return 0; },
            [&](const EnvelopeScan::Packet& p, uint16_t bytes) {
                ++packets;
                bytes_total += bytes + WaveX::UartProtocol::UART_FRAME_OVERHEAD;
                EXPECT_LE(bytes, 276u);
                EXPECT_LE(p.header.columns, 64u);
                return true;
            });
    }
    EXPECT_FALSE(scan.Active());
    EXPECT_EQ(packets, 18u);
    EXPECT_EQ(bytes_total, 4560u + 18u * 30u);
}
