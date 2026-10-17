#pragma once
#include <Arduino.h>
class LiquidCrystal_I2C {
public:
  LiquidCrystal_I2C(int, int, int);
  void init();
  void backlight();
  void clear();
  void setCursor(int, int);
  template <typename T> void print(T) {}
  template <typename T> void print(T, int) {}
};
