#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include "soc/gpio_struct.h"
#include <Wire.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define WIFI_SSID "Rishab's OnePlus Nord"
#define WIFI_PASSWORD "gitsomemf"

#define API_KEY "AIzaSyCWoqmmU3XD9cih0_SqemQ3GLk361g4d-M"
#define DATABASE_URL "neuro-badge-default-rtdb.asia-southeast1.firebasedatabase.app"

#define DHTPIN 4
#define DHTTYPE DHT11 
DHT dht(DHTPIN, DHTTYPE);

#define MQ135_PIN 34   
#define BUTTON_PIN 15  

LiquidCrystal_I2C lcd(0x27, 16, 2); 

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

volatile unsigned long lastDebounceTime = 0;
volatile bool fatigueLogged = false;
String currentStatus = "NORMAL";

void IRAM_ATTR logFatigueISR() {
  unsigned long currentTime = millis();
  if ((currentTime - lastDebounceTime) > 250) { 
    fatigueLogged = true;
    lastDebounceTime = currentTime;
  }
}

void setup() {
  Serial.begin(115200);
  
  dht.begin();
  Wire.begin(5, 18);
  lcd.init();
  lcd.backlight();
  pinMode(BUTTON_PIN, INPUT_PULLUP); 
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), logFatigueISR, FALLING);

  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  lcd.clear();
  lcd.print("WiFi Connected!");
  delay(1000);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase Ready");
  }
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  lcd.clear();
}

void loop() {
  int raw_adc = analogRead(MQ135_PIN);
  int simulated_ppm = map(raw_adc, 0, 4095, 400, 2000); 
  float temp = dht.readTemperature();

  lcd.clear();

  if (fatigueLogged) {
    currentStatus = "FATIGUE LOGGED!";
    
    lcd.setCursor(0, 0);
    lcd.print(currentStatus);
    lcd.setCursor(0, 1);
    lcd.print("System Alerted.");
    
    Firebase.RTDB.setString(&fbdo, "NeuroBadge/Status", currentStatus);
    
    fatigueLogged = false;
    delay(2000); 
    return; 
  }

  if (simulated_ppm > 1000) {
    lcd.setCursor(0, 0);
    lcd.print("CO2 HIGH: " + String(simulated_ppm));
    lcd.setCursor(0, 1);

    if (temp > 30.0) {
       currentStatus = "ACT: Purifier ON";
       lcd.print(currentStatus); 
    } else {
       currentStatus = "ACT: Close Wndws";
       lcd.print(currentStatus);
    }
  } else {
    currentStatus = "NORMAL";
    lcd.setCursor(0, 0);
    lcd.print("AQI Normal: " + String(simulated_ppm));
    lcd.setCursor(0, 1);
    lcd.print("Temp: " + String(temp, 1) + "C");
  }

  if (Firebase.ready()) {
    Firebase.RTDB.setInt(&fbdo, "NeuroBadge/AQI", simulated_ppm);
    Firebase.RTDB.setFloat(&fbdo, "NeuroBadge/Temperature", temp);
    Firebase.RTDB.setString(&fbdo, "NeuroBadge/Status", currentStatus);
  }

  delay(2000); 
}
