#include "usb_midi_host.h"

#include "config/hardware_config.h"
#include "usb_midi_port_internal.h"

#include "usb_midi_protocol.hpp"

#if WAVEX_ESP_USB_MIDI_ENABLED && (WAVEX_USB_MIDI_INPUT_ENABLED || WAVEX_USB_MIDI_OUTPUT_ENABLED)
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "midi_out.h"
#include "midi_task.h"
#include "usb/usb_host.h"

#include "midi/clock_input.hpp"
#include <cstring>

namespace wavex_midi {
namespace {
constexpr char tag[] = "usb_midi_host";
// Application lifetime: the selected stack never changes until reboot. All
// client, endpoint and buffer state below belongs to this one task. IDF owns
// DMA allocation/cache maintenance; buffers are touched only after completion.

#ifndef WAVEX_TEST_BUILD
Host host;
#endif
}  // namespace

void Host::Event(const usb_host_client_event_msg_t* event, void* arg) {
    auto& self = *static_cast<Host*>(arg);
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        self.address_ = event->new_dev.address;
    else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
             event->dev_gone.dev_hdl == self.device_) {
        self.closing_ = true;
        SetUsbConnection(UsbConnection::Waiting);
    }
}
void Host::Fault() {
    closing_ = true;
    SetUsbConnection(UsbConnection::Error);
    ESP_LOGW(tag, "USB MIDI transfer failed; reconnect adapter to retry");
}
void Host::Transfer(usb_transfer_t* transfer) {
    auto& self = *static_cast<Host*>(transfer->context);
    if (transfer == self.rx_) {
        self.rx_pending_ = false;
        self.received_at_ = static_cast<uint32_t>(esp_timer_get_time());
        self.rx_ready_ = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
    } else {
        self.tx_pending_ = false;
        Complete(
            Port::Usb,
            transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes == 4);
    }
    // Do not forward MIDI, resubmit or release buffers from a callback.
    if (!self.closing_ && transfer->status != USB_TRANSFER_STATUS_COMPLETED)
        self.Fault();
}
void Host::Open(uint8_t address) {
    if (usb_host_device_open(client_, address, &device_) != ESP_OK) {
        device_ = nullptr;
        SetUsbConnection(UsbConnection::Waiting);
        return;
    }
    const usb_config_desc_t* config = nullptr;
    if (usb_host_get_active_config_descriptor(device_, &config) != ESP_OK ||
        !FindUsbMidiInterface(reinterpret_cast<const uint8_t*>(config),
                              config ? config->wTotalLength : 0,
                              interface_)) {
        closing_ = true;
        SetUsbConnection(UsbConnection::Unsupported);
        return;
    }
    if (usb_host_interface_claim(client_, device_, interface_.number, 0) != ESP_OK) {
        Fault();
        return;
    }
    claimed_ = true;
    rx_->device_handle = tx_->device_handle = device_;
    rx_->bEndpointAddress = interface_.in;
    rx_->num_bytes = interface_.in_size;
    tx_->bEndpointAddress = interface_.out;
    tx_->num_bytes = 4;
    parser_.Reset();
    clock_.Reset();
    if (usb_host_transfer_submit(rx_) != ESP_OK) {
        Fault();
        return;
    }
    rx_pending_ = true;
    Ready(Port::Usb, interface_.out && WAVEX_USB_MIDI_OUTPUT_ENABLED);
    SetUsbConnection(UsbConnection::Connected);
    ESP_LOGI(tag, "MIDI 1.0 connected, interface %u, cable 0", interface_.number);
}
void Host::Receive() {
    if (!rx_ready_)
        return;
    rx_ready_ = false;
    // An event packet is indivisible. Drop malformed transfers rather than
    // letting trailing bytes become the next message's status or note.
    if (rx_->actual_num_bytes < 0 || rx_->actual_num_bytes > rx_->num_bytes ||
        rx_->actual_num_bytes % 4) {
        Fault();
        return;
    }
#if WAVEX_USB_MIDI_INPUT_ENABLED
    for (int pos = 0; pos < rx_->actual_num_bytes; pos += 4) {
        const auto* packet = rx_->data_buffer + pos;
        if (packet[0] >> 4)
            continue;  // first cable only; never combine independent parsers
        const auto size = UsbMidiPacketSize(packet);
        if (!size) {
            parser_.Reset();
            clock_.Reset();
            continue;
        }
        for (unsigned i = 1; i <= size; ++i) {
            WaveX::Protocol::MidiClockEventMessage clock_event;
            if (clock_.Feed(packet[i], received_at_, clock_event))
                inter_mcu_send_midi_clock(clock_event);
            WaveX::Midi::Event event;
            if (!parser_.Feed(packet[i], event))
                continue;
            auto& held = held_[event.channel][event.data1];
            if (event.type == WaveX::Midi::EventType::NoteOn) {
                if (held != 255 &&
                    inter_mcu_send_note_on_midi(event.data1, event.data2, event.channel) == ESP_OK)
                    ++held;
            } else if (event.type == WaveX::Midi::EventType::NoteOff) {
                if (held && inter_mcu_send_note_off_midi(event.data1, event.channel) == ESP_OK)
                    --held;
            } else {
                midi_forward_event(event);
            }
        }
    }
#endif
    if (usb_host_transfer_submit(rx_) == ESP_OK)
        rx_pending_ = true;
    else
        Fault();
}
void Host::Close() {
    Ready(Port::Usb, false);
    rx_ready_ = false;
    // Halt then flush; callbacks retire in-flight DMA before interface release.
    // On removal IDF may already have halted the endpoints. A failed halt does
    // not authorize freeing a pending transfer; keep pumping until it retires.
    if (!flushed_) {
        if (claimed_) {
            usb_host_endpoint_halt(device_, interface_.in);
            usb_host_endpoint_flush(device_, interface_.in);
            if (interface_.out) {
                usb_host_endpoint_halt(device_, interface_.out);
                usb_host_endpoint_flush(device_, interface_.out);
            }
        }
        flushed_ = true;
    }
    // Bound cleanup traffic; other input and UI retain inter-MCU access.
    unsigned budget = 8;
    while (release_cursor_ < 16 * 128 && budget) {
        auto& count = held_[release_cursor_ / 128][release_cursor_ % 128];
        if (count) {
            if (inter_mcu_send_note_off_midi(static_cast<uint8_t>(release_cursor_ % 128),
                                             static_cast<uint8_t>(release_cursor_ / 128)) != ESP_OK)
                return;
            --count;
            --budget;
        } else {
            ++release_cursor_;
        }
    }
    if (rx_pending_ || tx_pending_ || release_cursor_ < 16 * 128)
        return;
    if (claimed_) {
        if (usb_host_interface_release(client_, device_, interface_.number) != ESP_OK)
            return;
        claimed_ = false;
    }
    if (usb_host_device_close(client_, device_) != ESP_OK)
        return;
    device_ = nullptr;
    closing_ = flushed_ = false;
    release_cursor_ = 0;
    interface_ = {};
    parser_.Reset();
    clock_.Reset();
}
esp_err_t Host::Init() {
    usb_host_config_t config{};
    config.root_port_unpowered = true;
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    auto result = usb_host_install(&config);
    const bool installed = result == ESP_OK;
    usb_host_client_config_t client_config{};
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = Event;
    client_config.async.callback_arg = this;
    if (result == ESP_OK)
        result = usb_host_client_register(&client_config, &client_);
    if (result == ESP_OK)
        result = usb_host_transfer_alloc(512, 0, &rx_);
    if (result == ESP_OK)
        result = usb_host_transfer_alloc(4, 0, &tx_);
    if (result == ESP_OK)
        result = usb_host_lib_set_root_port_power(true);
    if (result != ESP_OK) {
        ESP_LOGE(tag, "host init failed: %s", esp_err_to_name(result));
        usb_host_transfer_free(rx_);
        usb_host_transfer_free(tx_);
        if (client_)
            usb_host_client_deregister(client_);
        if (installed) {
            usb_host_device_free_all();
            uint32_t flags = 0;
            usb_host_lib_handle_events(0, &flags);
            usb_host_uninstall();
        }
        SetUsbConnection(UsbConnection::Error);
        return result;
    }
    rx_->callback = tx_->callback = Transfer;
    rx_->context = tx_->context = this;
    SetUsbConnection(UsbConnection::Waiting);
    return ESP_OK;
}
void Host::Service() {
    uint32_t flags = 0;
    usb_host_lib_handle_events(0, &flags);
    usb_host_client_handle_events(client_, 0);
    if (closing_) {
        Close();
    } else if (!device_ && address_) {
        const auto address = address_;
        address_ = 0;
        Open(address);
    } else if (device_) {
        Receive();
#if WAVEX_USB_MIDI_OUTPUT_ENABLED
        if (!closing_ && interface_.out && !tx_pending_) {
            WaveX::Midi::ClockPacket packet;
            if (Take(Port::Usb, packet)) {
                const auto bytes = packet.Usb();
                std::memcpy(tx_->data_buffer, bytes.data(), bytes.size());
                if (usb_host_transfer_submit(tx_) == ESP_OK)
                    tx_pending_ = true;
                else {
                    Complete(Port::Usb, false);
                    Fault();
                }
            }
        }
#endif
    }
}

#ifndef WAVEX_TEST_BUILD
esp_err_t StartUsbHost() {
    return xTaskCreate(
               [](void*) {
                   if (host.Init() != ESP_OK) {
                       vTaskDelete(nullptr);
                       return;
                   }
                   for (;;) {
                       host.Service();
                       vTaskDelay(1);
                   }
               },
               "usb_midi_host",
               WAVEX_USB_MIDI_TASK_STACK_SIZE,
               nullptr,
               WAVEX_USB_MIDI_TASK_PRIORITY,
               nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
#endif
}  // namespace wavex_midi
#else
namespace wavex_midi {
esp_err_t StartUsbHost() {
    return ESP_OK;
}
}  // namespace wavex_midi
#endif
