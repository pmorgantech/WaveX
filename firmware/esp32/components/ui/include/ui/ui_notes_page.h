#pragma once
#include "components/ui_value_tile.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UINotesPage : public UIPage {
   public:
    explicit UINotesPage(uint8_t step) : step_(step) {}
    const char* name() const override { return "Step Notes"; }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    WaveX::Protocol::SeqNotesMessage state_{};
    ValueTile tiles_[4]{};
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t expected_ = 0, read_at_ = 0, keys_ = UINT32_MAX, feedback_at_ = 0;
    uint8_t step_ = 0, lane_ = 0, focus_ = 1;
    bool ready_ = false, arm_target_ = true, feedback_ = false;
    void read();
    void service();
    void render();
    bool edit(const WaveX::Protocol::SeqPatternOpMessage&);
    bool lane(WaveX::Protocol::SeqNoteLaneState);
    void adjust(uint8_t, int);
    void selectStep(int);
    void transport(uint8_t, uint8_t, uint8_t);
    static void tick(lv_timer_t*);
};
std::shared_ptr<UIPage> createNotesPage(uint8_t step);
}  // namespace wavex_ui
