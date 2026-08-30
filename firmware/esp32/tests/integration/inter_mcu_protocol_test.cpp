// ESP32-side inter-MCU consumption path, end to end on the host:
//
//   real UART frame bytes -> ParseUartPacket (shared codec)
//     -> PacketRouter::route_uart_message (real router, real weak handlers)
//       -> inter_mcu_* boundary (capture mocks assert content)
//   and, mirroring esp_uart_link.cpp, the router's stats callback feeds a
//   real StatisticsManager whose counters are asserted.
//
// This intentionally does NOT re-run pure encode/decode round-trip matrices -
// those belong to firmware/shared/tests. What is asserted here is that a
// frame the Daisy would send arrives at the ESP32's consumer boundary with
// the right values, after every real conversion/validation step in between.
//
// The previous version of this file routed the *parsed payload* through
// route_packet(), which expects a full WaveX packet with its own header and
// CRC - validation always failed, nothing was ever dispatched, and the tests
// passed on the encode/parse half alone.

#include <gtest/gtest.h>

#include "../../shared/spi_protocol/protocol.h"
#include "../mocks/esp32_mocks.h"
#include "comm/statistics.h"
#include "packet_router.h"
#include "uart_protocol.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace WaveX::Comm;
using namespace WaveX::Protocol;
using namespace WaveX::UartProtocol;
using WaveX::Test::GetInterMcuCapture;
using WaveX::Test::ResetInterMcuCapture;

namespace {

class InterMcuProtocolIntegrationTest : public ::testing::Test {
   protected:
    void SetUp() override {
        ResetInterMcuCapture();
        router_ = std::make_unique<PacketRouter>();
        // Same wiring esp_uart_link.cpp performs at init: routed packets are
        // counted into the StatisticsManager.
        router_->set_stats_callback(
            [this](uint8_t msg_type) { stats_.increment_packet_stat(msg_type); });
    }

    // Encode a message exactly as the Daisy's TX path does.
    std::vector<uint8_t> EncodeUartFrame(uint8_t msg_type,
                                         const void* payload,
                                         size_t payload_size,
                                         uint16_t seq = 0,
                                         uint8_t flags = 0) {
        std::vector<uint8_t> buffer(UART_MAX_PAYLOAD + UART_FRAME_OVERHEAD);
        size_t frame_size = CreateUartPacket(
            buffer.data(), buffer.size(), msg_type, payload, payload_size, seq, flags);
        buffer.resize(frame_size);
        return buffer;
    }

    // The ESP32 RX path: parse the frame, then route the payload the way
    // esp_uart_link.cpp does. Returns false when the frame is rejected at
    // the parse stage (in which case nothing may have been dispatched).
    bool ReceiveFrame(const std::vector<uint8_t>& frame) {
        if (frame.empty()) {
            return false;
        }
        uint8_t msg_type = 0;
        uint8_t flags = 0;
        uint16_t seq = 0;
        uint8_t payload[UART_MAX_PAYLOAD];
        size_t payload_size = sizeof(payload);
        if (!ParseUartPacket(
                frame.data(), frame.size(), msg_type, payload, payload_size, seq, flags)) {
            stats_.increment_invalid_packet();
            return false;
        }
        router_->route_uart_message(
            msg_type, payload_size ? payload : nullptr, payload_size, flags, seq);
        return true;
    }

    StatisticsManager stats_;
    std::unique_ptr<PacketRouter> router_;
};

// Heartbeat: uptime/counters arrive as sent; x10 CPU fields arrive divided
// down to percent at the inter_mcu boundary.
TEST_F(InterMcuProtocolIntegrationTest, HeartbeatFrameReachesBackendState) {
    HeartbeatMessage heartbeat(12345, 100, 200, 256, 128, 512);

    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat), 7)));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.heartbeat_calls, 1);
    EXPECT_EQ(cap.hb_uptime_ms, 12345u);
    EXPECT_EQ(cap.hb_rx_total, 100u);
    EXPECT_EQ(cap.hb_loop_counter, 200u);
    EXPECT_FLOAT_EQ(cap.hb_cpu_avg, 25.6f);
    EXPECT_FLOAT_EQ(cap.hb_cpu_min, 12.8f);
    EXPECT_FLOAT_EQ(cap.hb_cpu_max, 51.2f);

    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.heartbeat_packets, 1u);
    EXPECT_EQ(s.total_packets, 1u);
}

// Meter push: Q15 wire values arrive as floats in 0..1.
TEST_F(InterMcuProtocolIntegrationTest, MeterPushFrameReachesMeterBoundaryAsFloats) {
    MeterPushMessage meter(0x4000, 0x6000, 0x7FFF, 0x5000);

    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_METER_PUSH, &meter, sizeof(meter))));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.meter_calls, 1);
    EXPECT_FLOAT_EQ(cap.meter_rms_left, 0x4000 / 32767.0f);
    EXPECT_FLOAT_EQ(cap.meter_rms_right, 0x6000 / 32767.0f);
    EXPECT_FLOAT_EQ(cap.meter_peak_left, 1.0f);
    EXPECT_FLOAT_EQ(cap.meter_peak_right, 0x5000 / 32767.0f);

    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.meter_push_packets, 1u);
}

// Browse response: a real BrowseRespPacket payload (header + wire entries)
// is forwarded verbatim to the browse listener boundary.
TEST_F(InterMcuProtocolIntegrationTest, BrowseResponseFrameReachesListenerVerbatim) {
    FileEntryWire entries[3] = {FileEntryWire(1, 0, "DRUMS"),
                                FileEntryWire(0, 44100, "kick.wav", 44100, 2, 16, 500),
                                FileEntryWire(0, 96000, "snare.wav", 48000, 1, 24, 1000)};

    std::vector<uint8_t> packet(2048);
    size_t packet_len =
        ProtocolHandler::CreateBrowseRespPacket(packet.data(), packet.size(), 3, entries, 3);
    ASSERT_GT(packet_len, 0u);

    // CreateBrowseRespPacket produces a full WaveX packet; the browse payload
    // inside it is what travels in a UART frame's payload on the live link.
    uint8_t msg_type = 0, flags = 0;
    uint16_t seq = 0;
    uint8_t payload[2048];
    size_t payload_size = sizeof(payload);
    ASSERT_TRUE(ProtocolHandler::ParseWaveXPacket(
        packet.data(), packet_len, msg_type, payload, payload_size, seq, flags));
    ASSERT_EQ(msg_type, MSG_BROWSE_RESP);

    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_BROWSE_RESP, payload, payload_size)));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_resp_calls, 1);
    // WaveX packets are size-class quantized, so the payload extracted from
    // the packet (and therefore delivered here) may carry trailing padding.
    ASSERT_GE(cap.browse_resp_data.size(), sizeof(BrowseRespHeader) + 3 * sizeof(FileEntryWire));

    BrowseRespHeader header;
    memcpy(&header, cap.browse_resp_data.data(), sizeof(header));
    EXPECT_EQ(header.total_count, 3u);
    EXPECT_EQ(header.n, 3);

    FileEntryWire entry;
    memcpy(&entry,
           cap.browse_resp_data.data() + sizeof(header) + sizeof(FileEntryWire),
           sizeof(entry));
    EXPECT_STREQ(entry.name, "kick.wav");
    EXPECT_EQ(entry.sample_rate, 44100u);
    EXPECT_EQ(entry.channels, 2);
    EXPECT_EQ(entry.bits_per_sample, 16);
}

// Wave chunk: header + samples in one frame; samples arrive intact.
TEST_F(InterMcuProtocolIntegrationTest, WaveChunkFrameDeliversSamples) {
    constexpr uint16_t kCount = 8;
    struct {
        WaveChunkMessage header;
        int16_t samples[kCount];
    } __attribute__((packed)) msg;
    msg.header = WaveChunkMessage(4096, kCount);
    for (int i = 0; i < kCount; ++i) {
        msg.samples[i] = static_cast<int16_t>(i * 1000 - 3500);
    }

    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_WAVE_CHUNK, &msg, sizeof(msg))));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.wave_chunk_calls, 1);
    EXPECT_EQ(cap.wave_chunk_offset, 4096u);
    ASSERT_EQ(cap.wave_chunk_samples.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(cap.wave_chunk_samples[i], i * 1000 - 3500) << "sample " << i;
    }
}

// Envelope chunk: the router validates channels/length before handing the
// column run to the cache callback.
TEST_F(InterMcuProtocolIntegrationTest, EnvelopeChunkFrameDeliversColumns) {
    constexpr uint16_t kColumns = 4;
    struct {
        EnvelopeChunkMessage header;
        EnvelopeColumn columns[kColumns * 2];
    } __attribute__((packed)) msg;
    msg.header.sample_id = 3;
    msg.header.generation = 9;
    msg.header.start_frame = 100;
    msg.header.end_frame = 500;
    msg.header.total_columns = kColumns;
    msg.header.first_column = 0;
    msg.header.columns = kColumns;
    msg.header.channels = 2;
    for (uint16_t c = 0; c < kColumns; ++c) {
        msg.columns[c * 2 + 0] =
            EnvelopeColumn(static_cast<int16_t>(-(c + 1)), static_cast<int16_t>(c + 1));
        msg.columns[c * 2 + 1] = EnvelopeColumn(static_cast<int16_t>(-100 * (c + 1)),
                                                static_cast<int16_t>(100 * (c + 1)));
    }

    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_ENVELOPE_CHUNK, &msg, sizeof(msg))));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.envelope_chunk_calls, 1);
    EXPECT_EQ(cap.envelope_header.sample_id, 3);
    EXPECT_EQ(cap.envelope_header.generation, 9);
    EXPECT_EQ(cap.envelope_header.channels, 2);
    ASSERT_EQ(cap.envelope_columns.size(), static_cast<size_t>(kColumns) * 2);
    EXPECT_EQ(cap.envelope_columns[0].min_sample, -1);
    EXPECT_EQ(cap.envelope_columns[1].max_sample, 100);   // col 0, right channel
    EXPECT_EQ(cap.envelope_columns[7].min_sample, -400);  // col 3, right channel
}

// Sample status and storage status: both consumers see converted values.
TEST_F(InterMcuProtocolIntegrationTest, StatusFramesReachTheirConsumers) {
    SampleStatusMessage status(11, 1, 2, 48000, 96000);
    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_SAMPLE_STATUS, &status, sizeof(status))));

    StorageStatusMessage storage(0);
    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_STORAGE_STATUS, &storage, sizeof(storage))));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.sample_status_calls, 1);
    EXPECT_EQ(cap.sample_status_id, 11);
    EXPECT_EQ(cap.sample_status_rate, 48000u);
    EXPECT_EQ(cap.sample_status_frames, 96000u);
    ASSERT_EQ(cap.storage_status_calls, 1);
    EXPECT_FALSE(cap.storage_status_mounted);

    // Both are 0x30-block responses: counted as other_known, not unknown.
    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.other_known_packets, 2u);
    EXPECT_EQ(s.unknown_packets, 0u);
}

// A corrupted frame is rejected before routing: nothing is dispatched, and
// the invalid counter (fed the way esp_uart_link feeds it) increments.
TEST_F(InterMcuProtocolIntegrationTest, CorruptedCrcFrameIsRejectedBeforeDispatch) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);
    std::vector<uint8_t> frame = EncodeUartFrame(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    ASSERT_FALSE(frame.empty());

    frame[frame.size() - 3] ^= 0xFF;  // CRC low byte (CRC sits before END)
    EXPECT_FALSE(ValidateUartFrame(frame.data(), frame.size()));
    EXPECT_FALSE(ReceiveFrame(frame));

    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 0);
    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.invalid_packets, 1u);
    EXPECT_EQ(s.heartbeat_packets, 0u);
}

TEST_F(InterMcuProtocolIntegrationTest, TruncatedFrameIsRejectedBeforeDispatch) {
    HeartbeatMessage heartbeat(1000, 50, 100);
    std::vector<uint8_t> frame = EncodeUartFrame(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    ASSERT_FALSE(frame.empty());

    frame.resize(frame.size() / 2);  // e.g. interrupted mid-frame
    EXPECT_FALSE(ReceiveFrame(frame));
    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 0);
}

// An ACK frame parses fine but must not dispatch the typed handler - the
// router short-circuits on the flag.
TEST_F(InterMcuProtocolIntegrationTest, AckFrameDoesNotDispatchHandler) {
    HeartbeatMessage heartbeat(1000, 50, 100);
    ASSERT_TRUE(ReceiveFrame(
        EncodeUartFrame(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat), 5, UART_FLAG_ACK)));

    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 0);
    // But it is still counted as received traffic.
    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.total_packets, 1u);
}

// A realistic RX mix: every frame must land in its own counter and consumer,
// with values from the LAST frame of each type winning.
TEST_F(InterMcuProtocolIntegrationTest, MixedTrafficIsFullyAccounted) {
    for (uint32_t i = 1; i <= 5; ++i) {
        HeartbeatMessage hb(i * 1000, i, i * 10);
        ASSERT_TRUE(ReceiveFrame(
            EncodeUartFrame(MSG_HEARTBEAT, &hb, sizeof(hb), static_cast<uint16_t>(i))));
    }
    MeterPushMessage meter(100, 200, 300, 400);
    ASSERT_TRUE(ReceiveFrame(EncodeUartFrame(MSG_METER_PUSH, &meter, sizeof(meter), 6)));

    const auto& cap = GetInterMcuCapture();
    EXPECT_EQ(cap.heartbeat_calls, 5);
    EXPECT_EQ(cap.hb_uptime_ms, 5000u) << "last heartbeat must win";
    EXPECT_EQ(cap.meter_calls, 1);

    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.heartbeat_packets, 5u);
    EXPECT_EQ(s.meter_push_packets, 1u);
    EXPECT_EQ(s.total_packets, 6u);
    EXPECT_EQ(s.invalid_packets, 0u);
}

// Maximum-size payload survives the frame codec and reaches the consumer
// bit-for-bit (browse responses are the type that actually gets this large).
TEST_F(InterMcuProtocolIntegrationTest, MaximumPayloadReachesConsumerIntact) {
    std::vector<uint8_t> large_payload(UART_MAX_PAYLOAD);
    for (size_t i = 0; i < large_payload.size(); ++i) {
        large_payload[i] = static_cast<uint8_t>(i * 31 + 7);
    }

    ASSERT_TRUE(
        ReceiveFrame(EncodeUartFrame(MSG_BROWSE_RESP, large_payload.data(), large_payload.size())));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_resp_calls, 1);
    ASSERT_EQ(cap.browse_resp_data.size(), large_payload.size());
    EXPECT_EQ(memcmp(cap.browse_resp_data.data(), large_payload.data(), large_payload.size()), 0);
}

// An empty-payload frame is legal at the codec level; the router must drop it
// at the CopyMessage guard (payload smaller than SyncMessage) rather than
// dispatching a handler on garbage.
TEST_F(InterMcuProtocolIntegrationTest, EmptyPayloadFrameParsesButDoesNotDispatch) {
    std::vector<uint8_t> frame = EncodeUartFrame(MSG_SYNC, nullptr, 0);
    ASSERT_FALSE(frame.empty());
    EXPECT_EQ(frame.size(), UART_FRAME_OVERHEAD);

    EXPECT_TRUE(ReceiveFrame(frame));

    // Nothing crossed the boundary...
    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 0);
    EXPECT_EQ(GetInterMcuCapture().meter_calls, 0);
    // ...but the packet was still counted as received sync traffic.
    wavex_packet_stats_t s;
    stats_.get_packet_stats(&s);
    EXPECT_EQ(s.sync_packets, 1u);
}

}  // namespace
