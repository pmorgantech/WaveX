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

    // Test NoteOff - parse it back too (previously only created+validated,
    // so a NoteOff that carried the wrong type byte or velocity passed).
    size_t created_off = ProtocolHandler::CreateNoteOffPacket(
        buffer_.data(), buffer_.size(), original.note, original.channel);

    ASSERT_GT(created_off, 0);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created_off));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_NOTE_OFF);

    NoteMessage parsed_off;
    ASSERT_TRUE(ProtocolHandler::ParseNoteMessage(buffer_.data(), parsed_off));
    EXPECT_EQ(parsed_off.note, original.note);
    EXPECT_EQ(parsed_off.channel, original.channel);
    EXPECT_EQ(parsed_off.velocity, 0);  // NoteOff is sent with velocity 0
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

// (BrowseReqMessage test deleted with ParseBrowseReq - it exercised a wire
// format nothing sends. The live [start_index u8][path][NUL] format is
// covered by daisy/tests/unit/comm/message_dispatch_test.cpp.)

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

// A failed load rides the same message: state = LOAD_FAILED, frames_played =
// the reason. The states are pinned to their wire values because an older
// frontend keys on 0x10/0x11 numerically.
TEST_F(MessageTypeTest, SampleStatusLoadFailedCarriesReason) {
    static_assert(SAMPLE_STATUS_LOAD_COMPLETE == 0x10, "wire value");
    static_assert(SAMPLE_STATUS_LOAD_PROGRESS == 0x11, "wire value");
    static_assert(SAMPLE_STATUS_LOAD_FAILED == 0x12, "wire value");

    SampleStatusMessage original(7, SAMPLE_STATUS_LOAD_FAILED, 0, 0, SAMPLE_LOAD_FAIL_RAM);
    size_t created =
        ProtocolHandler::CreateSampleStatusPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0);

    SampleStatusMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SAMPLE_STATUS, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.sample_id, 7);
    EXPECT_EQ(parsed.state, SAMPLE_STATUS_LOAD_FAILED);
    EXPECT_EQ(parsed.frames_played, static_cast<uint32_t>(SAMPLE_LOAD_FAIL_RAM));
}

// A Pool page is one frame: header + up to MAX_SAMPLE_META_PAGE records must
// fit UART_MAX_PAYLOAD, or the reply the design relies on cannot be sent.
TEST_F(MessageTypeTest, SampleMetaPageFitsOneFrame) {
    EXPECT_EQ(sizeof(SampleMetaPageHeader), 8u);
    EXPECT_EQ(sizeof(SampleMetaPageReqMessage), 4u);
    const size_t page_bytes =
        sizeof(SampleMetaPageHeader) + MAX_SAMPLE_META_PAGE * sizeof(SampleMetadata);
    EXPECT_LE(page_bytes, 2048u) << "UART_MAX_PAYLOAD";

    SampleMetaPageReqMessage req(40, 20);
    EXPECT_EQ(req.first, 40);
    EXPECT_EQ(req.count, 20);

    SampleMetadata m;
    EXPECT_EQ(m.used_by, 0);
    EXPECT_EQ(m.flags, 0);
    m.flags = SAMPLE_META_RESIDENT | SAMPLE_META_PINNED;
    m.used_by = (1u << 3) | (1u << 7);
    EXPECT_EQ(m.used_by, 0x88);
}

TEST_F(MessageTypeTest, InstrumentMessagesRoundTrip) {
    InstOpMessage request(0x12345678u, 2, INST_OP_SFZ_PROBE, "/Instruments/Grand Piano.sfz");
    const size_t request_size = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_INST_OP, &request, sizeof(request));
    ASSERT_GT(request_size, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), request_size));
    InstOpMessage parsed_request;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_INST_OP, &parsed_request, sizeof(parsed_request)));
    EXPECT_EQ(parsed_request.request_id, request.request_id);
    EXPECT_EQ(parsed_request.slot, 2);
    EXPECT_EQ(parsed_request.op, INST_OP_SFZ_PROBE);
    EXPECT_STREQ(parsed_request.path, request.path);

    InstStatusMessage status;
    status.request_id = request.request_id;
    status.slot = request.slot;
    status.op = INST_OP_SFZ_LOAD;
    status.state = INST_STATUS_LOAD_PROGRESS;
    status.flags = INST_STATUS_MISSING_FILES;
    status.error = INST_ERROR_MISSING_SAMPLES;
    status.zone_count = 12;
    status.sample_count = 5;
    status.current_index = 2;
    status.missing_count = 1;
    status.total_bytes = 24u * 1024u * 1024u;
    status.available_bytes = 32u * 1024u * 1024u;
    status.loaded_bytes = 9u * 1024u * 1024u;
    status.current_bytes = 4u * 1024u * 1024u;
    status.current_loaded_bytes = 2u * 1024u * 1024u;
    detail::CopyWireString(status.current_name, sizeof(status.current_name), "velocity-3.wav");

    const size_t status_size = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_INST_STATUS, &status, sizeof(status));
    ASSERT_GT(status_size, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), status_size));
    InstStatusMessage parsed_status;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_INST_STATUS, &parsed_status, sizeof(parsed_status)));
    EXPECT_EQ(parsed_status.request_id, status.request_id);
    EXPECT_EQ(parsed_status.state, INST_STATUS_LOAD_PROGRESS);
    EXPECT_EQ(parsed_status.total_bytes, status.total_bytes);
    EXPECT_EQ(parsed_status.current_loaded_bytes, status.current_loaded_bytes);
    EXPECT_STREQ(parsed_status.current_name, status.current_name);
    EXPECT_STREQ(MessageTypeName(MSG_INST_STATUS), "INST_STATUS");
}

TEST_F(MessageTypeTest, SetModSlotMessageRoundTrips) {
    // param-locks-and-modulation.md §9 stage 4: INST_OP_SET_MOD_SLOT rides
    // MSG_INST_OP via a second InstOpMessage constructor - path stays empty,
    // the mod_* fields carry the payload instead.
    InstOpMessage request(0xABCDEF01u, 9, 4 /*mod_slot_index*/, 5, 2, -32767, 1, 1);
    const size_t size = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_INST_OP, &request, sizeof(request));
    ASSERT_GT(size, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), size));

    InstOpMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_INST_OP, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.request_id, request.request_id);
    EXPECT_EQ(parsed.slot, 9);
    EXPECT_EQ(parsed.op, INST_OP_SET_MOD_SLOT);
    EXPECT_EQ(parsed.mod_slot_index, 4);
    EXPECT_EQ(parsed.mod_source, 5);
    EXPECT_EQ(parsed.mod_dest, 2);
    EXPECT_EQ(parsed.mod_depth, -32767);
    EXPECT_EQ(parsed.mod_curve, 1);
    EXPECT_EQ(parsed.mod_flags, 1);
    EXPECT_STREQ(parsed.path, "");
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

// Storage availability is sent UNSOLICITED by the backend, so the frontend has
// no request to correlate it with - the round trip is the only thing pinning
// the wire format.
TEST_F(MessageTypeTest, StorageStatusMessage) {
    for (uint8_t mounted: {uint8_t{0}, uint8_t{1}}) {
        StorageStatusMessage original(mounted);

        size_t created =
            ProtocolHandler::CreateStorageStatusPacket(buffer_.data(), buffer_.size(), original);

        ASSERT_GT(created, 0u);
        EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
        EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_STORAGE_STATUS);

        StorageStatusMessage parsed;
        EXPECT_TRUE(ProtocolHandler::ParseMessage(
            buffer_.data(), MSG_STORAGE_STATUS, &parsed, sizeof(parsed)));
        EXPECT_EQ(parsed.mounted, original.mounted);
    }
}

// Diagnostics telemetry, like storage status, is unsolicited: the frontend has
// no request to correlate it with, so this round trip is the only thing
// pinning the wire format. Every field is set to a distinct value - a
// memcmp-style check would pass even if two same-width neighbours were swapped
// during a later edit.
TEST_F(MessageTypeTest, DiagPushMessage) {
    // 102 bytes of payload + 4 header + 2 CRC = 108, so this must fit the
    // 128-byte class. If a field is added that pushes it past 122 the packet
    // silently promotes to 256 and doubles its cost on the link.
    EXPECT_EQ(sizeof(DiagPushMessage), 102u);

    DiagPushMessage original;
    original.callback_hz_x10 = 10002;
    original.ring_low_water = 1420;
    original.underruns = 3;
    original.prebuffer_filled = 1024;
    original.engine_cpu_x10 = 74;
    original.engine_cpu_max_x10 = 121;
    original.wav_sample_rate = 44100;
    original.wav_channels = 2;
    original.wav_bits = 16;
    original.playing = 1;
    original.resampling = 1;
    original.ring_pushes = 4321;
    original.ring_discards = 7;
    original.sd_mounted = 1;
    original.sd_speed_index = 3;
    original.sd_reads = 210;
    original.sd_bytes = 1720320;
    original.sd_lat_avg_us = 1700;
    original.sd_lat_max_us = 2900;
    original.sd_errors = 2;
    original.sd_recoveries = 1;
    original.sd_last_fatfs = 1;  // FR_DISK_ERR
    original.sd_hal_err = 0x00000002;
    original.sample_ram_free = 33554432;
    original.sample_ram_largest = 16777216;
    original.sample_failed_allocs = 4;
    original.sample_count = 12;
    original.link_total_us = 1234;
    original.link_max_us = 89;
    original.link_rx_frames = 21;
    original.link_tx_frames = 43;
    original.link_errors = 5;
    original.link_seq_drops = 6;
    original.link_queue_overflows = 8;
    original.midi_notes = 12;
    original.midi_ccs = 40;
    original.midi_clock_ticks = 48;
    original.measured_bpm_x100 = 12004;
    original.sync_state = 2;
    original.transport_playing = 1;
    original.pattern = 3;
    original.step = 9;
    original.interval_ms = 500;
    original.heap_total = 393216;
    original.heap_free = 271360;

    size_t created =
        ProtocolHandler::CreateDiagPushPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0u);
    EXPECT_EQ(created, 128u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_DIAG_PUSH);

    DiagPushMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_DIAG_PUSH, &parsed, sizeof(parsed)));

    EXPECT_EQ(parsed.callback_hz_x10, original.callback_hz_x10);
    EXPECT_EQ(parsed.ring_low_water, original.ring_low_water);
    EXPECT_EQ(parsed.underruns, original.underruns);
    EXPECT_EQ(parsed.prebuffer_filled, original.prebuffer_filled);
    EXPECT_EQ(parsed.engine_cpu_x10, original.engine_cpu_x10);
    EXPECT_EQ(parsed.engine_cpu_max_x10, original.engine_cpu_max_x10);
    EXPECT_EQ(parsed.wav_sample_rate, original.wav_sample_rate);
    EXPECT_EQ(parsed.wav_channels, original.wav_channels);
    EXPECT_EQ(parsed.wav_bits, original.wav_bits);
    EXPECT_EQ(parsed.playing, original.playing);
    EXPECT_EQ(parsed.resampling, original.resampling);
    EXPECT_EQ(parsed.ring_pushes, original.ring_pushes);
    EXPECT_EQ(parsed.ring_discards, original.ring_discards);
    EXPECT_EQ(parsed.sd_mounted, original.sd_mounted);
    EXPECT_EQ(parsed.sd_speed_index, original.sd_speed_index);
    EXPECT_EQ(parsed.sd_reads, original.sd_reads);
    EXPECT_EQ(parsed.sd_bytes, original.sd_bytes);
    EXPECT_EQ(parsed.sd_lat_avg_us, original.sd_lat_avg_us);
    EXPECT_EQ(parsed.sd_lat_max_us, original.sd_lat_max_us);
    EXPECT_EQ(parsed.sd_errors, original.sd_errors);
    EXPECT_EQ(parsed.sd_recoveries, original.sd_recoveries);
    EXPECT_EQ(parsed.sd_last_fatfs, original.sd_last_fatfs);
    EXPECT_EQ(parsed.sd_hal_err, original.sd_hal_err);
    EXPECT_EQ(parsed.sample_ram_free, original.sample_ram_free);
    EXPECT_EQ(parsed.sample_ram_largest, original.sample_ram_largest);
    EXPECT_EQ(parsed.sample_failed_allocs, original.sample_failed_allocs);
    EXPECT_EQ(parsed.sample_count, original.sample_count);
    EXPECT_EQ(parsed.link_total_us, original.link_total_us);
    EXPECT_EQ(parsed.link_max_us, original.link_max_us);
    EXPECT_EQ(parsed.link_rx_frames, original.link_rx_frames);
    EXPECT_EQ(parsed.link_tx_frames, original.link_tx_frames);
    EXPECT_EQ(parsed.link_errors, original.link_errors);
    EXPECT_EQ(parsed.link_seq_drops, original.link_seq_drops);
    EXPECT_EQ(parsed.link_queue_overflows, original.link_queue_overflows);
    EXPECT_EQ(parsed.midi_notes, original.midi_notes);
    EXPECT_EQ(parsed.midi_ccs, original.midi_ccs);
    EXPECT_EQ(parsed.midi_clock_ticks, original.midi_clock_ticks);
    EXPECT_EQ(parsed.measured_bpm_x100, original.measured_bpm_x100);
    EXPECT_EQ(parsed.sync_state, original.sync_state);
    EXPECT_EQ(parsed.transport_playing, original.transport_playing);
    EXPECT_EQ(parsed.pattern, original.pattern);
    EXPECT_EQ(parsed.step, original.step);
    EXPECT_EQ(parsed.interval_ms, original.interval_ms);
    EXPECT_EQ(parsed.heap_total, original.heap_total);
    EXPECT_EQ(parsed.heap_free, original.heap_free);
}

// heap_total / heap_free were appended to the END of DiagPushMessage rather
// than filed next to the other memory fields, specifically so that a backend
// still running the 94-byte layout keeps working against a frontend built
// with the 102-byte one. The two MCUs are flashed independently, so that is a
// real configuration, not a hypothetical.
//
// This is the test that pins that property: it forges the OLD payload (the
// first 94 bytes, which is every field up to and including interval_ms),
// packs it, and parses it as the new struct. Every pre-existing field must
// survive unshifted and the two new ones must read zero - which is exactly
// the "not reported" sentinel the Daisy tab renders as unknown. Without this,
// nothing stops a later edit from inserting a field mid-struct and silently
// re-interpreting an older backend's sd_bytes as its heap size.
TEST_F(MessageTypeTest, DiagPushMessageParsesPreHeapLayout) {
    static const size_t kLegacySize = 94u;
    ASSERT_EQ(offsetof(DiagPushMessage, heap_total), kLegacySize);

    DiagPushMessage source;
    source.callback_hz_x10 = 10000;
    source.ring_low_water = 1420;
    source.underruns = 3;
    source.sd_mounted = 1;
    source.sample_ram_free = 33554432;
    source.sample_count = 12;
    source.link_rx_frames = 21;
    source.measured_bpm_x100 = 12004;
    source.step = 9;
    source.interval_ms = 500;
    // Set on the sender side but deliberately NOT transmitted: they live past
    // the 94-byte cut, which is the whole point of the exercise.
    source.heap_total = 393216;
    source.heap_free = 271360;

    // A pre-stage-8 backend emits only the first 94 bytes; the packet's
    // payload region is zero-padded out to the size class from there.
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_DIAG_PUSH, &source, kLegacySize);
    ASSERT_GT(created, 0u);
    // Same size class as the 102-byte message, so this change did not move
    // the packet's cost on the link either.
    EXPECT_EQ(created, 128u);
    ASSERT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    DiagPushMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_DIAG_PUSH, &parsed, sizeof(parsed)));

    EXPECT_EQ(parsed.callback_hz_x10, source.callback_hz_x10);
    EXPECT_EQ(parsed.ring_low_water, source.ring_low_water);
    EXPECT_EQ(parsed.underruns, source.underruns);
    EXPECT_EQ(parsed.sd_mounted, source.sd_mounted);
    EXPECT_EQ(parsed.sample_ram_free, source.sample_ram_free);
    EXPECT_EQ(parsed.sample_count, source.sample_count);
    EXPECT_EQ(parsed.link_rx_frames, source.link_rx_frames);
    EXPECT_EQ(parsed.measured_bpm_x100, source.measured_bpm_x100);
    EXPECT_EQ(parsed.step, source.step);
    EXPECT_EQ(parsed.interval_ms, source.interval_ms);

    EXPECT_EQ(parsed.heap_total, 0u);
    EXPECT_EQ(parsed.heap_free, 0u);
}

// The control-parameter IDs are a single flat enum on the wire, so two labels
// sharing a value is a routing bug waiting for the second one to be used.
// PARAM_LFO_RATE/PARAM_LFO_DEPTH used to be 0x08/0x09 - the same values as
// PARAM_PAN/PARAM_PITCH, which the Voice page sends and the engine handles.
TEST_F(MessageTypeTest, ControlParameterIdsAreUnique) {
    const uint8_t ids[] = {PARAM_VOLUME,
                           PARAM_FILTER_CUTOFF,
                           PARAM_FILTER_RESONANCE,
                           PARAM_ENVELOPE_ATTACK,
                           PARAM_ENVELOPE_DECAY,
                           PARAM_ENVELOPE_SUSTAIN,
                           PARAM_ENVELOPE_RELEASE,
                           PARAM_PAN,
                           PARAM_PITCH,
                           PARAM_MODULATION_MATRIX,
                           PARAM_LFO_RATE,
                           PARAM_LFO_DEPTH};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(ids) / sizeof(ids[0]); ++j) {
            EXPECT_NE(ids[i], ids[j])
                << "control parameter ids " << i << " and " << j << " collide";
        }
    }
    // The two that are already live keep their wire values; renumbering the
    // dead LFO labels must not have moved them.
    EXPECT_EQ(PARAM_PAN, 0x08);
    EXPECT_EQ(PARAM_PITCH, 0x09);
}

// A default-constructed telemetry message must be all zeros: the collector
// fills only the fields it has, and a garbage default would show as real data.
TEST_F(MessageTypeTest, DiagPushMessageDefaultsToZero) {
    DiagPushMessage msg;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&msg);
    for (size_t i = 0; i < sizeof(msg); ++i) {
        EXPECT_EQ(raw[i], 0u) << "byte " << i << " not zeroed";
    }
}

TEST_F(MessageTypeTest, DiagSubscribeMessage) {
    DiagSubscribeMessage original(1, 2);

    size_t created =
        ProtocolHandler::CreateDiagSubscribePacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_DIAG_SUBSCRIBE);

    DiagSubscribeMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_DIAG_SUBSCRIBE, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.enable, original.enable);
    EXPECT_EQ(parsed.interval_hz, original.interval_hz);
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

// Test SamplePlayIndexMessage creation and parsing. loop_gap_ms rides on the
// play request (a property of the audition, not the sample), so it must
// survive the wire alongside the index.
TEST_F(MessageTypeTest, SamplePlayIndexMessage) {
    SamplePlayIndexMessage original(5, /*loop_gap_ms=*/250);

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
    EXPECT_EQ(parsed.loop_gap_ms, 250);
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

// The internal sequence counter must skip the reserved value 0 when the
// uint16 wraps (review M4): a receiver-side SequenceTracker rejects seq 0,
// so a generator that wraps through it silently loses one packet per 64K.
TEST_F(MessageTypeTest, CreatePacketSequenceNumberSkipsZeroOnWrap) {
    SyncMessage msg(0);
    uint8_t msg_type = 0;
    uint16_t seq = 0;
    uint8_t flags = 0;

    uint16_t prev_seq = 0;
    bool saw_wrap = false;
    // More than one full uint16 cycle so the wrap is exercised regardless of
    // the counter's starting value (it is shared across tests in this binary).
    for (uint32_t i = 0; i < 70000; ++i) {
        size_t created = ProtocolHandler::CreatePacket(
            buffer_.data(), buffer_.size(), MSG_SYNC, &msg, sizeof(msg));
        ASSERT_GT(created, 0u);
        size_t payload_size = sizeof(msg);
        ASSERT_TRUE(ProtocolHandler::ParseWaveXPacket(
            buffer_.data(), created, msg_type, &msg, payload_size, seq, flags));
        ASSERT_NE(seq, 0u) << "iteration " << i;
        if (i > 0 && seq < prev_seq) {
            saw_wrap = true;
            EXPECT_EQ(seq, 1u) << "wrap must land on 1, not 0";
        }
        prev_seq = seq;
    }
    EXPECT_TRUE(saw_wrap);
}

// Review H4: GetOptimalSizeCode saturates to the 2048-byte class for any
// oversize payload; CreateWaveXPacket must reject payloads beyond the
// class's real capacity (2042 = 2048 - header - crc) instead of
// overrunning the caller's buffer and underflowing the zero-pad memset.
TEST_F(MessageTypeTest, CreateWaveXPacketRejectsPayloadBeyondLargestClass) {
    std::vector<uint8_t> payload(2049, 0xAB);

    // Exactly at capacity: fits the 2048-byte class.
    size_t created = ProtocolHandler::CreateWaveXPacket(
        buffer_.data(), buffer_.size(), MSG_WAVE_CHUNK, payload.data(), 2042, 7, 0);
    EXPECT_EQ(created, 2048u);

    // One past capacity through six past: must fail cleanly, not overflow.
    for (size_t oversize = 2043; oversize <= 2048; ++oversize) {
        created = ProtocolHandler::CreateWaveXPacket(
            buffer_.data(), buffer_.size(), MSG_WAVE_CHUNK, payload.data(), oversize, 7, 0);
        EXPECT_EQ(created, 0u) << "payload_size=" << oversize;
    }
}

// CV calibration messages (Stage A analog path, item 5 stage 4).
TEST_F(MessageTypeTest, CvCalMessage) {
    CvCalMessage original(3, 1, 1.1f, -0.05f, 0.9f, 0.02f, 1.05f, -0.01f, 2.7f);

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_CV_CAL_SET, &original, sizeof(original));
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    CvCalMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_CV_CAL_SET, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.group, 3);
    EXPECT_EQ(parsed.persist, 1);
    EXPECT_FLOAT_EQ(parsed.vcf_cut_gain, 1.1f);
    EXPECT_FLOAT_EQ(parsed.vcf_cut_off, -0.05f);
    EXPECT_FLOAT_EQ(parsed.vcf_q_gain, 0.9f);
    EXPECT_FLOAT_EQ(parsed.vcf_q_off, 0.02f);
    EXPECT_FLOAT_EQ(parsed.vca_gain, 1.05f);
    EXPECT_FLOAT_EQ(parsed.vca_off, -0.01f);
    EXPECT_FLOAT_EQ(parsed.cutoff_k, 2.7f);
}

TEST_F(MessageTypeTest, CvCalGetMessage) {
    CvCalGetMessage original(5);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_CV_CAL_GET, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    CvCalGetMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_CV_CAL_GET, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.group, 5);
}

TEST_F(MessageTypeTest, CvTestMessage) {
    CvTestMessage original(0, 1, 0.75f, 0.25f, 1.0f);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_CV_TEST, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    CvTestMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_CV_TEST, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.enable, 1);
    EXPECT_FLOAT_EQ(parsed.cutoff, 0.75f);
    EXPECT_FLOAT_EQ(parsed.resonance, 0.25f);
    EXPECT_FLOAT_EQ(parsed.vca, 1.0f);
}

// ---- Phase 2 sequencer / transport / MIDI clock messages ----

TEST_F(MessageTypeTest, SeqTransportMessage) {
    SeqTransportMessage original(
        SEQ_TRANSPORT_CONTINUE, SEQ_CLOCK_MIDI, SEQ_INPUT_LIVE_RECORD, 1, 14025, 48);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SEQ_TRANSPORT, &original, sizeof(original));
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    SeqTransportMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SEQ_TRANSPORT, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.command, SEQ_TRANSPORT_CONTINUE);
    EXPECT_EQ(parsed.clock_source, SEQ_CLOCK_MIDI);
    EXPECT_EQ(parsed.input_mode, SEQ_INPUT_LIVE_RECORD);
    EXPECT_EQ(parsed.quantize, 1);
    EXPECT_EQ(parsed.tempo_bpm_x100, 14025);
    EXPECT_EQ(parsed.song_position, 48);
}

TEST_F(MessageTypeTest, SeqPatternOpMessage) {
    // A SET_STEP op: track 3, step 12, on, velocity 110.
    SeqPatternOpMessage original(SEQ_OP_SET_STEP, 3, 12, 1, 110, 0);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SEQ_PATTERN_OP, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    SeqPatternOpMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SEQ_PATTERN_OP, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.op, SEQ_OP_SET_STEP);
    EXPECT_EQ(parsed.track, 3);
    EXPECT_EQ(parsed.step, 12);
    EXPECT_EQ(parsed.arg_u8, 1);
    EXPECT_EQ(parsed.arg_u16, 110);
    EXPECT_EQ(parsed.arg_s16, 0);
}

TEST_F(MessageTypeTest, SeqPatternOpMessageSignedMicroOffset) {
    // Micro-timing op carries a signed offset - verify negatives survive.
    SeqPatternOpMessage original(SEQ_OP_SET_STEP_MICRO, 1, 4, 2, 8, -6);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SEQ_PATTERN_OP, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    SeqPatternOpMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SEQ_PATTERN_OP, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.op, SEQ_OP_SET_STEP_MICRO);
    EXPECT_EQ(parsed.arg_u8, 2);   // retrig_count
    EXPECT_EQ(parsed.arg_u16, 8);  // retrig_rate_ticks
    EXPECT_EQ(parsed.arg_s16, -6);
}

TEST_F(MessageTypeTest, SeqPlayheadMessage) {
    SeqPlayheadMessage original(2, 47, 1, 2 /*locked*/, 13000, 9);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SEQ_PLAYHEAD, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    SeqPlayheadMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SEQ_PLAYHEAD, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.pattern, 2);
    EXPECT_EQ(parsed.step, 47);
    EXPECT_EQ(parsed.playing, 1);
    EXPECT_EQ(parsed.sync_state, 2);
    EXPECT_EQ(parsed.measured_bpm_x100, 13000);
    EXPECT_EQ(parsed.loop_count, 9u);
}

TEST_F(MessageTypeTest, MidiClockEventMessage) {
    MidiClockEventMessage original(MIDI_CLK_TICK, 1 /*USB*/, 12345, 20833, 0);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_MIDI_CLOCK_EVENT, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    MidiClockEventMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_MIDI_CLOCK_EVENT, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.event, MIDI_CLK_TICK);
    EXPECT_EQ(parsed.source, 1);
    EXPECT_EQ(parsed.tick_seq, 12345);
    EXPECT_EQ(parsed.esp_delta_us, 20833u);
    EXPECT_EQ(parsed.spp_beats16, 0);
}

TEST_F(MessageTypeTest, MidiCcMessage) {
    MidiCcMessage original(1 /*modwheel*/, 64, 3);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_MIDI_CC, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    MidiCcMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_MIDI_CC, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.cc, 1);
    EXPECT_EQ(parsed.value, 64);
    EXPECT_EQ(parsed.channel, 3);
}

TEST_F(MessageTypeTest, SeqClockOutMessage) {
    SeqClockOutMessage original(MIDI_CLK_SPP, 777, 32);
    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SEQ_CLOCK_OUT, &original, sizeof(original));
    ASSERT_GT(created, 0u);

    SeqClockOutMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SEQ_CLOCK_OUT, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.event, MIDI_CLK_SPP);
    EXPECT_EQ(parsed.tick_seq, 777);
    EXPECT_EQ(parsed.spp_beats16, 32);
}

// Playback region / loop / gain. The sentinels are the part worth pinning: a
// frontend that does not know the file length sends end_frame 0 and expects
// the backend to read it as "to the end", not as "an empty region".
TEST_F(MessageTypeTest, SampleEditMessage) {
    SampleEditMessage original(2, 1, -35, 44100, 396900, 88200, 352800, 5, 120);

    size_t created =
        ProtocolHandler::CreateSampleEditPacket(buffer_.data(), buffer_.size(), original);

    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_EDIT_SET);

    SampleEditMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_EDIT_SET, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.slot, original.slot);
    EXPECT_EQ(parsed.loop_enabled, original.loop_enabled);
    EXPECT_EQ(parsed.gain_db_x10, original.gain_db_x10);
    EXPECT_EQ(parsed.start_frame, original.start_frame);
    EXPECT_EQ(parsed.end_frame, original.end_frame);
    EXPECT_EQ(parsed.loop_start, original.loop_start);
    EXPECT_EQ(parsed.loop_end, original.loop_end);
    EXPECT_EQ(parsed.fade_in_ms, 5);
    EXPECT_EQ(parsed.fade_out_ms, 120);
}

// De-click is the DEFAULT, not an opt-in: a region that starts mid-waveform
// starts on a step. A default-constructed command must therefore carry a
// non-zero fade, and 0 must be reachable as a deliberate "off".
TEST_F(MessageTypeTest, SampleEditDefaultsToDeclickAndZeroMeansOff) {
    SampleEditMessage defaulted;
    EXPECT_EQ(defaulted.fade_in_ms, kDefaultDeclickMs);
    EXPECT_EQ(defaulted.fade_out_ms, kDefaultDeclickMs);
    EXPECT_GT(kDefaultDeclickMs, 0);

    SampleEditMessage off(0, 0, 0, 0, 0, 0, 0, 0, 0);
    size_t created = ProtocolHandler::CreateSampleEditPacket(buffer_.data(), buffer_.size(), off);
    ASSERT_GT(created, 0u);
    SampleEditMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_EDIT_SET, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.fade_in_ms, 0);
    EXPECT_EQ(parsed.fade_out_ms, 0);
}

// Negative gain must survive the wire. gain_db_x10 is the only signed field
// in the message, and attenuation is its common case.
TEST_F(MessageTypeTest, SampleEditMessageCarriesNegativeGain) {
    SampleEditMessage original(0, 0, -240, 0, 0, 0, 0);

    size_t created =
        ProtocolHandler::CreateSampleEditPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);

    SampleEditMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_EDIT_SET, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.gain_db_x10, -240);
}

// The record is the single source of truth for every playback and display
// path, so its round trip and its clamping are both load-bearing.
TEST_F(MessageTypeTest, SampleMetadataRoundTrip) {
    EXPECT_EQ(sizeof(SampleMetadata), 90u);  // + 4 header + 2 CRC fits PKT_SIZE_128

    SampleMetadata original;
    original.sample_id = 7;
    original.generation = 3;
    original.sample_rate = 44100;
    original.total_frames = 7938000;  // 3:00, past the old 32-bit duration wrap
    original.start_frame = 44100;
    original.end_frame = 7000000;
    original.loop_start = 100000;
    original.loop_end = 6000000;
    original.gain_db_x10 = -35;
    original.fade_in_ms = 12;
    original.fade_out_ms = 250;
    original.channels = 2;
    original.bits_per_sample = 24;
    original.loop_enabled = 1;
    original.channel_mode = SAMPLE_CH_MONO_SUM;
    original.flags = 1;
    detail::CopyWireString(original.name, sizeof(original.name), "amen-full.wav");

    size_t created =
        ProtocolHandler::CreateSampleMetaPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);
    EXPECT_EQ(created, 128u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_META);

    SampleMetadata parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SAMPLE_META, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.sample_id, original.sample_id);
    EXPECT_EQ(parsed.generation, original.generation);
    EXPECT_EQ(parsed.sample_rate, original.sample_rate);
    EXPECT_EQ(parsed.total_frames, original.total_frames);
    EXPECT_EQ(parsed.start_frame, original.start_frame);
    EXPECT_EQ(parsed.end_frame, original.end_frame);
    EXPECT_EQ(parsed.loop_start, original.loop_start);
    EXPECT_EQ(parsed.loop_end, original.loop_end);
    EXPECT_EQ(parsed.gain_db_x10, original.gain_db_x10);
    EXPECT_EQ(parsed.fade_in_ms, 12);
    EXPECT_EQ(parsed.fade_out_ms, 250);
    EXPECT_EQ(parsed.channels, original.channels);
    EXPECT_EQ(parsed.bits_per_sample, original.bits_per_sample);
    EXPECT_EQ(parsed.loop_enabled, original.loop_enabled);
    EXPECT_EQ(parsed.channel_mode, SAMPLE_CH_MONO_SUM);
    EXPECT_EQ(parsed.flags, original.flags);
    EXPECT_STREQ(parsed.name, "amen-full.wav");
}

// The envelope is the reason the preview does not have to alias. Its wire
// shape is worth pinning: the payload length depends on `channels`, which is
// data, not a constant - get that wrong and the receiver reads past the frame.
TEST_F(MessageTypeTest, EnvelopeReqRoundTrip) {
    EXPECT_EQ(sizeof(EnvelopeReqMessage), 12u);

    EnvelopeReqMessage original(7, 1256, 44100, 7000000);
    size_t created =
        ProtocolHandler::CreateEnvelopeReqPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_ENVELOPE_REQ);

    EnvelopeReqMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_ENVELOPE_REQ, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.sample_id, 7);
    EXPECT_EQ(parsed.columns, 1256);
    EXPECT_EQ(parsed.start_frame, 44100u);
    EXPECT_EQ(parsed.end_frame, 7000000u);
}

TEST_F(MessageTypeTest, EnvelopeChunkCarriesBothChannels) {
    EXPECT_EQ(sizeof(EnvelopeChunkMessage), 20u);
    EXPECT_EQ(sizeof(EnvelopeColumn), 4u);

    constexpr uint16_t kColumns = 3;
    EnvelopeChunkMessage header;
    header.sample_id = 7;
    header.generation = 2;
    header.start_frame = 1000;
    header.end_frame = 5000;
    header.total_columns = 96;
    header.first_column = 12;
    header.columns = kColumns;
    header.channels = 2;

    // Deliberately asymmetric L/R: a summed-to-mono format would lose this,
    // and a loop seam has to be judged on both channels (roadmap 1.5.7).
    const EnvelopeColumn columns[kColumns * 2] = {
        {-32768, 32767}, {0, 0}, {-100, 200}, {-3000, 4000}, {-1, 1}, {-20000, 100}};

    size_t created = ProtocolHandler::CreateEnvelopeChunkPacket(
        buffer_.data(), buffer_.size(), header, columns, kColumns * 2);
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_ENVELOPE_CHUNK);

    // The header lands first, then the columns, so a receiver can size the
    // rest of the payload from what it has already read.
    const uint8_t* payload = buffer_.data() + 4;
    EnvelopeChunkMessage parsed_header;
    memcpy(&parsed_header, payload, sizeof(parsed_header));
    EXPECT_EQ(parsed_header.sample_id, 7);
    EXPECT_EQ(parsed_header.generation, 2);
    EXPECT_EQ(parsed_header.start_frame, 1000u);
    EXPECT_EQ(parsed_header.end_frame, 5000u);
    EXPECT_EQ(parsed_header.total_columns, 96);
    EXPECT_EQ(parsed_header.first_column, 12);
    EXPECT_EQ(parsed_header.columns, kColumns);
    EXPECT_EQ(parsed_header.channels, 2);

    const auto* parsed_columns =
        reinterpret_cast<const EnvelopeColumn*>(payload + sizeof(EnvelopeChunkMessage));
    for (size_t i = 0; i < kColumns * 2; ++i) {
        EXPECT_EQ(parsed_columns[i].min_sample, columns[i].min_sample) << "column " << i;
        EXPECT_EQ(parsed_columns[i].max_sample, columns[i].max_sample) << "column " << i;
    }
}

// A full-width stereo run is 1280 x 2 x 4 = 10240 bytes, well past one packet,
// so an over-large run must be refused rather than truncated into a waveform
// that silently omits its tail.
TEST_F(MessageTypeTest, EnvelopeChunkRefusesAnOversizedRun) {
    EnvelopeChunkMessage header;
    header.channels = 2;
    header.columns = MAX_ENVELOPE_COLUMNS;
    std::vector<EnvelopeColumn> columns(MAX_ENVELOPE_COLUMNS * 2);
    EXPECT_EQ(ProtocolHandler::CreateEnvelopeChunkPacket(
                  buffer_.data(), buffer_.size(), header, columns.data(), columns.size()),
              0u);
}

// Resolve() is what every consumer relies on to turn the 0 sentinels into
// real bounds. If it were wrong, streaming audition, RAM voices and the
// preview would all be wrong together - which is the point of sharing it.
TEST_F(MessageTypeTest, SampleMetadataResolveExpandsSentinels) {
    SampleMetadata m;
    m.total_frames = 1000;
    m.Resolve();
    EXPECT_EQ(m.start_frame, 0u);
    EXPECT_EQ(m.end_frame, 1000u);
    EXPECT_EQ(m.loop_start, 0u);
    EXPECT_EQ(m.loop_end, 1000u);
}

TEST_F(MessageTypeTest, SampleMetadataResolveClampsToTheRegion) {
    SampleMetadata m;
    m.total_frames = 1000;
    m.start_frame = 200;
    m.end_frame = 800;
    m.loop_start = 100;  // before start
    m.loop_end = 900;    // past end
    m.Resolve();
    EXPECT_EQ(m.loop_end, 800u);    // pulled back to end_frame
    EXPECT_EQ(m.loop_start, 200u);  // pushed up to start_frame
}

TEST_F(MessageTypeTest, SampleMetadataResolveRejectsInvertedRegion) {
    SampleMetadata m;
    m.total_frames = 1000;
    m.start_frame = 900;
    m.end_frame = 400;  // before start: nonsense, must not survive
    m.Resolve();
    EXPECT_LT(m.start_frame, m.end_frame);
}

// A zeroed record must not divide by zero or produce a region: an unloaded
// sample is a real state, and every consumer calls Resolve() unconditionally.
TEST_F(MessageTypeTest, SampleMetadataResolveIsSafeWhenEmpty) {
    SampleMetadata m;
    m.Resolve();
    EXPECT_EQ(m.total_frames, 0u);
    EXPECT_EQ(m.end_frame, 0u);
    EXPECT_EQ(m.loop_end, 0u);
}

// Mixer ops (output-routing-and-mixer.md §4). Every op shares one struct, so
// the round trip has to survive each `value` encoding rather than just one.
TEST_F(MessageTypeTest, MixOpMessage) {
    MixOpMessage original(MIX_OP_SET_GAIN, 7, 6000);  // 0 dB on track 7
    size_t created = ProtocolHandler::CreateMixOpPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    MixOpMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(buffer_.data(), MSG_MIX_OP, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.op, MIX_OP_SET_GAIN);
    EXPECT_EQ(parsed.track, 7);
    EXPECT_EQ(parsed.value, 6000);
}

// A full mute mask uses the top bit of `value`, which is the case an
// accidentally-signed field would mangle.
TEST_F(MessageTypeTest, MixOpMuteMaskSurvivesTheTopBit) {
    MixOpMessage original(MIX_OP_SET_MUTE_MASK, 0, 0xFFFF);
    size_t created = ProtocolHandler::CreateMixOpPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);

    MixOpMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(buffer_.data(), MSG_MIX_OP, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.value, 0xFFFF);
    EXPECT_EQ(parsed.op, MIX_OP_SET_MUTE_MASK);
}

TEST_F(MessageTypeTest, MixOpPanExtremesSurvive) {
    for (uint16_t pan: {uint16_t(0), uint16_t(32768), uint16_t(65535)}) {
        MixOpMessage original(MIX_OP_SET_PAN, 3, pan);
        size_t created =
            ProtocolHandler::CreateMixOpPacket(buffer_.data(), buffer_.size(), original);
        ASSERT_GT(created, 0u);
        MixOpMessage parsed;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(buffer_.data(), MSG_MIX_OP, &parsed, sizeof(parsed)));
        EXPECT_EQ(parsed.value, pan);
    }
}

TEST_F(MessageTypeTest, MixMetersMessage) {
    MixMetersMessage original;
    for (uint8_t i = 0; i < WAVEX_MIX_TRACKS; ++i) {
        original.peak[i] = static_cast<uint8_t>(i * 17);
    }
    size_t created =
        ProtocolHandler::CreateMixMetersPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    MixMetersMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_MIX_METERS, &parsed, sizeof(parsed)));
    for (uint8_t i = 0; i < WAVEX_MIX_TRACKS; ++i) {
        EXPECT_EQ(parsed.peak[i], static_cast<uint8_t>(i * 17)) << "track " << int(i);
    }
}

// A default-constructed meter frame must read as silence on every track, not
// as whatever the stack held - the frontend draws these directly.
TEST_F(MessageTypeTest, MixMetersDefaultsToSilence) {
    MixMetersMessage msg;
    for (uint8_t i = 0; i < WAVEX_MIX_TRACKS; ++i) {
        EXPECT_EQ(msg.peak[i], 0) << "track " << int(i);
    }
}

TEST_F(MessageTypeTest, SampleMetaReqMessage) {
    SampleMetaReqMessage original(42);
    size_t created =
        ProtocolHandler::CreateSampleMetaReqPacket(buffer_.data(), buffer_.size(), original);
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));

    SampleMetaReqMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_SAMPLE_META_REQ, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.sample_id, 42);
}

// MSG_SAMPLE_SELECT. id 0 is a meaningful sentinel (clears that slot's
// binding), so both a real id and the sentinel must survive, on more than
// one slot so the field isn't mistaken for padding.
TEST_F(MessageTypeTest, SampleSelectMessage) {
    for (uint16_t id: {uint16_t{0}, uint16_t{7}, uint16_t{0xFFFF}}) {
        for (uint8_t slot: {uint8_t{0}, uint8_t{5}, uint8_t{15}}) {
            SampleSelectMessage original(id, slot);

            size_t created = ProtocolHandler::CreatePacket(
                buffer_.data(), buffer_.size(), MSG_SAMPLE_SELECT, &original, sizeof(original));
            ASSERT_GT(created, 0u);
            EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
            EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_SELECT);

            // Non-matching sentinels so a zero read is a real read.
            SampleSelectMessage parsed(1, 3);
            ASSERT_TRUE(ProtocolHandler::ParseMessage(
                buffer_.data(), MSG_SAMPLE_SELECT, &parsed, sizeof(parsed)));
            EXPECT_EQ(parsed.sample_id, id);
            EXPECT_EQ(parsed.slot, slot);
        }
    }
}

TEST_F(MessageTypeTest, TrackBindingMessagesRoundTrip) {
    for (uint8_t track: {uint8_t{0}, uint8_t{7}, uint8_t{15}, uint8_t{0xFF}}) {
        TrackBindingReqMessage original(track);
        const size_t created =
            ProtocolHandler::CreateTrackBindingReqPacket(buffer_.data(), buffer_.size(), original);
        ASSERT_GT(created, 0u);
        EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
        EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_TRACK_BINDING_REQ);

        TrackBindingReqMessage parsed(0);
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            buffer_.data(), MSG_TRACK_BINDING_REQ, &parsed, sizeof(parsed)));
        EXPECT_EQ(parsed.track, track);
    }

    for (uint8_t state:
         {TRACK_BINDING_EMPTY, TRACK_BINDING_SAMPLE, TRACK_BINDING_PATCH, TRACK_BINDING_LOADING}) {
        TrackBindingMessage original(5, state, state == TRACK_BINDING_SAMPLE ? 42 : 0);
        // The Patch name is the only way the frontend can name an import - an
        // import's samples never arrive as MSG_SAMPLE_META - so it has to
        // survive the round trip, NUL included.
        snprintf(original.name, sizeof(original.name), "GrandPiano.sfz");
        const size_t created =
            ProtocolHandler::CreateTrackBindingPacket(buffer_.data(), buffer_.size(), original);
        ASSERT_GT(created, 0u);
        EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
        EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_TRACK_BINDING);

        TrackBindingMessage parsed;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            buffer_.data(), MSG_TRACK_BINDING, &parsed, sizeof(parsed)));
        EXPECT_EQ(parsed.track, 5);
        EXPECT_EQ(parsed.state, state);
        EXPECT_EQ(parsed.sample_id, state == TRACK_BINDING_SAMPLE ? 42 : 0);
        EXPECT_STREQ(parsed.name, "GrandPiano.sfz");
    }

    // A name filling the field leaves no room for a terminator on the wire, so
    // it must survive as bytes rather than as a C string. Pin that here; the
    // frontend re-terminates its cached copy on receipt
    // (inter_mcu_store_track_binding) so the UI can treat it as a string.
    TrackBindingMessage full(1, TRACK_BINDING_PATCH, 0);
    memset(full.name, 'A', sizeof(full.name));
    const size_t created =
        ProtocolHandler::CreateTrackBindingPacket(buffer_.data(), buffer_.size(), full);
    ASSERT_GT(created, 0u);
    TrackBindingMessage parsed_full;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer_.data(), MSG_TRACK_BINDING, &parsed_full, sizeof(parsed_full)));
    EXPECT_EQ(memcmp(parsed_full.name, full.name, sizeof(full.name)), 0);
}

// MSG_SAMPLE_UNLOAD (previously untested). Unlike SampleSelect, id 0 is
// REJECTED by the receiver rather than treated as a wildcard, so the id
// arriving intact is what stands between "free this sample" and "free
// nothing" (or, with a wildcard-permissive receiver, "free everything").
TEST_F(MessageTypeTest, SampleUnloadMessage) {
    SampleUnloadMessage original(311);

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_SAMPLE_UNLOAD, &original, sizeof(original));
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_SAMPLE_UNLOAD);

    SampleUnloadMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SAMPLE_UNLOAD, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.sample_id, 311);
}

// MSG_CV_CAL_RESP reuses CvCalMessage (the reply to both SET and GET); the
// SET round trip alone does not pin the RESP type byte.
TEST_F(MessageTypeTest, CvCalRespRoundTrip) {
    CvCalMessage original(2, 0, 0.98f, 0.01f, 1.02f, -0.03f, 0.97f, 0.04f, 3.1f);

    size_t created = ProtocolHandler::CreatePacket(
        buffer_.data(), buffer_.size(), MSG_CV_CAL_RESP, &original, sizeof(original));
    ASSERT_GT(created, 0u);
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(buffer_.data(), created));
    EXPECT_EQ(ProtocolHandler::GetMessageType(buffer_.data()), MSG_CV_CAL_RESP);

    CvCalMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_CV_CAL_RESP, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.group, 2);
    EXPECT_FLOAT_EQ(parsed.vcf_cut_gain, 0.98f);
    EXPECT_FLOAT_EQ(parsed.vca_off, 0.04f);
    EXPECT_FLOAT_EQ(parsed.cutoff_k, 3.1f);
}

// ParseMessage's expected-type check is the only thing standing between a
// dispatcher and interpreting one message's bytes as another's - it must
// actually reject a mismatch, not just be a comment.
TEST_F(MessageTypeTest, ParseMessageRejectsMismatchedType) {
    HeartbeatMessage hb(1, 2, 3);
    size_t created = ProtocolHandler::CreateHeartbeatPacket(buffer_.data(), buffer_.size(), hb);
    ASSERT_GT(created, 0u);

    SyncMessage sync_parsed;
    EXPECT_FALSE(
        ProtocolHandler::ParseMessage(buffer_.data(), MSG_SYNC, &sync_parsed, sizeof(sync_parsed)));

    HeartbeatMessage hb_parsed;
    EXPECT_TRUE(ProtocolHandler::ParseMessage(buffer_.data(), hb_parsed));
}

// Known-answer CRC vectors (CRC-16/CCITT-FALSE): consistency checks
// (crc(x) == crc(x)) pass for ANY function of the input, so only fixed
// expected values pin the polynomial, init value, and the deliberate
// empty-input-returns-0 special case.
TEST_F(MessageTypeTest, CrcMatchesKnownAnswerVectors) {
    const CRCTestVectors::TestVector* vectors = CRCTestVectors::GetTestVectors();
    const size_t count = CRCTestVectors::GetTestVectorCount();
    ASSERT_GE(count, 5u);
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(ProtocolHandler::CalculateWaveXCrc(vectors[i].data, vectors[i].length),
                  vectors[i].expected_crc)
            << "vector " << i;
    }
}

// The shared test helpers must agree with the production packet layout -
// this is also the regression test for ExtractPacketComponents' old flags
// extraction, which always produced 0 and made any flags assertion vacuous.
TEST_F(MessageTypeTest, HelperExtractsFlagsSequenceAndPayload) {
    ControlChangeMessage msg(PARAM_FILTER_CUTOFF, 4, 0x1234);
    auto packet = ProtocolTestHelper::CreateWaveXPacket(
        MSG_CONTROL_CHANGE, &msg, sizeof(msg), /*sequence=*/0x0BAD, PKT_FLAG_ACK | PKT_FLAG_NACK);
    ASSERT_FALSE(packet.empty());
    EXPECT_TRUE(ProtocolHandler::ValidatePacket(packet.data(), packet.size()));
    EXPECT_TRUE(ProtocolTestHelper::ValidatePacketStructure(packet.data(), packet.size()));

    uint8_t msg_type = 0;
    uint16_t sequence = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
    ASSERT_TRUE(ProtocolTestHelper::ExtractPacketComponents(
        packet.data(), packet.size(), msg_type, sequence, flags, payload));
    EXPECT_EQ(msg_type, MSG_CONTROL_CHANGE);
    EXPECT_EQ(sequence, 0x0BAD);
    EXPECT_EQ(flags, PKT_FLAG_ACK | PKT_FLAG_NACK);
    // Payload is the packet's whole zero-padded payload region; the message
    // must sit at its head, byte-exact.
    ASSERT_GE(payload.size(), sizeof(msg));
    EXPECT_EQ(memcmp(payload.data(), &msg, sizeof(msg)), 0);
}

// The invalid-packet helpers must actually produce invalid packets.
TEST_F(MessageTypeTest, HelperInvalidPacketsFailValidation) {
    NoteMessage msg(60, 100, 0);
    auto bad_crc = ProtocolTestHelper::CreateInvalidCRCPacket(MSG_NOTE_ON, &msg, sizeof(msg));
    ASSERT_FALSE(bad_crc.empty());
    EXPECT_FALSE(ProtocolHandler::ValidatePacket(bad_crc.data(), bad_crc.size()));

    auto malformed = ProtocolTestHelper::CreateMalformedPacket();
    EXPECT_FALSE(ProtocolHandler::ValidatePacket(malformed.data(), malformed.size()));
    EXPECT_FALSE(ProtocolTestHelper::ValidatePacketStructure(malformed.data(), malformed.size()));
}

// A frame smaller than header (4) + CRC (2) must be rejected up front:
// buffer_size - 2 on a 0- or 1-byte buffer underflows size_t, turning the
// CRC pass into a ~SIZE_MAX-byte scan with out-of-bounds reads.
TEST_F(MessageTypeTest, ValidateRejectsBuffersSmallerThanMinimumFrame) {
    uint8_t tiny[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    EXPECT_FALSE(ProtocolHandler::ValidatePacket(nullptr, sizeof(tiny)));
    for (size_t size = 0; size < 6; ++size) {
        EXPECT_FALSE(ProtocolHandler::ValidatePacket(tiny, size)) << "size " << size;
    }
}

// The August 2026 hardening added a minimum-size guard to ValidateWaveXPacket
// and left the two "legacy" CRC entry points 120 lines away with the same
// size_t underflow (`packet_size - 2` wrapping to ~SIZE_MAX). A test written
// against one function could not catch that; this one sweeps EVERY public
// entry point that takes a raw (buffer, size) pair, so a new one added
// without a guard - or an old one that drifts - fails here.
//
// The buffer is a heap allocation sized EXACTLY to the length under test, not
// a fixed stack array: an overread off a stack array lands in adjacent stack
// and reads a plausible byte, whereas one byte past a heap allocation is an
// ASan redzone. Under `make test-asan` this turns the whole sweep into an
// out-of-bounds detector rather than a return-value check.
TEST_F(MessageTypeTest, EveryRawBufferEntryPointRejectsUndersizedFrames) {
    // Smallest legal frame is a 4-byte header plus a 2-byte CRC.
    constexpr size_t kMinFrame = 6;

    for (size_t size = 0; size < kMinFrame; ++size) {
        std::vector<uint8_t> exact(size, 0xA5);
        const uint8_t* data = exact.empty() ? nullptr : exact.data();

        // A null buffer must be rejected at every size, including plausible ones.
        EXPECT_FALSE(ProtocolHandler::ValidatePacket(nullptr, size)) << "size " << size;
        EXPECT_FALSE(ProtocolHandler::ValidatePacketCrc(nullptr, size)) << "size " << size;
        EXPECT_EQ(ProtocolHandler::CalculatePacketCrc(nullptr, size), 0) << "size " << size;

        if (!data) {
            continue;
        }

        EXPECT_FALSE(ProtocolHandler::ValidatePacket(data, size)) << "size " << size;
        EXPECT_FALSE(ProtocolHandler::ValidatePacketCrc(data, size)) << "size " << size;

        // CalculatePacketCrc is the lower-level primitive - "CRC every byte
        // but the trailing 2" - so a 2..5 byte buffer is a legal, if useless,
        // call that reads entirely in bounds. Only a size under 2 underflows.
        // Below that it must return the sentinel; at or above it the contract
        // is simply "does not read outside the buffer", and the sanitizer is
        // the assertion. Calling it here is what puts it under ASan's watch.
        const uint16_t crc = ProtocolHandler::CalculatePacketCrc(data, size);
        if (size < sizeof(uint16_t)) {
            EXPECT_EQ(crc, 0) << "size " << size;
        }
    }
}

// A null buffer paired with a large, entirely plausible size is the other half
// of the guard - the size check alone would let it through to a CRC pass over
// a null pointer.
TEST_F(MessageTypeTest, RawBufferEntryPointsRejectNullWithPlausibleSize) {
    for (size_t size: {size_t{6}, size_t{64}, size_t{1024}}) {
        EXPECT_FALSE(ProtocolHandler::ValidatePacket(nullptr, size)) << "size " << size;
        EXPECT_FALSE(ProtocolHandler::ValidatePacketCrc(nullptr, size)) << "size " << size;
        EXPECT_EQ(ProtocolHandler::CalculatePacketCrc(nullptr, size), 0) << "size " << size;
    }
}
