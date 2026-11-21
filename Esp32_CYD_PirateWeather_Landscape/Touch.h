// =====================================================================================
//  Touch.h  -  XPT2046 bit-bang driver for the CYD (ESP32-2432S028R), landscape 320x240
//
//  The CYD touch controller is NOT on the display SPI bus but on its own pins.
//  Bit-banging avoids any SPI conflict with TFT_eSPI and needs no extra library.
// =====================================================================================
#pragma once
#include <Arduino.h>

#define XPT_PIN_IRQ   36
#define XPT_PIN_MOSI  32
#define XPT_PIN_MISO  39
#define XPT_PIN_CLK   25
#define XPT_PIN_CS    33

#ifndef TOUCH_Z_THRESHOLD
#define TOUCH_Z_THRESHOLD 350
#endif

class CYDTouch {
public:
  // Calibration reference points (screen coordinates, landscape 320x240)
  static const int16_t X_REF0 = 20;
  static const int16_t X_REF1 = 300;
  static const int16_t Y_REF0 = 20;
  static const int16_t Y_REF1 = 220;
  static const int16_t SCR_W  = 320;
  static const int16_t SCR_H  = 240;

  // Calibration: raw value at X_REF0 / X_REF1 and Y_REF0 / Y_REF1
  bool    swapXY = false;
  int16_t xRaw0  = 400;
  int16_t xRaw1  = 3700;
  int16_t yRaw0  = 400;
  int16_t yRaw1  = 3700;
  bool    flip   = false;   // screen rotated 180° relative to calibration

  void begin() {
    pinMode(XPT_PIN_IRQ, INPUT);
    pinMode(XPT_PIN_MISO, INPUT);
    pinMode(XPT_PIN_MOSI, OUTPUT);
    pinMode(XPT_PIN_CLK, OUTPUT);
    pinMode(XPT_PIN_CS, OUTPUT);
    digitalWrite(XPT_PIN_CS, HIGH);
    digitalWrite(XPT_PIN_CLK, LOW);
    digitalWrite(XPT_PIN_MOSI, LOW);
  }

  // Raw read. a = X channel (0xD0), b = Y channel (0x90), z = pressure.
  bool readRaw(int16_t &a, int16_t &b, int16_t &z) {
    digitalWrite(XPT_PIN_CS, LOW);
    int z1 = xfer(0xB1);
    int z2 = xfer(0xC1);
    z = (int16_t)(z1 + 4095 - z2);
    if (z1 == 0 || z < TOUCH_Z_THRESHOLD) {
      xfer(0xD0);                       // power down, PENIRQ enabled
      digitalWrite(XPT_PIN_CS, HIGH);
      return false;
    }
    xfer(0x91);                         // dummy conversion (first one is noisy)
    long sa = 0, sb = 0;
    for (int i = 0; i < 4; i++) {
      sb += xfer(0x91);
      sa += xfer(0xD1);
    }
    xfer(0xD0);
    digitalWrite(XPT_PIN_CS, HIGH);
    a = (int16_t)(sa / 4);
    b = (int16_t)(sb / 4);
    return true;
  }

  // Calibrated screen point (0..319 / 0..239)
  bool getPoint(int16_t &x, int16_t &y) {
    int16_t a, b, z;
    if (!readRaw(a, b, z)) return false;
    int16_t rx = swapXY ? b : a;
    int16_t ry = swapXY ? a : b;
    long dx = (long)xRaw1 - xRaw0;
    long dy = (long)yRaw1 - yRaw0;
    if (dx == 0) dx = 1;
    if (dy == 0) dy = 1;
    long sx = X_REF0 + ((long)rx - xRaw0) * (X_REF1 - X_REF0) / dx;
    long sy = Y_REF0 + ((long)ry - yRaw0) * (Y_REF1 - Y_REF0) / dy;
    if (sx < 0) sx = 0;
    if (sx > SCR_W - 1) sx = SCR_W - 1;
    if (sy < 0) sy = 0;
    if (sy > SCR_H - 1) sy = SCR_H - 1;
    if (flip) {
      sx = SCR_W - 1 - sx;
      sy = SCR_H - 1 - sy;
    }
    x = (int16_t)sx;
    y = (int16_t)sy;
    return true;
  }

private:
  uint16_t xfer(uint8_t cmd) {
    uint16_t result = 0;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(XPT_PIN_MOSI, (cmd >> i) & 1);
      digitalWrite(XPT_PIN_CLK, HIGH);
      delayMicroseconds(1);
      digitalWrite(XPT_PIN_CLK, LOW);
      delayMicroseconds(1);
    }
    digitalWrite(XPT_PIN_MOSI, LOW);
    for (int i = 11; i >= 0; i--) {
      digitalWrite(XPT_PIN_CLK, HIGH);
      delayMicroseconds(1);
      digitalWrite(XPT_PIN_CLK, LOW);
      delayMicroseconds(1);
      result |= (uint16_t)(digitalRead(XPT_PIN_MISO) ? 1 : 0) << i;
    }
    return result;
  }
};
