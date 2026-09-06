// WaveX UI Menu Item Definition
#pragma once

#include <functional>
#include <string>

namespace wavex_ui {

/// One row of a UIMenuPage.
///
/// A root-menu row answers three questions at once: where it goes (`label`),
/// what is in there (`purpose`), and what it is currently pointing at
/// (`context`). The third is the one that stops the menu being a table of
/// contents - "Play" is a place, "Play / Track 1" is a decision you can check
/// without opening it.
///
/// `context` and `ok` are polled about once a second while the menu is on
/// screen, so they must be cheap and must not block. Leave them empty when
/// there is no honest answer: a row with no context simply shows none, which
/// is correct, where a plausible-looking placeholder is not.
struct MenuItem {
    std::string label;
    std::string purpose;
    std::function<std::string()> context;
    std::function<bool()> ok;  // set to draw a health dot beside the context
    std::function<void()> onSelect;
};

}  // namespace wavex_ui
