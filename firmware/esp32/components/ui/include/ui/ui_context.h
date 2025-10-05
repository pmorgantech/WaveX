// WaveX UI Context abstraction
#pragma once

#include <functional>
#include <memory>
#include <string>
#include "ui/input_event.h"

namespace wavex_ui {

using InputHandler = std::function<void(const InputEvent&)>;

class UIContext {
public:
    UIContext(std::string name, InputHandler handler)
        : name_(std::move(name)), handler_(std::move(handler)) {}

    inline const std::string& name() const { return name_; }

    inline void handleEvent(const InputEvent& evt) const {
        if (handler_) handler_(evt);
    }

private:
    std::string name_;
    InputHandler handler_;
};

} // namespace wavex_ui


