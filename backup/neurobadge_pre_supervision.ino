/*
 * Neuro-Badge v3 :: Asymmetric Predictive Mitigation on a Dual-Core Edge Node
 * BCSE497J Project-I | Rishab Rajesh Nagwani (23BAI0085) | VIT Vellore
 * Guide: Dr. Yokesh Babu S
 *
 * WHAT v3 ADDS OVER v2, AND WHY
 *
 * (a) Real actuators instead of a control path on the onboard LED.
 *     SONGLE SRD-05VDC relay on GPIO23 switches the ventilation load.
 *     5 V active buzzer on GPIO22 through a 2N2222 low-side switch.
 *     Onboard LED on GPIO2 moved from digitalWrite to LEDC PWM.
 *
 * (b) Graded annunciation replaces the haptic channel.
 *     Severity is carried by LED intensity (gamma-corrected so perceived
 *     brightness, not duty cycle, is linear in severity) and hazard CLASS is
 *     carried by buzzer rhythm. The buzzers are active units with a fixed
 *     internal oscillator, so pitch is not available as a coding dimension;
 *     rhythm is, and rhythm survives a noisy room better than pitch anyway.
 *
 * (c) Proactive actuation. A least-squares trend on the gas channel over a
 *     30 s window projects the time remaining until the hazard threshold is
 *     crossed. If that projection falls inside a 90 s horizon the system
 *     starts ventilating BEFORE the threshold is reached. A ventilation loop
 *     has dead time of tens of seconds; acting on threshold crossing means the
 *     occupants are already exposed while the air is being exchanged.
 *
 * (d) Dual-core FreeRTOS partitioning with semaphore handoff.
 *     APP_CPU (core 1) carries the deterministic control path: taskSense and
 *     taskActuate. PRO_CPU (core 0) carries taskUiNet next to the Wi-Fi and
 *     TCP stack. Nothing polls: every task blocks on a semaphore or on a
 *     periodic delay, so the scheduler runs the idle task between events and
 *     the control core is measurably idle. The split is not decorative - the
 *     DHT11 read holds interrupts for milliseconds, an LCD frame costs about
 *     25 ms on the I2C bus and a Firebase write can block for hundreds of
 *     milliseconds. On a single-threaded loop() every one of those delays is
 *     added to actuation latency. Here none of them are, and the firmware
 *     measures and reports the latency it actually achieves.
 *
 * Retained from v2: datasheet Rs/R0 gas curve, power-on R0 reference,
 * baseline-relative occupancy signature, hazard hysteresis, non-blocking Wi-Fi.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// Change these two at the venue. The ESP32 radio is 802.11 b/g/n and cannot
// see a 5 GHz network at all, so the hotspot must be set to 2.4 GHz.
#define WIFI_SSID       "Rishab's OnePlus Nord"
#define WIFI_PASSWORD   "gitsomemf"

#define API_KEY         "AIzaSyCWoqmmU3XD9cih0_SqemQ3GLk361g4d-M"
#define DATABASE_URL    "neuro-badge-default-rtdb.asia-southeast1.firebasedatabase.app"

// Pin map
#define DHTPIN          4
#define DHTTYPE         DHT11
#define MQ135_PIN       34      // ADC1_CH6, input only
#define BUTTON_PIN      19      // fatigue logger, internal pull-up
#define I2C_SDA         5
#define I2C_SCL         18
#define RELAY_PIN       23      // SONGLE relay module IN
#define BUZZER_PIN      22      // 2N2222 base through 1 kOhm
#define LED_PIN         2       // onboard LED, PWM severity channel

// Most single-channel SONGLE breakouts drive the coil through an inverting
// transistor or optocoupler, so IN LOW energises the relay. If yours clicks
// the wrong way round, change this to 0 - do not invert the logic anywhere
// else in the file.
#define RELAY_ACTIVE_LOW  1

// Set to 0 if the serial console garbles. 80 MHz roughly halves core current
// against the 240 MHz default and is still far above what this workload needs.
#define LOW_POWER_CPU     1

#define LED_LEDC_CH     0
#define LED_LEDC_FREQ   5000
#define LED_LEDC_BITS   8
#define LED_IDLE_DUTY   3       // barely-lit proof of life in S_NORMAL

// MQ135 sensor model. Verify against your breakout before trusting the ppm.
#define MQ135_VCC       5.0
#define MQ135_RL        10.0    // load resistor on the breakout, kOhm
#define DIVIDER_GAIN    1.0     // 1.0 = AOUT straight to GPIO34
#define ADC_MAX         4095.0
#define ADC_VREF        3.3
#define ADC_OVERSAMPLE  16
#define PARA            116.6020682
#define PARB            2.769034857
#define ATMO_CO2        415.0

// R0 is established at power-on rather than hard-coded. Three clean-air
// calibrations in one session on 28-08-2026 gave R0 = 208.0, 247.6 and
// 346.2 kOhm: Rs rose 66% while the air was unchanged, because the SnO2 heater
// is still baking off adsorbed contaminant and the datasheet wants 24 hours of
// preheat. Freezing the 90-minute figure and then powering up 20 minutes
// before a demonstration would report about 1700 ppm in clean air and latch a
// permanent false hazard. Referencing to the baseline the sensor actually has
// on the day makes the hazard a RATIO - Rs falling to 73% of clean air - which
// holds at any warm-up state. Pin this only after a genuine 24 h preheat.
#define MQ135_R0        0.0

// Thresholds
#define PPM_HAZARD_ON   1000.0
#define PPM_HAZARD_OFF  850.0

// Occupancy signature, measured relative to the room's own baseline. An
// absolute "warmer than 30 C" test is useless in a climate already sitting near
// 30 C. Occupants are simultaneously a CO2 source, a heat source and a moisture
// source; solvent vapour is none of those. That is what separates the two
// hazards, and the two hazards take opposite countermeasures.
#define TEMP_RISE_C     1.5
#define HUM_RISE_PCT    8.0
#define BASELINE_ALPHA  0.02

// Predictive horizon
#define TREND_N             15      // 15 samples x 2 s = 30 s regression window
#define MIN_SLOPE_PPM_MIN   40.0f   // below this the rise is noise, not a trend
#define PREDICT_HORIZON_S   90      // act if the crossing is this close

// Timing
#define SENSE_PERIOD_MS 2000
#define ACT_TICK_MS     20      // annunciation frame; only ticks when animating
#define PLAN_STALE_MS   6000    // 3 missed sense cycles -> declare a fault
#define FATIGUE_HOLD_MS 3000
#define FATIGUE_CHIRP_MS 150
#define DEBOUNCE_US     250000UL
#define WIFI_RETRY_MS   15000

// Clean-air reference window. 30 s is right for a real run. Drop it to 5 while
// bench-testing so you are not waiting half a minute after every upload.
#define CAL_SECONDS     30

// Exercise relay, buzzer and LED once at boot, before calibration, so the
// hardware can be confirmed without waiting for a gas event.
#define SELFTEST_ON_BOOT 1

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

enum SystemState {
  S_NORMAL,        // air within limits
  S_PREVENT,       // threshold not crossed yet but projected inside the horizon
  S_VENTILATION,   // high CO2e WITH an occupancy signature -> exchange air
  S_INTRUSION,     // high CO2e WITHOUT one -> chemical, isolate instead
  S_FAULT          // sensing stalled; annunciate, do not pretend to control
};

struct Plan {
  SystemState state;
  uint8_t     severity;    // 0..100, drives LED intensity
  uint16_t    eta_s;       // projected seconds to threshold, 0xFFFF = none
  float       ppm, temp, hum, rs, slope;
  uint32_t    stamp_us;    // when the sample that produced this was taken
  uint32_t    seq;
};

// One entry of a buzzer rhythm: hold the output in state `on` for `ms`.
//
// This has to be declared HERE, above the first function in the file, rather
// than next to the pattern tables that use it. The Arduino IDE generates a
// prototype for every top-level function and injects the whole block
// immediately before the first function definition. A prototype that names a
// type declared later in the file therefore sees an undeclared type, and
// patternFor() fails with "'Step' does not name a type". Plain g++ does not
// reproduce this, because it does not synthesise prototypes -- so the host-side
// harness now lints for it explicitly.
struct Step { uint16_t ms; bool on; };

static SemaphoreHandle_t semPlan;   // binary: taskSense / fatigue ISR -> taskActuate
static SemaphoreHandle_t semNet;    // binary: taskSense / fatigue ISR -> taskUiNet
static SemaphoreHandle_t muxPlan;   // mutex guarding g_plan

static Plan  g_plan;
static float mq135_r0   = MQ135_R0;
static float g_temp     = 25.0f;
static float g_hum      = 50.0f;
static bool  hazard_latched = false;
static float temp_base  = NAN;
static float hum_base   = NAN;
static bool  relay_state = false;
static bool  firebase_started = false;
static uint32_t last_wifi_try = 0;

static volatile bool     g_fatigue     = false;   // ISR -> actuator
static volatile bool     g_fatigue_ui  = false;   // actuator -> UI
static volatile uint32_t g_last_isr_us = 0;
static volatile uint32_t g_latency_us  = 0;       // sample -> relay assertion
static volatile float    g_act_duty    = 0.0f;    // control-core busy fraction
static volatile float    g_rs_now      = 0.0f;    // latest sensor resistance, for status

// Bench override, driven from the serial console. It exists so the actuator
// path can be proven without provoking a real gas event, and it announces
// itself on every trace line so a forced state can never be mistaken for a
// measurement.
// Raw pin test in progress: taskActuate must not reclaim the relay pin.
static volatile bool        g_pin_test    = false;
// Runtime copy of RELAY_ACTIVE_LOW so the polarity can be flipped from the
// console during bring-up instead of by recompiling.
static volatile bool        g_relay_active_low = RELAY_ACTIVE_LOW;

static volatile bool        g_force_on    = false;
static volatile SystemState g_force_state = S_NORMAL;
static volatile uint32_t    g_force_until = 0;

static float    trend_ppm[TREND_N];
static uint32_t trend_ms[TREND_N];
static uint8_t  trend_count = 0, trend_head = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define LEDC_SETUP()   ledcAttach(LED_PIN, LED_LEDC_FREQ, LED_LEDC_BITS)
  #define LEDC_WRITE(d)  ledcWrite(LED_PIN, (d))
#else
  #define LEDC_SETUP()   do { ledcSetup(LED_LEDC_CH, LED_LEDC_FREQ, LED_LEDC_BITS); \
                              ledcAttachPin(LED_PIN, LED_LEDC_CH); } while (0)
  #define LEDC_WRITE(d)  ledcWrite(LED_LEDC_CH, (d))
#endif

// Interrupt context: timestamp, debounce, raise flags, release both waiters.
// No I2C, no Serial, no floating point, no mutex.
void IRAM_ATTR fatigueISR() {
  uint32_t now_us = micros();
  if (now_us - g_last_isr_us < DEBOUNCE_US) return;
  g_last_isr_us = now_us;
  g_fatigue = true;

  BaseType_t hp = pdFALSE;
  xSemaphoreGiveFromISR(semPlan, &hp);
  xSemaphoreGiveFromISR(semNet, &hp);
  if (hp == pdTRUE) portYIELD_FROM_ISR();
}

// The SAR ADC is noisy enough at a clean-air operating point that a single
// conversion moves the reported concentration by a couple of hundred ppm.
// Sixteen conversions cost about 3 ms once every two seconds.
float readRs() {
  uint32_t acc = 0;
  for (int i = 0; i < ADC_OVERSAMPLE; i++) {
    acc += analogRead(MQ135_PIN);
    delayMicroseconds(200);
  }
  float adc   = (float)acc / ADC_OVERSAMPLE;
  float v_pin = (adc / ADC_MAX) * ADC_VREF;
  float v_out = v_pin / DIVIDER_GAIN;
  if (v_out < 0.02) v_out = 0.02;
  if (v_out > MQ135_VCC - 0.02) v_out = MQ135_VCC - 0.02;
  return MQ135_RL * (MQ135_VCC - v_out) / v_out;
}

float rsToPPM(float rs) {
  if (mq135_r0 <= 0.0f) return ATMO_CO2;
  return PARA * pow(rs / mq135_r0, -PARB);
}

void trendPush(float p, uint32_t ms) {
  trend_ppm[trend_head] = p;
  trend_ms[trend_head]  = ms;
  trend_head = (trend_head + 1) % TREND_N;
  if (trend_count < TREND_N) trend_count++;
}

// Ordinary least squares of ppm on time, in ppm per minute. A regression over
// the whole window rather than a two-point difference, because the gas channel
// is noisy and a difference of two noisy samples is noisier than either.
float trendSlope() {
  if (trend_count < 5) return 0.0f;
  uint8_t first = (trend_head + TREND_N - trend_count) % TREND_N;
  uint32_t t0 = trend_ms[first];
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (uint8_t i = 0; i < trend_count; i++) {
    uint8_t k = (first + i) % TREND_N;
    double x = (double)(trend_ms[k] - t0) / 60000.0;
    double y = trend_ppm[k];
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  double n = trend_count;
  double den = n * sxx - sx * sx;
  if (fabs(den) < 1e-9) return 0.0f;
  return (float)((n * sxy - sx * sy) / den);
}

SystemState classifyCore(float p, float t, float h) {
  if (hazard_latched) {
    if (p < PPM_HAZARD_OFF) hazard_latched = false;
  } else {
    if (p > PPM_HAZARD_ON)  hazard_latched = true;
  }

  if (!hazard_latched) {
    // Baselines drift only while the air is clean, so a hazard cannot drag the
    // reference along with it and mask itself.
    if (isnan(temp_base)) { temp_base = t; hum_base = h; }
    else {
      temp_base = (1.0f - BASELINE_ALPHA) * temp_base + BASELINE_ALPHA * t;
      hum_base  = (1.0f - BASELINE_ALPHA) * hum_base  + BASELINE_ALPHA * h;
    }
    return S_NORMAL;
  }

  if (isnan(temp_base)) { temp_base = t; hum_base = h; }
  bool occupancy = (t > temp_base + TEMP_RISE_C) || (h > hum_base + HUM_RISE_PCT);
  return occupancy ? S_VENTILATION : S_INTRUSION;
}

static uint8_t sevFromPpm(float p) {
  float s = 60.0f + 40.0f * (p - PPM_HAZARD_ON) / 1000.0f;
  if (s < 60.0f) s = 60.0f;
  if (s > 100.0f) s = 100.0f;
  return (uint8_t)s;
}

void buildPlan(Plan &pl, float p, float t, float h, float rs, uint32_t stamp) {
  SystemState s = classifyCore(p, t, h);
  float slope = trendSlope();
  uint16_t eta = 0xFFFF;

  // Proactive branch. Only from a clean state: once a hazard is latched the
  // measured class governs, not a projection.
  if (s == S_NORMAL && slope >= MIN_SLOPE_PPM_MIN && p < PPM_HAZARD_ON) {
    float secs = (PPM_HAZARD_ON - p) / slope * 60.0f;
    if (secs >= 0.0f && secs <= (float)PREDICT_HORIZON_S) {
      s = S_PREVENT;
      eta = (uint16_t)secs;
    }
  }

  uint8_t sev = 0;
  switch (s) {
    case S_PREVENT:
      // 15 at the far edge of the horizon, 60 at the moment of crossing.
      sev = (uint8_t)(15.0f + 45.0f * (1.0f - (float)eta / (float)PREDICT_HORIZON_S));
      break;
    case S_VENTILATION: sev = sevFromPpm(p); break;
    case S_INTRUSION:   sev = sevFromPpm(p) > 80 ? sevFromPpm(p) : 80; break;
    case S_FAULT:       sev = 40; break;
    default:            sev = 0; break;
  }

  pl.state = s; pl.severity = sev; pl.eta_s = eta;
  pl.ppm = p; pl.temp = t; pl.hum = h; pl.rs = rs; pl.slope = slope;
  pl.stamp_us = stamp;
}

// Actuation layer.
//
// The relay is the ventilation load. S_VENTILATION and S_PREVENT close it;
// S_INTRUSION deliberately does NOT. That asymmetry is the whole point of
// classifying: exchanging air with the outside during a solvent or exhaust
// intrusion increases exposure rather than reducing it, so the correct
// response to the same gas reading is the opposite actuation depending on
// which cause produced it. A system that only thresholds cannot make that
// distinction and will do the wrong thing half the time.
bool relayFor(SystemState s) {
  return (s == S_VENTILATION || s == S_PREVENT);
}

void relayWrite(bool on) {
  relay_state = on;
  digitalWrite(RELAY_PIN, (g_relay_active_low ? !on : on) ? HIGH : LOW);
}

void buzzWrite(bool on) { digitalWrite(BUZZER_PIN, on ? HIGH : LOW); }

// Rhythm carries the hazard class. Active buzzers have a fixed internal
// oscillator, so pitch is unavailable; rhythm is also easier to recognise
// across a noisy room than a pitch difference. Step is declared with the other
// types near the top of the file, not here; see the note there.

static const Step PAT_SILENT[]  = { {1000, false} };
static const Step PAT_PREVENT[] = { {80, true}, {9920, false} };                    // one chirp / 10 s
static const Step PAT_VENT[]    = { {120, true}, {120, false},
                                    {120, true}, {4640, false} };                   // double / 5 s
static const Step PAT_INTRUDE[] = { {80, true}, {80, false}, {80, true},
                                    {80, false}, {80, true}, {1600, false} };       // triple / 2 s
static const Step PAT_FAULT[]   = { {500, true}, {7500, false} };                   // one long / 8 s

void patternFor(SystemState s, const Step **p, uint8_t *n) {
  switch (s) {
    case S_PREVENT:     *p = PAT_PREVENT; *n = 2; break;
    case S_VENTILATION: *p = PAT_VENT;    *n = 4; break;
    case S_INTRUSION:   *p = PAT_INTRUDE; *n = 6; break;
    case S_FAULT:       *p = PAT_FAULT;   *n = 2; break;
    default:            *p = PAT_SILENT;  *n = 1; break;
  }
}

// Perceived brightness goes roughly as duty^(1/2.2), so to make severity read
// linearly to the eye the duty has to be raised to that power. Without this a
// severity of 50 looks like about 73% brightness and the gradation is lost at
// the top of the scale, which is exactly where it has to be readable.
uint8_t gammaDuty(uint8_t sev) {
  if (sev == 0) return 0;
  float d = 255.0f * powf((float)sev / 100.0f, 2.0f);
  if (d < 8.0f) d = 8.0f;
  return (uint8_t)(d + 0.5f);
}

static uint8_t breathe(uint32_t t_ms, uint16_t period_ms, uint8_t peak) {
  float phase = (float)(t_ms % period_ms) / (float)period_ms;
  float env = 0.5f * (1.0f - cosf(2.0f * PI * phase));
  return (uint8_t)(peak * env);
}

// Only these states animate. In S_NORMAL the LED sits at a fixed idle duty and
// the buzzer is silent, so the actuation task has nothing to do between sense
// cycles and blocks indefinitely instead of ticking - which is where the power
// saving actually comes from.
bool animated(SystemState s) {
  return (s == S_PREVENT || s == S_VENTILATION || s == S_INTRUSION || s == S_FAULT);
}

void ledUpdate(SystemState s, uint8_t sev, uint32_t t_ms) {
  uint8_t peak = gammaDuty(sev);
  uint8_t duty;
  switch (s) {
    case S_PREVENT:     duty = breathe(t_ms, 4000, peak); break;
    case S_VENTILATION: duty = breathe(t_ms, 2000, peak); break;
    case S_INTRUSION:   duty = ((t_ms % 200) < 100) ? peak : 0; break;
    case S_FAULT:       duty = ((t_ms % 2000) < 1000) ? peak : 0; break;
    default:            duty = LED_IDLE_DUTY; break;
  }
  LEDC_WRITE(duty);
}

const char* stateLabel(SystemState s) {
  switch (s) {
    case S_PREVENT:     return "PRE-VENT";
    case S_VENTILATION: return "VENTILATE";
    case S_INTRUSION:   return "ISOLATE";
    case S_FAULT:       return "SENSOR FAULT";
    default:            return "NORMAL";
  }
}

// Core 1, priority 4 - the highest in the system. Wakes on the semaphore when a
// new decision exists, and otherwise on a 20 ms frame tick only while something
// is animating. Everything slow - DHT, LCD, Wi-Fi, Firebase - is somewhere else,
// so the sample-to-actuation latency this task reports is the real figure.
void taskActuate(void *arg) {
  Plan pl;
  memset(&pl, 0, sizeof(pl));
  pl.state = S_NORMAL;

  uint32_t last_plan_ms = millis();
  uint32_t last_tick_ms = millis();
  uint32_t pat_elapsed  = 0;
  uint8_t  pat_idx      = 0;
  uint32_t chirp_until  = 0;
  uint32_t busy_us = 0, win_start = micros();

  relayWrite(false);
  LEDC_WRITE(LED_IDLE_DUTY);

  for (;;) {
    TickType_t wait = (animated(pl.state) || chirp_until)
                        ? pdMS_TO_TICKS(ACT_TICK_MS) : portMAX_DELAY;
    bool got = (xSemaphoreTake(semPlan, wait) == pdTRUE);

    uint32_t t0  = micros();
    uint32_t now = millis();

    if (g_fatigue) {
      g_fatigue = false;
      g_fatigue_ui = true;
      chirp_until = now + FATIGUE_CHIRP_MS;
    }

    if (got && xSemaphoreTake(muxPlan, pdMS_TO_TICKS(5)) == pdTRUE) {
      Plan np = g_plan;
      xSemaphoreGive(muxPlan);
      if (np.seq != pl.seq) {
        if (np.state != pl.state) { pat_idx = 0; pat_elapsed = 0; }
        pl = np;
        last_plan_ms = now;
        if (!g_pin_test) relayWrite(relayFor(pl.state));
        g_latency_us = micros() - pl.stamp_us;
      }
    }

    // Fail-safe. If sensing stops, do not keep annunciating a stale decision as
    // though it were current. The relay HOLDS its last commanded position -
    // dropping a running exhaust fan on a sensor fault would be the more
    // dangerous of the two failures - and the fault pattern makes the loss of
    // sensing audible rather than silent.
    if (pl.state != S_FAULT && (now - last_plan_ms) > PLAN_STALE_MS) {
      pl.state = S_FAULT; pl.severity = 40;
      pat_idx = 0; pat_elapsed = 0;
    }

    uint32_t dt = now - last_tick_ms;
    last_tick_ms = now;

    if (chirp_until) {
      buzzWrite(true);
      if ((int32_t)(now - chirp_until) >= 0) { chirp_until = 0; buzzWrite(false); }
    } else {
      const Step *pat; uint8_t patn;
      patternFor(pl.state, &pat, &patn);
      pat_elapsed += dt;
      uint8_t guard = 0;
      while (pat_elapsed >= pat[pat_idx].ms && guard++ < 32) {
        pat_elapsed -= pat[pat_idx].ms;
        pat_idx = (pat_idx + 1) % patn;
      }
      buzzWrite(pat[pat_idx].on);
    }

    ledUpdate(pl.state, pl.severity, now);

    busy_us += micros() - t0;
    uint32_t win = micros() - win_start;
    if (win >= 5000000UL) {
      g_act_duty = 100.0f * (float)busy_us / (float)win;
      busy_us = 0; win_start = micros();
    }
  }
}

// Core 1, priority 3. Periodic, preempted by taskActuate whenever a decision or
// a frame tick is due.
void taskSense(void *arg) {
  TickType_t last = xTaskGetTickCount();
  uint32_t seq = 0;

  for (;;) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) g_temp = t;
    if (!isnan(h)) g_hum  = h;

    float rs = readRs();
    uint32_t stamp = micros();
    float p = rsToPPM(rs);
    trendPush(p, millis());

    Plan pl;
    buildPlan(pl, p, g_temp, g_hum, rs, stamp);
    g_rs_now = rs;

    if (g_force_on) {
      if ((int32_t)(millis() - g_force_until) >= 0) {
        g_force_on = false;
        Serial.println(F("[test] force expired, back under sensor control"));
      } else {
        pl.state = g_force_state;
        pl.severity = (g_force_state == S_INTRUSION)   ? 90 :
                      (g_force_state == S_VENTILATION) ? 75 :
                      (g_force_state == S_PREVENT)     ? 40 : 0;
        pl.eta_s = (g_force_state == S_PREVENT) ? 45 : 0xFFFF;
      }
    }

    pl.seq = ++seq;

    xSemaphoreTake(muxPlan, portMAX_DELAY);
    g_plan = pl;
    xSemaphoreGive(muxPlan);

    xSemaphoreGive(semPlan);
    xSemaphoreGive(semNet);

    vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSE_PERIOD_MS));
  }
}

void render(const Plan &pl, bool fatigue_banner) {
  lcd.clear();

  if (fatigue_banner) {
    lcd.setCursor(0, 0); lcd.print("FATIGUE LOGGED!");
    lcd.setCursor(0, 1); lcd.print("Timestamped.");
    return;
  }

  lcd.setCursor(0, 0);
  switch (pl.state) {
    case S_NORMAL:
      lcd.print("AQI OK: "); lcd.print((int)pl.ppm);
      break;
    case S_PREVENT:
      lcd.print("RISE "); lcd.print((int)pl.ppm);
      lcd.print(" T-"); lcd.print((int)pl.eta_s);
      break;
    case S_FAULT:
      lcd.print("SENSOR FAULT");
      break;
    default:
      lcd.print("CO2e HI: "); lcd.print((int)pl.ppm);
      break;
  }

  lcd.setCursor(0, 1);
  switch (pl.state) {
    case S_NORMAL:
      lcd.print("T:");   lcd.print(pl.temp, 1);
      lcd.print("C H:"); lcd.print((int)pl.hum); lcd.print("%");
      if (WiFi.status() != WL_CONNECTED) lcd.print(" *");
      break;
    case S_PREVENT:     lcd.print("PRE-VENT  S:"); lcd.print(pl.severity); break;
    case S_VENTILATION: lcd.print("PURIFIER  S:"); lcd.print(pl.severity); break;
    case S_INTRUSION:   lcd.print("ISOLATE!  S:"); lcd.print(pl.severity); break;
    default:            lcd.print("Check sensor");  break;
  }
}

void wifiKick() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - last_wifi_try < WIFI_RETRY_MS) return;
  last_wifi_try = millis();
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void firebaseKick() {
  if (WiFi.status() != WL_CONNECTED || firebase_started) return;
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  firebase_started = true;
}

void publish(const Plan &pl) {
  if (!firebase_started || !Firebase.ready()) return;
  Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/AQI",         (int)pl.ppm);
  Firebase.RTDB.setFloat(&fbdo,  "NeuroBadge/Temperature", pl.temp);
  Firebase.RTDB.setFloat(&fbdo,  "NeuroBadge/Humidity",    pl.hum);
  Firebase.RTDB.setString(&fbdo, "NeuroBadge/Status",      stateLabel(pl.state));
  Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/Severity",    pl.severity);
  Firebase.RTDB.setFloat(&fbdo,  "NeuroBadge/SlopePpmMin", pl.slope);
  Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/EtaSeconds",  pl.eta_s == 0xFFFF ? -1 : (int)pl.eta_s);
  Firebase.RTDB.setBool(&fbdo,   "NeuroBadge/Relay",       relay_state);
  Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/LatencyUs",   (int)g_latency_us);
}

void trace(const Plan &pl) {
  Serial.print(F("Rs="));   Serial.print(pl.rs, 2);
  Serial.print(F("k ppm=")); Serial.print(pl.ppm, 0);
  Serial.print(F(" T="));   Serial.print(pl.temp, 1);
  Serial.print(F(" H="));   Serial.print(pl.hum, 0);
  Serial.print(F(" dT="));  Serial.print(pl.temp - temp_base, 1);
  Serial.print(F(" dH="));  Serial.print(pl.hum - hum_base, 0);
  Serial.print(F(" slope=")); Serial.print(pl.slope, 0);
  Serial.print(F("ppm/min eta="));
  if (pl.eta_s == 0xFFFF) Serial.print(F("-")); else Serial.print(pl.eta_s);
  Serial.print(F("s state=")); Serial.print(stateLabel(pl.state));
  Serial.print(F(" sev="));    Serial.print(pl.severity);
  Serial.print(F(" relay="));  Serial.print(relay_state ? 1 : 0);
  Serial.print(F(" lat="));    Serial.print(g_latency_us);
  Serial.print(F("us actduty=")); Serial.print(g_act_duty, 2);
  Serial.print(F("% wifi="));  Serial.print(WiFi.status() == WL_CONNECTED ? "up" : "down");
  Serial.println(g_force_on ? "  [FORCED - bench override, not a measurement]" : "");
}

void printHelp() {
  Serial.println(F("[test] r = pulse relay pin 1.5 s   b = buzzer   l = LED sweep"));
  Serial.println(F("[test] f = inject a fatigue event"));
  Serial.println(F("[test] v = force VENTILATE   i = force ISOLATE   p = force PRE-VENT"));
  Serial.println(F("[test] n = clear force   o = flip relay polarity   t = status   ? = help"));
}

void printStatus() {
  float trip = mq135_r0 * pow(PPM_HAZARD_ON  / PARA, -1.0 / PARB);
  float clr  = mq135_r0 * pow(PPM_HAZARD_OFF / PARA, -1.0 / PARB);
  Serial.println(F("---------------- status ----------------"));
  Serial.print(F("R0, clean-air reference : ")); Serial.print(mq135_r0, 1); Serial.println(F(" kOhm"));
  Serial.print(F("Rs now                  : ")); Serial.print(g_rs_now, 1);  Serial.println(F(" kOhm"));
  Serial.print(F("Rs must fall BELOW      : ")); Serial.print(trip, 1);      Serial.println(F(" kOhm to trip a hazard"));
  Serial.print(F("Rs must rise ABOVE      : ")); Serial.print(clr, 1);       Serial.println(F(" kOhm to clear it"));
  Serial.print(F("still to fall           : ")); Serial.print(g_rs_now - trip, 1); Serial.println(F(" kOhm"));
  Serial.print(F("relay pin logic         : active ")); Serial.println(g_relay_active_low ? "LOW" : "HIGH");
  Serial.print(F("relay commanded         : ")); Serial.println(relay_state ? "CLOSED" : "OPEN");
  Serial.println(F("----------------------------------------"));
}

void forceState(SystemState s) {
  g_force_state = s;
  g_force_until = millis() + 20000;
  g_force_on = true;
  Serial.print(F("[test] FORCING ")); Serial.print(stateLabel(s));
  Serial.println(F(" for 20 s - bench override, not a measurement"));
}

// Typed commands from the serial console. Read on the UI task, so nothing here
// can delay the control path.
void pollSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 'r':
        // The control task is locked out for the duration, so this is three
        // clean cycles rather than a pulse that may be reclaimed mid-way.
        g_pin_test = true;
        Serial.print(F("[test] three relay cycles, GPIO")); Serial.print(RELAY_PIN);
        Serial.print(F(", active ")); Serial.println(g_relay_active_low ? "LOW" : "HIGH");
        for (int i = 1; i <= 3; i++) {
          Serial.print(F("[test]   cycle ")); Serial.print(i);
          Serial.print(F(" - CLOSED, pin drives "));
          Serial.println(g_relay_active_low ? "LOW" : "HIGH");
          relayWrite(true);  delay(1200);
          Serial.println(F("[test]   OPEN"));
          relayWrite(false); delay(800);
        }
        g_pin_test = false;
        Serial.println(F("[test] done. No click and no status-LED change means the"));
        Serial.println(F("[test] module never saw the signal - check the IN wire, then"));
        Serial.println(F("[test] send 'o' to try the opposite polarity."));
        break;
      case 'o':
        g_relay_active_low = !g_relay_active_low;
        Serial.print(F("[test] relay polarity now active "));
        Serial.println(g_relay_active_low ? "LOW" : "HIGH");
        relayWrite(false);
        Serial.println(F("[test] re-send 'r' to retest. If this one works, set"));
        Serial.println(F("[test] RELAY_ACTIVE_LOW in the source to match and reflash."));
        break;
      case 'b':
        Serial.println(F("[test] buzzer 300 ms"));
        buzzWrite(true); delay(300); buzzWrite(false);
        break;
      case 'l':
        Serial.println(F("[test] LED sweep"));
        for (int duty = 0;   duty <= 255; duty += 5) { LEDC_WRITE(duty); delay(4); }
        for (int duty = 255; duty >= 0;   duty -= 5) { LEDC_WRITE(duty); delay(4); }
        LEDC_WRITE(LED_IDLE_DUTY);
        break;
      case 'f':
        g_fatigue = true;
        xSemaphoreGive(semPlan);
        Serial.println(F("[test] fatigue event injected"));
        break;
      case 'v': forceState(S_VENTILATION); break;
      case 'i': forceState(S_INTRUSION);   break;
      case 'p': forceState(S_PREVENT);     break;
      case 'n':
        g_force_on = false;
        Serial.println(F("[test] force cleared"));
        break;
      case 't': printStatus(); break;
      case '?': printHelp();   break;
      default: break;
    }
  }
}

// Core 0, priority 1, alongside the Wi-Fi and TCP stack. Everything here is
// allowed to be slow. Blocks on semNet; it never polls and never spins.
void taskUiNet(void *arg) {
  uint32_t fatigue_until = 0;

  for (;;) {
    xSemaphoreTake(semNet, portMAX_DELAY);
    pollSerial();

    Plan pl;
    xSemaphoreTake(muxPlan, portMAX_DELAY);
    pl = g_plan;
    xSemaphoreGive(muxPlan);

    uint32_t now = millis();
    if (g_fatigue_ui) {
      g_fatigue_ui = false;
      fatigue_until = now + FATIGUE_HOLD_MS;
      render(pl, true);
      Serial.println(F("[event] fatigue logged"));
      if (firebase_started && Firebase.ready()) {
        Firebase.RTDB.setString(&fbdo, "NeuroBadge/Status", "FATIGUE LOGGED");
        Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/FatigueAtPPM", (int)pl.ppm);
      }
      continue;
    }

    wifiKick();
    firebaseKick();
    if (now >= fatigue_until) render(pl, false);
    publish(pl);
    trace(pl);
  }
}

void autoCalibrate() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Calibrating R0");
  lcd.setCursor(0, 1);
  lcd.print("Clean air "); lcd.print(CAL_SECONDS); lcd.print("s");

  Serial.print(F("Establishing clean-air reference over "));
  Serial.print(CAL_SECONDS); Serial.println(F(" s."));
  Serial.println(F("Keep the sensor in clean air and do not breathe on it."));
  Serial.println(F("The fatigue button does nothing until this finishes."));

  double sum = 0.0;
  const int n = CAL_SECONDS * 10;
  for (int i = 0; i < n; i++) { sum += readRs(); delay(100); }
  float rs_clean = (float)(sum / n);
  mq135_r0 = rs_clean * pow((ATMO_CO2 / PARA), (1.0 / PARB));

  Serial.print(F("Reference established: Rs_clean = ")); Serial.print(rs_clean, 2);
  Serial.print(F(" kOhm, R0 = ")); Serial.print(mq135_r0, 2); Serial.println(F(" kOhm"));
  Serial.print(F("Hazard trips when Rs falls below "));
  Serial.print(mq135_r0 * pow(PPM_HAZARD_ON / PARA, -1.0 / PARB), 1);
  Serial.println(F(" kOhm."));
}

// Boot-time actuator check. Runs before calibration so a wiring fault is found
// in seconds rather than by trying to provoke a hazard. Each step announces
// itself on serial, so a step that prints but does nothing audible or visible
// isolates the fault to that channel's wiring.
void selfTest() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Actuator test");

  lcd.setCursor(0, 1); lcd.print("relay...");
  Serial.println(F("[selftest] relay CLOSED - expect a click and the module LED"));
  relayWrite(true);
  delay(700);
  Serial.println(F("[selftest] relay OPEN  - expect a second click"));
  relayWrite(false);
  delay(400);

  lcd.setCursor(0, 1); lcd.print("buzzer...       ");
  Serial.println(F("[selftest] buzzer - expect two short chirps"));
  for (int i = 0; i < 2; i++) {
    buzzWrite(true);  delay(120);
    buzzWrite(false); delay(200);
  }

  lcd.setCursor(0, 1); lcd.print("LED...          ");
  Serial.println(F("[selftest] LED - expect a fade up and down on GPIO2"));
  for (int duty = 0;   duty <= 255; duty += 5) { LEDC_WRITE(duty); delay(5); }
  for (int duty = 255; duty >= 0;   duty -= 5) { LEDC_WRITE(duty); delay(5); }
  LEDC_WRITE(LED_IDLE_DUTY);

  Serial.println(F("[selftest] done"));
  delay(300);
}

void setup() {
#if LOW_POWER_CPU
  // Before Serial.begin, because changing the CPU clock moves the APB clock the
  // UART divisor is derived from.
  setCpuFrequencyMhz(80);
#endif
  Serial.begin(115200);
  delay(300);

  // Drive the pin to the inactive level BEFORE it becomes an output, otherwise
  // the relay clicks closed for the few microseconds between pinMode and the
  // first digitalWrite.
  digitalWrite(RELAY_PIN, g_relay_active_low ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);

  pinMode(BUZZER_PIN, OUTPUT);
  buzzWrite(false);

  LEDC_SETUP();
  LEDC_WRITE(0);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  dht.begin();

  // Uploading firmware resets the ESP32 but does not power-cycle the LCD, so the
  // HD44780 keeps whatever state the previous sketch left it in. If that sketch
  // was interrupted mid nibble-pair, controller and library then disagree about
  // nibble phase and every character is assembled from misaligned bits, which
  // renders as stable non-ASCII garbage rather than a blank screen. Issuing the
  // init sequence twice with a settle gap resynchronises it. 100 kHz is the
  // conservative bus speed for breadboard wiring on weak internal pull-ups.
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(50);
  lcd.init();
  delay(50);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Neuro-Badge v3");
  lcd.setCursor(0, 1); lcd.print("Warming up...");
  delay(1500);

#if SELFTEST_ON_BOOT
  selfTest();
#endif

  if (mq135_r0 <= 0.0f) autoCalibrate();
  Serial.print(F("[cal] R0 = ")); Serial.print(mq135_r0, 3); Serial.println(F(" kOhm"));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  last_wifi_try = millis();

  semPlan = xSemaphoreCreateBinary();
  semNet  = xSemaphoreCreateBinary();
  muxPlan = xSemaphoreCreateMutex();

  memset(&g_plan, 0, sizeof(g_plan));
  g_plan.state = S_NORMAL;
  g_plan.eta_s = 0xFFFF;

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), fatigueISR, FALLING);

  // Control path on APP_CPU, isolated from the Wi-Fi and TCP stack that lives on
  // PRO_CPU. A network stall therefore cannot extend actuation latency.
  xTaskCreatePinnedToCore(taskActuate, "actuate", 4096, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(taskSense,   "sense",   4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskUiNet,   "uinet",   8192, NULL, 1, NULL, 0);

  lcd.clear();
  Serial.println(F("[rtos] actuate(c1,p4) sense(c1,p3) uinet(c0,p1) started"));
  printHelp();
}

// The Arduino loop task is a polling task with nothing left to poll. Deleting it
// returns its stack and removes it from the ready list, so the scheduler on core
// 1 has only taskSense and taskActuate to consider and can reach the idle task
// between events.
void loop() {
  vTaskDelete(NULL);
}
