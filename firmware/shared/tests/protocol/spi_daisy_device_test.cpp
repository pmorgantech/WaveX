#include <gtest/gtest.h>

#include "../../../daisy/src/comm/mcu_link.h"

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
        incoming.Finish(false);
        incoming.Reset();
        dispatching = false;
        next_sequence = 1;
        outgoing.Finish(false);
        outgoing.Reset();
        transfer.Release();
        ready_gate = ReadyGate{};
        rx_sequence = SequenceTracker{};
        stats = {};
        last_poll_ms = 0;
        falling_edges.store(0);
        SpiDaisyMock::clock_override = 0;
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
        dma_pending_flags = 0;
        SpiDaisyMock::irq_pending = {};
        rx_stream = {};
        tx_stream = {};
        for (auto& port: ::gpio_ports)
            port.OSPEEDR = 0xaaaaaaaau;
        Spi_Init(hw, &handle);
        ASSERT_TRUE(initialized);
    }
    void Queue(uint16_t seq) {
        uint8_t frame[kFrameBytes]{};
        const uint8_t payload[] = {0x42};
        const auto bytes = WaveX::UartProtocol::CreateUartPacket(
            frame, sizeof(frame), MSG_BROWSE_RESP, payload, sizeof(payload), seq, 0);
        ASSERT_TRUE(Spi_SendPreCreatedPacket(frame, bytes));
    }
    void Receive(uint16_t seq, size_t payload_size = 1) {
        uint8_t payload[kFrameBytes]{};
        std::memset(payload, 0x5a, payload_size);
        ASSERT_NE(WaveX::UartProtocol::CreateUartPacket(
                      rx_dma, sizeof(rx_dma), MSG_BROWSE_REQ, payload, payload_size, seq, 0),
                  0u);
    }
    void FallingEdge() {
        SpiDaisyMock::pending_exti |= ready_mask;
        EXTI15_10_IRQHandler();
    }
};

TEST_F(SpiDaisyDevice, OutputSlewSurvivesRecoveryWithoutChangingOtherPins) {
    const auto check_speeds = [&] {
        for (unsigned port = 0; port < 11; ++port) {
            uint32_t expected = 0xaaaaaaaau;
            for (const auto pin: {config.pin_config.sclk, config.pin_config.mosi}) {
                if (pin.port != port)
                    continue;
                const uint32_t shift = 2u * pin.pin;
                expected &= ~(3u << shift);
#if WAVEX_DAISY_SPI_FAST_GPIO_ENABLED
                expected |= 3u << shift;
#endif
            }
            EXPECT_EQ(::gpio_ports[port].OSPEEDR, expected);
        }
    };
    check_speeds();
    SpiDaisyMock::ready = true;
    daisy::SpiHandle::fail_launch = true;
    ProcessQueuedSpiMessage();
    ProcessQueuedSpiMessage();
    ASSERT_EQ(SpiDaisyMock::inits, 2u);  // Vendor init restored low slew first.
    check_speeds();
}

TEST_F(SpiDaisyDevice, RejectsClockTreeThatDoesNotProduceRequestedSpeed) {
    initialized = false;
    SpiDaisyMock::clock_override = 24000000;  // changed upstream clock tree
    EXPECT_FALSE(Spi_Init(hw, &handle));
    EXPECT_FALSE(initialized);
    EXPECT_LT(Spi_Send(MSG_HEARTBEAT, nullptr, 0), 0);
}

TEST_F(SpiDaisyDevice, CompletionRetiresLateTxInterruptAfterLibraryReleasesOwner) {
    Queue(1);
    SpiDaisyMock::ready = true;
    ProcessQueuedSpiMessage();
    Receive(1);
    // Captured on hardware: RX has completed, libDaisy's owner is already
    // -1, but TX HT/TC and its NVIC interrupt are still pending. The vendor
    // handler would return without acknowledging either, causing a storm.
    dma_pending_flags = DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTCIF3;
    SpiDaisyMock::irq_pending[DMA2_Stream3_IRQn] = true;
    daisy::SpiHandle::Complete();
    EXPECT_EQ(dma_pending_flags, 0u);
    EXPECT_FALSE(SpiDaisyMock::irq_pending[DMA2_Stream3_IRQn]);
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Complete);
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::routed, 1u);
    EXPECT_EQ(outgoing.Count(), 0u);
}

TEST_F(SpiDaisyDevice, CompletionCannotPublishOrClearFlagsWhileDmaRemainsEnabled) {
    Queue(1);
    SpiDaisyMock::ready = true;
    ProcessQueuedSpiMessage();
    Receive(1);
    dma_pending_flags = DMA_LIFCR_CTCIF3;
    // A premature/error completion cannot authorize buffer reuse.
    FinishDma(nullptr, daisy::SpiHandle::Result::OK);
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Failed);
    EXPECT_EQ(dma_pending_flags, DMA_LIFCR_CTCIF3);
    EXPECT_FALSE(SpiDaisyMock::irq_enabled[DMA2_Stream2_IRQn]);
    EXPECT_FALSE(SpiDaisyMock::irq_enabled[DMA2_Stream3_IRQn]);
    EXPECT_TRUE(outgoing.Owned());
    EXPECT_EQ(SpiDaisyMock::routed, 0u);
    SpiDaisyMock::hold_dma_enabled = true;
    ProcessQueuedSpiMessage();
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Stopping);
    EXPECT_TRUE(outgoing.Owned());
    EXPECT_EQ(outgoing.Count(), 1u);
}

TEST_F(SpiDaisyDevice, SendDmaRetainsRxInOwnedCopyBeforeDispatch) {
    Queue(1);
    SpiDaisyMock::ready = true;
    ProcessQueuedSpiMessage();
    ASSERT_EQ(SpiDaisyMock::launches, 1u);
    EXPECT_FALSE(SpiDaisyMock::cs_high);
    Receive(20, kMaxPayload);
    daisy::SpiHandle::Complete();
    SpiDaisyMock::on_route = [&] {
        EXPECT_EQ(transfer.Get(), MasterTransfer::State::Idle);
        EXPECT_TRUE(dispatching);
        Queue(2);
        ProcessQueuedSpiMessage();  // Reentrant outer service must not dispatch.
        EXPECT_EQ(SpiDaisyMock::routed, 1u);
    };
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::routed, 1u);
    EXPECT_EQ(SpiDaisyMock::routed_bytes, kMaxPayload);
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
    EXPECT_EQ(tx_dma[5], 1);
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
    EXPECT_EQ(tx_dma[5], 1);
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

TEST_F(SpiDaisyDevice, TxPumpRetainsRxWithoutDispatchAndCannotOverwriteFullQueue) {
    SpiDaisyMock::ready = true;
    for (uint16_t seq = 1; seq <= 8; ++seq) {
        Spi_PumpTx();
        Receive(seq, seq);
        daisy::SpiHandle::Complete();
        Spi_PumpTx();
        EXPECT_EQ(SpiDaisyMock::routed, 0u);
        FallingEdge();
        SpiDaisyMock::now += kPollIntervalMs;
    }
    ASSERT_TRUE(incoming.Full());
    const auto launches = SpiDaisyMock::launches;
    Spi_PumpTx();
    EXPECT_EQ(SpiDaisyMock::launches, launches);
    for (size_t bytes = 1; bytes <= 8; ++bytes) {
        ProcessQueuedSpiMessage();
        EXPECT_EQ(SpiDaisyMock::routed_bytes, bytes);
    }
    EXPECT_EQ(SpiDaisyMock::routed, 8u);
}

TEST_F(SpiDaisyDevice, TxPumpInsideHandlerDoesNotRecursivelyDispatchNextCommand) {
    SpiDaisyMock::ready = true;
    Spi_PumpTx();
    Receive(1, 3);
    daisy::SpiHandle::Complete();
    Spi_PumpTx();
    FallingEdge();
    SpiDaisyMock::now += kPollIntervalMs;
    SpiDaisyMock::on_route = [&] {
        EXPECT_EQ(SpiDaisyMock::routed, 1u);
        Spi_PumpTx();  // TX-only progress is explicitly allowed inside a handler.
        ASSERT_EQ(transfer.Get(), MasterTransfer::State::Running);
        Receive(2, 7);
        daisy::SpiHandle::Complete();
        Spi_PumpTx();
        ProcessQueuedSpiMessage();
        EXPECT_EQ(SpiDaisyMock::routed, 1u);
    };
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::routed_bytes, 3u);
    SpiDaisyMock::on_route = {};
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::routed, 2u);
    EXPECT_EQ(SpiDaisyMock::routed_bytes, 7u);
}

TEST_F(SpiDaisyDevice, ReadyCompletionCanLaunchQueuedFrameWithoutMillisecondDelay) {
    Queue(1);
    Queue(2);
    SpiDaisyMock::ready = true;
    Spi_PumpTx();
    daisy::SpiHandle::Complete();
    FallingEdge();
    Spi_PumpTx();  // Same millisecond, but new READY and returned DMA ownership.
#if WAVEX_DAISY_SPI_FAST_SCHEDULING_ENABLED
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
    EXPECT_EQ(tx_dma[5], 2);
#else
    EXPECT_EQ(SpiDaisyMock::launches, 1u);
    SpiDaisyMock::now += kPollIntervalMs;
    Spi_PumpTx();
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
    EXPECT_EQ(tx_dma[5], 2);
#endif
    for (int pass = 0; pass < 100; ++pass)
        Spi_PumpTx();  // READY alone cannot authorize concurrent DMA.
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
}

TEST_F(SpiDaisyDevice, FastDispatchPlacesReplyInNextReadyFrame) {
    SpiDaisyMock::ready = true;
    Spi_PumpTx();
    Receive(1);
    daisy::SpiHandle::Complete();
    FallingEdge();
    SpiDaisyMock::now += kPollIntervalMs;
    SpiDaisyMock::on_route = [&] { Queue(2); };
    ProcessQueuedSpiMessage();
    ASSERT_EQ(SpiDaisyMock::routed, 1u);
    ASSERT_EQ(SpiDaisyMock::launches, 2u);
#if WAVEX_DAISY_SPI_FAST_SCHEDULING_ENABLED
    EXPECT_EQ(tx_dma[5], 2);  // Response was prepared after dispatch.
#else
    EXPECT_EQ(tx_dma[5], 0);  // Legacy mode already armed an empty frame.
#endif
}

TEST_F(SpiDaisyDevice, DeferredReplyGetsForegroundPassBeforeAnotherEmptyPoll) {
    SpiDaisyMock::ready = true;
    Spi_PumpTx();
    Receive(1);
    daisy::SpiHandle::Complete();
    FallingEdge();
    SpiDaisyMock::now += kPollIntervalMs;
    ProcessQueuedSpiMessage();
    ASSERT_EQ(SpiDaisyMock::routed, 1u);
#if WAVEX_DAISY_SPI_FAST_SCHEDULING_ENABLED
    EXPECT_EQ(SpiDaisyMock::launches, 1u);
    EXPECT_EQ(transfer.Get(), MasterTransfer::State::Idle);
    Queue(2);  // A deferred producer later in the same foreground pass.
    ProcessQueuedSpiMessage();
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
    EXPECT_EQ(tx_dma[5], 2);
#else
    EXPECT_EQ(SpiDaisyMock::launches, 2u);
    EXPECT_EQ(tx_dma[5], 0);
#endif
}

TEST_F(SpiDaisyDevice, SelectedLinkQueuesExactMaximumPayloadAndReportsBackpressure) {
    uint8_t payload[kMaxPayload]{};
    ASSERT_TRUE(LinkTxIdle());
    for (int i = 0; i < 8; ++i)
        ASSERT_EQ(LinkSend(MSG_BROWSE_RESP, payload, sizeof(payload)), sizeof(payload));
    EXPECT_FALSE(LinkTxIdle());
    EXPECT_LT(LinkSend(MSG_BROWSE_RESP, payload, sizeof(payload)), 0);
    EXPECT_LT(LinkSend(256, payload, sizeof(payload)), 0);
    EXPECT_LT(LinkSend(MSG_BROWSE_RESP, nullptr, 1), 0);
    EXPECT_EQ(outgoing.Count(), 8u);
}
}  // namespace
