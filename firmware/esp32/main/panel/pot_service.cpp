#include "pot_service.h"

#include "config/hardware_config.h"
#include "freertos/FreeRTOS.h"
#include "mcp3208.h"
#include "pot_store.h"
namespace wavex_panel {
using namespace WaveX::Panel;
namespace {
constexpr uint8_t channels[4][2] = {{WAVEX_POT1_A_CHANNEL, WAVEX_POT1_B_CHANNEL},
                                    {WAVEX_POT2_A_CHANNEL, WAVEX_POT2_B_CHANNEL},
                                    {WAVEX_POT3_A_CHANNEL, WAVEX_POT3_B_CHANNEL},
                                    {WAVEX_POT4_A_CHANNEL, WAVEX_POT4_B_CHANNEL}};
constexpr bool validChannels() {
    unsigned mask = 0;
    for (auto& pair: channels)
        for (auto ch: pair) {
            if (ch >= 8 || (mask & (1u << ch)))
                return false;
            mask |= 1u << ch;
        }
    return mask == 255;
}
static_assert(validChannels(), "Pot wipers must use distinct MCP3208 channels");
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
PotStatus published;
std::array<int16_t, 4> deltas{};
PotCommand pending = PotCommand::None;
uint8_t requested_index = 0;
bool cancel = false, online = false;
// Below this line all state belongs exclusively to panel_task.
PotStatus state;
EndlessPot decoders[4];
PotCalibrationSession session;
bool adc_initialized = false, attempted = false;
uint32_t last_attempt = 0;
void resetDecoders() {
    for (size_t i = 0; i < 4; ++i)
        decoders[i].Configure(state.calibration[i]);
    portENTER_CRITICAL(&mux);
    deltas = {};
    portEXIT_CRITICAL(&mux);
}
}  // namespace
void InitPots() {
    state = {};
    session.Cancel();
    state.last_result = LoadPotCalibration(state.calibration);
    resetDecoders();
    attempted = adc_initialized = false;
    portENTER_CRITICAL(&mux);
    pending = PotCommand::None;
    cancel = false;
    published = state;
    online = true;
    portEXIT_CRITICAL(&mux);
}
void ServicePots(uint32_t now_ms) {
    PotCommand command;
    uint8_t index;
    bool cancelled;
    portENTER_CRITICAL(&mux);
    command = pending;
    index = requested_index;
    pending = PotCommand::None;
    cancelled = cancel;
    cancel = false;
    portEXIT_CRITICAL(&mux);
    if (cancelled) {
        command = PotCommand::None;
        session.Cancel();
        state.selected = -1;
        resetDecoders();
    }
    if (command != PotCommand::None) {
        state.last_result = ESP_OK;
        if (command == PotCommand::Begin) {
            if (!state.adc_ready)
                state.last_result = ESP_ERR_INVALID_STATE;
            else {
                state.selected = static_cast<int8_t>(index);
                session.Begin();
                resetDecoders();
            }
        } else if (command == PotCommand::Verify) {
            if (state.selected != index || !session.Verify())
                state.last_result = ESP_ERR_INVALID_STATE;
        } else if (command == PotCommand::Save || command == PotCommand::Disable) {
            if (command == PotCommand::Save &&
                (state.selected != index || session.State() != PotCalibrationSession::Stage::Ready))
                state.last_result = ESP_ERR_INVALID_STATE;
            else {
                auto next = state.calibration;
                if (command == PotCommand::Save)
                    next[index] = session.Candidate();
                else
                    next[index].enabled = false;
                state.last_result = SavePotCalibration(next);
                if (state.last_result == ESP_OK) {
                    state.calibration = next;
                    session.Cancel();
                    state.selected = -1;
                    resetDecoders();
                }
            }
        }
    }
#if WAVEX_PANEL_POTS_ENABLED
    if (!adc_initialized && (!attempted || now_ms - last_attempt >= 1000)) {
        attempted = true;
        last_attempt = now_ms;
        adc_initialized = InitAdc() == ESP_OK;
        if (!adc_initialized)
            ++state.errors;
    }
    state.adc_ready = adc_initialized && ReadAdc(state.raw) == ESP_OK;
    if (adc_initialized && !state.adc_ready) {
        ++state.errors;
        CloseAdc();
        adc_initialized = false;
        last_attempt = now_ms;
    }
#endif
    std::array<int16_t, 4> steps{};
    if (state.adc_ready) {
        ++state.scans;
        for (size_t i = 0; i < 4; ++i)
            state.wipers[i] = {state.raw[channels[i][0]], state.raw[channels[i][1]]};
    }
    if (session.State() != PotCalibrationSession::Stage::Idle && state.selected >= 0) {
        if (state.adc_ready)
            session.Update(state.raw[channels[state.selected][0]],
                           state.raw[channels[state.selected][1]]);
        else
            session.Missing();
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!state.adc_ready || session.State() != PotCalibrationSession::Stage::Idle) {
            decoders[i].Reset();
            state.readings[i] = {};
        } else {
            state.readings[i] =
                decoders[i].Update(state.raw[channels[i][0]], state.raw[channels[i][1]], now_ms);
            steps[i] = state.readings[i].steps;
        }
    }
    state.stage = session.State();
    state.candidate = session.Candidate();
    state.progress = static_cast<uint8_t>(session.Progress());
    portENTER_CRITICAL(&mux);
    // Requests arriving during I/O suppress input until their next service pass.
    state.busy = pending != PotCommand::None || cancel;
    published = state;
    if (!state.busy)
        for (size_t i = 0; i < 4; ++i)
            deltas[i] =
                state.readings[i].valid
                    ? static_cast<int16_t>(std::clamp(int(deltas[i]) + steps[i], -32767, 32767))
                    : 0;
    portEXIT_CRITICAL(&mux);
}
void StopPots() {
    CloseAdc();
    adc_initialized = false;
    portENTER_CRITICAL(&mux);
    online = false;
    published.adc_ready = false;
    deltas = {};
    portEXIT_CRITICAL(&mux);
}
PotStatus ReadPots() {
    portENTER_CRITICAL(&mux);
    const auto result = published;
    portEXIT_CRITICAL(&mux);
    return result;
}
bool RequestPot(PotCommand command, uint8_t index) {
    if (index >= 4 || command == PotCommand::None)
        return false;
    portENTER_CRITICAL(&mux);
    const bool accepted = online && !published.busy && pending == PotCommand::None && !cancel;
    if (accepted) {
        pending = command;
        requested_index = index;
        published.busy = true;
        deltas = {};
    }
    portEXIT_CRITICAL(&mux);
    return accepted;
}
void CancelPotCalibration() {
    portENTER_CRITICAL(&mux);
    cancel = true;
    published.busy = true;
    deltas = {};
    portEXIT_CRITICAL(&mux);
}
std::array<int16_t, 4> TakePotSteps() {
    portENTER_CRITICAL(&mux);
    const auto result = published.busy ? std::array<int16_t, 4>{} : deltas;
    deltas = {};
    portEXIT_CRITICAL(&mux);
    return result;
}
}  // namespace wavex_panel
