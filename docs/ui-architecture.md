# WaveX UI Architecture (Navigator-First Design)

**Last Updated**: 2025-11-02
**Version**: 2.0 (Major Refactor - Global State Reduction)

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
    virtual std::vector<SoftkeyDef> getSoftkeys() const = 0;
    virtual void onEnter(lv_obj_t* parent) = 0;
    virtual void onExit() = 0;
};
```

**Responsibilities**:
- Define page name (shown in header)
- Define softkey labels and actions
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

### What Was Removed
1. **ui_globals.cpp/h**: Global pointer to sample load/save page
2. **ui_task.cpp statics**:
   - `s_content_area`, `s_hotkey_region`, `s_hotkey_buttons`, `s_hotkey_labels`
   - `s_current_screen`
   - Procedural menu creation functions (`create_main_menu`, `create_sample_menu`)
   - Direct hotkey label management functions

### What Remains (Justified)
1. **Meter display objects** (`s_meter_bar_l`, `s_meter_bar_r`, etc.)
   - Must persist between page switches for real-time updates
   - Managed by `process_deferred_meter_updates()`
2. **Deferred state** (`s_meter_update_pending`, `s_deferred_rms_left`, etc.)
   - Synchronization between meter timer and UI task
   - Prevents LVGL lock contention

### How Pages Access Shared Services
**Before** (Anti-pattern):
```cpp
// In sample_load_save.cpp
extern wavex_sample_load_save_page_t* g_sample_load_save_page;
if (g_sample_load_save_page) {
    wavex_sample_load_save_update(g_sample_load_save_page);
}
```

**After** (Preferred):
```cpp
// Pages no longer exposed globally
// Navigator manages page lifecycle
// Shared services accessed via dependency injection or context
if (auto* sample_page = wavex_sample_load_save_get_active()) {
    wavex_sample_load_save_update(sample_page);
}
```

## Page Implementation Guide

### Example: Simple Page
```cpp
class MyCustomPage : public UIPage {
public:
    const char* name() const override { return "My Page"; }

    std::vector<SoftkeyDef> getSoftkeys() const override {
        return {
            {0, "Back", [](void*) { UINavigator::instance().pop(); }},
            {5, "Select", [](void*) { /* handle select */ }}
        };
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

**See**: `sample_load_save.cpp` lines 210, 259

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
2. Implement `name()`, `getSoftkeys()`, `onEnter()`, `onExit()`
3. Register in navigator initialization (typically in `ui_navigation_integration.cpp`)
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

- **LVGL Threading Rules**: `.cursor/rules/lvgl-threading.mdc`
- **System Architecture**: `docs/architecture.md`
- **Page implementation how-to**: `docs/ui-system-implementation-guide.md`
- Historical: `docs/archive/navigation-integration-guide.md`, `docs/archive/sample-browser-redesign.md`
