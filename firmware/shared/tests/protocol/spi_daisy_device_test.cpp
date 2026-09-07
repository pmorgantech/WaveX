#include <gtest/gtest.h>

#include "../../../daisy/src/comm/daisy_spi_link.cpp"

extern "C" void dsy_spi_global_init() {
    // The scheduler must never be cleared while hardware still owns a stream.
    EXPECT_EQ(rx_stream.CR & DMA_SxCR_EN, 0u);
    EXPECT_EQ(tx_stream.CR & DMA_SxCR_EN, 0u);
    ++SpiDaisyMock::scheduler_resets;
}

namespace WaveX {
namespace Comm {
void ProcessInterMcuMessage(uint8_t, uint16_t, const uint8_t* payload, size_t bytes) {
    ++SpiDaisyMock::routed;
    SpiDaisyMock::routed_bytes = bytes;
    SpiDaisyMock::first_payload = payload[0];
    if (SpiDaisyMock::on_route)
        SpiDaisyMock::on_route();
}
}  // namespace Comm
}  // namespace WaveX

namespace {
using namespace WaveX::Comm;
using namespace WaveX::Protocol;
using namespace WaveX::Protocol::Spi;

class SpiDaisyDevice : public ::testing::Test {
   protected:
    daisy::DaisySeed hw;
    daisy::SpiHandle handle;
    void SetUp() override {
        initialized = false;
        outgoing.Finish(false);
        outgoing.Reset();
        transfer.Release();
        ready_gate = ReadyGate{};
        rx_sequence = SequenceTracker{};
        stats = {};
        last_poll_ms = 0;
        falling_edges.store(0);
        SpiDaisyMock::now = 10;
        SpiDaisyMock::pending_exti = 0;
        SpiDaisyMock::ready = false;
        SpiDaisyMock::cs_high = true;
        SpiDaisyMock::hold_dma_enabled = false;
        SpiDaisyMock::reset = false;
        SpiDaisyMock::launches = SpiDaisyMock::inits = SpiDaisyMock::resets = 0;
        SpiDaisyMock::scheduler_resets = SpiDaisyMock::routed = 0;
        SpiDaisyMock::before_tick = SpiDaisyMock::during_ready_read = SpiDaisyMock::on_route = {};
        daisy::SpiHandle::fail_launch = false;
        rx_stream = {};
        tx_stream = {};
        Spi_Init(hw, &handle);
        ASSERT_TRUE(initialized);
    }
    void Queue(uint16_t seq) {
        uint8_t frame[kFrameBytes]{};
        const uint8_t payload[] = {0x42};
        const auto bytes = ProtocolHandler::CreateWaveXPacket(
            frame, sizeof(frame), MSG_BROWSE_RESP, payload, sizeof(payload), seq, 0);
        ASSERT_TRUE(Spi_SendPreCreatedPacket(frame, bytes));
    }
    void Receive(uint16_t seq, size_t payload_size = 1) {
        uint8_t payload[kFrameBytes]{};
        std::memset(payload, 0x5a, payload_size);
        ASSERT_NE(ProtocolHandler::CreateWaveXPacket(
                      rx_dma, sizeof(rx_dma), MSG_BROWSE_REQ, payload, payload_size, seq, 0),
                  0u);
    }
    void FallingEdge() {
        SpiDaisyMock::pending_exti |= ready_mask;
        EXTI15_10_IRQHandler();
    }
};

TEST_F(SpiDaisyDevice, SendDmaPublishesRxAndRetainsCompletedBufferDuringDispatch) {
    Queue(1);
    SpiDaisyMock::ready = true;
    ProcessQueuedSpiMessage();
    ASSERT_EQ(SpiDaisyMock::launches, 1u);
    EXPECT_FALSE(SpiDaisyMock::cs_high);
    Receive(20, kFrameBytes - 6);
    daisy::SpiHandle::Complete();
    SpiDaisyMock::on_route = [&] {
        EXPECT_EQ(transfer.Get(), MasterTransfer::State::Complete);
        Queue(2);
        EXPECT_FALSE(outgoing.Begin(tx_dma));
    };
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::routed, 1u);
    EXPECT_EQ(SpiDaisyMock::routed_bytes, kFrameBytes - 6);
    EXPECT_EQ(SpiDaisyMock::first_payload, 0x5a);
    EXPECT_EQ(stats.packets_sent, 1u);
    EXPECT_EQ(outgoing.Count(), 1u);
    EXPECT_EQ(SpiDaisyMock::launches, 1u);  // Stale READY high cannot clock again.
}

TEST_F(SpiDaisyDevice, EmptyFrameCompletionDoesNotConsumeNewlyQueuedResponse) {
    SpiDaisyMock::ready = true;
    ProcessQueuedSpiMessage();
    Queue(1);
    daisy::SpiHandle::Complete();
    ProcessQueuedSpiMessage();
    EXPECT_EQ(outgoing.Count(), 1u);
    EXPECT_EQ(stats.packets_sent, 0u);
    FallingEdge();
    SpiDaisyMock::now += kPollIntervalMs;
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
    daisy::SpiHandle::Complete();
    ProcessQueuedSpiMessage();
    EXPECT_EQ(outgoing.Count(), 0u);
    EXPECT_EQ(stats.packets_sent, 1u);
}

TEST_F(SpiDaisyDevice, ExtiDuringForegroundPreparationCannotLaunchOrOverwriteDma) {
    Queue(1);
    SpiDaisyMock::ready = true;
    SpiDaisyMock::before_tick = [&] { FallingEdge(); };
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::launches, 1u);
    EXPECT_EQ(tx_dma[2], 1);
    EXPECT_EQ(outgoing.Count(), 1u);
    FallingEdge();
    EXPECT_EQ(SpiDaisyMock::launches, 1u);
}

TEST_F(SpiDaisyDevice, UnstableReadySampleDefersLaunch) {
    SpiDaisyMock::ready = true;
    SpiDaisyMock::during_ready_read = [&] { FallingEdge(); };
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::launches, 0u);
    SpiDaisyMock::during_ready_read = {};
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::launches, 1u);
}

TEST_F(SpiDaisyDevice, TimeoutWaitsForHardwareStopAndPreservesHeadThenReinitializes) {
    Queue(1);
    SpiDaisyMock::ready = true;
    ProcessQueuedSpiMessage();
    SpiDaisyMock::hold_dma_enabled = true;
    SpiDaisyMock::now += kTransferTimeoutMs;
    ProcessQueuedSpiMessage();
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Stopping);
    EXPECT_TRUE(outgoing.Owned());
    EXPECT_TRUE(SpiDaisyMock::cs_high);
    EXPECT_TRUE(SpiDaisyMock::reset);
    EXPECT_FALSE(SpiDaisyMock::irq_enabled[SPI1_IRQn]);
    EXPECT_FALSE(SpiDaisyMock::irq_enabled[DMA2_Stream2_IRQn]);
    EXPECT_EQ(SpiDaisyMock::scheduler_resets, 1u);  // Only initial setup.
    for (int pass = 0; pass < 100; ++pass)
        ProcessQueuedSpiMessage();  // Returns promptly despite stuck EN bits.
    EXPECT_EQ(SpiDaisyMock::launches, 1u);
    EXPECT_EQ(outgoing.Count(), 1u);
    SpiDaisyMock::hold_dma_enabled = false;
    rx_stream.CR = tx_stream.CR = 0;
    ProcessQueuedSpiMessage();
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Idle);
    EXPECT_EQ(SpiDaisyMock::inits, 2u);
    EXPECT_EQ(SpiDaisyMock::scheduler_resets, 2u);
    EXPECT_EQ(SpiDaisyMock::priorities[SPI1_IRQn], 10u);
    EXPECT_EQ(SpiDaisyMock::priorities[DMA2_Stream2_IRQn], 10u);
    EXPECT_EQ(SpiDaisyMock::priorities[DMA2_Stream3_IRQn], 10u);
    FallingEdge();
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
    EXPECT_EQ(tx_dma[2], 1);
}

TEST_F(SpiDaisyDevice, TimeoutAlsoCoversEmptyPollAndFailedLaunch) {
    SpiDaisyMock::ready = true;
    daisy::SpiHandle::fail_launch = true;
    ProcessQueuedSpiMessage();
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Failed);
    ProcessQueuedSpiMessage();
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Idle);
    EXPECT_EQ(SpiDaisyMock::inits, 2u);
    FallingEdge();
    daisy::SpiHandle::fail_launch = false;
    SpiDaisyMock::now += kPollIntervalMs;
    ProcessQueuedSpiMessage();
    SpiDaisyMock::now += kTransferTimeoutMs;
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::inits, 3u);
}

TEST_F(SpiDaisyDevice, DuplicateRxIsNotDispatchedAgain) {
    SpiDaisyMock::ready = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
        ProcessQueuedSpiMessage();
        Receive(20);
        daisy::SpiHandle::Complete();
        ProcessQueuedSpiMessage();
        FallingEdge();
        SpiDaisyMock::now += kPollIntervalMs;
    }
    EXPECT_EQ(SpiDaisyMock::routed, 1u);
}
}  // namespace
