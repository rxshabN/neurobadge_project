/*
 * Neuro-Badge :: MQ135 R0 Calibration Utility
 * -------------------------------------------
 * Run this ONCE, in clean outdoor-equivalent air, AFTER the sensor has been
 * powered continuously for at least 20 minutes (heater burn-in).
 *
 * It measures the sensor resistance Rs in clean air and derives R0, the
 * reference resistance used by the datasheet log-log curve:
 *
 *     ppm = PARA * (Rs / R0) ^ (-PARB)
 *
 * Write the printed R0 into neurobadge.ino as MQ135_R0.
 * Re-run whenever the sensor is moved to a materially different environment.
 */

#define MQ135_PIN      34      // ADC1_CH6
#define ADC_MAX        4095.0  // 12-bit
#define ADC_VREF       3.3     // ESP32 full-scale at 11 dB attenuation
#define ADC_OVERSAMPLE 16      // must match neurobadge.ino

// --- Wiring-dependent constants: verify these against YOUR module ---
#define MQ135_VCC      5.0     // Heater/divider supply. 5.0 if AOUT is fed
                               // through a divider; 3.3 if module runs off 3V3.
#define MQ135_RL       10.0    // Load resistor on the breakout, in kOhm.
                               // Most MQ135 modules use 10k. Check the board.
#define DIVIDER_GAIN   1.0     // 1.0 = AOUT wired straight to GPIO34.
                               // If you add a 2-resistor divider, set this to
                               // Vout/Vin (e.g. 0.66 for 10k/20k).

// --- Datasheet curve fit for CO2 (Rs/R0 vs ppm, log-log) ---
#define PARA           116.6020682
#define PARB           2.769034857
#define ATMO_CO2       415.0   // Global background CO2, ppm (2026)

#define SAMPLE_COUNT   600     // 60 s at 10 Hz
#define SAMPLE_DELAY   100

float readRs() {
  uint32_t acc = 0;
  for (int i = 0; i < ADC_OVERSAMPLE; i++) {
    acc += analogRead(MQ135_PIN);
    delayMicroseconds(200);
  }
  float adc = (float)acc / ADC_OVERSAMPLE;
  float v_pin = (adc / ADC_MAX) * ADC_VREF;
  float v_out = v_pin / DIVIDER_GAIN;          // undo the divider, if any
  if (v_out < 0.02) v_out = 0.02;              // guard against divide-by-zero
  if (v_out > MQ135_VCC - 0.02) v_out = MQ135_VCC - 0.02;
  return MQ135_RL * (MQ135_VCC - v_out) / v_out;   // kOhm
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("Neuro-Badge :: MQ135 R0 calibration"));
  Serial.println(F("Confirm the sensor has been powered >= 20 minutes."));
  Serial.println(F("Place it in clean, well-ventilated air and do not breathe on it."));
  Serial.println(F("Averaging for 60 seconds..."));
  Serial.println();

  double rs_sum = 0.0;
  double adc_sum = 0.0;
  float rs_min = 1e9, rs_max = -1e9;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    float rs = readRs();
    rs_sum  += rs;
    adc_sum += analogRead(MQ135_PIN);
    if (rs < rs_min) rs_min = rs;
    if (rs > rs_max) rs_max = rs;

    if (i % 100 == 0) {
      Serial.print(F("  t="));
      Serial.print(i / 10);
      Serial.print(F("s  Rs="));
      Serial.print(rs, 2);
      Serial.println(F(" kOhm"));
    }
    delay(SAMPLE_DELAY);
  }

  float rs_clean = rs_sum / SAMPLE_COUNT;
  float adc_mean = adc_sum / SAMPLE_COUNT;

  // R0 such that the curve returns ATMO_CO2 when Rs == Rs_clean
  float r0 = rs_clean * pow((ATMO_CO2 / PARA), (1.0 / PARB));

  Serial.println();
  Serial.println(F("================ RESULT ================"));
  Serial.print(F("Mean raw ADC        : ")); Serial.println(adc_mean, 1);
  Serial.print(F("Mean Rs (clean air) : ")); Serial.print(rs_clean, 3); Serial.println(F(" kOhm"));
  Serial.print(F("Rs spread min/max   : ")); Serial.print(rs_min, 2);
  Serial.print(F(" / ")); Serial.print(rs_max, 2); Serial.println(F(" kOhm"));
  Serial.print(F("Assumed baseline CO2: ")); Serial.print(ATMO_CO2, 0); Serial.println(F(" ppm"));
  Serial.println();
  Serial.print(F(">>> MQ135_R0 = ")); Serial.print(r0, 3); Serial.println(F("   // kOhm"));
  Serial.println(F("Copy this value into neurobadge.ino"));
  Serial.println(F("========================================"));
  Serial.println();
  Serial.println(F("Sanity check: the line below should read close to 415 ppm."));
  Serial.print(F("Back-computed CO2e  : "));
  Serial.print(PARA * pow(rs_clean / r0, -PARB), 1);
  Serial.println(F(" ppm"));

  // A wide Rs spread means the heater has not settled. Warn explicitly.
  if ((rs_max - rs_min) > (0.25 * rs_clean)) {
    Serial.println();
    Serial.println(F("WARNING: Rs drifted more than 25% during averaging."));
    Serial.println(F("The heater has not stabilised. Wait 10 more minutes and re-run."));
  }
}

void loop() {
  delay(10000);
}
