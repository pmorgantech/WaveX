// WaveX Instrument editor
#pragma once

#include "components/ui_dial.h"
#include "components/ui_value_tile.h"
#include "input_event.h"
#include "instrument_edit_model.h"
#include "lfo_model.h"
#include "modulator_model.h"
#include "oscillator_model.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

// Shared Instrument stages. Oscillator controls use revisioned backend snapshots;
// all sound settings audition immediately with a backend-owned Apply/Revert point.
class UIInstrumentPage : public UIPage {
   public:
    const char* name() const override { return "Instrument"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    bool canLeave() override;
    void onInput(const InputEvent& evt) override;
    void onTrackChanged() override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char* out, size_t cap, size_t len) override;
    bool consoleCommand(const char* args, char* reply, size_t cap) override;
    const char* contextLine() const override { return context_line_; }

    // Tabs share the selected Instrument.
    enum class Stage : uint8_t { Oscillator = 0, Envelopes, Amp, Filter, Mod, Lfo, kCount };

   private:
    static constexpr int kStageCount = static_cast<int>(Stage::kCount);
    // Oscillator selection plus five settings.
    static constexpr int kMaxParams = 8;

    // CC-backed controls and a sentinel for the revisioned oscillator editor.
    struct Param {
        const char* label;
        uint8_t wire_param;
        int32_t value;
        Stage stage;
        const char* unit;
    };

    static constexpr uint8_t kParamNone = 0xFF;
    static constexpr uint8_t kParamOscillator = 0xFE;
    static constexpr uint8_t kParamModulator = 0xFD;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* tabview_ = nullptr;
    lv_obj_t* tab_body_[kStageCount] = {};

    /// Envelopes uses dials, every other stage uses value tiles. Four
    /// parameters of the same kind compared against each other is what a dial
    /// row is for; a stage whose parameters are unrelated reads better as
    /// tiles, where each carries its own units.
    Dial dials_[kMaxParams] = {};
    ValueTile tiles_[kStageCount][kMaxParams] = {};

    /// Curve panes. Points live in fixed members rather than being allocated
    /// per redraw: lv_line does not copy the array, so it has to outlive every
    /// frame that draws it, and the ESP32 guide (§8) wants no allocation on a
    /// path that runs at frame rate.
    static constexpr int kEnvCurvePoints = 5;
    static constexpr int kFilterCurvePoints = 33;
    lv_obj_t* env_curve_ = nullptr;
    lv_obj_t* filter_curve_ = nullptr;
    lv_point_precise_t env_pts_[kEnvCurvePoints] = {};
    lv_point_precise_t filter_pts_[kFilterCurvePoints] = {};

    /// Built by refreshHeader() and handed to the navigator's header.
    char context_line_[160] = "";
    char status_[64] = "";  // longest caller string is ~57 bytes
    /// A tab's rows are built the first time it is shown, so entering the page
    /// costs one stage's widgets rather than five (docs/roadmap.md: these pages
    /// are slow to enter, not slow to run).
    bool stage_built_[kStageCount] = {};

    // Current value of every parameter, by stage and param index. paramsForStage()
    // describes the chain and its defaults; this holds what the user has since
    // moved, so the description stays in one place and the state in another.
    int32_t stage_values_[kStageCount][kMaxParams] = {};
    bool values_seeded_ = false;

    LfoModel lfo_;
    uint32_t lfo_read_at_ = 0, lfo_pending_at_ = 0;
    bool lfoStage() const { return stage_ == static_cast<int>(Stage::Lfo); }
    void readLfo();
    void serviceLfo();
    void selectLfo(int index);
    void refreshLfo();
    InstrumentEditModel sound_;
    uint8_t display_track_ = 0, requested_action_ = 0;
    uint32_t sound_read_at_ = 0, sound_pending_at_ = 0, action_read_id_ = 0;
    bool track_change_pending_ = false;
    void readSound();
    void serviceSound();
    void soundAction(uint8_t op);
    void sendSound(uint8_t op);
    ModulatorModel modulator_;
    uint8_t selected_env_ = 0, selected_slot_ = 0;
    uint32_t mod_read_at_ = 0, mod_pending_at_ = 0;
    bool mod_timed_out_ = false;
    void readModulator();
    void serviceModulator();
    void selectModulator(int index);
    void applyModulator();
    void refreshModulator();
    bool modStage() const {
        return stage_ == static_cast<int>(Stage::Envelopes) ||
               stage_ == static_cast<int>(Stage::Mod);
    }
    bool draftActive() const {
        return lfo_.Dirty() || lfo_.Pending() || oscillator_.Dirty() || oscillator_.Pending() ||
               modulator_.Dirty() || modulator_.Pending() || sound_.Outgoing() ||
               sound_.Pending() || requested_action_;
    }
    OscillatorModel oscillator_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t* oscillator_status_ = nullptr;
    uint32_t read_at_ = 0, pending_at_ = 0;
    bool alive_ = false, timed_out_ = false;
    void readOscillator();
    void serviceOscillator();
    void selectOscillator(uint8_t oscillator);
    void applyOscillator(uint8_t operation);
    void refreshOscillator();
    static void tick(lv_timer_t* timer);
    int stage_ = 0;  ///< index into Stage, and the active tab index
    int param_ = 0;  ///< index into the focused stage's parameters
    bool editing_ = false;

    static void tabChangedCb(lv_event_t* e);

    void buildStageRows(int stage);
    lv_obj_t* buildCurvePane(lv_obj_t* parent,
                             int x,
                             int y,
                             int w,
                             int h,
                             const char* title,
                             const char* right,
                             lv_point_precise_t* pts,
                             int count);
    void refreshEnvCurve();
    void refreshFilterCurve();
    void selectStage(int stage);
    void refreshHeader();
    void refreshParams();
    void refreshStatus(const char* text);
    void stepParam(int steps);
    void moveStage(int delta);
    void moveParam(int delta);
    void sendParam(const Param& p);
    void seedValues();
    uint8_t currentTrack() const;

    /// Parameters belonging to a stage, written into `out`.
    int paramsForStage(Stage s, Param* out, int max) const;
};

std::shared_ptr<UIPage> createInstrumentPage();

}  // namespace wavex_ui
