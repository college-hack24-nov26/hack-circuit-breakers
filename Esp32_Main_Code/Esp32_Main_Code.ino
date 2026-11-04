/************************************************************
  AGRIMIND - ESP32 MAIN CONTROLLER
  Blynk + Soil + Rain + DHT11 + NPK/RS485 + LCD + Flow + Pumps

  IMPORTANT:
  1) Enter your Wi-Fi and NEW Blynk Auth Token below.
  2) Pumps must remain disconnected during first testing.
  3) Relay boards differ: set RELAY_ACTIVE_LOW after testing.
  4) Soil calibration values and flow pulses/litre need calibration.
************************************************************/

#define BLYNK_TEMPLATE_ID   "TMPL3cLa_rlT2"
#define BLYNK_TEMPLATE_NAME "Agrimind"
#define BLYNK_AUTH_TOKEN    "fT5JIHPBNniYoAA5tLpqnLkVD7QdhNx5"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <ModbusMaster.h>

// -------------------- WIFI --------------------
char ssid[] = "OnePlus Nord CE4";
char pass[] = "Joshi@606";

// -------------------- PIN MAP --------------------
#define SOIL_AO       34
#define SOIL_DO       14
#define RAIN_AO       35
#define RAIN_DO       27

#define DHT_PIN       13
#define DHT_TYPE      DHT11

#define LCD_SDA       21
#define LCD_SCL       19
#define LCD_ADDRESS   0x27

#define MAIN_RELAY    32
#define REFILL_RELAY  33

#define FLOW_PIN      26

#define MAX485_DE_RE  4
#define MAX485_RX     16
#define MAX485_TX     17

#define WIFI_LED      2

// -------------------- HARDWARE SETTINGS --------------------
// Most common relay modules are active LOW. If your relay board
// is active HIGH, change this to false AFTER testing with pumps disconnected.
#define RELAY_ACTIVE_LOW true

// Many ESP32 dev boards have an active-HIGH LED on GPIO2.
// If your board's LED behaves oppositely, change this to false.
#define LED_ACTIVE_HIGH true

// Soil sensor calibration: replace after measuring your own sensor.
#define SOIL_DRY_RAW 3000
#define SOIL_WET_RAW 1500
#define SOIL_DRY_RAW_HIGHER true

// Common rain modules are active LOW, but verify your module.
#define RAIN_ACTIVE_LOW true

// Starting estimate only; calibrate using a measured volume.
#define FLOW_PULSES_PER_LITRE 450.0f

// NPK sensor manual: address 1, 9600 baud, 8N1, registers 0..2.
#define NPK_SLAVE_ID 1
#define NPK_BAUD_RATE 9600

// Soil hysteresis thresholds.
#define SOIL_DRY_THRESHOLD 35
#define SOIL_WET_THRESHOLD 60

// Flow protection: after pump start, allow this long for first pulse.
// If no pulse arrives, pump is stopped and fault stays latched until reboot.
#define FLOW_TIMEOUT_MS 15000UL
#define ENABLE_FLOW_PROTECTION true

// -------------------- OBJECTS --------------------
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
ModbusMaster npk;
BlynkTimer timer;

// -------------------- STATE --------------------
volatile uint32_t flowPulses = 0;

float flowRate = 0.0f;
float temperature = NAN;
float humidity = NAN;
float nitrogen = NAN, phosphorus = NAN, potassium = NAN;

int soilRaw = 0;
int soilPercent = 0;

bool rainDetected = false;
bool mainPumpState = false;
bool refillPumpState = false;
bool autoMode = false;            // SAFETY: automatic mode starts OFF
bool flowFault = false;           // Latched; not cleared automatically
bool dhtAvailable = false;
bool npkAvailable = false;

unsigned long pumpStartedAt = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastBlynkAttempt = 0;

// -------------------- FORWARD DECLARATIONS --------------------
void updateMainPump(bool on);
void updateRefillPump(bool on);
void automaticIrrigation();
void sendPumpStatus();

// -------------------- BLYNK HELPERS --------------------
void sendBlynkFloat(uint8_t pin, float value) {
  if (Blynk.connected() && !isnan(value)) {
    Blynk.virtualWrite(pin, value);
  }
}

void sendPumpStatus() {
  if (!Blynk.connected()) return;

  if (flowFault) {
    Blynk.virtualWrite(V11, "FLOW FAULT - REBOOT");
  } else if (mainPumpState) {
    Blynk.virtualWrite(V11, "MAIN PUMP ON");
  } else if (refillPumpState) {
    Blynk.virtualWrite(V11, "REFILL PUMP ON");
  } else {
    Blynk.virtualWrite(V11, "PUMPS OFF");
  }
}

// -------------------- RELAY CONTROL --------------------
void writeRelayPin(uint8_t pin, bool on) {
  // Convert logical ON/OFF to the electrical level for this relay board.
  uint8_t level;
  if (RELAY_ACTIVE_LOW) {
    level = on ? LOW : HIGH;
  } else {
    level = on ? HIGH : LOW;
  }
  digitalWrite(pin, level);
}

void updateMainPump(bool on) {
  if (on && flowFault) on = false;

  if (on && refillPumpState) {
    updateRefillPump(false);       // Prevent both pumps running together
  }

  if (mainPumpState == on) return;

  mainPumpState = on;
  writeRelayPin(MAIN_RELAY, on);

  if (on) {
    pumpStartedAt = millis();
  }

  if (Blynk.connected()) {
    Blynk.virtualWrite(V0, on ? 1 : 0);
  }
  sendPumpStatus();

  Serial.println(on ? "MAIN PUMP: ON" : "MAIN PUMP: OFF");
}

void updateRefillPump(bool on) {
  if (on && mainPumpState) {
    updateMainPump(false);         // Prevent both pumps running together
  }

  if (refillPumpState == on) return;

  refillPumpState = on;
  writeRelayPin(REFILL_RELAY, on);

  if (Blynk.connected()) {
    Blynk.virtualWrite(V1, on ? 1 : 0);
  }
  sendPumpStatus();

  Serial.println(on ? "REFILL PUMP: ON" : "REFILL PUMP: OFF");
}

// -------------------- BLYNK CONTROLS --------------------
// V0: Main pump manual switch. Works only when Auto Irrigation is OFF.
BLYNK_WRITE(V0) {
  bool requested = (param.asInt() == 1);

  if (autoMode) {
    Blynk.virtualWrite(V0, mainPumpState ? 1 : 0);
    return;
  }

  if (requested && flowFault) {
    Blynk.virtualWrite(V0, 0);
    sendPumpStatus();
    return;
  }

  updateMainPump(requested);
}

// V1: Refill pump switch.
BLYNK_WRITE(V1) {
  updateRefillPump(param.asInt() == 1);
}

// V2: Automatic irrigation switch.
BLYNK_WRITE(V2) {
  bool requested = (param.asInt() == 1);

  if (requested) {
    autoMode = true;
    updateRefillPump(false);
    automaticIrrigation();
  } else {
    autoMode = false;
    updateMainPump(false);
  }

  Blynk.virtualWrite(V2, autoMode ? 1 : 0);
  sendPumpStatus();
}

BLYNK_CONNECTED() {
  // Report actual safe local states; do not restore old ON commands.
  Blynk.virtualWrite(V0, mainPumpState ? 1 : 0);
  Blynk.virtualWrite(V1, refillPumpState ? 1 : 0);
  Blynk.virtualWrite(V2, autoMode ? 1 : 0);
  sendPumpStatus();
}

// -------------------- SOIL SENSOR --------------------
void readSoilSensor() {
  soilRaw = analogRead(SOIL_AO);

  long pct;
  if (SOIL_DRY_RAW_HIGHER) {
    pct = map(soilRaw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
  } else {
    pct = map(soilRaw, SOIL_WET_RAW, SOIL_DRY_RAW, 100, 0);
  }

  soilPercent = constrain((int)pct, 0, 100);
  sendBlynkFloat(V10, (float)soilPercent);

  Serial.print("Soil raw: ");
  Serial.print(soilRaw);
  Serial.print(" | Moisture: ");
  Serial.print(soilPercent);
  Serial.println("%");
}

// -------------------- RAIN SENSOR --------------------
void readRainSensor() {
  int value = digitalRead(RAIN_DO);
  rainDetected = RAIN_ACTIVE_LOW ? (value == LOW) : (value == HIGH);

  if (Blynk.connected()) {
    Blynk.virtualWrite(V9, rainDetected ? "RAIN DETECTED" : "NO RAIN");
  }

  Serial.println(rainDetected ? "RAIN DETECTED" : "NO RAIN");
}

// -------------------- AUTOMATIC IRRIGATION --------------------
void automaticIrrigation() {
  if (!autoMode) return;

  if (rainDetected || flowFault) {
    updateMainPump(false);
    return;
  }

  if (soilPercent <= SOIL_DRY_THRESHOLD) {
    updateMainPump(true);
  } else if (soilPercent >= SOIL_WET_THRESHOLD) {
    updateMainPump(false);
  }
  // Between 35% and 60%, keep the previous pump state (hysteresis).
}

// -------------------- DHT11 --------------------
void readDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    dhtAvailable = false;
    Serial.println("DHT11 read failed");
    return;
  }

  temperature = t;
  humidity = h;
  dhtAvailable = true;

  sendBlynkFloat(V4, temperature);
  sendBlynkFloat(V5, humidity);

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.print(" C | Humidity: ");
  Serial.print(humidity);
  Serial.println("%");
}

// -------------------- FLOW SENSOR --------------------
void IRAM_ATTR flowPulseISR() {
  flowPulses++;
}

void readFlowSensor() {
  static unsigned long previousRead = 0;
  unsigned long now = millis();

  if (now - previousRead < 1000) return;

  noInterrupts();
  uint32_t pulses = flowPulses;
  flowPulses = 0;
  interrupts();

  float elapsedSeconds = (now - previousRead) / 1000.0f;
  previousRead = now;

  flowRate = (pulses * 60.0f) /
             (FLOW_PULSES_PER_LITRE * elapsedSeconds);

  sendBlynkFloat(V3, flowRate);

  if (mainPumpState && pulses > 0) {
    // A pulse confirms flow. The timer below is only a no-pulse safety check.
    pumpStartedAt = now;
  }

  if (ENABLE_FLOW_PROTECTION && mainPumpState &&
      (now - pumpStartedAt > FLOW_TIMEOUT_MS)) {
    flowFault = true;              // LATCH fault; never auto-clear
    updateMainPump(false);
    Serial.println("FLOW FAULT: no pulses; main pump stopped. Reboot after checking flow path.");
  }

  Serial.print("Flow: ");
  Serial.print(flowRate, 1);
  Serial.println(" L/min");
  sendPumpStatus();
}

// -------------------- MAX485 / NPK --------------------
void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH);
}

void postTransmission() {
  digitalWrite(MAX485_DE_RE, LOW);
}

void readNPK() {
  uint8_t result = npk.readHoldingRegisters(0x0000, 3);

  if (result == npk.ku8MBSuccess) {
    nitrogen   = (float)npk.getResponseBuffer(0);
    phosphorus = (float)npk.getResponseBuffer(1);
    potassium  = (float)npk.getResponseBuffer(2);
    npkAvailable = true;

    sendBlynkFloat(V6, nitrogen);
    sendBlynkFloat(V7, phosphorus);
    sendBlynkFloat(V8, potassium);

    Serial.print("N: "); Serial.print(nitrogen);
    Serial.print(" | P: "); Serial.print(phosphorus);
    Serial.print(" | K: "); Serial.println(potassium);
  } else {
    npkAvailable = false;
    Serial.print("NPK Modbus error: ");
    Serial.println(result);
  }
}

// -------------------- LCD --------------------
void printLCDLine(uint8_t row, const char *message) {
  lcd.setCursor(0, row);
  lcd.print(message);
  int len = strlen(message);
  while (len++ < 16) lcd.print(' ');
}

void updateLCD() {
  char line1[17];
  char line2[17];

  if (dhtAvailable) {
    snprintf(line1, sizeof(line1), "T:%2.0fC H:%2.0f%%", temperature, humidity);
  } else {
    snprintf(line1, sizeof(line1), "DHT11 READ ERROR");
  }

  if (flowFault) {
    snprintf(line2, sizeof(line2), "FLOW FAULT");
  } else if (rainDetected) {
    snprintf(line2, sizeof(line2), "RAIN - PUMP OFF");
  } else if (mainPumpState) {
    snprintf(line2, sizeof(line2), "MAIN PUMP: ON");
  } else if (refillPumpState) {
    snprintf(line2, sizeof(line2), "REFILL PUMP ON");
  } else {
    snprintf(line2, sizeof(line2), "PUMPS: OFF");
  }

  printLCDLine(0, line1);
  printLCDLine(1, line2);
}

// -------------------- WIFI LED --------------------
void updateWiFiLED() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  digitalWrite(WIFI_LED, (connected == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

// -------------------- WIFI / BLYNK CONNECTION --------------------
void maintainConnections() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiAttempt >= 10000UL) {
      lastWiFiAttempt = now;
      Serial.println("Wi-Fi disconnected; trying reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
    }
    return;
  }

  if (!Blynk.connected() && now - lastBlynkAttempt >= 5000UL) {
    lastBlynkAttempt = now;
    Serial.println("Connecting to Blynk...");
    Blynk.connect(1000);
  }
}

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  // Set relay output latch to OFF BEFORE switching pins to OUTPUT.
  // This reduces startup glitches on active-low relay modules.
  digitalWrite(MAIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(REFILL_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(MAIN_RELAY, OUTPUT);
  pinMode(REFILL_RELAY, OUTPUT);
  writeRelayPin(MAIN_RELAY, false);
  writeRelayPin(REFILL_RELAY, false);
  mainPumpState = false;
  refillPumpState = false;

  pinMode(SOIL_AO, INPUT);
  pinMode(SOIL_DO, INPUT);
  pinMode(RAIN_AO, INPUT);
  pinMode(RAIN_DO, INPUT);
  pinMode(FLOW_PIN, INPUT_PULLUP);

  digitalWrite(MAX485_DE_RE, LOW);
  pinMode(MAX485_DE_RE, OUTPUT);

  pinMode(WIFI_LED, OUTPUT);
  digitalWrite(WIFI_LED, LED_ACTIVE_HIGH ? LOW : HIGH);

  // LCD library installed on your computer uses begin() with NO arguments.
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.begin();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AGRIMIND SYSTEM");
  lcd.setCursor(0, 1);
  lcd.print("PUMPS ARE OFF");

  dht.begin();

  // RS485 NPK sensor
  Serial2.begin(NPK_BAUD_RATE, SERIAL_8N1, MAX485_RX, MAX485_TX);
  npk.begin(NPK_SLAVE_ID, Serial2);
  npk.preTransmission(preTransmission);
  npk.postTransmission(postTransmission);

  // Initial sensor readings before automatic mode can be enabled.
  readSoilSensor();
  readRainSensor();
  readDHT();

  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);

  // Wi-Fi: phone hotspot must provide 2.4 GHz compatibility.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(ssid, pass);

  Blynk.config(BLYNK_AUTH_TOKEN);

  // Sensor and display tasks
  timer.setInterval(2500L, readDHT);
  timer.setInterval(2000L, readSoilSensor);
  timer.setInterval(2000L, readRainSensor);
  timer.setInterval(1000L, readFlowSensor);
  timer.setInterval(2000L, automaticIrrigation);
  timer.setInterval(500L, updateLCD);
  timer.setInterval(1000L, updateWiFiLED);
  timer.setInterval(15000L, readNPK);
  timer.setInterval(500L, maintainConnections);

  Serial.println();
  Serial.println("AGRIMIND STARTED - BOTH PUMPS OFF");
  Serial.println("Auto irrigation is OFF. Enable it from Blynk only after sensor checks.");
}

// -------------------- LOOP --------------------
void loop() {
  if (WiFi.status() == WL_CONNECTED && Blynk.connected()) {
    Blynk.run();
  }
  timer.run();
}
