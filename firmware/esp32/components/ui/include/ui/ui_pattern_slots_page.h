#pragma once
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UIPatternSlotsPage : public UIPage {
   public:
    const char* name() const override { return "Project Patterns"; }
    const char* contextLine() const override { return "Select now or launch at the next loop"; }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    static void tick(lv_timer_t*);
    static void keyboardEvent(lv_event_t*);
    static void inputEvent(lv_event_t*);
    void service();
    void read();
    void render();
    void move(int);
    void send(uint8_t);
    bool ready() const;
    lv_obj_t *input_ = nullptr, *keyboard_ = nullptr, *hint_ = nullptr, *last_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    WaveX::Protocol::SeqSlotStatusMessage status_;
    uint32_t read_id_ = 0, seen_id_ = 0, pending_id_ = 0, requested_at_ = 0, received_at_ = 0,
             pending_at_ = 0;
    bool alive_ = false, valid_ = false;
    uint8_t slot_ = 0;
    uint32_t soft_state_ = UINT32_MAX;
    char message_[192]{};
};
std::shared_ptr<UIPage> createPatternSlotsPage();
}  // namespace wavex_ui
