// The EnvelopePanel's transport on the target: the inter-MCU link.
//
// Split out of envelope_panel.cpp for the same reason envelope_cache_init.cpp
// is split out of envelope_cache.cpp: the panel is compiled for the host
// test suite, where inter_mcu does not exist, and it takes its link injected
// precisely so it can be tested with a fake one. This is the one place that
// knows the link is the UART.

#include "envelope_panel.h"
#include "inter_mcu.h"

namespace wavex_ui {
namespace {

bool SendEnvelopeReq(uint16_t sample_id, uint16_t columns, uint32_t start, uint32_t end) {
    return inter_mcu_send_envelope_req(sample_id, columns, start, end) == ESP_OK;
}

void ListenForChunks(EnvelopePanel::ChunkCb cb, void* user) {
    inter_mcu_set_envelope_chunk_listener(cb, user);
}

}  // namespace

EnvelopePanel::Link EspEnvelopeLink() {
    EnvelopePanel::Link link;
    link.send = &SendEnvelopeReq;
    link.listen = &ListenForChunks;
    link.request_cursor = &inter_mcu_request_sample_playhead;
    link.read_cursor = &inter_mcu_get_sample_playhead;
    return link;
}

}  // namespace wavex_ui
