/*
 * ============================================================================
 * Neuro-Badge v2 :: Edge Root-Cause Analysis for Cognitive Health Protection
 * ============================================================================
 * BCSE497J Project-I | Rishab Rajesh Nagwani (23BAI0085) | VIT Vellore
 * Guide: Dr. Yokesh Babu S
 *
 * Hardware actually present on the prototype:
 *   ESP32 (CP2102)  - dual-core Xtensa LX6 ASIP
 *   MQ135           - SnO2 gas sensor, analog, GPIO34 (ADC1_CH6)
 *   DHT11           - 1-Wire temperature/humidity, GPIO4
 *   LCD 16x2 I2C    - local UI, SDA=GPIO5 SCL=GPIO18, addr 0x27
 *   Push button     - fatigue logger, GPIO15, falling-edge interrupt
 *   Onboard LED     - GPIO2, standing in for the mitigation actuator
 *
 * WHAT CHANGED FROM v1, AND WHY
 *
 * 1. The PPM figure is now a measurement rather than a rescale.
 *    v1 did  map(raw_adc, 0, 4095, 400, 2000)  and called the result
 *    "simulated_ppm". That is a linear stretch of an ADC count with no
 *    physical meaning. v2 computes sensor resistance Rs from the divider,
 *    then applies the MQ135 datasheet log-log curve against a measured R0:
 *        ppm = PARA * (Rs/R0)^(-PARB)
 *    R0 comes from calibrate_r0.ino, run in clean air after burn-in.
 *
 * 2. Wi-Fi no longer blocks.
 *    v1 sat in  while (WiFi.status() != WL_CONNECTED)  forever. One failed
 *    hotspot and the device is a brick showing "Connecting WiFi". v2 connects
 *    asynchronously and retries on a timer. Sensing, root-cause analysis, the
 *    LCD and the actuator all work with no network at all. Cloud telemetry is
 *    an enhancement to the edge system, not a dependency of it.
 *
 * 3. Explicit finite state machine.
 *    v1's states were implicit in nested ifs. v2 names them, which is what the
 *    FSM diagram in the report actually describes.
 *
 * 4. Hysteresis on the hazard threshold.
 *    A bare  ppm > 1000  test chatters the actuator when readings sit near the
 *    line. v2 enters hazard at 1000 ppm and leaves it at 850 ppm.
 *
 * 5. Actuation channel is real and observable.
 *    The relay and haptic motor are not yet on the board. The control path is
 *    implemented and asserted on GPIO2, where the onboard LED makes it visible.
 *    Dropping in the relay module is a wiring change, not a software change.
 *
 * 6. Non-blocking main loop.
 *    v1's delay(2000) stalled everything including the LCD response to the
 *    fatigue button. v2 schedules on millis().
 * ============================================================================
 */

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ---------------------------------------------------------------- credentials
// Change these two lines at the venue. Phone hotspot must be on 2.4 GHz:
// the ESP32's 802.11 b/g/n radio cannot see a 5 GHz network at all.
#define WIFI_SSID       "Rishab's OnePlus Nord"
#define WIFI_PASSWORD   "gitsomemf"

#define API_KEY         "AIzaSyCWoqmmU3XD9cih0_SqemQ3GLk361g4d-M"
#define DATABASE_URL    "neuro-badge-default-rtdb.asia-southeast1.firebasedatabase.app"

// ------------------------------------------------------------------ hardware
#define DHTPIN          4
#define DHTTYPE         DHT11
#define MQ135_PIN       34      // ADC1_CH6
#define BUTTON_PIN      15      // fatigue logger, internal pull-up
#define ACTUATOR_PIN    2       // onboard LED == mitigation actuator channel
                                // Move to GPIO5 when the relay module arrives.

#define I2C_SDA         5
#define I2C_SCL         18

// ------------------------------------------------------- MQ135 sensor model
// Verify against your module before trusting the ppm figure.
#define MQ135_VCC       5.0     // breakout supply rail
#define MQ135_RL        10.0    // load resistor on the breakout, kOhm
#define DIVIDER_GAIN    1.0     // 1.0 = AOUT straight to GPIO34
#define ADC_MAX         4095.0
#define ADC_VREF        3.3
#define ADC_OVERSAMPLE  16      // see readRs()

// Datasheet log-log fit, CO2 branch
#define PARA            116.6020682
#define PARB            2.769034857
#define ATMO_CO2        415.0

// R0 is established at power-on rather than hard-coded, and that is a
// deliberate decision backed by measurement.
//
// Three clean-air calibrations across one session on 28-08-2026:
//     first run          Rs 131.5 kOhm   ->  R0 208.0
//     ~1 hour later      Rs 156.5 kOhm   ->  R0 247.6
//     ~90 minutes        Rs 219.0 kOhm   ->  R0 346.2
//
// Rs rose 66% while the air was unchanged. The SnO2 heater is still baking off
// adsorbed contaminant; the datasheet asks for 24 hours of preheat for stable
// operation and we are giving it tens of minutes. Freezing any one of those
// numbers into the firmware means the reported concentration is wrong by
// whatever the next session's warm-up differs by — hard-coding the 90-minute
// figure and then powering up 20 minutes before a demonstration would report
// 1700 ppm in clean air and latch into a permanent false hazard.
//
// Self-calibrating instead makes the reference, and therefore the threshold,
// scale with the baseline the sensor actually has on the day. The hazard trips
// when Rs falls to 73% of the clean-air baseline, which is a ratio and holds at
// any warm-up state.
//
// Set this to a positive number only if the sensor has had a genuine 24-hour
// preheat and you have a reason to pin the reference.
#define MQ135_R0        0.0

// -------------------------------------------------------------- thresholds
#define PPM_HAZARD_ON   1000.0  // enter hazard
#define PPM_HAZARD_OFF  850.0   // leave hazard (hysteresis band)

// Occupancy signature, measured RELATIVE to the room's own baseline rather than
// against an absolute number. An absolute "warmer than 30 C" test is useless in
// a climate whose ambient already sits near 30 C: every hazard would classify as
// poor ventilation and the intrusion branch could never fire. What actually
// distinguishes the two cases is that occupants are simultaneously a CO2 source,
// a heat source and a moisture source, so a crowding event pushes temperature
// AND humidity above whatever that room was sitting at. Solvent vapour moves
// neither.
#define TEMP_RISE_C     1.5     // degrees above baseline
#define HUM_RISE_PCT    8.0     // percentage points above baseline
#define BASELINE_ALPHA  0.02    // EMA weight; baseline tracks only while NORMAL

// ------------------------------------------------------------------ timing
#define SENSE_PERIOD_MS    2000
#define DHT_PERIOD_MS      1000
#define WIFI_RETRY_MS      15000
#define FATIGUE_HOLD_MS    3000
#define DEBOUNCE_US        250000UL

// ------------------------------------------------------------------ globals
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

enum SystemState {
  S_NORMAL,             // air within limits
  S_VENTILATION,        // high CO2e + warm -> occupancy load, actuate exhaust
  S_INTRUSION,          // high CO2e + normal temp -> chemical, isolate instead
  S_FATIGUE             // user-asserted subjective event, transient
};

SystemState state = S_NORMAL;
bool hazard_latched = false;      // hysteresis latch

volatile bool fatigue_flag = false;
volatile unsigned long last_isr_us = 0;

float mq135_r0     = MQ135_R0;
float ppm          = ATMO_CO2;
float temperature  = 25.0;
float humidity     = 50.0;

float temp_base    = NAN;   // slow room baselines, seeded on first good reading
float hum_base     = NAN;

unsigned long next_sense     = 0;
unsigned long last_dht_ms    = 0;
unsigned long last_wifi_try  = 0;
unsigned long fatigue_until  = 0;

bool firebase_started = false;

// ------------------------------------------------------------------- ISR
// Kept minimal: timestamp, debounce, set a flag. No I2C, no Serial, no
// floating point in interrupt context.
void IRAM_ATTR fatigueISR() {
  unsigned long now_us = micros();
  if (now_us - last_isr_us > DEBOUNCE_US) {
    fatigue_flag = true;
    last_isr_us = now_us;
  }
}

// -------------------------------------------------------------- sensor math
// The ESP32 SAR ADC is noisy, and at a clean-air operating point near 440 counts
// a single conversion swings enough to move the reported concentration by a
// couple of hundred ppm. Averaging 16 conversions costs about 3 ms once every
// two seconds and cuts that noise by roughly a factor of four.
float readRs() {
  uint32_t acc = 0;
  for (int i = 0; i < ADC_OVERSAMPLE; i++) {
    acc += analogRead(MQ135_PIN);
    delayMicroseconds(200);
  }
  float adc = (float)acc / ADC_OVERSAMPLE;

  float v_pin = (adc / ADC_MAX) * ADC_VREF;
  float v_out = v_pin / DIVIDER_GAIN;
  if (v_out < 0.02) v_out = 0.02;
  if (v_out > MQ135_VCC - 0.02) v_out = MQ135_VCC - 0.02;
  return MQ135_RL * (MQ135_VCC - v_out) / v_out;   // kOhm
}

float rsToPPM(float rs) {
  if (mq135_r0 <= 0.0) return ATMO_CO2;
  return PARA * pow(rs / mq135_r0, -PARB);
}

// Power-on reference measurement. See the note on MQ135_R0 above for why this
// is the normal path rather than a fallback.
void autoCalibrate() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Calibrating R0");
  lcd.setCursor(0, 1); lcd.print("Clean air 30s");

  Serial.println(F("Establishing clean-air reference over 30 s."));
  Serial.println(F("Keep the sensor in clean air and do not breathe on it."));

  double sum = 0.0;
  const int n = 300;
  for (int i = 0; i < n; i++) {
    sum += readRs();
    delay(100);
  }
  float rs_clean = sum / n;
  mq135_r0 = rs_clean * pow((ATMO_CO2 / PARA), (1.0 / PARB));

  Serial.print(F("Reference established: Rs_clean = ")); Serial.print(rs_clean, 2);
  Serial.print(F(" kOhm, R0 = ")); Serial.print(mq135_r0, 2); Serial.println(F(" kOhm"));
  Serial.print(F("Hazard will trip when Rs falls below "));
  Serial.print(mq135_r0 * pow(PPM_HAZARD_ON / PARA, -1.0 / PARB), 1);
  Serial.println(F(" kOhm."));
}

// ------------------------------------------------------- root-cause analysis
// Two hazards, opposite countermeasures. The occupancy signature separates them:
// people are simultaneously a CO2 source, a heat source and a moisture source, so
// a crowding event lifts temperature and humidity above the room's own baseline.
// Solvent vapour lifts the gas channel and leaves both of those flat.
SystemState classify(float p, float t, float h) {
  if (hazard_latched) {
    if (p < PPM_HAZARD_OFF) hazard_latched = false;
  } else {
    if (p > PPM_HAZARD_ON)  hazard_latched = true;
  }

  if (!hazard_latched) {
    // Baselines drift only while the air is clean, so a hazard cannot pull the
    // reference along with it and mask itself.
    if (isnan(temp_base)) { temp_base = t; hum_base = h; }
    else {
      temp_base = (1.0 - BASELINE_ALPHA) * temp_base + BASELINE_ALPHA * t;
      hum_base  = (1.0 - BASELINE_ALPHA) * hum_base  + BASELINE_ALPHA * h;
    }
    return S_NORMAL;
  }

  if (isnan(temp_base)) { temp_base = t; hum_base = h; }

  bool occupancy_signature = (t > temp_base + TEMP_RISE_C) ||
                             (h > hum_base  + HUM_RISE_PCT);
  return occupancy_signature ? S_VENTILATION : S_INTRUSION;
}

// Ventilation hazard drives the exhaust/purifier. A chemical intrusion must
// NOT drive it: pulling more outside air in during a solvent event makes the
// exposure worse. That asymmetry is the point of classifying at all.
void driveActuator(SystemState s) {
  digitalWrite(ACTUATOR_PIN, (s == S_VENTILATION) ? HIGH : LOW);
}

const char* stateLabel(SystemState s) {
  switch (s) {
    case S_VENTILATION: return "ACT: Purifier ON";
    case S_INTRUSION:   return "ACT: Close Wndws";
    case S_FATIGUE:     return "FATIGUE LOGGED!";
    default:            return "NORMAL";
  }
}

// ------------------------------------------------------------------- display
void render(SystemState s) {
  lcd.clear();

  if (s == S_FATIGUE) {
    lcd.setCursor(0, 0); lcd.print("FATIGUE LOGGED!");
    lcd.setCursor(0, 1); lcd.print("Timestamped.");
    return;
  }

  lcd.setCursor(0, 0);
  if (s == S_NORMAL) {
    lcd.print("AQI OK: ");
    lcd.print((int)ppm);
  } else {
    lcd.print("CO2e HI: ");
    lcd.print((int)ppm);
  }

  lcd.setCursor(0, 1);
  if (s == S_NORMAL) {
    lcd.print("T:");   lcd.print(temperature, 1);
    lcd.print("C H:"); lcd.print((int)humidity);
    lcd.print("%");
    if (WiFi.status() != WL_CONNECTED) lcd.print(" *");   // * = offline
  } else {
    lcd.print(stateLabel(s));
  }
}

// ------------------------------------------------------------------ network
void wifiKick() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - last_wifi_try < WIFI_RETRY_MS) return;

  last_wifi_try = millis();
  Serial.println(F("[wifi] attempting connection"));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void firebaseKick() {
  if (WiFi.status() != WL_CONNECTED || firebase_started) return;

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println(F("[firebase] anonymous session established"));
  }
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  firebase_started = true;
}

void publish(SystemState s) {
  if (!firebase_started || !Firebase.ready()) return;

  Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/AQI",         (int)ppm);
  Firebase.RTDB.setFloat(&fbdo,  "NeuroBadge/Temperature", temperature);
  Firebase.RTDB.setFloat(&fbdo,  "NeuroBadge/Humidity",    humidity);
  Firebase.RTDB.setString(&fbdo, "NeuroBadge/Status",      stateLabel(s));
}

// --------------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(ACTUATOR_PIN, OUTPUT);
  digitalWrite(ACTUATOR_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), fatigueISR, FALLING);

  dht.begin();

  // I2C and display bring-up, deliberately defensive.
  //
  // Uploading new firmware resets the ESP32 but does NOT power-cycle the LCD, so
  // the HD44780 controller keeps whatever state the previous sketch left it in.
  // If that sketch was interrupted part-way through a 4-bit nibble pair, the
  // controller and the library then disagree about nibble phase, and every
  // subsequent character is assembled from misaligned bits — which renders as
  // stable, consistent, non-ASCII garbage rather than as a blank screen.
  //
  // Issuing the initialisation sequence twice with a settle gap re-synchronises a
  // controller left in that state. 100 kHz is the conservative bus speed for
  // breadboard-length wiring, where trace capacitance and the ESP32's weak
  // internal pull-ups leave little margin.
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(50);

  lcd.init();
  delay(50);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcd.setCursor(0, 0); lcd.print("Neuro-Badge v2");
  lcd.setCursor(0, 1); lcd.print("Warming up...");
  delay(1500);

  if (mq135_r0 <= 0.0) autoCalibrate();

  Serial.print(F("[cal] R0 = ")); Serial.print(mq135_r0, 3); Serial.println(F(" kOhm"));

  // Non-blocking from here on. If the hotspot never appears the device still
  // senses, classifies, actuates and displays.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  last_wifi_try = millis();

  next_sense = millis();
  lcd.clear();
}

// ---------------------------------------------------------------------- loop
void loop() {
  unsigned long now = millis();

  wifiKick();
  firebaseKick();

  // Subjective fatigue event. Handled ahead of the periodic sense cycle so the
  // response is bounded by loop latency rather than by the 2 s sense period.
  if (fatigue_flag) {
    fatigue_flag = false;
    state = S_FATIGUE;
    fatigue_until = now + FATIGUE_HOLD_MS;

    render(S_FATIGUE);
    Serial.println(F("[event] fatigue logged"));

    if (firebase_started && Firebase.ready()) {
      Firebase.RTDB.setString(&fbdo, "NeuroBadge/Status", stateLabel(S_FATIGUE));
      Firebase.RTDB.setInt(&fbdo,    "NeuroBadge/FatigueAtPPM", (int)ppm);
    }
  }

  if (now - last_dht_ms >= DHT_PERIOD_MS) {
    last_dht_ms = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;    // hold last good value on a failed read
    if (!isnan(h)) humidity    = h;
  }

  if ((long)(now - next_sense) >= 0) {
    next_sense = now + SENSE_PERIOD_MS;

    float rs = readRs();
    ppm = rsToPPM(rs);

    SystemState s = classify(ppm, temperature, humidity);
    driveActuator(s);

    // Let the fatigue banner stand for its hold window before the periodic
    // display resumes.
    if (now >= fatigue_until) {
      state = s;
      render(state);
    }

    publish(s);

    Serial.print(F("Rs="));    Serial.print(rs, 2);
    Serial.print(F("k ppm=")); Serial.print(ppm, 0);
    Serial.print(F(" T="));    Serial.print(temperature, 1);
    Serial.print(F(" H="));    Serial.print(humidity, 0);
    // Deltas against the rolling baseline are what the classifier actually
    // tests, so print them directly rather than making you subtract in your
    // head. Before demonstrating the intrusion case, wait until dT and dH have
    // both collapsed back toward zero.
    Serial.print(F(" dT="));   Serial.print(temperature - temp_base, 1);
    Serial.print(F(" dH="));   Serial.print(humidity - hum_base, 0);
    Serial.print(F(" state=")); Serial.print(stateLabel(s));
    Serial.print(F(" act="));  Serial.print(digitalRead(ACTUATOR_PIN));
    Serial.print(F(" wifi=")); Serial.println(WiFi.status() == WL_CONNECTED ? "up" : "down");
  }
}
