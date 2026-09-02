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
 * @brief Edits one voice as five tabbed parameter groups.
 *
 * Tabs rather than a list because every group is a property of *the same
 * voice* - the rule `docs/ui-information-architecture.md` §2 pins, and the
 * same reason the Sample group is tabbed. The tab set and its order come from
 * §4: Sample, Env, Amp, Filter, Mod.
 *
 * That bar order is deliberately NOT the signal order. The audio actually
 * travels SAMPLE -> MOD -> ENV -> AMP -> FILTER, but Mod is the one stage the
 * protocol cannot carry yet, so §4 puts it last, where an unwired tab is least
 * in the way of the four that work. If Mod ever becomes real, moving it back
 * into signal position is a one-line change to `Stage` - the tab bar is built
 * from that enum, not from a second list.
 *
 * Like the Play group, this is a single `UIPage` that builds its own tabview
 * rather than a `UITabHostPage` over child pages. The state being edited -
 * which sample, every parameter value, whether the encoder is in edit mode -
 * is voice-scoped, not tab-scoped, and the header and status line that report
 * it therefore sit ABOVE the tabview. Built into a tab body they would vanish
 * on every tab switch, taking the only readout of what is being edited with
 * them.
 *
 * Scope, stated plainly because half of this chain is real and half is not yet:
 *
 * - **SAMPLE, ENV, AMP, FILTER are live.** SAMPLE cycles through resident
 *   samples (probing inter_mcu_get_sample_meta() the same way the Sample
 *   Manager page's list does - there is no dedicated "list of loaded ids"
 *   query) and SLOT chooses which instrument slot (0..15, MSG_NOTE_ON's
 *   channel) that sample is bound to; both ride MSG_SAMPLE_SELECT. ENV/AMP/
 *   FILTER map onto the PARAM_* control changes the engine already applies to
 *   sounding voices, so edits are audible immediately. Picking a DIFFERENT
 *   already-resident sample for a slot can also be done from the Sample
 *   Manager page, which has the fuller list UI.
 * - **MOD is a placeholder.** Nothing in the protocol carries a modulation
 *   source, destination or depth, and inventing a matrix in the UI before the
 *   engine has one would be drawing controls that do nothing - the mistake this
 *   codebase has made before (see the sample edit page's "drawn but inert"
 *   note). The tab is shown, marked, and left unwired.
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

    /// Tabs, in bar order (see the class note on why Mod is last).
    enum class Stage : uint8_t { Sample = 0, Envelopes, Amp, Filter, Mod, kCount };

   private:
    static constexpr int kStageCount = static_cast<int>(Stage::kCount);
    /// Widest stage is now Sample, at five (SAMPLE/SLOT/PITCH/PAN/GAIN).
    static constexpr int kMaxParams = 5;

    /// One editable parameter. `wire_param` is kParamNone for anything the
    /// protocol cannot carry yet, which is how a control declares itself inert
    /// rather than pretending. kParamSample/kParamSlot are real and live, but
    /// ride MSG_SAMPLE_SELECT rather than MSG_CONTROL_CHANGE, so stepParam()/
    /// sendParam() special-case them instead of treating `value` as a raw CC.
    struct Param {
        const char* label;
        uint8_t wire_param;
        uint16_t value;
        Stage stage;
        const char* unit;
    };

    static constexpr uint8_t kParamNone = 0xFF;
    static constexpr uint8_t kParamSample = 0xFE;
    static constexpr uint8_t kParamSlot = 0xFD;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* tabview_ = nullptr;
    lv_obj_t* tab_body_[kStageCount] = {};
    lv_obj_t* param_rows_[kStageCount][kMaxParams] = {};
    lv_obj_t* param_bars_[kStageCount][kMaxParams] = {};
    /// A tab's rows are built the first time it is shown, so entering the page
    /// costs one stage's widgets rather than five (docs/backlog.md: these pages
    /// are slow to enter, not slow to run).
    bool stage_built_[kStageCount] = {};

    // Current value of every parameter, by stage and slot. paramsForStage()
    // describes the chain and its defaults; this holds what the user has since
    // moved, so the description stays in one place and the state in another.
    uint16_t stage_values_[kStageCount][kMaxParams] = {};
    bool values_seeded_ = false;

    char voice_name_[24] = "Init Voice";
    uint16_t sample_id_ = 0;
    int stage_ = 0;  ///< index into Stage, and the active tab index
    int param_ = 0;  ///< index into the focused stage's parameters
    bool editing_ = false;

    static void tabChangedCb(lv_event_t* e);

    void buildStrip(lv_obj_t* parent);
    void buildStageRows(int stage);
    void selectStage(int stage);
    void refreshHeader();
    void refreshParams();
    void refreshStatus(const char* text);
    void stepParam(int steps);
    void moveStage(int delta);
    void moveParam(int delta);
    void sendParam(const Param& p);
    void seedValues();
    void cycleSample(int direction);
    uint8_t currentSlot() const;

    /// Parameters belonging to a stage, written into `out`.
    int paramsForStage(Stage s, Param* out, int max) const;
};

std::shared_ptr<UIPage> createVoicePage();

}  // namespace wavex_ui
