// WaveX On-Screen Keyboard / Pad Grid
#pragma once

#include <lvgl.h>

#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

/**
 * @brief A 4x4 grid of touch pads that trigger notes on the Daisy.
 *
 * A grid whose cell n sends note `root + n` IS a chromatic keyboard spanning
 * 16 semitones, so "virtual piano" and "pad grid" are one widget here - they
 * differ only in the note map and the labels, not in the input handling or the
 * message path (features/digital-voice-audition.md §3).
 *
 * This is currently the ONLY way to trigger a digital voice without external
 * MIDI hardware, and therefore the only way to hear the per-voice filter,
 * envelope and live parameter edits at all. Note that the sample browser's
 * Audition button does NOT exercise this path: it uses the streaming ring
 * buffer, which bypasses VoiceManager entirely.
 *
 * Requires a 16-bit sample to be RAM-resident (loaded from the browser) - the
 * Daisy drops the note otherwise, since there is nothing to play.
 */
class UIKeyboardPage : public UIPage {
   public:
    static constexpr int kRows = 4;
    static constexpr int kCols = 4;
    static constexpr int kPads = kRows * kCols;

    const char* name() const override { return "Keyboard"; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   private:
    static void pad_event_cb(lv_event_t* e);

    void press(int index);
    void release(int index);
    void releaseAll();
    void setRoot(int root);
    void refreshLabels();

    lv_obj_t* pads_[kPads] = {nullptr};
    lv_obj_t* pad_labels_[kPads] = {nullptr};
    lv_obj_t* status_label_ = nullptr;

    // The note actually sent for a pad that is currently down. Held per pad
    // rather than recomputed from root_note_ at release time, because the root
    // can change (or the page can exit) between press and release - recomputing
    // would send Note Off for a note that was never started and leave the real
    // one hanging.
    uint8_t sounding_note_[kPads] = {0};
    bool pad_down_[kPads] = {false};

    int root_note_ = 60;  // C4, scientific pitch notation
    uint8_t velocity_ = 100;
};

std::shared_ptr<UIPage> createKeyboardPage();

}  // namespace wavex_ui
