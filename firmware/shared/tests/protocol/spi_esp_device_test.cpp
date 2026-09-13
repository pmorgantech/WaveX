#include <gtest/gtest.h>

#include "../../../esp32/main/links/esp_spi_link.cpp"
#include <array>

namespace {
class SpiEspDevice : public ::testing::Test {
   protected:
    WaveX::Comm::PacketRouter packet_router;
    void SetUp() override {
        SpiEspMock::semaphore = 0;
        SpiEspMock::ready = 0;
        SpiEspMock::now_us = 0;
        SpiEspMock::queued = SpiEspMock::waits = SpiEspMock::freed = SpiEspMock::routed = 0;
        SpiEspMock::task_deletes = 0;
        SpiEspMock::owned = nullptr;
        SpiEspMock::fail_queue = SpiEspMock::defer_setup = false;
        SpiEspMock::on_wait = {};
        SpiEspMock::on_route = {};
        ASSERT_EQ(spi_link_init(), ESP_OK);
        spi_link_set_packet_router(&packet_router);
        ASSERT_EQ(spi_link_start(), ESP_OK);
    }
    void TearDown() override {
        EXPECT_FALSE(running.load());
        EXPECT_EQ(spi_link_stop(), ESP_OK);
        SpiEspMock::on_wait = {};
        SpiEspMock::on_route = {};
    }
    void Queue(uint16_t type = MSG_BROWSE_REQ) {
        const uint8_t payload = 0x42;
        ASSERT_EQ(spi_link_send(type, &payload, 1), 1);
    }
    int Complete(spi_slave_transaction_t** completed, size_t bits, uint16_t rx_seq = 0) {
        auto* trans = SpiEspMock::owned;
        if (rx_seq) {
            const uint8_t payload = 0x5a;
            EXPECT_NE(WaveX::UartProtocol::CreateUartPacket(static_cast<uint8_t*>(trans->rx_buffer),
                                                            kFrameBytes,
                                                            MSG_BROWSE_RESP,
                                                            &payload,
                                                            1,
                                                            rx_seq,
                                                            0),
                      0u);
        }
        trans->trans_len = bits;
        SpiEspMock::slave.post_trans_cb(trans);
        *completed = trans;
        SpiEspMock::owned = nullptr;
        return ESP_OK;
    }
};

#if WAVEX_ESP_SPI_MISO_DRIVE_CAPABILITY >= 0
TEST(SpiEspConfiguration, FailedDriveSetupReleasesDriverWithoutStartingTask) {
    SpiEspMock::task = nullptr;
    SpiEspMock::freed = 0;
    SpiEspMock::fail_drive = true;
    ASSERT_EQ(spi_link_init(), ESP_OK);
    EXPECT_EQ(spi_link_start(), ESP_ERR_INVALID_STATE);
    EXPECT_FALSE(driver_initialized);
    EXPECT_FALSE(running.load());
    EXPECT_EQ(SpiEspMock::task, nullptr);
    EXPECT_EQ(SpiEspMock::freed, 1u);
    SpiEspMock::fail_drive = false;
    EXPECT_EQ(spi_link_stop(), ESP_OK);
}
#endif

TEST_F(SpiEspDevice, TenTimeoutsNeverRequeueOrConsumeMessageBehindEmptyFrame) {
    unsigned phase = 0;
    std::array<uint8_t, kFrameBytes> original{};
    SpiEspMock::on_wait = [&](spi_slave_transaction_t** result) {
        if (phase < 10) {
            if (phase == 0)
                Queue();
            ++phase;
            EXPECT_EQ(SpiEspMock::queued, 1u);
            EXPECT_EQ(std::memcmp(transaction.tx_buffer, original.data(), kFrameBytes), 0);
            EXPECT_EQ(outgoing.Count(), 1u);
            return ESP_ERR_TIMEOUT;
        }
        if (phase++ == 10)
            return Complete(result, kFrameBytes * 8);
        EXPECT_EQ(SpiEspMock::queued, 2u);
        EXPECT_EQ(static_cast<const uint8_t*>(transaction.tx_buffer)[4], MSG_BROWSE_REQ);
        EXPECT_EQ(outgoing.Count(), 1u);
        stop_requested.store(true);
        return Complete(result, kFrameBytes * 8);
    };
    SpiEspMock::task(nullptr);
    EXPECT_EQ(SpiEspMock::waits, 12u);
    EXPECT_EQ(stats.packets_sent, 1u);
    EXPECT_EQ(outgoing.Count(), 0u);
}

TEST_F(SpiEspDevice, ShortTransferRejectsRxAndRetriesSameTxThenFiltersDuplicateRx) {
    Queue();
    unsigned phase = 0;
    SpiEspMock::on_wait = [&](spi_slave_transaction_t** result) {
        EXPECT_EQ(SpiEspMock::queued, phase + 1);
        if (phase < 2) {
            EXPECT_EQ(static_cast<const uint8_t*>(transaction.tx_buffer)[5], 1);
        }
        if (phase++ == 0)
            return Complete(result, 32 * 8, 20);  // Valid bytes in RAM do not make a full transfer.
        if (phase == 2) {
            EXPECT_EQ(SpiEspMock::routed, 0u);
            EXPECT_EQ(stats.packets_sent, 0u);
            return Complete(result, kFrameBytes * 8, 20);
        }
        stop_requested.store(true);
        return Complete(result, kFrameBytes * 8, 20);  // Repeated peer frame.
    };
    SpiEspMock::on_route = [&] { Queue(MSG_STATUS_REQUEST); };  // No queue-mutex deadlock.
    SpiEspMock::task(nullptr);
    EXPECT_EQ(SpiEspMock::routed, 1u);
    EXPECT_EQ(stats.packets_sent, 2u);
    EXPECT_EQ(stats.crc_errors, 1u);
}

TEST_F(SpiEspDevice, ReadyIsAssertedOnlyWhenHardwareSetupCallbackRuns) {
    SpiEspMock::defer_setup = true;
    SpiEspMock::on_wait = [&](spi_slave_transaction_t** result) {
        EXPECT_EQ(SpiEspMock::ready, 0);  // Enqueue did not advertise hardware readiness.
        SpiEspMock::slave.post_setup_cb(SpiEspMock::owned);
        EXPECT_EQ(SpiEspMock::ready, 1);
        stop_requested.store(true);
        const int status = Complete(result, kFrameBytes * 8);
        EXPECT_EQ(SpiEspMock::ready, 0);
        return status;
    };
    SpiEspMock::task(nullptr);
}

TEST_F(SpiEspDevice, StopTimeoutRetainsDriverBuffersUntilDescriptorReturns) {
    SpiEspMock::on_wait = [&](spi_slave_transaction_t** result) {
        auto* tx = tx_dma;
        auto* rx = rx_dma;
        EXPECT_EQ(spi_link_stop(), ESP_ERR_TIMEOUT);
        EXPECT_EQ(SpiEspMock::freed, 0u);
        EXPECT_EQ(tx_dma, tx);
        EXPECT_EQ(rx_dma, rx);
        EXPECT_TRUE(outgoing.Owned());
        return Complete(result, kFrameBytes * 8);
    };
    SpiEspMock::task(nullptr);
    EXPECT_EQ(spi_link_stop(), ESP_OK);
    EXPECT_EQ(SpiEspMock::freed, 1u);
    EXPECT_EQ(tx_dma, nullptr);
    EXPECT_EQ(rx_dma, nullptr);
}

TEST_F(SpiEspDevice, TxUsesSixteenBitSequenceAndSupportsMaximumLogicalPayload) {
    next_sequence = 255;
    std::array<uint8_t, kMaxPayload> payload{};
    ASSERT_EQ(spi_link_send(MSG_BROWSE_RESP, payload.data(), payload.size()),
              static_cast<int>(payload.size()));
    Queue();
    unsigned phase = 0;
    SpiEspMock::on_wait = [&](spi_slave_transaction_t** result) {
        const auto* tx = static_cast<const uint8_t*>(transaction.tx_buffer);
        if (phase++ == 0) {
            EXPECT_EQ(tx[5], 255);
            EXPECT_EQ(tx[6], 0);
        } else {
            EXPECT_EQ(tx[5], 0);
            EXPECT_EQ(tx[6], 1);
            stop_requested.store(true);
        }
        return Complete(result, kFrameBytes * 8);
    };
    SpiEspMock::task(nullptr);
    EXPECT_EQ(stats.packets_sent, 2u);
}
}  // namespace
