// WaveX Play page - Pads and Keys performance surfaces
#pragma once

#include <lvgl.h>

#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

/**
 * @brief The performance surface: a pad grid and a piano keyboard, tabbed.
 *
 * Two layouts over ONE behaviour. Everything except the key geometry and the
 * semitone each key carries is shared - note-on/off, latch, the panic key, the
 * voice-parameter strip, and the note bookkeeping that keeps a transpose or a
 * page exit from stranding a sounding note. See
 * `docs/ui-information-architecture.md` §3 for why they are one page rather
 * than two: duplicating that behaviour is how the two surfaces would drift into
 * behaving differently.
 *
 * This is currently the only way to trigger a digital voice without external
 * MIDI hardware, so it is also the only way to hear the per-voice filter,
 * envelope and live parameter edits at all. Note the sample browser's Audition
 * does NOT exercise this path - it streams through the ring buffer and bypasses
 * VoiceManager entirely - and a 16-bit sample must already be RAM-resident,
 * AND bound to the SLOT this page is currently sending notes on (Sample
 * Manager page's slot selector, or an SFZ instrument loaded to that slot), or
 * the Daisy drops the note (roadmap Phase 2.5 item 1).
 */
class UIPlayPage : public UIPage {
   public:
    /// Voice parameters editable here, in display order. Cutoff..Release are
    /// the digital-path destinations; values ride MSG_CONTROL_CHANGE and reach
    /// both sounding voices and the next trigger. Slot is different - it is
    /// not a voice parameter at all, but which instrument slot (MIDI channel,
    /// 0..15) subsequent note-on/off go out on (roadmap Phase 2.5 item 1,
    /// "retire the fallback": a slot with no sample bound via the Sample
    /// Manager page just drops the note). It rides the SAME encoder/softkey
    /// cycle as the others purely so this page does not need a second input
    /// surface, not because it is a live-voice parameter.
    enum class Param : uint8_t { Cutoff, Resonance, Attack, Decay, Sustain, Release, Slot, kCount };

    // 16 pads + 25 piano keys, rounded up. Both surfaces are built at page
    // entry and coexist, so the table spans them rather than being per-tab -
    // which is also what lets releaseAll() cover a note left sounding on the
    // tab you just switched away from.
    static constexpr int kMaxKeys = 48;
    static constexpr int kPadRows = 4;
    static constexpr int kPadCols = 4;
    static constexpr int kPadCount = kPadRows * kPadCols;

    const char* name() const override { return "Play"; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;

   private:
    struct Key {
        lv_obj_t* obj = nullptr;
        lv_obj_t* label = nullptr;
        // Semitones above the root, NOT an absolute note. Held this way so a
        // transpose is a single root change rather than a rewrite of every key.
        int8_t offset = 0;
        // Resting fill, captured at construction. Held per key rather than
        // re-derived in the refresh, because "is this a pad or a piano white
        // key" is otherwise index arithmetic that breaks the moment either
        // surface changes size.
        uint32_t bg_normal = 0;
        uint32_t text_normal = 0;
        bool is_black = false;
        bool down = false;
        // The note actually sent at press. Remembered rather than recomputed at
        // release, because the root can change in between - recomputing would
        // send Note Off for a note that was never started and leave the real
        // one hanging.
        uint8_t sent_note = 0;
    };

    static void keyEventCb(lv_event_t* e);
    static void tabChangedCb(lv_event_t* e);

    void buildStrip(lv_obj_t* parent);
    void buildPads(lv_obj_t* tab);
    void buildKeys(lv_obj_t* tab);
    lv_obj_t* makeKey(lv_obj_t* parent, int8_t offset, bool is_black, uint32_t bg, uint32_t text);

    void press(int index);
    void release(int index);
    void releaseAll();
    void toggleLatch(int index);
    void setRoot(int root);
    void refreshKeys();

    void stepParam(int direction);
    void selectParam(int direction);
    void sendParam();
    void refreshParamLabel();

    int noteFor(const Key& k) const;
    uint8_t currentSlot() const;

    Key keys_[kMaxKeys];
    int key_count_ = 0;

    lv_obj_t* tabview_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* param_label_ = nullptr;

    uint16_t param_value_[static_cast<size_t>(Param::kCount)] = {0};
    Param current_param_ = Param::Cutoff;

    int root_note_ = 60;  // C4, scientific pitch notation
    uint8_t velocity_ = 100;
    bool latch_ = false;
};

std::shared_ptr<UIPage> createPlayPage();

}  // namespace wavex_ui
