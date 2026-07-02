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
    ControlChangeMessage original(PARAM_VOLUME, 0, 0x7FFF);

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
    NoteMessage original(60, 127, 0);  // Middle C, max velocity

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
    SampleCtrlMessage original(0, SAMPLE_PLAY_START, 1.0f);

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
    PreviewReqMessage original(0, 0, 1000, 4);

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

// Test MeterPushMessage creation and parsing
TEST_F(MessageTypeTest, MeterPushMessage) {
    MeterPushMessage original(0x7FFF, 0x4000, 0x7FFF, 0x4000);

    size_t created =
        ProtocolHandler::CreateMeterPushPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_METER_PUSH);

    MeterPushMessage parsed;
    bool result =
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_METER_PUSH, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.rms_left, original.rms_left);
    EXPECT_EQ(parsed.rms_right, original.rms_right);
    EXPECT_EQ(parsed.peak_left, original.peak_left);
    EXPECT_EQ(parsed.peak_right, original.peak_right);
}

// Test WaveChunkMessage creation and parsing (header + trailing sample payload)
TEST_F(MessageTypeTest, WaveChunkMessage) {
    WaveChunkMessage header(4096, 128);
    std::vector<int16_t> samples(128);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<int16_t>(i * 7 - 300);
    }

    size_t created = ProtocolHandler::CreateWaveChunkPacket(
        buffer_.data(), buffer_.size(), header, samples.data(), samples.size() * sizeof(int16_t));

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_WAVE_CHUNK);

    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    std::vector<uint8_t> parsed_payload(sizeof(WaveChunkMessage) +
                                        samples.size() * sizeof(int16_t));
    size_t parsed_payload_size = parsed_payload.size();

    bool result = ProtocolHandler::ParseWaveXPacket(
        buffer_.data(), created, msg_type, parsed_payload.data(), parsed_payload_size, seq, flags);

    ASSERT_TRUE(result);
    EXPECT_EQ(msg_type, MSG_WAVE_CHUNK);

    const WaveChunkMessage* parsed_header =
        reinterpret_cast<const WaveChunkMessage*>(parsed_payload.data());
    EXPECT_EQ(parsed_header->offset, header.offset);
    EXPECT_EQ(parsed_header->count, header.count);

    const int16_t* parsed_samples =
        reinterpret_cast<const int16_t*>(parsed_payload.data() + sizeof(WaveChunkMessage));
    for (size_t i = 0; i < samples.size(); ++i) {
        EXPECT_EQ(parsed_samples[i], samples[i]) << "sample index " << i;
    }
}

// Test HeartbeatMessage creation and parsing
TEST_F(MessageTypeTest, HeartbeatMessage) {
    HeartbeatMessage original(1000, 5000, 10000, 256, 128, 512);

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

// Test BrowseResp creation and parsing (header + entries array)
TEST_F(MessageTypeTest, BrowseRespMessage) {
    FileEntryWire entries[3] = {FileEntryWire(1, 0, "dir1"),
                                FileEntryWire(0, 1024, "file1.wav", 44100, 2, 16, 5000),
                                FileEntryWire(0, 2048, "file2.wav", 48000, 1, 24, 9000)};

    size_t created =
        ProtocolHandler::CreateBrowseRespPacket(buffer_.data(), buffer_.size(), 3, entries, 3);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_BROWSE_RESP);

    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    std::vector<uint8_t> parsed_payload(sizeof(BrowseRespHeader) + 3 * sizeof(FileEntryWire));
    size_t parsed_payload_size = parsed_payload.size();

    bool result = ProtocolHandler::ParseWaveXPacket(
        buffer_.data(), created, msg_type, parsed_payload.data(), parsed_payload_size, seq, flags);

    ASSERT_TRUE(result);
    EXPECT_EQ(msg_type, MSG_BROWSE_RESP);

    const BrowseRespHeader* parsed_header =
        reinterpret_cast<const BrowseRespHeader*>(parsed_payload.data());
    EXPECT_EQ(parsed_header->total_count, 3u);
    EXPECT_EQ(parsed_header->n, 3);

    const FileEntryWire* parsed_entries =
        reinterpret_cast<const FileEntryWire*>(parsed_payload.data() + sizeof(BrowseRespHeader));
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(parsed_entries[i].is_dir, entries[i].is_dir) << "entry " << i;
        EXPECT_EQ(parsed_entries[i].size_bytes, entries[i].size_bytes) << "entry " << i;
        EXPECT_STREQ(parsed_entries[i].name, entries[i].name) << "entry " << i;
        EXPECT_EQ(parsed_entries[i].sample_rate, entries[i].sample_rate) << "entry " << i;
        EXPECT_EQ(parsed_entries[i].channels, entries[i].channels) << "entry " << i;
        EXPECT_EQ(parsed_entries[i].bits_per_sample, entries[i].bits_per_sample) << "entry " << i;
        EXPECT_EQ(parsed_entries[i].duration_ms, entries[i].duration_ms) << "entry " << i;
    }
}

// Test SampleStatusMessage creation and parsing
TEST_F(MessageTypeTest, SampleStatusMessage) {
    SampleStatusMessage original(1, 1, 2, 48000, 1000);

    size_t created =
        ProtocolHandler::CreateSampleStatusPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_STATUS);

    SampleStatusMessage parsed;
    bool result =
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SAMPLE_STATUS, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.sample_id, original.sample_id);
    EXPECT_EQ(parsed.state, original.state);
    EXPECT_EQ(parsed.channels, original.channels);
    EXPECT_EQ(parsed.sample_rate, original.sample_rate);
    EXPECT_EQ(parsed.frames_played, original.frames_played);
}

// Test SampleStopReq/Resp messages round trip
TEST_F(MessageTypeTest, SampleStopMessages) {
    SampleStopReqMessage req(3);

    size_t created_req =
        ProtocolHandler::CreateSampleStopReqPacket(buffer_.data(), buffer_.size(), req);

    ASSERT_GT(created_req, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_req));

    SampleStopReqMessage parsed_req;
    EXPECT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_STOP_REQ, &parsed_req, sizeof(parsed_req)));
    EXPECT_EQ(parsed_req.slot, req.slot);

    SampleStopRespMessage resp(1);

    size_t created_resp =
        ProtocolHandler::CreateSampleStopRespPacket(buffer_.data(), buffer_.size(), resp);

    ASSERT_GT(created_resp, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_resp));

    SampleStopRespMessage parsed_resp;
    EXPECT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_STOP_RESP, &parsed_resp, sizeof(parsed_resp)));
    EXPECT_EQ(parsed_resp.success, resp.success);
}

// Test ErrorMessage creation and parsing
TEST_F(MessageTypeTest, ErrorMessage) {
    ErrorMessage original(0x0001, "Test error message");

    size_t created = ProtocolHandler::CreateErrorPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_ERROR);

    ErrorMessage parsed;
    bool result = ProtocolHandler::ParseMessage(buffer_.data(), MSG_ERROR, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.code, original.code);
    EXPECT_STREQ(parsed.msg, original.msg);
}

// Test SyncMessage creation and parsing
TEST_F(MessageTypeTest, SyncMessage) {
    SyncMessage original(1000);

    size_t created = ProtocolHandler::CreateSyncPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SYNC);

    SyncMessage parsed;
    bool result = ProtocolHandler::ParseMessage(buffer_.data(), MSG_SYNC, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.timestamp_ms, original.timestamp_ms);
}

// Test AckMessage creation and parsing
TEST_F(MessageTypeTest, AckMessage) {
    AckMessage original(0x1234);

    size_t created = ProtocolHandler::CreateAckPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_ACK);

    AckMessage parsed;
    bool result = ProtocolHandler::ParseMessage(buffer_.data(), MSG_ACK, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.serial_id, original.serial_id);
}

// Test SamplePlayIndexMessage creation and parsing
TEST_F(MessageTypeTest, SamplePlayIndexMessage) {
    SamplePlayIndexMessage original(5);

    size_t created =
        ProtocolHandler::CreateSamplePlayIndexPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_PLAY_INDEX_REQ);

    SamplePlayIndexMessage parsed;
    bool result = ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_PLAY_INDEX_REQ, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.index, original.index);
}

// Test SampleGetPathMessage creation and parsing
TEST_F(MessageTypeTest, SampleGetPathMessage) {
    SampleGetPathMessage original(10);

    size_t created =
        ProtocolHandler::CreateSampleGetPathPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_GET_PATH_REQ);

    SampleGetPathMessage parsed;
    bool result = ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_GET_PATH_REQ, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.index, original.index);
}

// Test SamplePathResponseMessage creation and parsing
TEST_F(MessageTypeTest, SamplePathResponseMessage) {
    SamplePathResponseMessage original(10, "/samples/test.wav");

    size_t created =
        ProtocolHandler::CreateSamplePathResponsePacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_GET_PATH_RESP);

    SamplePathResponseMessage parsed;
    bool result = ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_GET_PATH_RESP, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.index, original.index);
    EXPECT_STREQ(parsed.path, original.path);
}

// Test DataRequestMessage creation and parsing (previously untested)
TEST_F(MessageTypeTest, DataRequestMessage) {
    DataRequestMessage original(2);  // 2 = wave data

    size_t created =
        ProtocolHandler::CreateDataRequestPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_DATA_REQUEST);

    DataRequestMessage parsed;
    bool result = ProtocolHandler::ParseDataRequest(buffer_.data(), parsed);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.request_type, original.request_type);
}

// Test StatusRequestMessage creation and parsing (previously untested; no dedicated
// Create*/Parse* helpers exist yet, so this exercises the generic packet path directly)
TEST_F(MessageTypeTest, StatusRequestMessage) {
    StatusRequestMessage original(STATUS_CATEGORY_SAMPLE_MEM);

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_STATUS_REQUEST, &original, sizeof(original));

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_STATUS_REQUEST);

    StatusRequestMessage parsed;
    bool result =
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_STATUS_REQUEST, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.category, original.category);
}

// Test SampleLoadMessage creation and parsing (previously untested; no dedicated
// Create*/Parse* helpers exist yet, so this exercises the generic packet path directly)
TEST_F(MessageTypeTest, SampleLoadMessage) {
    SampleLoadMessage original(42, 1048576, 48000, 2, 24, "/samples/kick.wav");

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SAMPLE_LOAD, &original, sizeof(original));

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_LOAD);

    SampleLoadMessage parsed;
    bool result =
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SAMPLE_LOAD, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.sample_id, original.sample_id);
    EXPECT_EQ(parsed.sample_size, original.sample_size);
    EXPECT_EQ(parsed.sample_rate, original.sample_rate);
    EXPECT_EQ(parsed.channels, original.channels);
    EXPECT_EQ(parsed.bit_depth, original.bit_depth);
    EXPECT_STREQ(parsed.path, original.path);
}

// Test SampleMemStatusMessage (+ SampleMemEntryMessage) creation and parsing (previously
// untested; no dedicated Create*/Parse* helpers exist yet, so this exercises the generic
// packet path directly)
TEST_F(MessageTypeTest, SampleMemStatusMessage) {
    SampleMemStatusMessage original(
        /*small_total_bytes=*/65536,
        /*small_free_bytes=*/32768,
        /*large_total_bytes=*/4194304,
        /*large_free_bytes=*/2097152,
        /*largest_free_bytes=*/1048576,
        /*in_use_bytes=*/3145728,
        /*failed_allocs=*/1);

    ASSERT_TRUE(original.AddEntry(SampleMemEntryMessage(1, 65536, 65536, 0, 0, 0, 48000, 2, 16)));
    ASSERT_TRUE(
        original.AddEntry(SampleMemEntryMessage(2, 1048576, 524288, 0xFF, 3, 8, 44100, 1, 24)));
    EXPECT_EQ(original.sample_count, 2);

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_STATUS_RESPONSE, &original, sizeof(original));

    ASSERT_GT(created, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_STATUS_RESPONSE);

    SampleMemStatusMessage parsed;
    bool result =
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_STATUS_RESPONSE, &parsed, sizeof(parsed));

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed.category, original.category);
    EXPECT_EQ(parsed.sample_count, original.sample_count);
    EXPECT_EQ(parsed.small_total_bytes, original.small_total_bytes);
    EXPECT_EQ(parsed.small_free_bytes, original.small_free_bytes);
    EXPECT_EQ(parsed.large_total_bytes, original.large_total_bytes);
    EXPECT_EQ(parsed.large_free_bytes, original.large_free_bytes);
    EXPECT_EQ(parsed.largest_free_bytes, original.largest_free_bytes);
    EXPECT_EQ(parsed.in_use_bytes, original.in_use_bytes);
    EXPECT_EQ(parsed.failed_allocs, original.failed_allocs);

    for (int i = 0; i < original.sample_count; ++i) {
        EXPECT_EQ(parsed.entries[i].sample_id, original.entries[i].sample_id) << "entry " << i;
        EXPECT_EQ(parsed.entries[i].allocated_bytes, original.entries[i].allocated_bytes)
            << "entry " << i;
        EXPECT_EQ(parsed.entries[i].loaded_bytes, original.entries[i].loaded_bytes)
            << "entry " << i;
        EXPECT_EQ(parsed.entries[i].cls, original.entries[i].cls) << "entry " << i;
        EXPECT_EQ(parsed.entries[i].page, original.entries[i].page) << "entry " << i;
        EXPECT_EQ(parsed.entries[i].slot, original.entries[i].slot) << "entry " << i;
        EXPECT_EQ(parsed.entries[i].sample_rate, original.entries[i].sample_rate) << "entry " << i;
        EXPECT_EQ(parsed.entries[i].channels, original.entries[i].channels) << "entry " << i;
        EXPECT_EQ(parsed.entries[i].bit_depth, original.entries[i].bit_depth) << "entry " << i;
    }
}

// SampleMemStatusMessage::AddEntry bounds check
TEST_F(MessageTypeTest, SampleMemStatusMessageAddEntryBounds) {
    SampleMemStatusMessage status(0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < WAVEX_SAMPLE_STATUS_MAX_ENTRIES; ++i) {
        EXPECT_TRUE(status.AddEntry(
            SampleMemEntryMessage(static_cast<uint16_t>(i), 0, 0, 0, 0, 0, 0, 0, 0)));
    }
    EXPECT_EQ(status.sample_count, WAVEX_SAMPLE_STATUS_MAX_ENTRIES);
    // One more push must be rejected, not overflow the fixed array.
    EXPECT_FALSE(status.AddEntry(SampleMemEntryMessage(99, 0, 0, 0, 0, 0, 0, 0, 0)));
    EXPECT_EQ(status.sample_count, WAVEX_SAMPLE_STATUS_MAX_ENTRIES);
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

    HeartbeatMessage payload(1000, 5000, 10000, 256, 128, 512);

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
