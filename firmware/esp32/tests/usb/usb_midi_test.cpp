#include <gtest/gtest.h>

#include "esp_timer.h"
#include "midi_out.h"
#include "midi_task.h"
#include "nvs.h"
#include "usb_midi_host.h"
#include "usb_midi_port_internal.h"
#include "usb_midi_task.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

using namespace wavex_midi;
namespace {
std::vector<uint8_t> Descriptor(bool output = true) {
    std::vector<uint8_t> d = {9,
                              2,
                              0,
                              0,
                              2,
                              1,
                              0,
                              0x80,
                              50,
                              9,
                              4,
                              0,
                              0,
                              0,
                              1,
                              1,
                              0,
                              0,  // Audio control, not a MIDI
                                  // endpoint
                              9,
                              4,
                              3,
                              0,
                              2,
                              1,
                              3,
                              0,
                              0,
                              7,
                              0x24,
                              1,
                              0,
                              1,
                              7,
                              0,
                              9,
                              5,
                              0x81,
                              2,
                              64,
                              0,
                              0,
                              0,
                              0};
    if (output)
        d.insert(d.end(), {9, 5, 2, 2, 64, 0, 0, 0, 0});
    d[2] = static_cast<uint8_t>(d.size());
    return d;
}
std::vector<uint8_t> descriptor;
usb_host_client_config_t client_config;
std::vector<usb_host_client_event_msg_t> events;
std::vector<usb_transfer_t*> allocated, pending;
std::vector<uint8_t> notes_on, notes_off;
std::vector<WaveX::Midi::Event> expression;
std::vector<WaveX::Protocol::MidiClockEventMessage> clocks;
int releases, closes, submits, frees, installs, uninstalls, device_starts, host_starts;
int fail_alloc, alloc_calls;
bool reject_on, reject_off, output_ready, tx_queued, tx_success, fail_claim, fail_submit;
esp_err_t nvs_result, commit_result;
int stored, staged;

void CompleteTransfer(usb_transfer_t* t,
                      usb_transfer_status_t status,
                      std::initializer_list<uint8_t> data = {}) {
    ASSERT_NE(std::find(pending.begin(), pending.end(), t), pending.end());
    pending.erase(std::remove(pending.begin(), pending.end(), t), pending.end());
    t->status = status;
    t->actual_num_bytes = static_cast<int>(data.size());
    std::copy(data.begin(), data.end(), t->data_buffer);
    t->callback(t);
}
class UsbMidi : public testing::Test {
   protected:
    Host host;
    void SetUp() override {
        descriptor = Descriptor();
        events.clear();
        pending.clear();
        allocated.clear();
        notes_on.clear();
        notes_off.clear();
        expression.clear();
        clocks.clear();
        releases = closes = submits = frees = installs = uninstalls = 0;
        device_starts = host_starts = alloc_calls = 0;
        fail_alloc = -1;
        reject_on = reject_off = output_ready = tx_queued = tx_success = fail_claim = fail_submit =
            false;
        nvs_result = commit_result = ESP_OK;
        stored = staged = -1;
        ASSERT_EQ(StartUsbPort(), ESP_OK);
    }
    void TearDown() override {
        // Test-owned replacement for the reboot boundary, never production teardown.
        for (auto* t: allocated) {
            delete[] t->data_buffer;
            delete t;
        }
        allocated.clear();
    }
    void Attach() {
        usb_host_client_event_msg_t event{};
        event.event = USB_HOST_CLIENT_EVENT_NEW_DEV;
        event.new_dev.address = 1;
        events.push_back(event);
        host.Service();
    }
    void Disconnect() {
        usb_host_client_event_msg_t event{};
        event.event = USB_HOST_CLIENT_EVENT_DEV_GONE;
        event.dev_gone.dev_hdl = this;
        events.push_back(event);
        host.Service();
    }
};
}  // namespace
const char* esp_err_to_name(esp_err_t) {
    return "injected";
}
int64_t esp_timer_get_time() {
    return 1000;
}
esp_err_t usb_host_install(const usb_host_config_t* c) {
    EXPECT_TRUE(c->root_port_unpowered);
    ++installs;
    return ESP_OK;
}
esp_err_t usb_host_uninstall() {
    ++uninstalls;
    return ESP_OK;
}
esp_err_t usb_host_client_register(const usb_host_client_config_t* c,
                                   usb_host_client_handle_t* out) {
    client_config = *c;
    *out = &client_config;
    return ESP_OK;
}
esp_err_t usb_host_client_deregister(usb_host_client_handle_t) {
    return ESP_OK;
}
esp_err_t usb_host_transfer_alloc(size_t n, int, usb_transfer_t** out) {
    if (alloc_calls++ == fail_alloc)
        return ESP_ERR_NO_MEM;
    auto* t = new usb_transfer_t{};
    t->data_buffer = new uint8_t[n]{};
    t->data_buffer_size = n;
    allocated.push_back(t);
    *out = t;
    return ESP_OK;
}
esp_err_t usb_host_transfer_free(usb_transfer_t* t) {
    if (!t)
        return ESP_OK;
    EXPECT_EQ(std::find(pending.begin(), pending.end(), t), pending.end());
    allocated.erase(std::remove(allocated.begin(), allocated.end(), t), allocated.end());
    delete[] t->data_buffer;
    delete t;
    ++frees;
    return ESP_OK;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t* t) {
    if (fail_submit)
        return ESP_FAIL;
    EXPECT_EQ(std::find(pending.begin(), pending.end(), t), pending.end());
    EXPECT_LE(static_cast<size_t>(t->num_bytes), t->data_buffer_size);
    pending.push_back(t);
    ++submits;
    return ESP_OK;
}
esp_err_t usb_host_lib_set_root_port_power(bool) {
    return ESP_OK;
}
esp_err_t usb_host_device_free_all() {
    return ESP_OK;
}
esp_err_t usb_host_lib_handle_events(uint32_t, uint32_t* flags) {
    *flags = 0;
    return ESP_OK;
}
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, uint32_t) {
    for (auto e: events) {
        // The fake device is the current fixture; use it for both event and open.
        if (e.event == USB_HOST_CLIENT_EVENT_DEV_GONE)
            e.dev_gone.dev_hdl = &client_config;
        client_config.async.client_event_callback(&e, client_config.async.callback_arg);
    }
    events.clear();
    return ESP_OK;
}
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t* out) {
    *out = &client_config;
    return ESP_OK;
}
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t) {
    EXPECT_TRUE(pending.empty());
    ++closes;
    return ESP_OK;
}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t,
                                                const usb_config_desc_t** out) {
    *out = reinterpret_cast<const usb_config_desc_t*>(descriptor.data());
    return ESP_OK;
}
esp_err_t usb_host_interface_claim(usb_host_client_handle_t,
                                   usb_device_handle_t,
                                   uint8_t n,
                                   uint8_t alt) {
    EXPECT_EQ(n, 3);
    EXPECT_EQ(alt, 0);
    return fail_claim ? ESP_FAIL : ESP_OK;
}
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t) {
    EXPECT_TRUE(pending.empty());
    ++releases;
    return ESP_OK;
}
esp_err_t usb_host_endpoint_halt(usb_device_handle_t, uint8_t) {
    return ESP_OK;
}
esp_err_t usb_host_endpoint_flush(usb_device_handle_t, uint8_t) {
    // Deliberately asynchronous: tests decide when cancellation retires DMA.
    return ESP_OK;
}
esp_err_t inter_mcu_send_note_on_midi(uint8_t note, uint8_t, uint8_t) {
    if (reject_on)
        return ESP_FAIL;
    notes_on.push_back(note);
    return ESP_OK;
}
esp_err_t inter_mcu_send_note_off_midi(uint8_t note, uint8_t) {
    if (reject_off)
        return ESP_FAIL;
    notes_off.push_back(note);
    return ESP_OK;
}
esp_err_t inter_mcu_send_midi_clock(const WaveX::Protocol::MidiClockEventMessage& e) {
    clocks.push_back(e);
    return ESP_OK;
}
void midi_forward_event(const WaveX::Midi::Event& e) {
    expression.push_back(e);
}
namespace wavex_midi {
void Ready(Port, bool value) {
    output_ready = value;
}
bool Take(Port, WaveX::Midi::ClockPacket& p) {
    if (!tx_queued || !output_ready)
        return false;
    tx_queued = false;
    p.bytes[0] = 0xf8;
    p.size = 1;
    return true;
}
void Complete(Port, bool value) {
    tx_success = value;
}
esp_err_t StartUsbHost() {
    ++host_starts;
    return ESP_OK;
}
}  // namespace wavex_midi
extern "C" esp_err_t usb_midi_task_start() {
    ++device_starts;
    return ESP_OK;
}
esp_err_t nvs_flash_init() {
    return nvs_result;
}
esp_err_t nvs_open(const char*, int, nvs_handle_t* h) {
    *h = 1;
    return nvs_result;
}
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t* value) {
    if (stored < 0)
        return ESP_ERR_NVS_NOT_FOUND;
    *value = static_cast<uint8_t>(stored);
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t value) {
    staged = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    if (commit_result == ESP_OK)
        stored = staged;
    return commit_result;
}
void nvs_close(nvs_handle_t) {}

TEST_F(UsbMidi, SelectsStreamingInterfaceAndRejectsAllTruncations) {
    UsbMidiInterface result;
    ASSERT_TRUE(FindUsbMidiInterface(descriptor.data(), descriptor.size(), result));
    EXPECT_EQ(result.number, 3);
    EXPECT_EQ(result.in, 0x81);
    EXPECT_EQ(result.out, 2);
    for (size_t n = 0; n < descriptor.size(); ++n) {
        auto exact = std::make_unique<uint8_t[]>(n);
        std::copy_n(descriptor.data(), n, exact.get());
        EXPECT_FALSE(FindUsbMidiInterface(exact.get(), n, result)) << n;
    }
    descriptor[18] = 0;
    EXPECT_FALSE(FindUsbMidiInterface(descriptor.data(), descriptor.size(), result));
}
TEST_F(UsbMidi, RejectsUmpAndDoesNotBorrowAnotherInterfacesEndpoints) {
    UsbMidiInterface result;
    descriptor[31] = 2;
    EXPECT_FALSE(FindUsbMidiInterface(descriptor.data(), descriptor.size(), result));
    descriptor = Descriptor();
    descriptor[34] = 1;
    EXPECT_FALSE(FindUsbMidiInterface(descriptor.data(), descriptor.size(), result));
    descriptor = Descriptor();
    descriptor[23] = 0xff;
    EXPECT_FALSE(FindUsbMidiInterface(descriptor.data(), descriptor.size(), result));
    descriptor = Descriptor(false);
    descriptor.insert(descriptor.end(), {9, 4, 4, 0, 1, 0xff, 0, 0, 0, 7, 5, 2, 2, 64, 0, 0});
    descriptor[2] = static_cast<uint8_t>(descriptor.size());
    ASSERT_TRUE(FindUsbMidiInterface(descriptor.data(), descriptor.size(), result));
    EXPECT_EQ(result.out, 0);  // Vendor interface's OUT is not ours.
}
TEST_F(UsbMidi, MalformedPacketCannotBecomeAnUnrelatedNote) {
    const uint8_t invalid[][4] = {{0, 0x90, 60, 100},
                                  {9, 0xb0, 60, 100},
                                  {9, 0x90, 0xff, 100},
                                  {2, 0xf2, 1, 0},
                                  {3, 0xf2, 0xff, 0},
                                  {5, 0x90, 0, 0},
                                  {6, 0, 0, 0},
                                  {4, 0x90, 1, 2}};
    for (const auto& packet: invalid)
        EXPECT_EQ(UsbMidiPacketSize(packet), 0);
    const uint8_t sysex[] = {7, 0xf0, 1, 0xf7};
    EXPECT_EQ(UsbMidiPacketSize(sysex), 3);
}
TEST_F(UsbMidi, FailedResubmitClosesSessionAndClearsOutputReadiness) {
    ASSERT_EQ(host.Init(), ESP_OK);
    Attach();
    CompleteTransfer(allocated[0], USB_TRANSFER_STATUS_COMPLETED, {9, 0x90, 60, 100});
    fail_submit = true;
    host.Service();
    host.Service();
    EXPECT_EQ(ReadUsbPort().connection, UsbConnection::Error);
    EXPECT_FALSE(output_ready);
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(notes_off, std::vector<uint8_t>{60});
}
TEST_F(UsbMidi, InputOnlyDeviceWorksAndPacketsKeepCableAndLengths) {
    descriptor = Descriptor(false);
    ASSERT_EQ(host.Init(), ESP_OK);
    Attach();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_FALSE(output_ready);
    CompleteTransfer(allocated[0],
                     USB_TRANSFER_STATUS_COMPLETED,
                     {0x09, 0x90, 60,   100,  0x19, 0x90, 61,   100,  0x0c, 0xc2,
                      11,   127,  0x0f, 0xf8, 0,    0,    0x09, 0x90, 60,   0});
    host.Service();
    EXPECT_EQ(notes_on, std::vector<uint8_t>{60});
    EXPECT_EQ(notes_off, std::vector<uint8_t>{60});
    ASSERT_EQ(expression.size(), 1u);
    EXPECT_EQ(expression[0].channel, 2);
    EXPECT_EQ(expression[0].data1, 11);
    EXPECT_EQ(clocks.size(), 1u);
}
TEST_F(UsbMidi, DisconnectWaitsForDmaAndReleasesHeldNotesBeforeReconnect) {
    ASSERT_EQ(host.Init(), ESP_OK);
    Attach();
    CompleteTransfer(
        allocated[0], USB_TRANSFER_STATUS_COMPLETED, {9, 0x90, 60, 100, 9, 0x90, 61, 100});
    host.Service();
    tx_queued = true;
    host.Service();
    ASSERT_EQ(pending.size(), 2u);
    reject_off = true;
    Disconnect();
    EXPECT_EQ(releases, 0);
    EXPECT_EQ(closes, 0);
    EXPECT_FALSE(output_ready);
    CompleteTransfer(allocated[0], USB_TRANSFER_STATUS_CANCELED);
    host.Service();
    EXPECT_EQ(closes, 0);
    CompleteTransfer(allocated[1], USB_TRANSFER_STATUS_CANCELED);
    host.Service();
    EXPECT_EQ(closes, 0);
    reject_off = false;
    host.Service();
    EXPECT_EQ(notes_off, (std::vector<uint8_t>{60, 61}));
    EXPECT_EQ(releases, 1);
    EXPECT_EQ(closes, 1);
    Attach();
    EXPECT_EQ(ReadUsbPort().connection, UsbConnection::Connected);
    EXPECT_EQ(allocated.size(), 2u);
    EXPECT_EQ(pending.size(), 1u);
}
TEST_F(UsbMidi, RejectedPressHasNoDisconnectReleaseAndMalformedTransferIsDropped) {
    ASSERT_EQ(host.Init(), ESP_OK);
    Attach();
    reject_on = true;
    CompleteTransfer(allocated[0], USB_TRANSFER_STATUS_COMPLETED, {9, 0x90, 60, 100});
    host.Service();
    CompleteTransfer(allocated[0], USB_TRANSFER_STATUS_COMPLETED, {9, 0x90, 61});
    host.Service();
    EXPECT_EQ(ReadUsbPort().connection, UsbConnection::Error);
    host.Service();
    EXPECT_TRUE(notes_off.empty());
    EXPECT_TRUE(notes_on.empty());
    EXPECT_EQ(closes, 1);
}
TEST_F(UsbMidi, ClaimAndAllocationFailuresCleanUpWithoutTransfers) {
    fail_alloc = 1;
    EXPECT_EQ(host.Init(), ESP_ERR_NO_MEM);
    EXPECT_EQ(frees, 1);
    EXPECT_EQ(uninstalls, 1);
    EXPECT_TRUE(pending.empty());
    Host another;
    fail_alloc = -1;
    ASSERT_EQ(another.Init(), ESP_OK);
    fail_claim = true;
    usb_host_client_event_msg_t event{};
    event.event = USB_HOST_CLIENT_EVENT_NEW_DEV;
    event.new_dev.address = 1;
    events.push_back(event);
    another.Service();
    another.Service();
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(releases, 0);
}
TEST_F(UsbMidi, ClockOutputCompletesOnlyAfterUsbTransfer) {
    ASSERT_EQ(host.Init(), ESP_OK);
    Attach();
    tx_queued = true;
    host.Service();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_FALSE(tx_success);
    EXPECT_EQ(allocated[1]->data_buffer[0], 15);
    EXPECT_EQ(allocated[1]->data_buffer[1], 0xf8);
    CompleteTransfer(allocated[1], USB_TRANSFER_STATUS_COMPLETED, {15, 0xf8, 0, 0});
    EXPECT_TRUE(tx_success);
}
TEST_F(UsbMidi, SavedModeAppliesOnlyAtRestartAndFailedCommitPreservesSelection) {
    EXPECT_EQ(device_starts, 1);
    EXPECT_EQ(host_starts, 0);
    ASSERT_TRUE(SaveUsbMode(UsbMode::Host));
    EXPECT_FALSE(SaveUsbMode(UsbMode::Device));
    EXPECT_TRUE(ReadUsbPort().saving);
    EXPECT_EQ(stored, -1);
    ServiceUsbSettings();
    EXPECT_EQ(stored, 1);
    EXPECT_EQ(ReadUsbPort().saved, UsbMode::Host);
    EXPECT_EQ(ReadUsbPort().active, UsbMode::Device);
    EXPECT_EQ(host_starts, 0);
    ASSERT_EQ(StartUsbPort(), ESP_OK);
    EXPECT_EQ(host_starts, 1);
    ASSERT_TRUE(SaveUsbMode(UsbMode::Device));
    commit_result = ESP_FAIL;
    ServiceUsbSettings();
    EXPECT_EQ(ReadUsbPort().saved, UsbMode::Host);
    EXPECT_EQ(ReadUsbPort().save_result, ESP_FAIL);
    EXPECT_FALSE(ReadUsbPort().saving);
    commit_result = ESP_OK;
    ASSERT_TRUE(SaveUsbMode(UsbMode::Device));
    ServiceUsbSettings();
    EXPECT_EQ(ReadUsbPort().saved, UsbMode::Device);
}
TEST_F(UsbMidi, InvalidOrUnreadableStoredModeFallsBackWithoutErasingNvs) {
    stored = 7;
    ASSERT_EQ(StartUsbPort(), ESP_OK);
    EXPECT_EQ(ReadUsbPort().active, UsbMode::Device);
    EXPECT_EQ(ReadUsbPort().save_result, ESP_ERR_INVALID_RESPONSE);
    EXPECT_EQ(stored, 7);
    nvs_result = ESP_FAIL;
    ASSERT_EQ(StartUsbPort(), ESP_OK);
    EXPECT_EQ(ReadUsbPort().save_result, ESP_FAIL);
    EXPECT_EQ(stored, 7);
    EXPECT_FALSE(SaveUsbMode(static_cast<UsbMode>(7)));
}
