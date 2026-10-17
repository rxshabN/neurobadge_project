#pragma once
#include <Arduino.h>
struct TwoWire {
  void begin(int, int);
  void setClock(uint32_t);
};
extern TwoWire Wire;
