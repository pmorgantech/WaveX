#pragma once
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UISongPage : public UIPage {
   public:
    const char* name() const override { return "Songs"; }
    const char* contextLine() const override {
        return "Arrange Patterns; Play starts at the selected section";
    }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    struct Control {
        UISongPage* owner = nullptr;
        lv_obj_t* button = nullptr;
        lv_obj_t* label = nullptr;
        uint8_t index = 0;
    };
    static void click(lv_event_t*);
    void service();
    void read();
    void render();
    void resetDraft();
    void song(int);
    void section(int);
    void adjust(int);
    void send(uint8_t, int destination = -1);
    bool confirmed() const;
    bool ready() const;
    lv_obj_t *heading_ = nullptr, *input_ = nullptr, *keyboard_ = nullptr, *hint_ = nullptr;
    Control rows_[6]{}, fields_[4]{}, arrows_[2]{};
    lv_timer_t* timer_ = nullptr;
    WaveX::Protocol::SeqSongStatusMessage status_;
    uint32_t read_id_ = 0, seen_ = 0, pending_ = 0, sent_at_ = 0, received_at_ = 0, pending_at_ = 0;
    uint32_t soft_state_ = UINT32_MAX;
    uint8_t song_ = 0, section_ = 0, focus_ = 0, pattern_ = 0, repeats_ = 1;
    uint16_t tempo_ = 12000;
    bool alive_ = false, valid_ = false, dirty_ = false, loop_ = false;
    char message_[160]{};
};
std::shared_ptr<UIPage> createSongPage();
}  // namespace wavex_ui
