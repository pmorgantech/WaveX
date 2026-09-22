#pragma once
#include <stdint.h>

typedef struct {
    uint32_t sync_packets;
    uint32_t control_change_packets;
    uint32_t note_on_packets;
    uint32_t note_off_packets;
    uint32_t sample_load_packets;
    uint32_t sample_data_packets;
    uint32_t parameter_update_packets;
    uint32_t status_request_packets;
    uint32_t status_response_packets;
    uint32_t sample_ctrl_packets;
    uint32_t data_request_packets;
    uint32_t meter_push_packets;
    uint32_t envelope_chunk_packets;
    uint32_t heartbeat_packets;
    uint32_t diag_push_packets;
    uint32_t error_packets;
    // Recognised message types with no counter of their own (the 0x30/0x40
    // response blocks). Separate from unknown_packets so that counter keeps
    // its diagnostic meaning: a non-zero UNKNOWN should mean corruption or a
    // version mismatch, not "the frontend has no bucket for browse replies".
    uint32_t other_known_packets;
    uint32_t unknown_packets;
    uint32_t total_packets;
    uint32_t invalid_packets;
} wavex_packet_stats_t;

typedef struct {
    uint32_t total_messages_sent;
    uint32_t ping_messages_sent;
    uint32_t test_messages_sent;
    uint32_t last_send_time;
} wavex_tx_stats_t;

typedef struct {
    uint32_t total_packets;
    uint32_t meter_packets;
    uint32_t heartbeat_packets;
    uint32_t control_packets;
    uint32_t invalid_packets;
} wavex_packet_summary_t;

typedef struct {
    float rms_left;
    float rms_right;
    float peak_left;
    float peak_right;
    uint32_t last_update_ms;
    bool valid;
} wavex_meter_data_t;
