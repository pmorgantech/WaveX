#include "ui_lgfx/ui_manager.h"
#include <cassert>

UIManager& UIManager::instance() {
  static UIManager mgr;
  return mgr;
}

void UIManager::push(Screen* s) {
  assert(s);
  screens_.push(s);
  s->draw();
}

void UIManager::pop() {
  if (!screens_.empty()) screens_.pop();
  if (!screens_.empty()) screens_.top()->draw();
}

Screen* UIManager::current() {
  return screens_.empty() ? nullptr : screens_.top();
}

void UIManager::redraw() {
  if (!screens_.empty()) screens_.top()->draw();
}

void UIManager::dispatch(const UIEvent &ev) {
  if (!screens_.empty()) screens_.top()->processEvent(ev);
}


