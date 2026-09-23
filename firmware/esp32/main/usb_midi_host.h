#pragma once
#include "usb/usb_host.h"

#include "midi/clock_input.hpp"
#include "midi/midi_stream_parser.hpp"
#include "usb_midi_protocol.hpp"

namespace wavex_midi {
class Host {
   public:
    esp_err_t Init();
    void Service();

   private:
    static void Event(const usb_host_client_event_msg_t* event, void* arg);
    static void Transfer(usb_transfer_t* transfer);
    void Open(uint8_t address);
    void Receive();
    void Close();
    void Fault();
    usb_host_client_handle_t client_ = nullptr;
    usb_device_handle_t device_ = nullptr;
    usb_transfer_t *rx_ = nullptr, *tx_ = nullptr;
    UsbMidiInterface interface_{};
    bool claimed_ = false, closing_ = false, flushed_ = false;
    bool rx_pending_ = false, tx_pending_ = false, rx_ready_ = false;
    uint8_t address_ = 0;
    uint32_t received_at_ = 0;
    WaveX::Midi::StreamParser parser_;
    WaveX::Midi::ClockInput clock_{1};
    // Only admitted presses count. Normal releases and disconnect cleanup
    // hand ownership to the existing retry-capable note-delivery service.
    uint8_t held_[16][128]{};
    unsigned release_cursor_ = 0;
};
}  // namespace wavex_midi
