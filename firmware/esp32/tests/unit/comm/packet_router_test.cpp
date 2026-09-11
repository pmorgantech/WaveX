// Tests for PacketRouter routing/dispatch.
//
// Observation points, in order of preference:
//  - The inter_mcu_* capture mocks (mocks/esp32_mocks.h): the REAL weak
//    handlers in packet_router.cpp run, including their unit conversions and
//    payload validation, and the capture records what crossed the
//    production/inter_mcu boundary.
//  - Strong overrides of the few handlers that only log (sync, error,
//    unknown): packet_router.cpp declares its handlers weak under
//    WAVEX_TEST_BUILD precisely so a test TU can pin dispatch for them.
//
// The previous version of this file defined extern "C" free functions named
// like the handlers; those never overrode the C++ member symbols, so its
// "handler called" flags could never become true and every EXPECT_FALSE on
// them passed vacuously.

#include "packet_router.h"

#include <gtest/gtest.h>

#include "../../mocks/esp32_mocks.h"
#include "../../utils/test_helpers.h"
#include "protocol.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace WaveX::Comm;
using namespace WaveX::Protocol;
using namespace WaveX::Test;

namespace {

// Capture state for the log-only handlers, which have no inter_mcu boundary
// to observe. Reset in SetUp.
struct HandlerCapture {
    int sync_calls = 0;
    uint32_t sync_timestamp = 0;
    int error_calls = 0;
    uint16_t error_code = 0;
    char error_msg[sizeof(ErrorMessage{}.msg) + 1] = {0};
    int unknown_calls = 0;
    uint8_t unknown_type = 0;
    size_t unknown_len = 0;
};

HandlerCapture g_handlers;

}  // namespace

// Strong definitions override the weak ones in packet_router.cpp at link time.
namespace WaveX {
namespace Comm {

void PacketRouter::handle_sync(const WaveX::Protocol::SyncMessage& msg) {
    g_handlers.sync_calls++;
    g_handlers.sync_timestamp = msg.timestamp_ms;
}

void PacketRouter::handle_error(const WaveX::Protocol::ErrorMessage& msg) {
    g_handlers.error_calls++;
    g_handlers.error_code = msg.code;
    memcpy(g_handlers.error_msg, msg.msg, sizeof(msg.msg));
    g_handlers.error_msg[sizeof(msg.msg)] = '\0';
}

void PacketRouter::handle_unknown_message(uint8_t type, const uint8_t* payload, size_t length) {
    (void)payload;
    g_handlers.unknown_calls++;
    g_handlers.unknown_type = type;
    g_handlers.unknown_len = length;
}

}  // namespace Comm
}  // namespace WaveX

class PacketRouterTest : public ::testing::Test {
   protected:
    void SetUp() override {
        router_ = std::make_unique<PacketRouter>();
        g_handlers = HandlerCapture{};
        ResetInterMcuCapture();
    }

    // Total dispatches observable through the inter_mcu capture plus the
    // strong-override handlers. Used to assert "nothing was dispatched".
    int TotalDispatches() const {
        const auto& cap = GetInterMcuCapture();
        return g_handlers.sync_calls + g_handlers.error_calls + g_handlers.unknown_calls +
               cap.instrument_map_calls + cap.seq_page_calls + cap.seq_playhead_calls +
               cap.heartbeat_calls + cap.meter_calls + cap.browse_resp_calls +
               cap.envelope_chunk_calls + cap.sample_status_calls + cap.inst_status_calls +
               cap.storage_status_calls + cap.stop_resp_calls + cap.diag_push_calls +
               cap.sample_meta_calls + cap.sample_mem_status_calls + cap.cv_cal_calls;
    }

    std::unique_ptr<PacketRouter> router_;
};

// A CRC-valid unified packet must reach the real heartbeat handler, which
// converts the x10 wire fields to percent before handing them to inter_mcu.
TEST_F(PacketRouterTest, RouteValidHeartbeatPacketReachesBackendWithConvertedCpu) {
    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_HEARTBEAT, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.heartbeat_calls, 1);
    EXPECT_EQ(cap.hb_uptime_ms, 1000u);
    EXPECT_EQ(cap.hb_rx_total, 5000u);
    EXPECT_EQ(cap.hb_loop_counter, 10000u);
    EXPECT_FLOAT_EQ(cap.hb_cpu_avg, 25.6f);
    EXPECT_FLOAT_EQ(cap.hb_cpu_min, 12.8f);
    EXPECT_FLOAT_EQ(cap.hb_cpu_max, 51.2f);
}

TEST_F(PacketRouterTest, RouteValidSyncPacketDispatchesTimestamp) {
    SyncMessage msg(0xCAFE1234);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_SYNC, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    EXPECT_EQ(g_handlers.sync_calls, 1);
    EXPECT_EQ(g_handlers.sync_timestamp, 0xCAFE1234u);
}

// Q15 wire values must arrive at the meter listener boundary as 0.0-1.0.
TEST_F(PacketRouterTest, RouteMeterPushConvertsQ15ToFloat) {
    MeterPushMessage msg(0x7FFF, 0x4000, 0x7FFF, 0x2000);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_METER_PUSH, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.meter_calls, 1);
    EXPECT_FLOAT_EQ(cap.meter_rms_left, 1.0f);
    EXPECT_FLOAT_EQ(cap.meter_rms_right, 0x4000 / 32767.0f);
    EXPECT_FLOAT_EQ(cap.meter_peak_left, 1.0f);
    EXPECT_FLOAT_EQ(cap.meter_peak_right, 0x2000 / 32767.0f);
}

// Browse responses are forwarded verbatim: header plus wire entries.
TEST_F(PacketRouterTest, RouteBrowseRespForwardsPayloadVerbatim) {
    std::vector<FileEntryWire> entries = {FileEntryWire(1, 0, "dir1"),
                                          FileEntryWire(0, 1024, "file1.wav")};
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateBrowseRespPacket(2, entries);
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_resp_calls, 1);
    // WaveX packets are size-class quantized (32..2048 bytes total), so the
    // delivered payload may carry zero padding after the real content.
    ASSERT_GE(cap.browse_resp_data.size(),
              sizeof(BrowseRespHeader) + entries.size() * sizeof(FileEntryWire));

    BrowseRespHeader header;
    memcpy(&header, cap.browse_resp_data.data(), sizeof(header));
    EXPECT_EQ(header.total_count, 2u);
    EXPECT_EQ(header.n, 2u);

    FileEntryWire first;
    memcpy(&first, cap.browse_resp_data.data() + sizeof(header), sizeof(first));
    EXPECT_EQ(first.is_dir, 1);
    EXPECT_STREQ(first.name, "dir1");
}

TEST_F(PacketRouterTest, RouteErrorMessageDeliversCodeAndBoundedText) {
    ErrorMessage msg(0x0042, "Test error");
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_ERROR, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    EXPECT_EQ(g_handlers.error_calls, 1);
    EXPECT_EQ(g_handlers.error_code, 0x0042);
    EXPECT_STREQ(g_handlers.error_msg, "Test error");
}

TEST_F(PacketRouterTest, RouteSampleStatusForwardsAllFields) {
    SampleStatusMessage msg(7, 1, 2, 48000, 12345);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_SAMPLE_STATUS, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.sample_status_calls, 1);
    EXPECT_EQ(cap.sample_status_id, 7);
    EXPECT_EQ(cap.sample_status_state, 1);
    EXPECT_EQ(cap.sample_status_channels, 2);
    EXPECT_EQ(cap.sample_status_rate, 48000u);
    EXPECT_EQ(cap.sample_status_frames, 12345u);
}

TEST_F(PacketRouterTest, RouteInstrumentStatusForwardsInspectionAndProgressFields) {
    InstStatusMessage msg;
    msg.request_id = 41;
    msg.op = INST_OP_SFZ_LOAD;
    msg.state = INST_STATUS_LOAD_PROGRESS;
    msg.total_bytes = 9000;
    msg.loaded_bytes = 3000;
    msg.current_bytes = 2000;
    msg.current_loaded_bytes = 1000;
    std::strcpy(msg.current_name, "soft.wav");
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_INST_STATUS, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.inst_status_calls, 1);
    EXPECT_EQ(cap.last_inst_status.request_id, 41u);
    EXPECT_EQ(cap.last_inst_status.loaded_bytes, 3000u);
    EXPECT_STREQ(cap.last_inst_status.current_name, "soft.wav");
}

TEST_F(PacketRouterTest, RouteStorageStatusConvertsWireByteToBool) {
    StorageStatusMessage mounted(1);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_STORAGE_STATUS, &mounted, sizeof(mounted));
    router_->route_packet(packet.data(), packet.size());

    StorageStatusMessage unmounted(0);
    std::vector<uint8_t> packet2 =
        ProtocolTestHelper::CreateWaveXPacket(MSG_STORAGE_STATUS, &unmounted, sizeof(unmounted));

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.storage_status_calls, 1);
    EXPECT_TRUE(cap.storage_status_mounted);

    router_->route_packet(packet2.data(), packet2.size());
    ASSERT_EQ(cap.storage_status_calls, 2);
    EXPECT_FALSE(cap.storage_status_mounted);
}

// handle_sample_stop_resp turns success==1 into true and anything else into
// false; both directions must survive the trip.
TEST_F(PacketRouterTest, RouteSampleStopRespConvertsSuccessByte) {
    SampleStopRespMessage ok(1);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_SAMPLE_STOP_RESP, &ok, sizeof(ok));
    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.stop_resp_calls, 1);
    EXPECT_TRUE(cap.stop_resp_success);

    SampleStopRespMessage failed(0);
    packet = ProtocolTestHelper::CreateWaveXPacket(MSG_SAMPLE_STOP_RESP, &failed, sizeof(failed));
    router_->route_packet(packet.data(), packet.size());
    ASSERT_EQ(cap.stop_resp_calls, 2);
    EXPECT_FALSE(cap.stop_resp_success);
}

// Valid envelope chunk: header + columns reach the cache callback intact.
TEST_F(PacketRouterTest, RouteEnvelopeChunkDeliversColumns) {
    constexpr uint16_t kColumns = 3;
    struct {
        EnvelopeChunkMessage header;
        EnvelopeColumn8 columns[kColumns];
    } __attribute__((packed)) msg;
    msg.header.sample_id = 5;
    msg.header.generation = 2;
    msg.header.start_frame = 0;
    msg.header.end_frame = 3000;
    msg.header.total_columns = kColumns;
    msg.header.first_column = 0;
    msg.header.columns = kColumns;
    msg.header.channels = 1;
    msg.columns[0] = EnvelopeColumn8{-10, 10};
    msg.columns[1] = EnvelopeColumn8{-20, 20};
    msg.columns[2] = EnvelopeColumn8{-30, 30};

    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_ENVELOPE_CHUNK, &msg, sizeof(msg));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.envelope_chunk_calls, 1);
    EXPECT_EQ(cap.envelope_header.sample_id, 5);
    EXPECT_EQ(cap.envelope_header.generation, 2);
    ASSERT_EQ(cap.envelope_columns.size(), 3u);
    EXPECT_EQ(cap.envelope_columns[1].min_sample, -5120);
    EXPECT_EQ(cap.envelope_columns[2].max_sample, 7740);
}

// Malformed envelope chunks (bad channel count, or a payload shorter than
// columns * channels implies) must be rejected by the handler's validation.
TEST_F(PacketRouterTest, RouteEnvelopeChunkMalformedIsDropped) {
    struct {
        EnvelopeChunkMessage header;
        EnvelopeColumn8 columns[2];
    } __attribute__((packed)) msg;
    msg.header.sample_id = 5;
    msg.header.total_columns = 2;
    msg.header.first_column = 0;
    msg.header.columns = 2;
    msg.columns[0] = EnvelopeColumn8{-1, 1};
    msg.columns[1] = EnvelopeColumn8{-2, 2};

    // channels == 0
    msg.header.channels = 0;
    auto packet = ProtocolTestHelper::CreateWaveXPacket(MSG_ENVELOPE_CHUNK, &msg, sizeof(msg));
    router_->route_packet(packet.data(), packet.size());
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);

    // channels > 2
    msg.header.channels = 3;
    packet = ProtocolTestHelper::CreateWaveXPacket(MSG_ENVELOPE_CHUNK, &msg, sizeof(msg));
    router_->route_packet(packet.data(), packet.size());
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);

    // stereo claim with mono-sized payload: 2 columns * 2 channels needs 4
    // EnvelopeColumn values but only 2 are present. Routed through the UART
    // entry point because the unified-packet path pads payloads up to the
    // packet size class, which would mask the truncation.
    msg.header.channels = 2;
    router_->route_uart_message(
        MSG_ENVELOPE_CHUNK, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 0, 1);
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);
}

TEST_F(PacketRouterTest, RouteInvalidPacketTooSmall) {
    std::vector<uint8_t> small_packet = {0x00, 0x01, 0x02};
    router_->route_packet(small_packet.data(), small_packet.size());
    EXPECT_EQ(TotalDispatches(), 0);
}

TEST_F(PacketRouterTest, RouteInvalidPacketCorruptedCRC) {
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateHeartbeatPacket(1000, 5000, 10000);
    packet[packet.size() - 2] ^= 0xFF;
    packet[packet.size() - 1] ^= 0xFF;

    router_->route_packet(packet.data(), packet.size());
    EXPECT_EQ(TotalDispatches(), 0);
}

// route_uart_message is the entry point the UART RX task actually uses; the
// already-parsed payload must dispatch identically to the unified-packet path.
TEST_F(PacketRouterTest, RouteUartMessageDispatchesHeartbeat) {
    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    router_->route_uart_message(
        MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 0, 0x1234);

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.heartbeat_calls, 1);
    EXPECT_EQ(cap.hb_uptime_ms, 1000u);
    EXPECT_FLOAT_EQ(cap.hb_cpu_avg, 25.6f);
}

// Unknown message types must go to handle_unknown_message with the original
// type and payload length, and nowhere else.
TEST_F(PacketRouterTest, RouteUnknownMessage) {
    uint8_t unknown_payload[] = {0x01, 0x02, 0x03};
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(0xEE, unknown_payload, sizeof(unknown_payload));
    ASSERT_FALSE(packet.empty());

    router_->route_packet(packet.data(), packet.size());

    EXPECT_EQ(g_handlers.unknown_calls, 1);
    EXPECT_EQ(g_handlers.unknown_type, 0xEE);
    // Size-class padding: the delivered length is at least the payload sent.
    EXPECT_GE(g_handlers.unknown_len, sizeof(unknown_payload));
    EXPECT_EQ(TotalDispatches(), 1);
}

// ACK/NACK flags short-circuit routing: the typed handler must NOT run, but
// the stats callback still counts the packet.
TEST_F(PacketRouterTest, AckFlagSuppressesHandlerButCountsPacket) {
    int stats_calls = 0;
    uint8_t stats_type = 0;
    router_->set_stats_callback([&](uint8_t type) {
        stats_calls++;
        stats_type = type;
    });

    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateWaveXPacket(
        MSG_HEARTBEAT, &msg, sizeof(msg), 0x1234, PKT_FLAG_ACK);
    router_->route_packet(packet.data(), packet.size());

    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 0);
    EXPECT_EQ(stats_calls, 1);
    EXPECT_EQ(stats_type, MSG_HEARTBEAT);
}

TEST_F(PacketRouterTest, NackFlagSuppressesHandler) {
    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateWaveXPacket(
        MSG_HEARTBEAT, &msg, sizeof(msg), 0x1234, PKT_FLAG_NACK);
    router_->route_packet(packet.data(), packet.size());

    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 0);
    EXPECT_EQ(TotalDispatches(), 0);
}

// The stats callback fires for every routed message, including ones the
// router has no handler for - that is what keeps the UNKNOWN counter honest.
TEST_F(PacketRouterTest, StatisticsCallbackFiresForRoutedAndUnknownTypes) {
    std::vector<uint8_t> seen;
    router_->set_stats_callback([&](uint8_t type) { seen.push_back(type); });

    HeartbeatMessage msg(1000, 5000, 10000);
    router_->route_uart_message(
        MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 0, 1);
    uint8_t junk[4] = {0};
    router_->route_uart_message(0xEE, junk, sizeof(junk), 0, 2);

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], MSG_HEARTBEAT);
    EXPECT_EQ(seen[1], 0xEE);
}

TEST_F(PacketRouterTest, RouteNullPacket) {
    router_->route_packet(nullptr, 0);
    EXPECT_EQ(TotalDispatches(), 0);
}

TEST_F(PacketRouterTest, RouteEmptyPacket) {
    std::vector<uint8_t> empty_packet;
    router_->route_packet(empty_packet.data(), 0);
    EXPECT_EQ(TotalDispatches(), 0);
}

// Review H3 regression tests: a CRC-valid frame whose payload is empty or
// shorter than the typed message must be dropped, not memcpy'd. Before the
// CopyMessage guard, a zero-length payload reached
// memcpy(&msg, nullptr, sizeof(msg)) - undefined behavior (segfault here on
// the host, arbitrary misbehavior on target).
TEST_F(PacketRouterTest, NullPayloadIsDroppedForEveryFixedSizeType) {
    const uint8_t types[] = {MSG_SYNC,
                             MSG_HEARTBEAT,
                             MSG_METER_PUSH,
                             MSG_STATUS_RESPONSE,
                             MSG_ENVELOPE_CHUNK,
                             MSG_SAMPLE_STATUS,
                             MSG_STORAGE_STATUS,
                             MSG_SAMPLE_META,
                             MSG_DIAG_PUSH,
                             MSG_SAMPLE_STOP_RESP,
                             MSG_CV_CAL_RESP,
                             MSG_ERROR};
    for (uint8_t t: types) {
        router_->route_uart_message(t, nullptr, 0, 0, 1);
    }
    EXPECT_EQ(TotalDispatches(), 0) << "a handler ran on an empty payload";

    // The router must still be functional afterwards.
    HeartbeatMessage hb(1000, 1, 1);
    router_->route_uart_message(
        MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&hb), sizeof(hb), 0, 2);
    EXPECT_EQ(GetInterMcuCapture().heartbeat_calls, 1);
}

TEST_F(PacketRouterTest, TruncatedPayloadIsDroppedForEveryFixedSizeType) {
    uint8_t partial[3] = {0x01, 0x02, 0x03};
    const uint8_t types[] = {MSG_SYNC,
                             MSG_HEARTBEAT,
                             MSG_METER_PUSH,
                             MSG_STATUS_RESPONSE,
                             MSG_ENVELOPE_CHUNK,
                             MSG_SAMPLE_STATUS,
                             MSG_STORAGE_STATUS,
                             MSG_SAMPLE_META,
                             MSG_DIAG_PUSH,
                             MSG_SAMPLE_STOP_RESP,
                             MSG_CV_CAL_RESP,
                             MSG_ERROR};
    for (uint8_t t: types) {
        router_->route_uart_message(t, partial, sizeof(partial), 0, 1);
    }
    EXPECT_EQ(TotalDispatches(), 0) << "a handler ran on a truncated payload";
}

// The error text field is NOT guaranteed NUL-terminated on the wire; the
// handler must bound its copy instead of running off the struct. Send a
// payload whose msg[] is entirely non-zero.
TEST_F(PacketRouterTest, ErrorMessageWithUnterminatedTextIsBounded) {
    ErrorMessage msg;
    msg.code = 0x0007;
    memset(msg.msg, 'A', sizeof(msg.msg));  // no terminator anywhere

    router_->route_uart_message(
        MSG_ERROR, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 0, 1);

    ASSERT_EQ(g_handlers.error_calls, 1);
    EXPECT_EQ(g_handlers.error_code, 0x0007);
    EXPECT_EQ(strlen(g_handlers.error_msg), sizeof(msg.msg));
}

TEST_F(PacketRouterTest, CompactStereoEnvelopeDecodesAnExactSizedUartPayload) {
    EnvelopeChunkMessage h;
    h.sample_id = 513;
    h.generation = 91;
    h.start_frame = 103;
    h.end_frame = 70000;
    h.total_columns = 1140;
    h.first_column = 64;
    h.columns = 2;
    h.channels = 2;
    const EnvelopeColumn8 data[] = {{-128, 127}, {0, 0}, {-1, 1}, {-64, 63}};
    std::vector<uint8_t> payload(sizeof(h) + sizeof(data));
    memcpy(payload.data(), &h, sizeof(h));
    memcpy(payload.data() + sizeof(h), data, sizeof(data));
    router_->route_uart_message(MSG_ENVELOPE_CHUNK, payload.data(), payload.size(), 0, 1);
    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.envelope_chunk_calls, 1);
    EXPECT_EQ(cap.envelope_header.sample_id, 513);
    EXPECT_EQ(cap.envelope_header.generation, 91);
    EXPECT_EQ(cap.envelope_header.start_frame, 103u);
    EXPECT_EQ(cap.envelope_header.end_frame, 70000u);
    EXPECT_EQ(cap.envelope_header.total_columns, 1140);
    EXPECT_EQ(cap.envelope_header.first_column, 64);
    EXPECT_EQ(cap.envelope_header.columns, 2);
    EXPECT_EQ(cap.envelope_header.channels, 2);
    EXPECT_EQ(cap.envelope_header.encoding, ENVELOPE_ENCODING_S8);
    ASSERT_EQ(cap.envelope_columns.size(), 4u);
    EXPECT_EQ(cap.envelope_columns[0].min_sample, INT16_MIN);
    EXPECT_EQ(cap.envelope_columns[0].max_sample, INT16_MAX);
    EXPECT_EQ(cap.envelope_columns[1].min_sample, 0);
    EXPECT_EQ(cap.envelope_columns[1].max_sample, 0);
    EXPECT_EQ(cap.envelope_columns[2].min_sample, -256);
    EXPECT_EQ(cap.envelope_columns[2].max_sample, 258);
    EXPECT_EQ(cap.envelope_columns[3].min_sample, -16384);
    EXPECT_EQ(cap.envelope_columns[3].max_sample, 16254);
}

TEST_F(PacketRouterTest, CompactEnvelopeRejectsEveryTruncationAndOldEncoding) {
    EnvelopeChunkMessage h;
    h.total_columns = h.columns = 2;
    h.channels = 2;
    const EnvelopeColumn8 data[] = {{-128, 127}, {0, 0}, {-1, 1}, {-64, 63}};
    std::vector<uint8_t> complete(sizeof(h) + sizeof(data));
    memcpy(complete.data(), &h, sizeof(h));
    memcpy(complete.data() + sizeof(h), data, sizeof(data));
    for (size_t length = 0; length < complete.size(); ++length) {
        const std::vector<uint8_t> truncated(complete.begin(), complete.begin() + length);
        router_->route_uart_message(MSG_ENVELOPE_CHUNK, truncated.data(), length, 0, 1);
    }
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);
    for (uint8_t encoding: {uint8_t{0}, uint8_t{255}}) {
        h.encoding = encoding;
        memcpy(complete.data(), &h, sizeof(h));
        router_->route_uart_message(MSG_ENVELOPE_CHUNK, complete.data(), complete.size(), 0, 1);
    }
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);
    h.encoding = ENVELOPE_ENCODING_S8;
    h.first_column = 1;  // past the end of the run
    memcpy(complete.data(), &h, sizeof(h));
    router_->route_uart_message(MSG_ENVELOPE_CHUNK, complete.data(), complete.size(), 0, 1);
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);
    h.first_column = 0;
    memcpy(complete.data(), &h, sizeof(h));
    complete[sizeof(h)] = 127;
    complete[sizeof(h) + 1] = 0;  // minimum greater than maximum
    router_->route_uart_message(MSG_ENVELOPE_CHUNK, complete.data(), complete.size(), 0, 1);
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 0);
}

TEST_F(PacketRouterTest, CompactEnvelopeAdmitsMaximumChunkButRejectsAnExtraValue) {
    EnvelopeChunkMessage h;
    h.channels = 2;
    h.total_columns = 1280;
    h.columns = MAX_ENVELOPE_CHUNK_VALUES / 2;
    std::vector<uint8_t> payload(sizeof(h) + MAX_ENVELOPE_CHUNK_VALUES * sizeof(EnvelopeColumn8));
    memcpy(payload.data(), &h, sizeof(h));
    router_->route_uart_message(MSG_ENVELOPE_CHUNK, payload.data(), payload.size(), 0, 1);
    ASSERT_EQ(GetInterMcuCapture().envelope_chunk_calls, 1);
    EXPECT_EQ(GetInterMcuCapture().envelope_columns.size(), MAX_ENVELOPE_CHUNK_VALUES);
    ++h.columns;
    payload.resize(sizeof(h) + static_cast<size_t>(h.columns) * 2 * sizeof(EnvelopeColumn8));
    memcpy(payload.data(), &h, sizeof(h));
    router_->route_uart_message(MSG_ENVELOPE_CHUNK, payload.data(), payload.size(), 0, 2);
    EXPECT_EQ(GetInterMcuCapture().envelope_chunk_calls, 1);
}

TEST_F(PacketRouterTest, SequencerReadbackRoutesCompleteValuesAndRejectsEveryTruncation) {
    SeqPatternSyncMessage page;
    page.request_id = 0xABCDEF12;
    page.track = 15;
    page.first_step = 48;
    page.valid = 1;
    page.length = 64;
    page.steps[15].on = 1;
    page.steps[15].velocity = 119;
    page.steps[15].micro_offset = -17;
    for (size_t len = 0; len < sizeof(page); ++len) {
        std::vector<uint8_t> exact(len, 0);
        router_->route_uart_message(MSG_SEQ_PATTERN_SYNC, exact.data(), exact.size(), 0, 1);
    }
    EXPECT_EQ(GetInterMcuCapture().seq_page_calls, 0);
    router_->route_uart_message(
        MSG_SEQ_PATTERN_SYNC, reinterpret_cast<const uint8_t*>(&page), sizeof(page), 0, 2);
    ASSERT_EQ(GetInterMcuCapture().seq_page_calls, 1);
    const auto& got = GetInterMcuCapture().last_seq_page;
    EXPECT_EQ(got.request_id, page.request_id);
    EXPECT_EQ(got.track, 15);
    EXPECT_EQ(got.first_step, 48);
    EXPECT_EQ(got.steps[15].velocity, 119);
    EXPECT_EQ(got.steps[15].micro_offset, -17);
}

TEST_F(PacketRouterTest, SequencerPlayheadRoutesToSnapshotBoundary) {
    SeqPlayheadMessage head(0, 63, 1, 2, 13925, 0x12345678);
    for (size_t len = 0; len < sizeof(head); ++len) {
        std::vector<uint8_t> exact(len, 0);
        router_->route_uart_message(MSG_SEQ_PLAYHEAD, exact.data(), exact.size(), 0, 1);
    }
    EXPECT_EQ(GetInterMcuCapture().seq_playhead_calls, 0);
    router_->route_uart_message(
        MSG_SEQ_PLAYHEAD, reinterpret_cast<const uint8_t*>(&head), sizeof(head), 0, 2);
    ASSERT_EQ(GetInterMcuCapture().seq_playhead_calls, 1);
    EXPECT_EQ(GetInterMcuCapture().last_seq_playhead.step, 63);
    EXPECT_EQ(GetInterMcuCapture().last_seq_playhead.loop_count, 0x12345678u);
}

TEST_F(PacketRouterTest, InstrumentMapReadbackRejectsEveryTruncation) {
    InstZoneSyncMessage message;
    message.request_id = 123;
    message.track = 15;
    message.pads[15] = {65530, 7, 75};
    for (size_t size = 0; size < sizeof(message); ++size)
        router_->route_uart_message(
            MSG_INST_ZONE_SYNC, reinterpret_cast<const uint8_t*>(&message), size, 0, 1);
    EXPECT_EQ(GetInterMcuCapture().instrument_map_calls, 0);
    router_->route_uart_message(
        MSG_INST_ZONE_SYNC, reinterpret_cast<const uint8_t*>(&message), sizeof(message), 0, 2);
    ASSERT_EQ(GetInterMcuCapture().instrument_map_calls, 1);
    EXPECT_EQ(GetInterMcuCapture().instrument_map.pads[15].sample_id, 65530);
    EXPECT_EQ(GetInterMcuCapture().instrument_map.pads[15].choke_group, 7);
}

TEST_F(PacketRouterTest, PadSoundReadbackRejectsTruncationAndRoutesIdentity) {
    InstPadSoundSyncMessage message{123, 122, 15, 15, 1, 0, 0, 1, 25, 1200, 2, 75, 450};
    for (size_t size = 0; size < sizeof(message); ++size)
        router_->route_uart_message(
            MSG_INST_PAD_SOUND_SYNC, reinterpret_cast<const uint8_t*>(&message), size, 0, 1);
    EXPECT_EQ(GetInterMcuCapture().pad_sound_calls, 0);
    router_->route_uart_message(
        MSG_INST_PAD_SOUND_SYNC, reinterpret_cast<const uint8_t*>(&message), sizeof(message), 0, 2);
    ASSERT_EQ(GetInterMcuCapture().pad_sound_calls, 1);
    EXPECT_EQ(GetInterMcuCapture().pad_sound.pad, 15);
    EXPECT_EQ(GetInterMcuCapture().pad_sound.cutoff_hz, 1200);
}

TEST_F(PacketRouterTest, TrackReadbackRejectsTruncationAndRoutesIdentity) {
    TrackStateMessage message{};
    message.request_id = 123;
    message.track = 15;
    message.valid = 1;
    message.midi_in = 255;
    for (size_t size = 0; size < sizeof(message); ++size)
        router_->route_uart_message(
            MSG_TRACK_STATE, reinterpret_cast<const uint8_t*>(&message), size, 0, 1);
    EXPECT_EQ(GetInterMcuCapture().track_state_calls, 0);
    router_->route_uart_message(
        MSG_TRACK_STATE, reinterpret_cast<const uint8_t*>(&message), sizeof(message), 0, 2);
    ASSERT_EQ(GetInterMcuCapture().track_state_calls, 1);
    EXPECT_EQ(GetInterMcuCapture().track_state.request_id, 123u);
    EXPECT_EQ(GetInterMcuCapture().track_state.midi_in, 255);
}
