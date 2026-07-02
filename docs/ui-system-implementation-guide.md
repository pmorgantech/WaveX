# WaveX UI System Implementation Guide

## Overview

This guide provides practical implementation details for developers working with the WaveX UI Navigation System. It covers page creation, input handling, softkey configuration, and integration patterns.

## Quick Start

### 1. Basic Integration

To integrate the navigation system into your existing UI task:

```cpp
// In ui_task.cpp
#include "ui/ui_navigation_integration.h"

void ui_task(void *pvParameters) {
    // ... existing initialization ...

    // Initialize navigation system
    wavex_ui::initNavigationSystem();

    // Set navigation context for input handling
    wavex_ui::InputDispatcher::instance().setActiveContext(
        wavex_ui::createNavigationContext()
    );

    // ... rest of task ...
}
```

### 2. Creating Custom Pages

Create a new page by inheriting from `UIPage`:

```cpp
#include "ui/ui_page.h"

class MyCustomPage : public wavex_ui::UIPage {
public:
    const char* name() const override { return "MyPage"; }

    void onEnter(lv_obj_t* parent) override {
        root_ = lv_obj_create(parent);
        lv_obj_set_size(root_, lv_pct(100), lv_pct(100));

        // Create your UI elements here
        auto label = lv_label_create(root_);
        lv_label_set_text(label, "Hello World!");
        lv_obj_center(label);
    }

    void onExit() override {
        if (root_) {
            lv_obj_del(root_);
            root_ = nullptr;
        }
    }

    void onInput(const wavex_ui::InputEvent& evt) override {
        switch (evt.type) {
            case wavex_ui::InputType::ButtonPress:
                wavex_ui::UINavigator::instance().pop();
                break;
            default:
                break;
        }
    }

    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> getSoftkeys() override {
        std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};
        keys[0] = {"Back", []() { wavex_ui::UINavigator::instance().pop(); }};
        return keys;
    }
};
```

### 3. Creating Menu Pages

Use `UIMenuPage` for hierarchical menus:

```cpp
#include "ui/ui_menu_page.h"

std::shared_ptr<wavex_ui::UIPage> createMyMenu() {
    auto menu = std::make_shared<wavex_ui::UIMenuPage>("My Menu");

    menu->addItem("Option 1", []() {
        ESP_LOGI("MENU", "Option 1 selected");
        // Handle option 1
    });

    menu->addItem("Submenu", []() {
        wavex_ui::UINavigator::instance().push(createSubMenu());
    });

    return menu;
}
```

### 4. Creating Settings Pages

Use `UISettingsPage` for parameter editing:

```cpp
#include "ui/ui_settings_page.h"

std::shared_ptr<wavex_ui::UIPage> createMySettings() {
    auto settings = std::make_shared<wavex_ui::UISettingsPage>("My Settings");

    settings->addSetting("Volume", 75, 0, 100, [](int value) {
        ESP_LOGI("SETTINGS", "Volume set to %d%%", value);
        // Apply volume setting
    });

    settings->addSetting("Channel", 1, 1, 16, [](int value) {
        ESP_LOGI("SETTINGS", "Channel set to %d", value);
        // Apply channel setting
    });

    return settings;
}
```

## Navigation Patterns

### 1. Basic Navigation

```cpp
// Push a new page
wavex_ui::UINavigator::instance().push(createMyPage());

// Pop current page
wavex_ui::UINavigator::instance().pop();

// Check if can pop
if (wavex_ui::UINavigator::instance().canPop()) {
    // Safe to pop
}
```

### 2. Conditional Navigation

```cpp
void handleMenuSelection() {
    if (someCondition) {
        // Navigate to different page based on condition
        wavex_ui::UINavigator::instance().push(createConditionalPage());
    } else {
        // Show error or alternative action
        ESP_LOGW("NAV", "Cannot navigate - condition not met");
    }
}
```

### 3. State Preservation

Pages automatically preserve their state between visits:

```cpp
class StatefulPage : public wavex_ui::UIPage {
private:
    int currentSelection_ = 0;
    std::string currentPath_ = "/default";

public:
    void onEnter(lv_obj_t* parent) override {
        // Recreate UI with preserved state
        // currentSelection_ and currentPath_ are still valid
    }

    void onExit() override {
        // Don't clear state variables
        // Only clean up LVGL objects
    }
};
```

## Input Handling

### 1. Basic Input Handling

```cpp
void onInput(const wavex_ui::InputEvent& evt) override {
    switch (evt.type) {
        case wavex_ui::InputType::EncoderLeft:
            // Move selection left/up
            break;
        case wavex_ui::InputType::EncoderRight:
            // Move selection right/down
            break;
        case wavex_ui::InputType::EncoderClick:
        case wavex_ui::InputType::ButtonPress:
            // Select/activate
            break;
        default:
            break;
    }
}
```

### 2. Advanced Input Handling

```cpp
void onInput(const wavex_ui::InputEvent& evt) override {
    switch (evt.type) {
        case wavex_ui::InputType::EncoderLeft:
            if (editingMode_) {
                adjustValue(-1);
            } else {
                moveSelection(-1);
            }
            break;
        case wavex_ui::InputType::EncoderRight:
            if (editingMode_) {
                adjustValue(+1);
            } else {
                moveSelection(+1);
            }
            break;
        case wavex_ui::InputType::ButtonPress:
            toggleEditMode();
            break;
        default:
            break;
    }
}
```

## Softkey Configuration

### 1. Basic Softkeys

```cpp
std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> getSoftkeys() override {
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};

    keys[0] = {"Back", []() { wavex_ui::UINavigator::instance().pop(); }};
    keys[1] = {"Select", [this]() { activateSelection(); }};

    return keys;
}
```

### 2. Dynamic Softkeys

```cpp
std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> getSoftkeys() override {
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};

    keys[0] = {"Back", []() { wavex_ui::UINavigator::instance().pop(); }};

    if (hasSelection()) {
        keys[1] = {"Edit", [this]() { editSelection(); }};
        keys[2] = {"Delete", [this]() { deleteSelection(); }};
    }

    if (canCreateNew()) {
        keys[3] = {"New", [this]() { createNew(); }};
    }

    return keys;
}
```

### 3. Conditional Softkeys

```cpp
std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> getSoftkeys() override {
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};

    // Always available
    keys[0] = {"Back", []() { wavex_ui::UINavigator::instance().pop(); }};

    // Conditionally available
    if (wavex_ui::UINavigator::instance().canPop()) {
        keys[1] = {"Up", []() { navigateUp(); }};
    }

    if (isPlaying_) {
        keys[2] = {"Stop", [this]() { stopPlayback(); }};
    } else {
        keys[2] = {"Play", [this]() { startPlayback(); }};
    }

    return keys;
}
```

## Best Practices

### 1. Page Lifecycle

- **onEnter**: Create LVGL objects, restore state
- **onExit**: Clean up LVGL objects, preserve state
- **onInput**: Handle user input, update UI
- **getSoftkeys**: Return current softkey configuration

### 2. Memory Management

- Use `std::shared_ptr` for page objects
- Clean up LVGL objects in `onExit`
- Preserve essential state between visits
- Avoid memory leaks in input handlers

### 3. Input Handling

- Handle all relevant input types
- Provide clear feedback for user actions
- Use consistent navigation patterns
- Implement proper error handling

### 4. State Management

- Preserve user selections and preferences
- Maintain navigation history
- Handle edge cases gracefully
- Provide clear visual feedback

## Page-by-Page Implementation

### Main Menu (UIMenuPage)
- **Layout**: Two-button grid (Sample, System)
- **Navigation**: Encoder rotates between options, click/press selects
- **Softkeys**: None (uses direct selection)

### Sample Menu (UIMenuPage)
- **Items**: Record, Edit, Load/Save
- **Softkeys**: Back button (when stack allows pop)
- **Navigation**: Encoder moves selection highlight, click/press activates

### System Menu (UIMenuPage)
- **Items**: Diagnostics, Settings
- **Softkeys**: Back button (when stack allows pop)
- **Navigation**: Same as Sample menu

### Sample Load/Save Page (UISampleLoadSavePage)
- **Layout**: File browser (left) + metadata panel (right)
- **Softkeys**: Back, Audition, Load, Save
- **Navigation**: Touch selection + encoder navigation through files

### Diagnostics Page (UIDiagnosticsPage)
- **Layout**: Three-column layout (ESP32 status, Daisy link, audio meters)
- **Softkeys**: Back button
- **Updates**: Live data every 500ms via timer

### Custom Pages
- **Layout**: Flexible - use full content area (1280×545px)
- **Navigation**: Encoder for parameter control, touch for direct interaction
- **Softkeys**: Context-specific actions (Back, Save, Cancel, etc.)

## Adding to Navigation System

### Adding to Existing Menus

Edit `firmware/esp32/components/ui/src/ui_navigation_integration.cpp`:

#### Under Sample Menu

```cpp
std::shared_ptr<wavex_ui::UIPage> createSampleMenu() {
    auto menu = std::make_shared<wavex_ui::UIMenuPage>("Sample Menu");

    // Existing items...
    menu->addItem("Record", []() { /* ... */ });
    menu->addItem("Edit", []() { /* ... */ });
    menu->addItem("Load/Save", []() { /* ... */ });

    // Add your new page
    menu->addItem("My Custom", []() {
        wavex_ui::UINavigator::instance().push(std::make_shared<MyCustomPage>());
    });

    return menu;
}
```

#### Under System Menu

```cpp
std::shared_ptr<wavex_ui::UIPage> createSystemMenu() {
    auto menu = std::make_shared<wavex_ui::UIMenuPage>("System Menu");

    // Existing items...
    menu->addItem("Diagnostics", []() { /* ... */ });
    menu->addItem("Settings", []() { /* ... */ });

    // Add your new page
    menu->addItem("My Settings", []() {
        wavex_ui::UINavigator::instance().push(std::make_shared<MyCustomPage>());
    });

    return menu;
}
```

### Creating New Menu Hierarchies

Create new menu functions for complex hierarchies:

```cpp
// In ui_navigation_integration.cpp or your own file

std::shared_ptr<wavex_ui::UIPage> createToolsMenu() {
    auto menu = std::make_shared<wavex_ui::UIMenuPage>("Tools");

    menu->addItem("Analyzer", []() {
        wavex_ui::UINavigator::instance().push(std::make_shared<AnalyzerPage>());
    });

    menu->addItem("Generator", []() {
        wavex_ui::UINavigator::instance().push(std::make_shared<GeneratorPage>());
    });

    return menu;
}

// Then add to main menu:
menu->addItem("Tools", []() {
    wavex_ui::UINavigator::instance().push(createToolsMenu());
});
```

## Styling and Layout

Use the theme constants for consistent appearance:

```cpp
#include "../styles/ui_theme.h"

// Colors
UI_COLOR_BACKGROUND, UI_COLOR_HEADER, UI_COLOR_CONTENT
UI_COLOR_TEXT, UI_COLOR_BUTTON, UI_COLOR_SELECTED

// Fonts
UI_FONT_NORMAL (14pt), UI_FONT_TITLE (22pt), UI_FONT_HEADER (32pt), UI_FONT_HOTKEY (36pt)

// Sizes
UI_HEADER_HEIGHT (75px), UI_HOTKEY_HEIGHT (100px)
UI_PADDING_SMALL (5px), UI_PADDING_MEDIUM (10px), UI_PADDING_LARGE (15px)
```

## File Organization

- **Headers**: `firmware/esp32/components/ui/include/ui/`
- **Implementations**: `firmware/esp32/components/ui/src/`
- **Pages**: `firmware/esp32/components/ui/pages/` (for complex page implementations)

## Troubleshooting

### Common Issues

1. **Pages not appearing**: Check if `onEnter` is called and LVGL objects are created
2. **Input not working**: Verify input context is set correctly
3. **Softkeys not updating**: Ensure `getSoftkeys()` returns valid configuration
4. **Memory leaks**: Check `onExit` implementation for proper cleanup
5. **Navigation errors**: Verify stack operations and null checks

### Debug Tips

1. **Enable logging**: Use ESP_LOGI for navigation events
2. **Check stack depth**: Monitor navigation stack size
3. **Validate input**: Log input events to verify routing
4. **Test edge cases**: Test empty menus, null selections, etc.
5. **Monitor memory**: Check for memory leaks during navigation

## Example Implementation

See the following files for complete examples:

- `ui_main_menu.cpp` - Main menu implementation
- `ui_file_browser.cpp` - File browser with state preservation
- `ui_settings_page.cpp` - Settings page with parameter editing
- `ui_navigation_demo.cpp` - Complete integration example

These examples demonstrate the full capabilities of the navigation system and provide templates for creating new pages and menus.
