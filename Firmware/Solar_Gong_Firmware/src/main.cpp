#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

// ---------------- Hardware configuration ----------------
static constexpr gpio_num_t RELAY_GPIO = GPIO_NUM_19;
static constexpr gpio_num_t BUTTON_GPIO = GPIO_NUM_4;
static constexpr gpio_num_t LED_GPIO = GPIO_NUM_5;
static constexpr bool RELAY_ACTIVE_HIGH = false;                   

// Light sensor (VEML7700 over I2C)
Adafruit_VEML7700 veml;

// NVS storage
Preferences preferences;

// ---------------- Behavior configuration ----------------
static constexpr float DEFAULT_LIGHT_THRESHOLD_LUX = 100.0f;
static constexpr float HYSTERESIS_LUX = 20.0f;
static constexpr unsigned long VALVE_OPEN_TIME_MS = 2000;
static constexpr uint64_t SLEEP_DURATION_US = 2ULL * 60ULL * 1000000ULL; // 2 minutes for testing
// static constexpr uint64_t SLEEP_DURATION_US = 20ULL * 3600ULL * 1000000ULL; // 20 hours for production

// Button timing
static constexpr unsigned long LONG_PRESS_DURATION_MS = 3000;  // 3 seconds to enter calibration
static constexpr unsigned long SHORT_PRESS_MAX_MS = 500;       // Under 0.5s = toggle
static constexpr unsigned long DEBOUNCE_DELAY_MS = 50;

// Testing configuration - day cycle
static constexpr unsigned long DAY_CYCLE_MS = 60000;  // 60 seconds = 1 "day" for testing
static constexpr unsigned long SENSOR_CHECK_INTERVAL_MS = 10000;  // Check sensor every 10 seconds

// RTC memory to persist data during deep sleep
RTC_DATA_ATTR bool triggeredToday = false;
RTC_DATA_ATTR bool manualMode = false;

// Global flag to control sleep behavior
bool stayAwake = false;
unsigned long awakeStartTime = 0;
static constexpr unsigned long AWAKE_TIMEOUT_MS = 20000; // 20 seconds

// Sensor monitoring timing
unsigned long lastSensorCheck = 0;
unsigned long dayStartTime = 0;

// ---------------- Forward declarations ----------------
void enterDeepSleep();
void setRelay(bool on);
void setLED(bool on);
float readLuxAveraged(uint8_t samples = 5);
float getThreshold();
void saveThreshold(float threshold);
void handleButton();
void calibrationMode();
void blinkLED(int times, int onTime, int offTime);
void waitForButtonRelease();

// ---------------- Helper functions ----------------
void setRelay(bool on) {
  pinMode(RELAY_GPIO, OUTPUT);
  digitalWrite(RELAY_GPIO, (RELAY_ACTIVE_HIGH ? on : !on));
}

void setLED(bool on) {
  pinMode(LED_GPIO, OUTPUT);
  digitalWrite(LED_GPIO, on ? HIGH : LOW);
}

float readLuxAveraged(uint8_t samples) {
  float sum = 0.0f;
  for (uint8_t i = 0; i < samples; ++i) {
    sum += veml.readLux();
    delay(20);
  }
  return sum / samples;
}

float getThreshold() {
  preferences.begin("solar-valve", true);
  float threshold = preferences.getFloat("threshold", DEFAULT_LIGHT_THRESHOLD_LUX);
  preferences.end();
  return threshold;
}

void saveThreshold(float threshold) {
  preferences.begin("solar-valve", false);
  preferences.putFloat("threshold", threshold);
  preferences.end();
  Serial.print("✓ Threshold saved: ");
  Serial.print(threshold, 1);
  Serial.println(" lux");
}

void blinkLED(int times, int onTime, int offTime) {
  for (int i = 0; i < times; i++) {
    setLED(true);
    delay(onTime);
    setLED(false);
    if (i < times - 1) delay(offTime);
  }
}

void waitForButtonRelease() {
  while (digitalRead(BUTTON_GPIO) == LOW) {
    delay(10);
  }
  delay(DEBOUNCE_DELAY_MS);
}

void calibrationMode() {
  Serial.println("\n--- CALIBRATION MODE ---");
  Serial.println("Press button to save current light level");
  
  unsigned long lastBlink = 0;
  bool ledState = false;
  
  while (true) {
    // Slow blinking (500ms on, 500ms off)
    if (millis() - lastBlink > 500) {
      ledState = !ledState;
      setLED(ledState);
      lastBlink = millis();
    }
    
    // Check for button press
    if (digitalRead(BUTTON_GPIO) == LOW) {
      delay(DEBOUNCE_DELAY_MS);
      waitForButtonRelease();
      setLED(false);
      
      // Read and save current light level
      float currentLux = readLuxAveraged(10);
      saveThreshold(currentLux);
      
      // Confirmation: 3 quick blinks
      blinkLED(3, 200, 200);
      Serial.println("✓ Calibration complete\n");
      break;
    }
    
    delay(10);
  }
  
  setLED(false);
}

void handleButton() {
  if (digitalRead(BUTTON_GPIO) == LOW) {
    delay(DEBOUNCE_DELAY_MS);
    unsigned long pressStart = millis();
    
    // Wait while button is held down
    while (digitalRead(BUTTON_GPIO) == LOW) {
      unsigned long pressDuration = millis() - pressStart;
      
      // Long press (3 seconds) - enter calibration mode
      if (pressDuration >= LONG_PRESS_DURATION_MS) {
        waitForButtonRelease();
        blinkLED(3, 100, 100);
        delay(500);
        
        stayAwake = true;
        awakeStartTime = millis();
        calibrationMode();
        return;
      }
      
      delay(10);
    }
    
    // Button released - check if it was a short press
    unsigned long pressDuration = millis() - pressStart;
    
    if (pressDuration < SHORT_PRESS_MAX_MS) {
      // Short press - toggle relay
      manualMode = !manualMode;
      setRelay(manualMode);
      setLED(manualMode);
      stayAwake = true;
      awakeStartTime = millis();
      Serial.print("✓ Relay ");
      Serial.println(manualMode ? "ON" : "OFF");
      delay(500);
    }
  }
}

// ---------------- Arduino lifecycle ----------------
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  Serial.begin(115200);
  // Don't wait for Serial - allow operation without USB
  delay(100);
  Serial.println("\n=== Solar Valve Controller ===");
  
  // Setup pins FIRST before any other operations
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  pinMode(LED_GPIO, OUTPUT);
  pinMode(RELAY_GPIO, OUTPUT);
  setLED(false);
  setRelay(false);
  
  // Startup confirmation blink (works on battery to show device is running)
  for (int i = 0; i < 2; i++) {
    setLED(true);
    delay(100);
    setLED(false);
    delay(100);
  }
  
  // Initialize light sensor
  Wire.begin();
  if (!veml.begin()) {
    Serial.println("✗ Sensor error");
    blinkLED(5, 100, 100);
    enterDeepSleep();
    return;
  }
  
  veml.setGain(VEML7700_GAIN_1_8);
  veml.setIntegrationTime(VEML7700_IT_100MS);
  veml.powerSaveEnable(false);
  delay(200);
  
  // Check for button press at startup
  handleButton();
  
  // Initialize timing for continuous monitoring
  dayStartTime = millis();
  lastSensorCheck = millis();
  
  Serial.println("Starting continuous monitoring mode");
  Serial.println("Day cycle: 60 seconds | Check interval: 10 seconds\n");
  
  // Keep device awake for continuous monitoring
  stayAwake = true;
  awakeStartTime = millis();
}

void enterDeepSleep() {
  Serial.print("Sleeping for ");
  Serial.print((unsigned long)(SLEEP_DURATION_US / 1000000ULL));
  Serial.println(" seconds...\n");
  Serial.flush();
  
  digitalWrite(LED_GPIO, LOW);
  digitalWrite(RELAY_GPIO, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);
  
  esp_deep_sleep_start();
}

void loop() {
  // Check if a new "day" has started (60 second cycle)
  if (millis() - dayStartTime >= DAY_CYCLE_MS) {
    triggeredToday = false;  // Reset flag for new day
    dayStartTime = millis();
    Serial.println("\n=== NEW DAY CYCLE (60s) - Flag reset ===");
    Serial.println();
  }
  
  // Check sensor every 10 seconds
  if (millis() - lastSensorCheck >= SENSOR_CHECK_INTERVAL_MS) {
    lastSensorCheck = millis();
    
    float currentLux = readLuxAveraged(5);
    float threshold = getThreshold();
    
    // Display current status
    Serial.print("[Check] Light: ");
    Serial.print(currentLux, 1);
    Serial.print(" lux | Threshold: ");
    Serial.print(threshold, 1);
    Serial.print(" lux | Flag: ");
    Serial.println(triggeredToday ? "TRIGGERED" : "READY");
    
    // Check if conditions are met to trigger relay
    if (currentLux >= threshold && !triggeredToday) {
      Serial.println(">>> Threshold exceeded & flag ready - ACTIVATING RELAY <<<");
      
      setRelay(true);
      blinkLED(1, 200, 0);
      setLED(true);
      delay(VALVE_OPEN_TIME_MS);
      setRelay(false);
      setLED(false);
      triggeredToday = true;
      
      Serial.println("✓ Relay activated (2s) - Flag set\n");
    } else if (currentLux >= threshold && triggeredToday) {
      Serial.println(">>> Threshold exceeded but already triggered today\n");
    } else {
      Serial.println(">>> Below threshold - No action\n");
    }
  }
  
  // Check for button presses
  if (digitalRead(BUTTON_GPIO) == LOW) {
    delay(DEBOUNCE_DELAY_MS);
    unsigned long pressStart = millis();
    
    // Wait while button is held
    while (digitalRead(BUTTON_GPIO) == LOW) {
      unsigned long pressDuration = millis() - pressStart;
      
      // Long press - enter calibration
      if (pressDuration >= LONG_PRESS_DURATION_MS) {
        waitForButtonRelease();
        blinkLED(3, 100, 100);
        delay(500);
        calibrationMode();
        // Reset timers after calibration
        lastSensorCheck = millis();
        return;
      }
      delay(10);
    }
    
    // Short press - toggle relay
    unsigned long pressDuration = millis() - pressStart;
    if (pressDuration < SHORT_PRESS_MAX_MS) {
      manualMode = !manualMode;
      setRelay(manualMode);
      setLED(manualMode);
      Serial.print("✓ Manual override - Relay ");
      Serial.println(manualMode ? "ON" : "OFF");
      delay(500);
    }
  }
  
  delay(100);
}
