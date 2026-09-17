#pragma once
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UIProjectFilesPage : public UIPage {
   public:
    const char* name() const override { return "Project Files"; }
    const char* contextLine() const override { return "Save / load the complete Project"; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& event) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override { return getSoftkeys(); }
    size_t consoleState(char* out, size_t cap, size_t len) override;
    bool consoleCommand(const char* args, char* reply, size_t cap) override;

   private:
    static void tick(lv_timer_t*);
    static void keyboardEvent(lv_event_t*);
    static void inputEvent(lv_event_t*);
    void service();
    void read();
    void render();
    bool ready() const;
    void choose(uint8_t op);
    void send(uint8_t op);
    void cancel();
    lv_obj_t* input_ = nullptr;
    lv_obj_t* keyboard_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_obj_t* last_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    WaveX::Protocol::ProjectStatusMessage status_{};
    uint32_t read_id_ = 0, pending_id_ = 0, requested_at_ = 0, pending_at_ = 0, received_at_ = 0,
             seen_read_id_ = 0;
    bool alive_ = false, valid_ = false;
    uint8_t confirm_ = 0;
    char name_[24]{}, message_[192]{};
};
std::shared_ptr<UIPage> createProjectFilesPage();
}  // namespace wavex_ui
