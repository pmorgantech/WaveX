#pragma once
#include "ui_lgfx/screen.h"

// Simple softkey touch zone mapping for 480x320
inline bool touch_in_softkey_left(int x, int y) { return (y >= 296 && y < 320 && x >= 0 && x < 160); }
inline bool touch_in_softkey_mid(int x, int y)  { return (y >= 296 && y < 320 && x >= 160 && x < 320); }
inline bool touch_in_softkey_right(int x, int y){ return (y >= 296 && y < 320 && x >= 320 && x < 480); }


