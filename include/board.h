#pragma once
#include <lvgl.h>
namespace board {
constexpr int Width = 320, Height = 480;
constexpr int Backlight = 1, Cs = 45, Clock = 47;
constexpr int D0 = 21, D1 = 48, D2 = 40, D3 = 39;
constexpr int TouchSda = 4, TouchScl = 8, TouchInterrupt = 3;
bool begin();
}
