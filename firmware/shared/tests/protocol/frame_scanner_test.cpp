// Host tests for the shared UART RX frame scanner (frame_scanner.hpp).
// This is the policy both links previously hand-rolled and let diverge
// (review H2 was exactly such a divergence); every behavior here is one
// the links depend on.

#include "frame_scanner.hpp"

#include <gtest/gtest.h>

#include "uart_protocol.h"

#include <cstring>
#include <vector>

using namespace WaveX::UartProtocol;

namespace {

struct Captured {
    uint8_t msg_type;
    uint16_t seq;
    std::vector<uint8_t> payload;
};

class FrameScannerTest : public ::testing::Test {
   protected:
    static constexpr size_t kCap = 4096;
    uint8_t storage_[kCap];
    FrameScanner scanner_{storage_, kCap};
    ScanStats stats_{};
    std::vector<Captured> frames_;

    void ScanAll() {
        scanner_.Scan(
            [&](const uint8_t* frame, size_t frame_len) {
                Captured c{};
                uint8_t flags = 0;
                uint8_t payload[UART_MAX_PAYLOAD];
                size_t payload_len = sizeof(payload);
                ASSERT_TRUE(ParseUartPacket(
                    frame, frame_len, c.msg_type, payload, payload_len, c.seq, flags));
                c.payload.assign(payload, payload + payload_len);
                frames_.push_back(std::move(c));
            },
            stats_);
    }

    std::vector<uint8_t> MakeFrame(uint8_t msg_type,
                                   uint16_t seq,
                                   const std::vector<uint8_t>& payload = {}) {
        std::vector<uint8_t> frame(UART_FRAME_OVERHEAD + payload.size());
        size_t n = CreateUartPacket(frame.data(),
                                    frame.size(),
                                    msg_type,
                                    payload.empty() ? nullptr : payload.data(),
                                    payload.size(),
                                    seq,
                                    0);
        EXPECT_GT(n, 0u);
        frame.resize(n);
        return frame;
    }

    void Append(const std::vector<uint8_t>& bytes) {
        scanner_.Append(bytes.data(), bytes.size(), stats_);
    }
};

TEST_F(FrameScannerTest, WholeFrameDispatches) {
    Append(MakeFrame(0x12, 7, {1, 2, 3}));
    ScanAll();
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].msg_type, 0x12);
    EXPECT_EQ(frames_[0].seq, 7);
    EXPECT_EQ(frames_[0].payload, (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(scanner_.Buffered(), 0u);
    EXPECT_EQ(stats_.frames, 1u);
}

TEST_F(FrameScannerTest, FrameFragmentedAcrossAppends) {
    auto frame = MakeFrame(0x34, 9, std::vector<uint8_t>(100, 0xAB));
    // Feed one byte at a time; the frame must survive buffering and emerge
    // exactly once when complete.
    for (size_t i = 0; i < frame.size(); ++i) {
        Append({frame[i]});
        ScanAll();
    }
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].payload.size(), 100u);
    EXPECT_EQ(stats_.crc_errors, 0u);
}

TEST_F(FrameScannerTest, BackToBackFramesBothDispatch) {
    auto a = MakeFrame(0x01, 1, {0xAA});
    auto b = MakeFrame(0x02, 2, {0xBB});
    a.insert(a.end(), b.begin(), b.end());
    Append(a);
    ScanAll();
    ASSERT_EQ(frames_.size(), 2u);
    EXPECT_EQ(frames_[0].msg_type, 0x01);
    EXPECT_EQ(frames_[1].msg_type, 0x02);
}

TEST_F(FrameScannerTest, GarbagePrefixIsSkipped) {
    std::vector<uint8_t> bytes = {0x00, 0xFF, 0x13, 0x37};
    auto frame = MakeFrame(0x12, 3, {9});
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    Append(bytes);
    ScanAll();
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].seq, 3);
}

// Review H2 regression: a buffer full of start-byte-free garbage must
// drain (keeping at most UART_FRAME_OVERHEAD-1 tail bytes), not wedge.
TEST_F(FrameScannerTest, StartByteFreeGarbageDrains) {
    std::vector<uint8_t> garbage(kCap, 0x00);  // no 0xA5 anywhere
    Append(garbage);
    ScanAll();
    EXPECT_LE(scanner_.Buffered(), UART_FRAME_OVERHEAD - 1);

    // And the link still works afterwards.
    Append(MakeFrame(0x12, 4, {1}));
    ScanAll();
    ASSERT_EQ(frames_.size(), 1u);
}

TEST_F(FrameScannerTest, CorruptedCrcResyncsToNextFrame) {
    auto bad = MakeFrame(0x12, 5, {1, 2, 3, 4});
    bad[6] ^= 0xFF;  // corrupt a payload byte -> CRC mismatch
    auto good = MakeFrame(0x12, 6, {5});
    bad.insert(bad.end(), good.begin(), good.end());
    Append(bad);
    ScanAll();
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].seq, 6);
    EXPECT_GE(stats_.crc_errors, 1u);
}

// A start byte followed by an impossible length field used to be treated
// as "incomplete" and stalled the stream until overflow; it must resync.
TEST_F(FrameScannerTest, BogusLengthFieldResyncsInsteadOfStalling) {
    std::vector<uint8_t> bogus = {UART_START_BYTE, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto good = MakeFrame(0x21, 8, {7, 7});
    bogus.insert(bogus.end(), good.begin(), good.end());
    Append(bogus);
    ScanAll();
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].msg_type, 0x21);
    EXPECT_GE(stats_.sync_errors, 1u);
}

TEST_F(FrameScannerTest, OverflowDropsOldestAndRecovers) {
    // Fill with garbage that keeps a partial pseudo-frame pinned (start byte
    // + huge-but-valid length so the scanner waits for more), then keep
    // appending: oldest bytes must be dropped, and a fresh frame appended
    // after the flood must still dispatch.
    std::vector<uint8_t> pinned = {UART_START_BYTE, 0x00, 0x08};  // len=2048: valid, incomplete
    Append(pinned);
    ScanAll();
    std::vector<uint8_t> flood(kCap, 0x11);
    Append(flood);
    EXPECT_GT(stats_.dropped_bytes, 0u);
    ScanAll();
    Append(MakeFrame(0x42, 10, {1, 2}));
    ScanAll();
    ASSERT_EQ(frames_.size(), 1u);
    EXPECT_EQ(frames_[0].msg_type, 0x42);
}

TEST_F(FrameScannerTest, MaxSizeFrameRoundTrips) {
    std::vector<uint8_t> payload(UART_MAX_PAYLOAD, 0x5A);
    // Scanner capacity must fit the largest frame; use a dedicated scanner
    // sized like the links' buffers.
    static uint8_t big_storage[UART_MAX_PAYLOAD + UART_FRAME_OVERHEAD + 64];
    FrameScanner big(big_storage, sizeof(big_storage));
    ScanStats stats{};
    std::vector<uint8_t> frame(UART_MAX_PAYLOAD + UART_FRAME_OVERHEAD);
    size_t n =
        CreateUartPacket(frame.data(), frame.size(), 0x11, payload.data(), payload.size(), 2, 0);
    ASSERT_GT(n, 0u);
    big.Append(frame.data(), n, stats);
    size_t got = 0;
    big.Scan([&](const uint8_t*, size_t frame_len) { got = frame_len; }, stats);
    EXPECT_EQ(got, n);
    EXPECT_EQ(stats.frames, 1u);
}

}  // namespace
