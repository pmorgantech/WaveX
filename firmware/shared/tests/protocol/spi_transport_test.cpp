#include "spi_protocol/spi_transport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace {
using namespace WaveX::Protocol;
using namespace WaveX::Protocol::Spi;
using Frame = std::array<uint8_t, kFrameBytes>;

struct Packet {
    Frame data{};
    size_t bytes;
    explicit Packet(uint16_t sequence, size_t payload_size = 4) {
        Frame payload{};
        payload.fill(0x5a);
        bytes = ProtocolHandler::CreateWaveXPacket(
            data.data(), data.size(), MSG_BROWSE_RESP, payload.data(), payload_size, sequence, 0);
    }
};

TEST(SpiTxOwnership, EmptyCompletionDoesNotConsumeMessageQueuedDuringWait) {
    TxQueue queue;
    Frame dma{};
    ASSERT_TRUE(queue.Begin(dma.data()));
    Packet message(1);
    ASSERT_TRUE(queue.Push(message.data.data(), message.bytes));
    // Any number of timeout/retry attempts must leave the driver-owned frame unchanged.
    for (int timeout = 0; timeout < 10; ++timeout) {
        EXPECT_FALSE(queue.Begin(dma.data()));
        EXPECT_EQ(dma, Frame{});
        EXPECT_TRUE(queue.Owned());
    }
    EXPECT_FALSE(queue.Finish(true));
    EXPECT_EQ(queue.Count(), 1u);
    ASSERT_TRUE(queue.Begin(dma.data()));
    EXPECT_EQ(dma, message.data);
    EXPECT_TRUE(queue.Finish(true));
    EXPECT_EQ(queue.Count(), 0u);
}

TEST(SpiTxOwnership, FailedAndShortTransfersRetainExactHeadForRetry) {
    TxQueue queue;
    Packet first(1), second(2);
    ASSERT_TRUE(queue.Push(first.data.data(), first.bytes));
    Frame dma{};
    ASSERT_TRUE(queue.Begin(dma.data()));
    ASSERT_TRUE(queue.Push(second.data.data(), second.bytes));
    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_EQ(dma, first.data);
        EXPECT_FALSE(queue.Finish(false));
        EXPECT_EQ(queue.Count(), 2u);
        ASSERT_TRUE(queue.Begin(dma.data()));
    }
    EXPECT_TRUE(queue.Finish(true));
    ASSERT_TRUE(queue.Begin(dma.data()));
    EXPECT_EQ(dma, second.data);
    EXPECT_TRUE(queue.Finish(true));
    EXPECT_FALSE(queue.Finish(true));  // A repeated completion cannot pop again.
}

TEST(SpiTxOwnership, FullDuplexRxIsProcessedBeforeBuffersAreReleased) {
    TxQueue queue;
    Packet sent(9), received(10), response(11);
    Frame tx{}, rx = received.data;
    ASSERT_TRUE(queue.Push(sent.data.data(), sent.bytes));
    ASSERT_TRUE(queue.Begin(tx.data()));
    MasterTransfer transfer;
    ASSERT_TRUE(transfer.Reserve(10));
    transfer.CompleteFromIsr(true);
    EXPECT_FALSE(transfer.Reserve(11));  // Even after DMA completion, RX is still owned.
    EXPECT_FALSE(queue.Begin(tx.data()));
    SequenceTracker sequence;
    size_t bytes = 0;
    ASSERT_EQ(InspectFrame(rx.data(), rx.size(), sequence, bytes), RxResult::Packet);
    ASSERT_TRUE(queue.Push(response.data.data(), response.bytes));  // RX dispatcher response.
    EXPECT_EQ(rx, received.data);
    EXPECT_TRUE(queue.Finish(true));  // The outgoing packet was sent only once.
    transfer.Release();
    ASSERT_TRUE(transfer.Reserve(12));
    ASSERT_TRUE(queue.Begin(tx.data()));
    EXPECT_EQ(tx, response.data);
}

TEST(SpiTxOwnership, QueueWrapAndBackpressurePreserveOrder) {
    TxQueue queue;
    Frame dma{};
    for (uint16_t base = 1; base < 40; base = static_cast<uint16_t>(base + 8)) {
        for (uint16_t offset = 0; offset < 8; ++offset) {
            Packet p(static_cast<uint16_t>(base + offset));
            ASSERT_TRUE(queue.Push(p.data.data(), p.bytes));
        }
        Packet overflow(500);
        EXPECT_FALSE(queue.Push(overflow.data.data(), overflow.bytes));
        for (uint16_t offset = 0; offset < 8; ++offset) {
            ASSERT_TRUE(queue.Begin(dma.data()));
            const Packet expected(static_cast<uint16_t>(base + offset));
            EXPECT_EQ(dma, expected.data);
            ASSERT_TRUE(queue.Finish(true));
        }
    }
}

TEST(SpiTxOwnership, RejectsMalformedPacketsAndNeverResetsOwnedFrame) {
    TxQueue queue;
    Packet p(1);
    EXPECT_FALSE(queue.Push(nullptr, p.bytes));
    EXPECT_FALSE(queue.Push(p.data.data(), 5));
    EXPECT_FALSE(queue.Push(p.data.data(), kFrameBytes + 1));
    EXPECT_FALSE(queue.Push(p.data.data(), p.bytes - 1));
    p.data[4] ^= 1;
    EXPECT_FALSE(queue.Push(p.data.data(), p.bytes));
    p.data[4] ^= 1;
    ASSERT_TRUE(queue.Push(p.data.data(), p.bytes));
    Frame dma{};
    ASSERT_TRUE(queue.Begin(dma.data()));
    queue.Reset();
    EXPECT_TRUE(queue.Owned());
    EXPECT_EQ(queue.Count(), 1u);
    queue.Finish(true);
    queue.Reset();
    EXPECT_EQ(queue.Count(), 0u);
}

TEST(SpiTxOwnership, LargeThenSmallTransferZeroesPhysicalPadding) {
    TxQueue queue;
    Packet large(1, kFrameBytes - 6), small(2);
    Frame dma{};
    ASSERT_EQ(large.bytes, kFrameBytes);
    ASSERT_TRUE(queue.Push(large.data.data(), large.bytes));
    ASSERT_TRUE(queue.Begin(dma.data()));
    queue.Finish(true);
    ASSERT_TRUE(queue.Push(small.data.data(), small.bytes));
    ASSERT_TRUE(queue.Begin(dma.data()));
    EXPECT_EQ(dma, small.data);
}

TEST(SpiMasterOwnership, IsReservedBeforeLaunchAndRejectsReentry) {
    MasterTransfer transfer;
    ASSERT_TRUE(transfer.Reserve(5));
    EXPECT_FALSE(transfer.Reserve(5));
    transfer.CompleteFromIsr(true);
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Complete);
    EXPECT_FALSE(transfer.Reserve(6));
    transfer.Release();
    EXPECT_TRUE(transfer.Reserve(7));
}

TEST(SpiMasterOwnership, TimeoutCoversEveryDuplexTransferAndClockWrap) {
    for (uint32_t start: {uint32_t{10}, UINT32_MAX - 50}) {
        MasterTransfer transfer;
        ASSERT_TRUE(transfer.Reserve(start));
        EXPECT_FALSE(transfer.Expired(start + kTransferTimeoutMs - 1));
        EXPECT_TRUE(transfer.Expired(start + kTransferTimeoutMs));
        transfer.Stop();
        for (int foreground_pass = 0; foreground_pass < 100; ++foreground_pass) {
            EXPECT_FALSE(transfer.Reserve(start + 200));
            transfer.CompleteFromIsr(true);  // Late IRQ cannot release stopping DMA.
            EXPECT_EQ(transfer.Get(), MasterTransfer::State::Stopping);
        }
        // The device adapter calls Release only after observing both DMA EN bits clear.
        transfer.Release();
        EXPECT_TRUE(transfer.Reserve(start + 300));
    }
}

TEST(SpiMasterOwnership, CompletionWinsTimeoutAndRepeatedCallbackCannotChangeOutcome) {
    MasterTransfer transfer;
    ASSERT_TRUE(transfer.Reserve(10));
    transfer.CompleteFromIsr(true);
    EXPECT_FALSE(transfer.Expired(1000));
    transfer.CompleteFromIsr(false);
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Complete);
}

TEST(SpiMasterOwnership, FailureCannotStartAnotherDmaBeforeRecovery) {
    MasterTransfer transfer;
    ASSERT_TRUE(transfer.Reserve(10));
    transfer.CompleteFromIsr(false);
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Failed);
    EXPECT_FALSE(transfer.Reserve(100));
    transfer.Stop();
    EXPECT_FALSE(transfer.Reserve(200));
    transfer.Release();
    EXPECT_TRUE(transfer.Reserve(300));
}

TEST(SpiReady, AlreadyHighAtStartupIsUsableExactlyOnce) {
    ReadyGate gate;
    EXPECT_TRUE(gate.Observe(true, 0));
    gate.Consume(0);
    for (int pass = 0; pass < 100; ++pass)
        EXPECT_FALSE(gate.Observe(true, 0));
}

TEST(SpiReady, ShortLowPulseCapturedByExtiAllowsNextFrame) {
    ReadyGate gate;
    ASSERT_TRUE(gate.Observe(true, 0));
    gate.Consume(0);
    EXPECT_TRUE(gate.Observe(true, 1));  // Foreground never sampled the low level.
    gate.Consume(1);
    EXPECT_FALSE(gate.Observe(true, 1));
}

TEST(SpiReady, LevelPollingRecoversMissedEdge) {
    ReadyGate gate;
    gate.Consume(0);
    EXPECT_FALSE(gate.Observe(false, 0));
    EXPECT_TRUE(gate.Observe(true, 0));
    gate.Consume(0);
    EXPECT_FALSE(gate.Observe(true, 0));
}

TEST(SpiReady, LowIsNeverPermissionToClockAndEdgeCounterCanWrap) {
    ReadyGate gate;
    gate.Consume(UINT32_MAX);
    EXPECT_FALSE(gate.Observe(false, 0));
    EXPECT_TRUE(gate.Observe(true, 0));
    gate.Consume(0);
    EXPECT_FALSE(gate.Observe(true, 0));
}

TEST(SpiRx, UsesActualPhysicalLengthEvenWhenDmaBufferContainsValidPacket) {
    Packet packet(1);
    SequenceTracker sequence;
    size_t bytes = 999;
    for (size_t actual: {size_t{0}, size_t{1}, packet.bytes, kFrameBytes - 1, kFrameBytes + 1}) {
        EXPECT_EQ(InspectFrame(packet.data.data(), actual, sequence, bytes), RxResult::Invalid);
        EXPECT_EQ(bytes, 0u);
        EXPECT_EQ(sequence.ExpectedSeq(), 1);
    }
    EXPECT_EQ(InspectFrame(packet.data.data(), kFrameBytes, sequence, bytes), RxResult::Packet);
    EXPECT_EQ(bytes, packet.bytes);
}

TEST(SpiRx, DuplicateIsNotDispatchedTwiceAndCorruptPacketDoesNotAdvanceSequence) {
    Packet packet(100);
    SequenceTracker sequence;
    size_t bytes = 0;
    packet.data[4] ^= 1;
    EXPECT_EQ(InspectFrame(packet.data.data(), kFrameBytes, sequence, bytes), RxResult::Invalid);
    EXPECT_EQ(sequence.ExpectedSeq(), 1);
    packet.data[4] ^= 1;
    EXPECT_EQ(InspectFrame(packet.data.data(), kFrameBytes, sequence, bytes), RxResult::Packet);
    EXPECT_EQ(InspectFrame(packet.data.data(), kFrameBytes, sequence, bytes), RxResult::Duplicate);
    Packet reboot(1);
    EXPECT_EQ(InspectFrame(reboot.data.data(), kFrameBytes, sequence, bytes), RxResult::Packet);
}

TEST(SpiRx, EmptyFramesAndInvalidSizeCodesDoNotReachHandlers) {
    Frame empty{};
    SequenceTracker sequence;
    size_t bytes = 0;
    EXPECT_EQ(InspectFrame(empty.data(), kFrameBytes, sequence, bytes), RxResult::Empty);
    EXPECT_EQ(InspectFrame(nullptr, kFrameBytes, sequence, bytes), RxResult::Invalid);
    empty[0] = 0xf;
    EXPECT_EQ(InspectFrame(empty.data(), kFrameBytes, sequence, bytes), RxResult::Invalid);
    EXPECT_EQ(sequence.ExpectedSeq(), 1);
}

TEST(SpiRx, AllPacketSizeClassesParseWithExplicitPayloadCapacity) {
    for (size_t payload_size: {size_t{0},
                               size_t{40},
                               size_t{100},
                               size_t{220},
                               size_t{450},
                               size_t{900},
                               kFrameBytes - 6}) {
        const Packet packet(1, payload_size);
        SequenceTracker sequence;
        size_t bytes = 0;
        ASSERT_EQ(InspectFrame(packet.data.data(), kFrameBytes, sequence, bytes), RxResult::Packet);
        Frame payload{};
        size_t capacity = payload.size();
        uint8_t type = 0, flags = 0;
        uint16_t seq = 0;
        ASSERT_TRUE(ProtocolHandler::ParseWaveXPacket(
            packet.data.data(), bytes, type, payload.data(), capacity, seq, flags));
        EXPECT_EQ(capacity, bytes - 6);
        EXPECT_EQ(seq, 1);
        for (size_t i = 0; i < payload_size; ++i)
            EXPECT_EQ(payload[i], 0x5a);
    }
}

TEST(SpiSequence, CrossesEightBitBoundaryAndWrapsWithoutReservedZero) {
    uint16_t seq = 1;
    SequenceTracker receiver;
    for (uint32_t packet_number = 1; packet_number <= 65537; ++packet_number) {
        EXPECT_NE(seq, 0);
        const auto accepted = receiver.Evaluate(seq);
        EXPECT_TRUE(accepted == SequenceTracker::Result::Accept ||
                    accepted == SequenceTracker::Result::ResyncAccept);
        if (packet_number == 256)
            EXPECT_EQ(seq, 256);
        if (packet_number == 65536)
            EXPECT_EQ(seq, 1);
        seq = NextSequence(seq);
    }
}
}  // namespace
