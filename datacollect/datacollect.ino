/*
 * Neuro-Badge :: Edge Impulse Data Forwarder Stream
 * -------------------------------------------------
 * Emits one CSV line per sample on the serial port, which the
 * edge-impulse-data-forwarder pipes into the Data Acquisition tab.
 *
 * Axes (declare them in the forwarder in THIS order):
 *     gas_raw, temperature, humidity
 *
 * Sample rate: 2 Hz.
 *
 * Why humidity is a third axis:
 *   Exhaled breath raises humidity sharply; solvent vapour does not. Humidity
 *   is therefore the cleanest single discriminator between poor_ventilation
 *   (human CO2 load) and high_voc (chemical intrusion), which is precisely the
 *   distinction the previous 3-class model failed to make. The first run
 *   collapsed to 55.5% because it had only gas level and temperature to work
 *   with, and the saturated sensor made both classes look identical.
 *
 * Why the DHT is cached:
 *   The DHT11 cannot be polled faster than roughly 1 Hz; reading it every
 *   500 ms returns NaN or stale data. The gas channel is sampled at the full
 *   2 Hz and the climate channels are refreshed every 1000 ms and held between
 *   refreshes. This is a deliberate, documented rate mismatch, not a bug.
 *
 * NOTHING may be printed to serial except the data lines. Any banner, prompt
 * or debug string corrupts the forwarder's stream.
 */

#include <DHT.h>

#define DHTPIN         4
#define DHTTYPE        DHT11
#define MQ135_PIN      34

#define SAMPLE_PERIOD_MS   500     // 2 Hz
#define DHT_PERIOD_MS      1000    // DHT11 hardware limit

DHT dht(DHTPIN, DHTTYPE);

float last_temp = 25.0;
float last_hum  = 50.0;
unsigned long last_dht_read = 0;
unsigned long next_sample   = 0;

void setup() {
  Serial.begin(115200);
  dht.begin();

  // Let the DHT settle and prime the cache before the stream opens, so the
  // very first samples are real readings rather than the fallback constants.
  delay(2000);
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) last_temp = t;
  if (!isnan(h)) last_hum  = h;
  last_dht_read = millis();
  next_sample   = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - last_dht_read >= DHT_PERIOD_MS) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) last_temp = t;   // hold the last good value on a failed read
    if (!isnan(h)) last_hum  = h;
    last_dht_read = now;
  }

  if ((long)(now - next_sample) >= 0) {
    next_sample += SAMPLE_PERIOD_MS;   // fixed cadence, no cumulative drift

    int gas_raw = analogRead(MQ135_PIN);

    Serial.print(gas_raw);
    Serial.print(',');
    Serial.print(last_temp, 1);
    Serial.print(',');
    Serial.println(last_hum, 1);
  }
}
