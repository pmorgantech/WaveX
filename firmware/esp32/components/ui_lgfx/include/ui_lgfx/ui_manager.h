#pragma once
#include <stack>
#include "ui_lgfx/screen.h"

class UIManager {
 public:
  static UIManager& instance();
  void push(Screen* s);
  void pop();
  Screen* current();
  void redraw();
  void dispatch(const UIEvent &ev);

 private:
  UIManager() = default;
  std::stack<Screen*> screens_;
};


