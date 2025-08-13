#pragma once
#include <stdint.h>

struct UIEvent {
  enum Type { TOUCH, BUTTON, ENCODER } type;
  int x{0}, y{0};
  int button{0};
  int delta{0};
};

class Screen {
 public:
  virtual ~Screen() = default;
  virtual void draw() = 0;
  virtual void processEvent(const UIEvent &ev) = 0;
  virtual bool needsAutoUpdate() const { return false; } // Override in screens that need auto-update
  virtual void updateContent() { } // Override in screens that need content-only updates
};


