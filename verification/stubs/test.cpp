#include "sketch.cpp"
#include <cassert>
#include <cstdio>

SerialStub Serial;  WiFiStub WiFi;  TwoWire Wire;  FirebaseStub Firebase;
static unsigned long g_ms = 0, g_us = 0;
void pinMode(int,int){} void digitalWrite(int,int){} int digitalRead(int){return 0;}
int analogRead(int){return 500;} void delay(unsigned long m){g_ms+=m;}
void delayMicroseconds(unsigned int u){g_us+=u;}
unsigned long millis(){return g_ms;} unsigned long micros(){return g_us;}
void attachInterrupt(int,void(*)(),int){} int digitalPinToInterrupt(int p){return p;}
bool setCpuFrequencyMhz(uint32_t){return true;}
void ledcSetup(uint8_t,double,uint8_t){} void ledcAttachPin(uint8_t,uint8_t){}
bool ledcAttach(uint8_t,uint32_t,uint8_t){return true;} void ledcWrite(uint8_t,uint32_t){}
void SerialStub::begin(unsigned long){}
int SerialStub::available(){return 0;}
int SerialStub::read(){return -1;}
int WiFiStub::status(){return 0;} void WiFiStub::disconnect(){}
void WiFiStub::begin(const char*,const char*){} void WiFiStub::mode(int){}
void TwoWire::begin(int,int){} void TwoWire::setClock(uint32_t){}
DHT::DHT(int,int){} void DHT::begin(){} float DHT::readTemperature(){return 30;} float DHT::readHumidity(){return 60;}
LiquidCrystal_I2C::LiquidCrystal_I2C(int,int,int){} void LiquidCrystal_I2C::init(){}
void LiquidCrystal_I2C::backlight(){} void LiquidCrystal_I2C::clear(){}
void LiquidCrystal_I2C::setCursor(int,int){}
bool RTDBStub::setInt(FirebaseData*,const char*,int){return true;}
bool RTDBStub::setFloat(FirebaseData*,const char*,float){return true;}
bool RTDBStub::setString(FirebaseData*,const char*,const char*){return true;}
bool RTDBStub::setBool(FirebaseData*,const char*,bool){return true;}
bool RTDBStub::updateNodeSilent(FirebaseData*,const char*,FirebaseJson*){return true;}
bool FirebaseStub::signUp(FirebaseConfig*,FirebaseAuth*,const char*,const char*){return true;}
void FirebaseStub::begin(FirebaseConfig*,FirebaseAuth*){} void FirebaseStub::reconnectWiFi(bool){}
bool FirebaseStub::ready(){return false;}
SemaphoreHandle_t xSemaphoreCreateBinary(){return (void*)1;}
SemaphoreHandle_t xSemaphoreCreateMutex(){return (void*)1;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t){return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t){return pdTRUE;}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t,BaseType_t*){return pdTRUE;}
BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,uint32_t,TaskHandle_t*,int){return pdPASS;}
void vTaskDelete(TaskHandle_t){} void vTaskDelayUntil(TickType_t*,TickType_t){}
TickType_t xTaskGetTickCount(){return (TickType_t)g_ms;}

static int fails = 0;
#define CHECK(c,msg) do{ if(!(c)){printf("  FAIL %s\n",msg); fails++;} else printf("  pass %s\n",msg);}while(0)

static void resetAll(){
  hazard_latched=false; temp_base=NAN; hum_base=NAN;
  clear_count=0; ingress_hold=false; ingress_ms=0;
  trend_count=0; trend_head=0; mq135_r0=100.0f;
}

int main(){
  printf("T1 trend regression\n"); resetAll();
  for(int i=0;i<TREND_N;i++) trendPush(400.0f + 100.0f*(i*2.0f/60.0f), i*2000);
  float sl = trendSlope();
  CHECK(fabs(sl-100.0f)<1.0f, "100 ppm/min ramp recovered");
  resetAll();
  for(int i=0;i<TREND_N;i++) trendPush(420.0f, i*2000);
  CHECK(fabs(trendSlope())<1.0f, "flat series -> ~0 slope");

  printf("T2 predictive branch\n"); resetAll();
  for(int i=0;i<TREND_N;i++) trendPush(800.0f + 100.0f*(i*2.0f/60.0f), i*2000);
  Plan p; buildPlan(p, 850.0f, 30.0f, 60.0f, 50.0f, 0);
  CHECK(p.state==S_PREVENT, "rising 100 ppm/min at 850 -> S_PREVENT");
  CHECK(p.eta_s>=85 && p.eta_s<=95, "eta approx 90 s");
  CHECK(relayFor(p.state)==true, "pre-ventilation closes the relay");
  CHECK(p.severity>=15 && p.severity<=25, "severity low at the far edge");
  buildPlan(p, 950.0f, 30.0f, 60.0f, 50.0f, 0);
  CHECK(p.eta_s<=35 && p.severity>=40, "closer crossing -> higher severity");

  printf("T3 slow drift must NOT pre-actuate\n"); resetAll();
  for(int i=0;i<TREND_N;i++) trendPush(900.0f + 10.0f*(i*2.0f/60.0f), i*2000);
  buildPlan(p, 905.0f, 30.0f, 60.0f, 50.0f, 0);
  CHECK(p.state==S_NORMAL, "10 ppm/min is noise, stays NORMAL");

  printf("T4 asymmetric response\n"); resetAll();
  for(int i=0;i<60;i++) classifyCore(420.0f, 30.0f, 60.0f);
  SystemState s = classifyCore(1100.0f, 32.5f, 62.0f);
  CHECK(s==S_VENTILATION, "hazard + occupancy signature -> VENTILATION");
  CHECK(relayFor(s)==true, "VENTILATION energises the relay");
  resetAll();
  for(int i=0;i<60;i++) classifyCore(420.0f, 30.0f, 60.0f);
  s = classifyCore(1100.0f, 30.0f, 60.0f);
  CHECK(s==S_INTRUSION, "hazard without signature -> INTRUSION");
  CHECK(relayFor(s)==false, "INTRUSION withholds ventilation");
  buildPlan(p, 1100.0f, 30.0f, 60.0f, 50.0f, 0);
  CHECK(p.state==S_INTRUSION && p.severity>=80, "intrusion severity floored at 80");

  printf("T5 hysteresis\n");
  s = classifyCore(900.0f, 30.0f, 60.0f);
  CHECK(s!=S_NORMAL, "900 ppm inside the band stays latched");
  s = classifyCore(800.0f, 30.0f, 60.0f);
  CHECK(s!=S_NORMAL, "one sample at 800 ppm does not clear the latch");
  classifyCore(800.0f, 30.0f, 60.0f);
  s = classifyCore(800.0f, 30.0f, 60.0f);
  CHECK(s==S_NORMAL, "three consecutive samples at 800 ppm clear the latch");

  printf("T6 LED severity curve\n");
  CHECK(gammaDuty(0)==0, "severity 0 -> off");
  bool mono=true; for(int i=1;i<100;i++) if(gammaDuty(i)>gammaDuty(i+1)) mono=false;
  CHECK(mono, "monotonic in severity");
  CHECK(gammaDuty(100)==255, "severity 100 -> full scale");
  CHECK(gammaDuty(1)>=8, "lowest step still visible");

  printf("T7 buzzer pattern timing over 10 s\n");
  struct { SystemState st; int want; const char* nm; } cases[] = {
    {S_VENTILATION, 4, "VENTILATION: 2 beeps per 5 s"},
    {S_INTRUSION,  15, "INTRUSION: 3 beeps per 2 s"},
    {S_PREVENT,     1, "PRE-VENT: 1 chirp per 10 s"},
    {S_NORMAL,      0, "NORMAL: silent"},
  };
  for (auto &c : cases) {
    const Step *pat; uint8_t n; patternFor(c.st,&pat,&n);
    uint32_t el=0; uint8_t idx=0; int edges=0; bool prev=false;
    // One tick short of 10 s: landing exactly on a period boundary would count
    // the first edge of the next cycle as well.
    int iters = 10000/ACT_TICK_MS - 1;
    for (int i=0;i<iters;i++) {
      el += ACT_TICK_MS;
      while (el >= pat[idx].ms) { el -= pat[idx].ms; idx=(idx+1)%n; }
      bool on = pat[idx].on;
      if (on && !prev) edges++;
      prev = on;
    }
    char m[96]; snprintf(m,sizeof(m),"%s (got %d)",c.nm,edges);
    CHECK(edges==c.want, m);
  }

  printf("T8 DHT11 frame validation\n");
  CHECK(!dhtPlausible(0.1f, 0.0f), "all-zero frame (T=0.1, H=0) rejected");
  CHECK(!dhtPlausible(NAN, 55.0f), "NaN temperature rejected");
  CHECK(dhtPlausible(25.1f, 61.0f), "room reading accepted");
  resetAll();
  temp_base = 25.0f; hum_base = 60.0f;
  CHECK(classifyCore(1100.0f, 25.2f, 60.0f) == S_INTRUSION,
        "seeded baseline: gas alone right after boot -> ISOLATE");

  printf("T9 fluctuating ingress (bench log 17-10-2026 replayed)\n"); resetAll();
  temp_base = 26.2f; hum_base = 65.0f; g_ms = 1000000;
  {
    const float seq[] = {1020, 670, 961, 1200, 760, 1052, 699, 540, 502, 492};
    bool closed = false; SystemState last = S_NORMAL;
    for (float v : seq) {
      trendPush(v, g_ms); Plan q; buildPlan(q, v, 26.4f, 65.0f, 50.0f, 0);
      if (relayFor(q.state)) closed = true;
      last = q.state; g_ms += 2000;
    }
    CHECK(!closed, "relay never closes while the source fluctuates");
    CHECK(last == S_NORMAL, "clears to NORMAL after 3 clean samples");
  }

  printf("T10 ingress hold on the predictive stage\n");
  {
    uint32_t t_isolate = ingress_ms;
    trend_count = 0; trend_head = 0; g_ms = t_isolate + 60000;
    for (int i = 0; i < TREND_N; i++) { trendPush(800.0f + 100.0f*(i*2.0f/60.0f), g_ms); g_ms += 2000; }
    Plan q; buildPlan(q, 850.0f, 26.4f, 65.0f, 50.0f, 0);
    CHECK(q.state == S_NORMAL, "rising trend 60 s after ISOLATE: no pre-ventilation");
    trend_count = 0; trend_head = 0; g_ms = t_isolate + 130000;
    for (int i = 0; i < TREND_N; i++) { trendPush(800.0f + 100.0f*(i*2.0f/60.0f), g_ms); g_ms += 2000; }
    Plan r; buildPlan(r, 850.0f, 26.4f, 65.0f, 50.0f, 0);
    CHECK(r.state == S_PREVENT, "rising trend after the 120 s hold: pre-ventilates");
  }

  printf("\n%s\n", fails? "FAILURES PRESENT" : "ALL CHECKS PASSED");
  return fails?1:0;
}
