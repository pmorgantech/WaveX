#include <gtest/gtest.h>

#include "../utils/test_helpers.h"
#include "protocol.h"

using namespace WaveX::Protocol;
using namespace WaveX::Test;

class MessageTypeTest : public ::testing::Test {
   protected:
    void SetUp() override { buffer_.resize(4096); }

    std::vector<uint8_t> buffer_;
};

// Test ControlChangeMessage creation and parsing
TEST_F(MessageTypeTest, ControlChangeMessage) {
    ControlChangeMessage original = {PARAM_VOLUME, 0, 0x7FFF};

    size_t created = ProtocolHandler::CreateControlChangePacket(
        buffer_.data(), buffer_.size(), original.parameter, original.channel, original.value);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    ControlChangeMessage parsed;
    bool result = ProtocolHandler::ParseControlChange(buffer_.data(), parsed);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.parameter, original.parameter);
    EXPECT_EQ(parsed.channel, original.channel);
    EXPECT_EQ(parsed.value, original.value);
}

// Test NoteMessage creation and parsing
TEST_F(MessageTypeTest, NoteMessage) {
    NoteMessage original = {60, 127, 0, 0};  // Middle C, max velocity

    size_t created_on = ProtocolHandler::CreateNoteOnPacket(
        buffer_.data(), buffer_.size(), original.note, original.velocity, original.channel);

    ASSERT_GT(created_on, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_on));

    NoteMessage parsed;
    bool result = ProtocolHandler::ParseNoteMessage(buffer_.data(), parsed);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.note, original.note);
    EXPECT_EQ(parsed.velocity, original.velocity);
    EXPECT_EQ(parsed.channel, original.channel);

    // Test NoteOff
    size_t created_off = ProtocolHandler::CreateNoteOffPacket(
        buffer_.data(), buffer_.size(), original.note, original.channel);

    ASSERT_GT(created_off, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_off));
}

// Test SampleCtrlMessage creation and parsing
TEST_F(MessageTypeTest, SampleCtrlMessage) {
    SampleCtrlMessage original = {0, SAMPLE_PLAY_START, 1.0f};

    size_t created =
        ProtocolHandler::CreateSampleCtrlPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    SampleCtrlMessage parsed;
    bool result = ProtocolHandler::ParseSampleCtrl(buffer_.data(), parsed);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.slot, original.slot);
    EXPECT_EQ(parsed.cmd, original.cmd);
    EXPECT_FLOAT_EQ(parsed.rate, original.rate);
}

// Test PreviewReqMessage creation and parsing
TEST_F(MessageTypeTest, PreviewReqMessage) {
    PreviewReqMessage original = {0, 0, 1000, 4};

    size_t created =
        ProtocolHandler::CreatePreviewReqPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    PreviewReqMessage parsed;
    bool result = ProtocolHandler::ParsePreviewReq(buffer_.data(), parsed);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.slot, original.slot);
    EXPECT_EQ(parsed.start, original.start);
    EXPECT_EQ(parsed.end, original.end);
    EXPECT_EQ(parsed.decim, original.decim);
}

// Test MeterPushMessage creation
TEST_F(MessageTypeTest, MeterPushMessage) {
    MeterPushMessage original = {0x7FFF, 0x4000, 0x7FFF, 0x4000};

    size_t created =
        ProtocolHandler::CreateMeterPushPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_METER_PUSH);
}

// Test WaveChunkMessage creation
TEST_F(MessageTypeTest, WaveChunkMessage) {
    WaveChunkMessage header = {0, 128};
    std::vector<int16_t> samples(128, 0x7FFF);

    size_t created = ProtocolHandler::CreateWaveChunkPacket(
        buffer_.data(), buffer_.size(), header, samples.data(), samples.size() * sizeof(int16_t));

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_WAVE_CHUNK);
}

// Test HeartbeatMessage creation and parsing
TEST_F(MessageTypeTest, HeartbeatMessage) {
    HeartbeatMessage original = {1000, 5000, 10000, 256, 128, 512};

    size_t created =
        ProtocolHandler::CreateHeartbeatPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    HeartbeatMessage parsed;
    bool result = ProtocolHandler::ParseMessage(buffer_.data(), parsed);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.uptime_ms, original.uptime_ms);
    EXPECT_EQ(parsed.rx_total, original.rx_total);
    EXPECT_EQ(parsed.loop_counter, original.loop_counter);
}

// Test BrowseReq parsing
TEST_F(MessageTypeTest, BrowseReqMessage) {
    char path[96] = "/samples";
    uint32_t start_index = 0;
    uint8_t max_entries = 20;

    // ParseBrowseReq expects format: path (null-terminated) + start_index (4 bytes) + max_entries
    // (1 byte)
    std::vector<uint8_t> payload;
    payload.reserve(strlen(path) + 1 + 4 + 1);
    payload.insert(payload.end(), path, path + strlen(path));
    payload.push_back('\0');  // null terminator
    payload.push_back(static_cast<uint8_t>(start_index & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 24) & 0xFF));
    payload.push_back(max_entries);

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_BROWSE_REQ, payload.data(), payload.size());

    ASSERT_GT(created, 0);

    char parsed_path[96];
    uint32_t parsed_start_index;
    uint8_t parsed_max_entries;

    // ParseBrowseReq expects the payload directly, not the full packet
    // Extract payload from packet first
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    std::vector<uint8_t> parsed_payload(payload.size());
    size_t parsed_payload_size = parsed_payload.size();

    bool parse_result = ProtocolHandler::ParseWaveXPacket(
        buffer_.data(), created, msg_type, parsed_payload.data(), parsed_payload_size, seq, flags);

    ASSERT_TRUE(parse_result);
    ASSERT_EQ(msg_type, MSG_BROWSE_REQ);

    // Now parse the browse request from the payload
    bool result = ProtocolHandler::ParseBrowseReq(parsed_payload.data(),
                                                  parsed_path,
                                                  sizeof(parsed_path),
                                                  parsed_start_index,
                                                  parsed_max_entries);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed_start_index, start_index);
    EXPECT_EQ(parsed_max_entries, max_entries);
    EXPECT_STREQ(parsed_path, path);
}

// Test BrowseResp creation
TEST_F(MessageTypeTest, BrowseRespMessage) {
    FileEntryWire entries[3] = {{1, 0, "dir1"}, {0, 1024, "file1.wav"}, {0, 2048, "file2.wav"}};

    size_t created =
        ProtocolHandler::CreateBrowseRespPacket(buffer_.data(), buffer_.size(), 3, entries, 3);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_BROWSE_RESP);
}

// Test SampleStatusMessage creation
TEST_F(MessageTypeTest, SampleStatusMessage) {
    SampleStatusMessage original = {1, 1, 2, 48000, 1000};

    size_t created =
        ProtocolHandler::CreateSampleStatusPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_STATUS);
}

// Test SampleStopReq/Resp messages
TEST_F(MessageTypeTest, SampleStopMessages) {
    SampleStopReqMessage req = {0, {0}};

    size_t created_req =
        ProtocolHandler::CreateSampleStopReqPacket(buffer_.data(), buffer_.size(), req);

    ASSERT_GT(created_req, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_req));

    SampleStopRespMessage resp = {1, {0}};

    size_t created_resp =
        ProtocolHandler::CreateSampleStopRespPacket(buffer_.data(), buffer_.size(), resp);

    ASSERT_GT(created_resp, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_resp));
}

// Test ErrorMessage creation
TEST_F(MessageTypeTest, ErrorMessage) {
    ErrorMessage original = {0x0001, "Test error message"};

    size_t created = ProtocolHandler::CreateErrorPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_ERROR);
}

// Test SyncMessage creation
TEST_F(MessageTypeTest, SyncMessage) {
    SyncMessage original = {1000, {0}};

    size_t created = ProtocolHandler::CreateSyncPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SYNC);
}

// Test AckMessage creation
TEST_F(MessageTypeTest, AckMessage) {
    AckMessage original = {0x1234};

    size_t created = ProtocolHandler::CreateAckPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_ACK);
}

// Test SamplePlayIndexMessage creation
TEST_F(MessageTypeTest, SamplePlayIndexMessage) {
    SamplePlayIndexMessage original = {5};

    size_t created =
        ProtocolHandler::CreateSamplePlayIndexPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_PLAY_INDEX_REQ);
}

// Test SampleGetPathMessage creation
TEST_F(MessageTypeTest, SampleGetPathMessage) {
    SampleGetPathMessage original = {10};

    size_t created =
        ProtocolHandler::CreateSampleGetPathPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_GET_PATH_REQ);
}

// Test SamplePathResponseMessage creation
TEST_F(MessageTypeTest, SamplePathResponseMessage) {
    SamplePathResponseMessage original = {10, "/samples/test.wav"};

    size_t created =
        ProtocolHandler::CreateSamplePathResponsePacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_GET_PATH_RESP);
}

// Test GetMessageType
TEST_F(MessageTypeTest, GetMessageType) {
    MessageType types[] = {
        MSG_SYNC, MSG_CONTROL_CHANGE, MSG_NOTE_ON, MSG_HEARTBEAT, MSG_BROWSE_REQ, MSG_BROWSE_RESP};

    for (auto type: types) {
        size_t created = ProtocolHandler::CreatePacket(
            buffer_.data(), buffer_.size(), static_cast<uint8_t>(type), nullptr, 0);

        ASSERT_GT(created, 0);
        EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), type);
    }
}

// Test ValidatePacket with invalid packets
TEST_F(MessageTypeTest, ValidatePacketInvalid) {
    // Too small
    EXPECT_FALSE(ProtocolHandler::ValidatePacket(buffer_.data(), 4));

    // Invalid CRC
    size_t created =
        ProtocolHandler::CreatePacket(buffer_.data(), buffer_.size(), MSG_HEARTBEAT, nullptr, 0);

    ASSERT_GT(created, 0);
    buffer_[created - 2] ^= 0xFF;  // Corrupt CRC
    EXPECT_FALSE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
}

// Test packet size optimization
TEST_F(MessageTypeTest, PacketSizeOptimization) {
    // Test GetOptimalSizeCode
    EXPECT_EQ(ProtocolHandler::GetOptimalSizeCode(10), PKT_SIZE_32);
    EXPECT_EQ(ProtocolHandler::GetOptimalSizeCode(50), PKT_SIZE_64);
    EXPECT_EQ(ProtocolHandler::GetOptimalSizeCode(100), PKT_SIZE_128);
    EXPECT_EQ(ProtocolHandler::GetOptimalSizeCode(200), PKT_SIZE_256);
    EXPECT_EQ(ProtocolHandler::GetOptimalSizeCode(500), PKT_SIZE_512);
    EXPECT_EQ(ProtocolHandler::GetOptimalSizeCode(1000), PKT_SIZE_1024);

    // Test GetPacketSizeFromCode
    EXPECT_EQ(ProtocolHandler::GetPacketSizeFromCode(PKT_SIZE_32), 32);
    EXPECT_EQ(ProtocolHandler::GetPacketSizeFromCode(PKT_SIZE_64), 64);
    EXPECT_EQ(ProtocolHandler::GetPacketSizeFromCode(PKT_SIZE_128), 128);
    EXPECT_EQ(ProtocolHandler::GetPacketSizeFromCode(PKT_SIZE_256), 256);
    EXPECT_EQ(ProtocolHandler::GetPacketSizeFromCode(PKT_SIZE_512), 512);
    EXPECT_EQ(ProtocolHandler::GetPacketSizeFromCode(PKT_SIZE_1024), 1024);
}

// Test ParseWaveXPacket
TEST_F(MessageTypeTest, ParseWaveXPacket) {
    uint8_t msg_type = MSG_HEARTBEAT;
    uint16_t sequence = 0x1234;
    uint8_t flags = 0;

    HeartbeatMessage payload = {1000, 5000, 10000, 256, 128, 512};

    size_t created = ProtocolHandler::CreateWaveXPacket(
        buffer_.data(), buffer_.size(), msg_type, &payload, sizeof(payload), sequence, flags);

    ASSERT_GT(created, 0);

    uint8_t parsed_type;
    uint16_t parsed_seq;
    uint8_t parsed_flags;
    std::vector<uint8_t> parsed_payload(256);  // Make sure it's large enough
    size_t parsed_payload_size = parsed_payload.size();

    bool result = ProtocolHandler::ParseWaveXPacket(buffer_.data(),
                                                    created,
                                                    parsed_type,
                                                    parsed_payload.data(),
                                                    parsed_payload_size,
                                                    parsed_seq,
                                                    parsed_flags);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed_type, msg_type);
    EXPECT_EQ(parsed_seq, sequence);
    EXPECT_EQ(parsed_flags, flags);

    // Verify the actual payload data matches (packet may have padding)
    // The parsed_payload_size will be the packet payload size (which includes padding)
    // but the actual data should match our original payload
    EXPECT_GE(parsed_payload_size, sizeof(payload));
    HeartbeatMessage* parsed_heartbeat = reinterpret_cast<HeartbeatMessage*>(parsed_payload.data());
    EXPECT_EQ(parsed_heartbeat->uptime_ms, payload.uptime_ms);
    EXPECT_EQ(parsed_heartbeat->rx_total, payload.rx_total);
    EXPECT_EQ(parsed_heartbeat->loop_counter, payload.loop_counter);
}

// Test CalculateWaveXCrc consistency
TEST_F(MessageTypeTest, CalculateWaveXCrc) {
    const uint8_t test_data[] = "Test data for CRC";
    size_t data_len = sizeof(test_data) - 1;

    uint16_t crc1 = ProtocolHandler::CalculateWaveXCrc(test_data, data_len);
    uint16_t crc2 = ProtocolHandler::CalculateWaveXCrc(test_data, data_len);

    EXPECT_EQ(crc1, crc2);

    // Different data should produce different CRC
    const uint8_t test_data2[] = "Different data";
    uint16_t crc3 = ProtocolHandler::CalculateWaveXCrc(test_data2, sizeof(test_data2) - 1);
    EXPECT_NE(crc1, crc3);
}
