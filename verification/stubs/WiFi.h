#pragma once
#include <Arduino.h>
#define WL_CONNECTED 3
#define WIFI_STA 1
struct WiFiStub {
  int  status();
  void disconnect();
  void begin(const char*, const char*);
  void mode(int);
};
extern WiFiStub WiFi;
