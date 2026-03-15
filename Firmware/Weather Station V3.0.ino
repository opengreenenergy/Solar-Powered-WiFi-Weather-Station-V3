/*
====================================================================================================================
  PROJECT      : Solar Powered WiFi Weather Station V3.0
  VERSION      : v3.1 Deep Sleep Edition
  UPDATED ON   : 30-Mar-2021
  AUTHOR       : Open Green Energy

  LICENSE
  ------------------------------------------------------------------------------------------------------------------
  Copyright (c) 2026 Open Green Energy

  Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
  https://creativecommons.org/licenses/by-nc-sa/4.0/

  HARDWARE USED
  ------------------------------------------------------------------------------------------------------------------
  MCU            : ESP32
  ENV SENSOR     : BME280
  UV SENSOR      : SI1145
  LIGHT SENSOR   : BH1750
  TEMP SENSOR    : DS18B20
  WEATHER METER  : SparkFun Weather Meter style wind and rain sensors
  BAT SENSE      : Voltage divider to ADC
  CLOUD          : Blynk or ThingSpeak
  POWER          : Solar + Li-ion battery

  WHAT THIS FIRMWARE DOES
  ------------------------------------------------------------------------------------------------------------------
  1. Wakes up from deep sleep at a fixed interval.
  2. Reads environmental sensors.
  3. Samples wind and rain for a short awake window.
  4. Uploads data to Blynk or ThingSpeak.
  5. Turns off WiFi and returns to deep sleep.

  IMPORTANT DEEP SLEEP LIMITATION
  ------------------------------------------------------------------------------------------------------------------
  This firmware is optimized for low power operation, but wind speed and rainfall pulses are only captured while the
  ESP32 is awake. Pulses occurring during deep sleep are not counted. Therefore:

  - Temperature, humidity, pressure, UV, lux, battery voltage, and DS18B20 temperature are suitable for deep sleep.
  - Wind speed and rainfall values are sample-based, not continuous, in this deep sleep version.

====================================================================================================================
*/

#include <BME280I2C.h>
#include <Adafruit_SI1145.h>
#include <BH1750.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Wire.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include "esp_sleep.h"

// ============================================================================
// USER CONFIGURATION
// ============================================================================

// Data upload destination
// Use either "BLYNK" or "Thingspeak"
const String App = "Thingspeak";

// Deep sleep interval
const uint64_t SLEEP_INTERVAL_MIN = 15;  // Minutes between uploads

// Awake sampling window for wind and rain pulse capture
const unsigned long SAMPLE_WINDOW_MS = 15000;  // 15 seconds

// WiFi connection timeout
const unsigned long WIFI_TIMEOUT_MS = 20000;

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define WIND_SPD_PIN 14
#define RAIN_PIN     25
#define WIND_DIR_PIN 35
#define VOLT_PIN     33
#define TEMP_PIN     4

// ============================================================================
// WIFI AND CLOUD CREDENTIALS
// ============================================================================
char ssid[] = "XXXX";
char pass[] = "XXXX";
char auth[] = "XXXX";

const char* server  = "api.thingspeak.com";
const char* api_key = "XXXX";

// ============================================================================
// SENSOR OBJECTS
// ============================================================================
WiFiClient client;
BME280I2C bme;
Adafruit_SI1145 uv = Adafruit_SI1145();
BH1750 lightMeter(0x23);
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

// ============================================================================
// RTC PERSISTENT VARIABLES
// These retain values across deep sleep resets
// ============================================================================
RTC_DATA_ATTR uint32_t bootCount = 0;

// ============================================================================
// GLOBAL MEASUREMENT VARIABLES
// ============================================================================
float temperature = 0.0f;
float humidity    = 0.0f;
float pressure    = 0.0f;
float UVindex     = 0.0f;
float lux         = 0.0f;
float windSpeed   = 0.0f;
float batteryVolt = 0.0f;
float ds18TempC   = 0.0f;

int vin = 0;
String windDir = "0";

// ============================================================================
// WIND SPEED VARIABLES
// ============================================================================
volatile unsigned long timeSinceLastTick = 0;
volatile unsigned long lastTick = 0;

// ============================================================================
// RAINFALL VARIABLES
// Note: In deep sleep mode, rain is captured only during awake time
// ============================================================================
volatile uint32_t rainTicks = 0;

// ============================================================================
// BATTERY VOLTAGE DIVIDER VALUES
// ============================================================================
float Vout = 0.0f;
float Vin  = 0.0f;
float R1   = 27000.0f;
float R2   = 100000.0f;
int val    = 0;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================
void initSensors();
bool wifiConnect();
void disconnectWiFi();
void readSensorsData();
void sampleWeatherMeters(unsigned long windowMs);
void sendData();
void enterDeepSleep();
void printWakeupReason();
void windDirCalc();
void IRAM_ATTR windTick();
void IRAM_ATTR rainTick();
void printData();

// ============================================================================
// SETUP
// Runs once at every wakeup from deep sleep
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  bootCount++;
  Serial.println();
  Serial.println("==================================================");
  Serial.println("Solar Powered WiFi Weather Station V3.1");
  Serial.print("Boot count: ");
  Serial.println(bootCount);
  printWakeupReason();

  Wire.begin();
  initSensors();

  // Configure weather meter input pins
  pinMode(WIND_SPD_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);
  pinMode(WIND_DIR_PIN, INPUT);
  pinMode(VOLT_PIN, INPUT);

  // Attach interrupts for awake-time sampling
  attachInterrupt(digitalPinToInterrupt(WIND_SPD_PIN), windTick, FALLING);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), rainTick, FALLING);

  // Read all slow sensors first
  readSensorsData();

  // Sample wind and rain during awake window
  sampleWeatherMeters(SAMPLE_WINDOW_MS);

  // Print measured values
  printData();

  // Connect WiFi, upload, then sleep
  if (wifiConnect()) {
    sendData();
    disconnectWiFi();
  } else {
    Serial.println("WiFi connection failed. Skipping upload this cycle.");
  }

  enterDeepSleep();
}

// ============================================================================
// LOOP
// Not used in deep sleep architecture
// ============================================================================
void loop() {
  // Intentionally empty
}

// ============================================================================
// SENSOR INITIALIZATION
// ============================================================================
void initSensors() {
  sensors.begin();

  if (!bme.begin()) {
    Serial.println("BME280 init failed");
  }

  if (!uv.begin(0x60)) {
    Serial.println("SI1145 init failed");
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 init failed");
  }
}

// ============================================================================
// WIFI CONNECT
// Supports both Blynk and ThingSpeak
// ============================================================================
bool wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  Serial.print("Connecting to WiFi");
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      return false;
    }
  }

  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());

  if (App == "BLYNK") {
    Blynk.config(auth);
    if (!Blynk.connect(10000)) {
      Serial.println("Blynk connection failed");
      return false;
    }
  }

  return true;
}

// ============================================================================
// WIFI DISCONNECT BEFORE SLEEP
// ============================================================================
void disconnectWiFi() {
  if (App == "BLYNK") {
    Blynk.disconnect();
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  delay(100);
}

// ============================================================================
// READ ALL SLOW-CHANGING SENSORS
// ============================================================================
void readSensorsData() {
  // BME280
  bme.read(pressure, temperature, humidity, BME280::TempUnit_Celsius, BME280::PresUnit_Pa);

  // DS18B20
  sensors.requestTemperatures();
  ds18TempC = sensors.getTempCByIndex(0);

  // SI1145 UV
  UVindex = uv.readUV();
  UVindex /= 100.0f;

  // BH1750 lux
  lux = lightMeter.readLightLevel();

  // Battery voltage
  val = analogRead(VOLT_PIN);
  Vout = (val * 3.3f) / 4095.0f;
  batteryVolt = Vout * (R2 + R1) / R2;

  // Wind direction
  windDirCalc();
}

// ============================================================================
// SAMPLE WIND SPEED AND RAIN FOR A SHORT AWAKE WINDOW
// Deep sleep cannot count pulses while sleeping, so this is sample-based only
// ============================================================================
void sampleWeatherMeters(unsigned long windowMs) {
  Serial.print("Sampling wind and rain for ");
  Serial.print(windowMs / 1000);
  Serial.println(" seconds...");

  // Reset awake-window counters
  rainTicks = 0;
  timeSinceLastTick = 0;
  lastTick = millis();

  unsigned long start = millis();
  while (millis() - start < windowMs) {
    delay(10);
  }

  // Convert wind pulse timing to speed
  // Original code used: windSpeed = 1000.0 / timeSinceLastTick
  if (timeSinceLastTick != 0) {
    windSpeed = 1000.0f / timeSinceLastTick;
  } else {
    windSpeed = 0.0f;
  }
}

// ============================================================================
// WIND SPEED INTERRUPT
// ============================================================================
void IRAM_ATTR windTick() {
  unsigned long now = millis();
  timeSinceLastTick = now - lastTick;
  lastTick = now;
}

// ============================================================================
// RAIN INTERRUPT
// ============================================================================
void IRAM_ATTR rainTick() {
  rainTicks++;
}

// ============================================================================
// WIND DIRECTION CALCULATION
// ============================================================================
void windDirCalc() {
  vin = analogRead(WIND_DIR_PIN);

  if      (vin < 150)  windDir = "202.5";
  else if (vin < 300)  windDir = "180";
  else if (vin < 400)  windDir = "247.5";
  else if (vin < 600)  windDir = "225";
  else if (vin < 900)  windDir = "292.5";
  else if (vin < 1100) windDir = "270";
  else if (vin < 1500) windDir = "112.5";
  else if (vin < 1700) windDir = "135";
  else if (vin < 2250) windDir = "337.5";
  else if (vin < 2350) windDir = "315";
  else if (vin < 2700) windDir = "67.5";
  else if (vin < 3000) windDir = "90";
  else if (vin < 3200) windDir = "22.5";
  else if (vin < 3400) windDir = "45";
  else if (vin < 4000) windDir = "0";
  else                 windDir = "0";
}

// ============================================================================
// PRINT DATA TO SERIAL MONITOR
// ============================================================================
void printData() {
  Serial.println("--------------- Sensor Data ---------------");
  Serial.print("Air Temperature [C]: ");
  Serial.println(temperature);

  Serial.print("Humidity [%]: ");
  Serial.println(humidity);

  Serial.print("Pressure [hPa]: ");
  Serial.println(pressure / 100.0f);

  Serial.print("UV Index: ");
  Serial.println(UVindex);

  Serial.print("Light [Lux]: ");
  Serial.println(lux);

  Serial.print("Wind Speed Raw: ");
  Serial.println(windSpeed);

  Serial.print("Wind Speed [km/h approx]: ");
  Serial.println(windSpeed * 2.4f * 4.5f);

  Serial.print("Wind Direction [deg]: ");
  Serial.println(windDir);

  Serial.print("Rain Ticks During Awake Window: ");
  Serial.println(rainTicks);

  Serial.print("Battery Voltage [V]: ");
  Serial.println(batteryVolt);

  Serial.print("DS18B20 Temperature [C]: ");
  Serial.println(ds18TempC);
  Serial.println("-------------------------------------------");
}

// ============================================================================
// SEND DATA TO CLOUD
// ============================================================================
void sendData() {
  if (App == "BLYNK") {
    Blynk.virtualWrite(V0, temperature);
    Blynk.virtualWrite(V1, humidity);
    Blynk.virtualWrite(V2, pressure / 100.0f);
    Blynk.virtualWrite(V3, UVindex);
    Blynk.virtualWrite(V4, windSpeed * 2.4f * 4.5f);  // km/h approx
    Blynk.virtualWrite(V5, windDir);
    Blynk.virtualWrite(V6, rainTicks);
    Blynk.virtualWrite(V7, batteryVolt);
    Blynk.virtualWrite(V8, ds18TempC);
    Blynk.virtualWrite(V9, lux);

    Blynk.run();
    delay(1000);
    Serial.println("Blynk upload complete");
  }
  else if (App == "Thingspeak") {
    WiFiClient tsClient;

    if (tsClient.connect(server, 80)) {
      Serial.println("Connected to ThingSpeak");

      String postStr = "";
      postStr += "GET /update?api_key=";
      postStr += api_key;
      postStr += "&field1=" + String(temperature);
      postStr += "&field2=" + String(humidity);
      postStr += "&field3=" + String(pressure / 100.0f);
      postStr += "&field4=" + String(UVindex);
      postStr += "&field5=" + String(windSpeed * 2.4f * 4.5f);  // km/h approx
      postStr += "&field6=" + String(windDir);
      postStr += "&field7=" + String(rainTicks);
      postStr += "&field8=" + String(batteryVolt);
      postStr += "&field9=" + String(ds18TempC);
      postStr += "&field10=" + String(lux);
      postStr += " HTTP/1.1\r\nHost: api.thingspeak.com\r\nConnection: close\r\n\r\n";

      tsClient.print(postStr);
      delay(2000);

      while (tsClient.available()) {
        String line = tsClient.readStringUntil('\n');
        Serial.println(line);
      }

      tsClient.stop();
      Serial.println("ThingSpeak upload complete");
    } else {
      Serial.println("ThingSpeak connection failed");
    }
  }
  else {
    Serial.print("Invalid app setting: ");
    Serial.println(App);
  }
}

// ============================================================================
// PRINT WAKEUP REASON
// ============================================================================
void printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  Serial.print("Wakeup reason: ");
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Timer"); break;
    case ESP_SLEEP_WAKEUP_EXT0:  Serial.println("External signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1:  Serial.println("External signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP:   Serial.println("ULP"); break;
    default:                     Serial.println("Power on or unknown"); break;
  }
}

// ============================================================================
// ENTER DEEP SLEEP
// ============================================================================
void enterDeepSleep() {
  uint64_t sleepUs = SLEEP_INTERVAL_MIN * 60ULL * 1000000ULL;

  Serial.println();
  Serial.print("Going to deep sleep for ");
  Serial.print(SLEEP_INTERVAL_MIN);
  Serial.println(" minutes...");
  Serial.flush();

  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();
}
