#pragma once

#include <cstdint>

namespace Colour {
  constexpr uint16_t black = 0x0000;
  constexpr uint16_t white = 0xFFFF;
  constexpr uint16_t red = 0xF800;
  constexpr uint16_t green = 0x07E0;
  constexpr uint16_t yellow = 0xFFE0;
  constexpr uint16_t cyan = 0x07FF;
  constexpr uint16_t orange = 0xFD20;
  constexpr uint16_t purple = 0x781F;
  constexpr uint16_t gray = 0xAD55;
  constexpr uint16_t lime = 0xB7E0;
}

class Display {
  public:
    static bool begin();
    static uint16_t width();
    static uint16_t height();
    static void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    static void text(const char* s, int16_t x, int16_t y, uint16_t color, uint8_t size = 1, uint16_t bg = Colour::black);
    static void push_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data);
    static void backlight(bool on);
    static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
};
