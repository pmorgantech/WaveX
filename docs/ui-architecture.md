# WaveX UI Architecture (Navigator-First Design)

**Last Updated**: 2026-08-30 (re-verified against code — the `UIPage` sample,
tab-group and Shift-modifier sections were stale since 2026-08-28; hardware
facts moved to [`ui-design-constraints.md`](ui-design-constraints.md))
**Version**: 2.2

**Hardware/toolkit summary** (full detail + sources in the constraints doc):
LVGL 9.4 on ESP32-P4, 5-inch 720×1280 MIPI-DSI panel software-rotated to
1280×720 landscape, RGB565, 30 FPS UI task, 75 px header + 100 px six-button
softkey bar → 1280×545 content area, Montserrat fonts only.

## Overview

The WaveX UI has been restructured to use a **stack-based navigation system** with the following improvements:

1. **Navigator-Centric Architecture**: All UI pages are managed through `UINavigator` with a unified push/pop lifecycle
2. **Global State Reduction**: Eliminated global singletons and static references to UI objects
3. **LVGL Thread Safety**: Centralized LVGL context management through `DisplayManager`
4. **Unified Softkey System**: Pages define softkeys via `UIPage::getSoftkeys()`, eliminating procedural hotkey management
5. **Dependency Injection**: Shared resources passed to pages via `UISharedContext` instead of globals

## Architecture Components

### 1. DisplayManager (New)
**File**: `components/ui/src/display_manager.cpp`

Encapsulates all LVGL and display hardware initialization:
- LVGL display initialization and MIPI DSI display setup
- Touch controller initialization (GT911)
- LVGL tick timer management
- ESP timer for 30 FPS meter updates

**Key Methods**:
- `DisplayManager::init()` - Initialize display, touch, and LVGL context
- `DisplayManager::deinit()` - Clean shutdown of display resources
- `DisplayManager::startLvglTick()` - Create and start LVGL tick timer
- `DisplayManager::display()` - Get current LVGL display handle

**Usage Pattern**:
```cpp
// In ui_task.cpp
esp_err_t lvgl_ret = wavex_ui::DisplayManager::instance().init();
// ... later during shutdown
wavex_ui::DisplayManager::instance().deinit();
```

### 2. UINavigator (Enhanced)
**File**: `components/ui/src/ui_navigator.cpp`

Stack-based navigation manager that coordinates:
- Page lifecycle (`onEnter`/`onExit` callbacks)
- Screen layout (header, content area, softkey bar)
- Softkey synchronization with active page

**Key Methods**:
- `push(page)` - Push page onto stack, trigger `onExit` of previous page
- `pop()` - Pop current page, trigger `onEnter` of previous page
- `active()` - Get currently active page
- `refreshSoftkeys()` - Update softkey bar based on active page's `getSoftkeys()`

**Stack Structure**:
```
Screen (LVGL object)
├─ Header (UI_HEADER_HEIGHT pixels)
│  └─ Title label (page name)
├─ Content area (page-specific widgets)
│  └─ UIPage::onEnter() creates content here
└─ Softkey bar (UI_HOTKEY_HEIGHT pixels)
   └─ SoftkeyBar manages 6 softkey buttons
```

### 3. UIPage (Base Class)
**File**: `components/ui/include/ui/ui_page.h`

Virtual base class for all navigable pages:
```cpp
class UIPage {
public:
    virtual ~UIPage() = default;
    virtual const char* name() const = 0;
    virtual void onEnter(lv_obj_t* parent) = 0;
    virtual void onExit() {}
    virtual void onInput(const InputEvent& evt) {}
    virtual std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() { return {}; }
    virtual std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() { return {}; }
};
```

**Responsibilities**:
- Define page name (shown in header)
- Define softkey labels/actions, and optionally a Shift-revealed alternate row (`getShiftedSoftkeys()` — see "Shift Modifier" below)
- Handle input directly via `onInput()` when a page needs more than softkeys (encoder deltas, touch)
- Create/destroy page UI in `onEnter`/`onExit`
- Never manipulate LVGL outside `onEnter`/`onExit` (these run with `LV_LOCK()` held)

### 4. SoftkeyBar
**File**: `components/ui/src/ui_softkey_bar.cpp`

Manages the bottom row of 6 softkey buttons:
- Handles touch events and button focus states
- Coordinates with encoder input via `InputDispatcher`
- Invokes softkey callbacks when activated

**Key Methods**:
- `setSoftkeys(defs)` - Set the 6 softkey definitions
- `create(parent)` - Create/recreate button widgets
- `focusNext()` / `focusPrev()` - Encoder navigation
- `pressFocused()` - Activate currently focused softkey

### 5. InputDispatcher
**File**: `components/ui/src/input_dispatcher.cpp`

Central event router for:
- Encoder movement (left/right deltas)
- Touch/button input
- Hardware keypad events

Routes events to the currently active navigation context (typically the `SoftkeyBar`).

### Shift Modifier

`BUTTON_SHIFT` is intercepted globally by `InputDispatcher::processAll()`
before dispatch to the active context, so every page gets the same modifier
for free and none can accidentally swallow it (`ui_softkey.h`,
`input_dispatcher.cpp`).

`UINavigator::toggleShift()` / `setShift(bool)` flip a **latched, not held**
state (`isShifted()`), shown as a SHIFT chip in the header. It is *sticky*:
it clears itself after one shifted key fires, and on navigation — a plain
toggle left on would make the next press do the wrong thing. A page that
defines no alternate row (`getShiftedSoftkeys()` returns the empty default)
is simply inert while shifted rather than blanking the softkey row —
`UINavigator::activePageHasShiftedKeys()` is what the header chip and input
routing check before treating Shift as meaningful on the current page.

### Tab Groups

Two distinct shapes exist for grouping related pages, per
[`ui-information-architecture.md`](ui-information-architecture.md) §2's rule
("tabs when the children share a subject, a menu list when they do not"):

- **`UITabHostPage`** (`ui_tab_host_page.h`) hosts existing, independent
  `UIPage`s unchanged — each keeps its own `onEnter`/`onExit`/softkeys/input
  handling, and the host forwards the page contract to whichever tab is
  selected. Used for groups like Sample (Browse/Manage/Edit/Record) and
  Settings, where converting the children into tab-body builders would be a
  large, risky rewrite. Children are entered lazily and exited when switched
  away from, so a hidden tab holds no LVGL objects and runs no timers.
- **A page building its own `lv_tabview`** (`tabGroupCreate()` /
  `tabGroupAddTab()` in `ui_tab_group.h`) is for stages that share state
  across the tab switch — e.g. `UIVoicePage`'s five stages (Sample, Env,
  Amp, Filter, Mod) share the voice being edited, so the header and status
  line must survive switching tabs. The Diagnostics page uses the same
  helper for its six tabs.

## Data Flow & Threading

### UI Update Flow (Normal)
```
UI Task (FreeRTOS)
├─ Poll InputDispatcher for queued events
├─ Dispatch to SoftkeyBar (encoder focus, button presses)
├─ Process deferred updates from background tasks
├─ Call DisplayManager's adaptive_refresh_control()
└─ Sleep 32ms (30 FPS target)
```

### Deferred Update Pattern (From Background Tasks)
```
Background Task (e.g., Meter Timer)
├─ Update volatile deferred state
├─ Signal UI task (e.g., s_meter_update_pending = true)
└─ Return (no LVGL calls)

UI Task (in main loop)
├─ Detect pending update flag
├─ Call LV_LOCK()
├─ Apply updates to LVGL widgets
└─ Call LV_UNLOCK()
```

**Critical Rule**: Never call LVGL functions from background tasks. Use deferred updates or `lv_async_call()`.

## Global State Reduction

The original migration (this section historically described) replaced global
page pointers and procedural menu-creation functions (`ui_globals.cpp/h`,
`create_main_menu`/`create_sample_menu`, and a `sample_load_save.cpp` page
that predates the current `UIPage` hierarchy) with the `UINavigator`
push/pop model above. All of those files are gone from the tree today —
pages are `UIPage` subclasses constructed by factory functions in
`ui_main_menu.cpp` (see "Page Implementation Guide" below) and owned by the
navigator stack, not by global pointers.

What remains, and is justified: meter display objects (`s_meter_bar_l`,
`s_meter_bar_r`, etc.) persist across page switches for real-time updates,
and deferred state (`s_meter_update_pending`, `s_deferred_rms_left`, etc.)
synchronizes the meter timer with the UI task without LVGL lock contention —
see "Deferred Update Pattern" above.

**Known gap, not yet fixed**: `components/ui` still depends on `main`
(`inter_mcu_*` free functions called directly from pages) rather than
through injected context — tracked in `docs/backlog.md` ("Break the
`components/ui` ⇄ `main` dependency cycle") as `E-ARCH1`, deliberately
deferred until after the current hardware bring-up pass.

## Page Implementation Guide

### Example: Simple Page
```cpp
class MyCustomPage : public UIPage {
public:
    const char* name() const override { return "My Page"; }

    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        std::array<Softkey, NUM_SOFTKEYS> keys{};
        keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
        keys[5] = {"Select", []() { /* handle select */ }};
        return keys;
    }

    void onEnter(lv_obj_t* parent) override {
        // Create UI widgets here
        auto label = lv_label_create(parent);
        lv_label_set_text(label, "Hello!");
    }

    void onExit() override {
        // Cleanup happens automatically when onEnter's widgets are deleted
    }
};
```

### Accessing Inter-MCU Data
Pages should request data **during** `onEnter()` or **on-demand** from softkey callbacks:
```cpp
void onEnter(lv_obj_t* parent) override {
    wavex_meter_data_t meter_data;
    inter_mcu_get_meter_data(&meter_data);  // Get current snapshot

    // Create UI based on snapshot
}
```

## Softkey Refresh Pattern

For pages where softkey labels change based on state (e.g., "Audition" → "Stop"):

```cpp
// In softkey callback or after state change
void onAuditionStateChange(bool is_playing) {
    // Update internal page state
    is_playing_ = is_playing;

    // Request softkey refresh through navigator
    UINavigator::instance().refreshSoftkeys();
    // Navigator will call your page's getSoftkeys() again
}
```

**See**: `ui_play_page.cpp` — `getSoftkeys()` builds the paged-param label from state, and the state-changing callbacks call `UINavigator::instance().refreshSoftkeys()` after updating it.

## LVGL Threading Compliance

### Safe Patterns
✅ **Within onEnter()/onExit()**:
```cpp
void onEnter(lv_obj_t* parent) override {
    // Already inside LV_LOCK()
    auto obj = lv_obj_create(parent);
}
```

✅ **From UI task with explicit lock**:
```cpp
LV_LOCK();
lv_obj_set_size(widget, 100, 100);
LV_UNLOCK();
```

✅ **From background task via deferred update**:
```cpp
// In background task
s_pending_value = new_value;
s_update_pending = true;

// In UI task
LV_LOCK();
apply_deferred_updates();
LV_UNLOCK();
```

### Unsafe Patterns
❌ **Direct LVGL calls from background task** (can deadlock)
❌ **Nested LV_LOCK() calls** (already held in onEnter)
❌ **Calling LVGL from interrupt handler**

## Build Configuration

### CMakeLists.txt Changes
- Added `display_manager.cpp` to `COMPONENT_SRCS`
- Added `esp_lcd_touch_gt911` to `REQUIRES` list
- Removed `ui_globals.cpp`

### Dependencies
- `lvgl` - LVGL graphics library
- `esp_lvgl_port` - ESP-IDF LVGL integration
- `esp_lcd_touch_gt911` - Capacitive touch controller
- `esp_driver_gpio` - GPIO interface
- `esp32_p4_nano` - BSP (board support package)

## Testing Checklist

- [ ] **Navigation Flow**: Main menu → Sample browser → Back → Diagnostics → Back
- [ ] **Softkey Refresh**: Start audition, verify "Audition" → "Stop", then "Stop" → "Audition"
- [ ] **Display Manager**: No warnings during LVGL init/display
- [ ] **Meter Updates**: Audio meters update smoothly at 30 FPS
- [ ] **Touch Input**: Buttons respond, softkeys react to touch
- [ ] **Encoder Input**: Left/right rotation changes focus and selection
- [ ] **Memory Stability**: No crashes after multiple page transitions

## Migration Notes for Future Work

### If Adding a New Page
1. Create class extending `UIPage`
2. Implement `name()`, `onEnter()`, and whichever of `onExit()`/`onInput()`/`getSoftkeys()`/`getShiftedSoftkeys()` the page needs (all have empty defaults)
3. Add a factory function and register it as a menu item or tab in `ui_main_menu.cpp` (`ui_navigation_integration.cpp` only bootstraps the root menu via `initNavigationSystem()` — it is not where individual pages are registered)
4. Do NOT add global static pointers to the page

### If Sharing State Between Pages
1. Use `UISharedContext` struct (to be formalized in future)
2. Pass context to page constructors
3. Avoid static page pointers or callbacks with hard-coded state

### If Adding Background Task Updates
1. Use volatile deferred state variables
2. Set a pending flag in task callback
3. Check flag in UI task main loop
4. Apply updates under `LV_LOCK()`

## Known Limitations

1. **Meter updates from deferred state**: Currently using separate `meter_update_cb` timer. Future: Integrate with navigator page lifecycle.
2. **Page recreation on push/pop**: Every page transition recreates UI widgets. For large pages, consider caching strategies (future work).
3. **No explicit context injection yet**: Pages still access `inter_mcu` directly. Future: `UISharedContext` struct to pass dependencies.

## References

- **Design constraints (for UI/UX work)**: `docs/ui-design-constraints.md`
- **LVGL Threading Rules**: the "LVGL Threading Compliance" section above is
  the canonical statement. (`.cursor/rules/lvgl-threading.mdc` exists only as
  an untracked local file - `.cursor/` is gitignored - so do not cite it as
  shared truth.)
- **System Architecture**: `docs/architecture.md`
- **Page implementation how-to**: `docs/ui-system-implementation-guide.md`
- Historical: `docs/archive/navigation-integration-guide.md`, `docs/archive/sample-browser-redesign.md`
