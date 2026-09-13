#pragma once
#include "components/ui_value_tile.h"
#include "config/hardware_config.h"
#include "sequencer_grid_model.h"
#include "ui_page.h"

namespace wavex_ui {
class UISequencerPage : public UIPage {
   public:
    const char* name() const override { return "Sequencer"; }
    const char* contextLine() const override { return context_; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent& event) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char* out, size_t cap, size_t len) override;
    bool consoleCommand(const char* args, char* reply, size_t cap) override;

   private:
    struct Cell {
        UISequencerPage* owner = nullptr;
        lv_obj_t* button = nullptr;
        lv_obj_t* label = nullptr;
        uint8_t row = 0, column = 0;
        uint32_t drawn = UINT32_MAX;
    };
    static void cellEvent(lv_event_t* event);
    static void rowEvent(lv_event_t* event);
    static void timerEvent(lv_timer_t* timer);
    void service();
    void render();
    void requestRow(uint8_t row);
    void window(uint8_t track, uint8_t step);
    void focus(uint8_t track, uint8_t step);
    void toggle(uint8_t row, uint8_t column);
    bool edit(const WaveX::Protocol::SeqPatternOpMessage& message);
    void adjust(uint8_t parameter, int delta);
    void transport();
    void clearRow();
    void lockMode(bool enabled);
    void adjustLock(uint8_t parameter, int delta);
    bool setLock(uint8_t id, uint16_t value);
    void renderLocks();
    uint8_t selectedRow() const;
    bool editable() const;
    bool valueStep(SequencerGridModel::Step& step) const;

#if WAVEX_UI_LATENCY_PROFILE_ENABLED
    // All accesses occur in the serialized UI domain. No logging on the path.
    struct LatencyTrace {
        uint32_t sequence = 0, request_id = 0, pixels = 0;
        uint8_t row = 0, column = 0;
        bool expected_on = false;
        int64_t pressed_us = 0, released_us = 0, sent_us = 0;
        int64_t accepted_us = 0, refreshed_us = 0;
    } latency_;
    uint32_t refresh_pixels_ = 0;
    static void refreshEvent(lv_event_t* event);
#endif
    SequencerGridModel model_;
    Cell cells_[4][16]{};
    Cell row_context_[4]{};
    lv_obj_t* row_buttons_[4]{};
    lv_obj_t* row_labels_[4]{};
    uint8_t drawn_rows_[4]{};
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    ValueTile tiles_[7]{};
    WaveX::Protocol::SeqPatternSyncMessage settings_{};
    WaveX::Protocol::SeqPlayheadMessage playhead_{};
    uint8_t selected_step_ = 0;
    uint8_t next_row_ = 0;
    uint32_t requested_at_ = 0;
    bool link_alive_ = false;
    bool clear_armed_ = false;
    bool locks_mode_ = false;
    uint8_t lock_slot_ = 0;
    uint8_t drawn_lock_slot_ = 0xff;
    char context_[96]{};
};
std::shared_ptr<UIPage> createSequencerPage();
}  // namespace wavex_ui
