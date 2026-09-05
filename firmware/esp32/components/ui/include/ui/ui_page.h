// WaveX UI Page Abstraction
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_softkey.h"

#include <memory>
#include <string>

namespace wavex_ui {

/// Base class for all UI pages/screens. Each page manages its own LVGL
/// objects, state, and input handling, and can be pushed/popped from a
/// navigation stack (see UINavigator).
class UIPage {
   public:
    virtual ~UIPage() = default;

    virtual const char* name() const = 0;
    virtual void onEnter(lv_obj_t* parent) = 0;
    virtual void onExit() {}
    virtual void onInput(const InputEvent& /*evt*/) {}

    /**
     * @brief The shared current Track (current_track.h) changed under the page.
     *
     * The panel's Track -/+ keys are global (panel-controls.md §4.3): the
     * InputDispatcher steps the Track and asks the Daisy for its binding, then
     * calls this so a page showing the Track can redraw. Default does
     * nothing; a page that shows the Track label or its binding overrides.
     */
    virtual void onTrackChanged() {}

    virtual std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() {
        return {};  // Default: no softkeys
    }

    /**
     * @brief Alternate softkey row, revealed while Shift is latched.
     *
     * Default is empty, which the navigator reads as "this page has no
     * alternates" - Shift then does nothing visible rather than blanking the
     * row, so pressing it on a page that does not use it is harmless.
     */
    virtual std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() { return {}; }

    /**
     * @brief Debug-harness hooks (docs/features/debug-harness-and-hil.md §4).
     *
     * consoleState() appends the page's own " key=value" pairs to a STATE
     * reply (WaveX::Debug::AppendKv*); consoleCommand() handles a "PAGE"
     * verb's arguments and writes a reply body, returning false when the
     * page does not know the command. Both run on the UI task under the
     * LVGL lock. Defaults do nothing; pages that carry test-relevant state
     * (a status line, a picker, a selected file) opt in.
     */
    virtual size_t consoleState(char* /*out*/, size_t /*cap*/, size_t len) { return len; }
    virtual bool consoleCommand(const char* /*args*/, char* /*reply*/, size_t /*cap*/) {
        return false;
    }

    lv_obj_t* root() const { return root_; }

   protected:
    lv_obj_t* root_ = nullptr;
};

}  // namespace wavex_ui
