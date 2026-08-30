// WaveX Voice / Preset editor
#pragma once

#include "input_event.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

/**
 * @brief Edits the voice as a signal chain rather than a pile of parameters.
 *
 * The chain, left to right, is the order the audio actually travels:
 *
 *     SAMPLE (pitch, pan, gain) -> MOD -> ENVELOPES -> AMP -> FILTER
 *
 * That ordering is not decoration. Before this page the envelope and filter
 * values lived as global state on the Keyboard page, edited through
 * MSG_CONTROL_CHANGE and belonging to nothing - so there was no answer to
 * "which sample do these settings apply to" and no way to keep two different
 * sounds. Here they are stages of one named voice.
 *
 * Scope, stated plainly because half of this chain is real and half is not yet:
 *
 * - **SAMPLE, ENVELOPES, AMP, FILTER are live.** They map onto
 *   MSG_SAMPLE_SELECT and the PARAM_* control changes the engine already
 *   applies to sounding voices, so edits are audible immediately.
 * - **MOD is a placeholder.** Nothing in the protocol carries a modulation
 *   source, destination or depth, and inventing a matrix in the UI before the
 *   engine has one would be drawing controls that do nothing - the mistake this
 *   codebase has made before (see the sample edit page's "drawn but inert"
 *   note). The stage is shown, marked, and left unwired.
 * - **Presets do not persist.** Naming and save/load need an on-disk format and
 *   protocol messages that do not exist yet; the name is held in RAM so the
 *   entity is real even while its storage is not.
 */
class UIVoicePage : public UIPage {
   public:
    const char* name() const override { return "Voice"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;

    /// Stages of the chain, in signal order. The focus walks these.
    enum class Stage : uint8_t { Sample = 0, Mod, Envelopes, Amp, Filter, kCount };

   private:
    static constexpr int kStageCount = static_cast<int>(Stage::kCount);

    /// One editable parameter. `wire_param` is PARAM_NONE for anything the
    /// protocol cannot carry yet, which is how a control declares itself inert
    /// rather than pretending.
    struct Param {
        const char* label;
        uint8_t wire_param;
        uint16_t value;
        Stage stage;
        const char* unit;
    };

    static constexpr uint8_t kParamNone = 0xFF;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* chain_[kStageCount] = {};
    lv_obj_t* chain_label_[kStageCount] = {};
    lv_obj_t* param_panel_ = nullptr;
    lv_obj_t* param_rows_[6] = {};
    lv_obj_t* param_bars_[6] = {};
    lv_obj_t* status_label_ = nullptr;

    // Current value of every parameter, by stage and slot. paramsForStage()
    // describes the chain and its defaults; this holds what the user has since
    // moved, so the description stays in one place and the state in another.
    uint16_t stage_values_[kStageCount][6] = {};
    bool values_seeded_ = false;

    char voice_name_[24] = "Init Voice";
    uint16_t sample_id_ = 0;
    int stage_ = 0;  ///< index into Stage
    int param_ = 0;  ///< index into the focused stage's parameters
    bool editing_ = false;

    void buildChain(lv_obj_t* parent);
    void buildParamPanel(lv_obj_t* parent);
    void refreshChain();
    void refreshParams();
    void refreshStatus(const char* text);
    void stepParam(int steps);
    void moveStage(int delta);
    void moveParam(int delta);
    void sendParam(const Param& p);
    void seedValues();

    /// Parameters belonging to the focused stage, written into `out`.
    int paramsForStage(Stage s, Param* out, int max) const;
};

std::shared_ptr<UIPage> createVoicePage();

}  // namespace wavex_ui
