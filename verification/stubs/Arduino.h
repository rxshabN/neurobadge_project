#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
using std::isnan;
using std::fabs;
using std::pow;

#define ESP_ARDUINO_VERSION_MAJOR 2
#define IRAM_ATTR
#define PI 3.1415926535897932384626433832795
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define FALLING 2
#define F(x) (x)
#define constrain(a,l,h) ((a)<(l)?(l):((a)>(h)?(h):(a)))

typedef std::string String;

void pinMode(int, int);
void digitalWrite(int, int);
int  digitalRead(int);
int  analogRead(int);
void delay(unsigned long);
void delayMicroseconds(unsigned int);
unsigned long millis();
unsigned long micros();
void attachInterrupt(int, void(*)(), int);
int  digitalPinToInterrupt(int);
bool setCpuFrequencyMhz(uint32_t);

void ledcSetup(uint8_t, double, uint8_t);
void ledcAttachPin(uint8_t, uint8_t);
bool ledcAttach(uint8_t, uint32_t, uint8_t);
void ledcWrite(uint8_t, uint32_t);

struct SerialStub {
  void begin(unsigned long);
  int  available();
  int  read();
  template <typename T> void print(T) {}
  template <typename T> void print(T, int) {}
  template <typename T> void println(T) {}
  template <typename T> void println(T, int) {}
  void println() {}
};
extern SerialStub Serial;

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef void*    SemaphoreHandle_t;
typedef void*    TaskHandle_t;
#define tskIDLE_PRIORITY 0
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY ((TickType_t)0xffffffff)
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portYIELD_FROM_ISR() do{}while(0)

SemaphoreHandle_t xSemaphoreCreateBinary();
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t*);
BaseType_t xTaskCreatePinnedToCore(void(*)(void*), const char*, uint32_t, void*, uint32_t, TaskHandle_t*, int);
void vTaskDelete(TaskHandle_t);
void vTaskDelayUntil(TickType_t*, TickType_t);
TickType_t xTaskGetTickCount();
