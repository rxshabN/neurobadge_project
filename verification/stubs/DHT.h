#pragma once
#include <Arduino.h>
#define DHT11 11
class DHT {
public:
  DHT(int, int);
  void begin();
  float readTemperature();
  float readHumidity();
};
