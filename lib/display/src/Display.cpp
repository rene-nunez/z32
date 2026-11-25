#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Display.h"
#include "pins.h"

static TFT_eSPI _tft;

bool Display::begin() {
  _tft.init();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  _tft.setRotation(1); // landscape 320x240
  _tft.setSwapBytes(true); // pushImage sends raw words: without the swap the blit/erase bursts come out byte-flipped (BGR glass),
                           // fillRect/text/drawPixel are unaffected by this flag
  _tft.fillScreen(Colour::black);

  return true;
}

uint16_t Display::width() {
  return _tft.width();
}

uint16_t Display::height() {
  return _tft.height();
}

void Display::fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  _tft.fillRect(x, y, w, h, color);
}

void Display::text(const char* s, int16_t x, int16_t y, uint16_t color, uint8_t size, uint16_t bg) {
  _tft.setCursor(x, y);
  _tft.setTextColor(color, bg);
  _tft.setTextSize(size);
  _tft.print(s);
}

void Display::backlight(bool on) {
  digitalWrite(TFT_BL, on ? HIGH : LOW);
}

void Display::push_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data) {
  if (w <= 0 || h <= 0) {
    return;
  }

  _tft.pushImage(x, y, (int32_t)w, (int32_t)h, (uint16_t*)data);
}

uint16_t Display::rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
