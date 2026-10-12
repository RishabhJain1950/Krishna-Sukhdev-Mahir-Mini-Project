#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_BMP280.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <TinyGPSPlus.h>

// ---------------- Pins ----------------
#define SDA_PIN       21
#define SCL_PIN       22
#define BUZZER_PIN    25
#define BUTTON_PIN    35   // external pull-up: released HIGH, pressed LOW
#define GPS_RX_PIN    32   // GPS TX -> ESP32 GPIO32

#define LORA_SS       5
#define LORA_RST      14
#define LORA_DIO0     26

#define MPU_ADDR      0x68

// ---------------- Devices ----------------
Adafruit_BMP280 bmp;
MAX30105 maxSensor;
TinyGPSPlus gps;
HardwareSerial GPS(2);

// ---------------- State ----------------
bool fallDetected = false;
float heartRate = 0.0f;
long lastBeat = 0;
unsigned long lastTelemetry = 0;

const float IMPACT_THRESHOLD = 2.5f;
const unsigned long TELEMETRY_INTERVAL = 500;

// ---------------- MPU6500 raw read ----------------
int16_t readMPU16(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;

  Wire.requestFrom(MPU_ADDR, 2);
  if (Wire.available() < 2) return 0;

  return ((int16_t)Wire.read() << 8) | Wire.read();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("TREKKER STARTING...");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // I2C
  Wire.begin(SDA_PIN, SCL_PIN);

  // MPU6500
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // PWR_MGMT_1
  Wire.write(0x00); // wake
  if (Wire.endTransmission() != 0) {
    Serial.println("MPU6500 FAILED");
    while (1) delay(10);
  }
  Serial.println("MPU6500 OK");

  // BMP280
  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 FAILED");
    while (1) delay(10);
  }
  Serial.println("BMP280 OK");

  // MAX3010x
  if (!maxSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX3010x FAILED");
    while (1) delay(10);
  }
  maxSensor.setup(60, 4, 2, 100, 411, 4096);
  Serial.println("MAX3010x OK");

  // GPS: module confirmed at 9600 baud, TX connected to GPIO32
  GPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, -1);
  Serial.println("GPS UART OK");

  // LoRa
  SPI.begin(18, 19, 23, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa FAILED");
    while (1) delay(10);
  }

  LoRa.enableCrc();
  Serial.println("LoRa OK");
  Serial.println("SYSTEM READY");
}

void loop() {
  // Continuously feed GPS parser
  while (GPS.available()) {
    gps.encode(GPS.read());
  }

  // Reset latched fall state
  if (digitalRead(BUTTON_PIN) == LOW) {
    fallDetected = false;
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("FALL RESET");
    delay(200); // simple debounce
  }

  // MPU6500
  float ax = readMPU16(0x3B) / 16384.0f;
  float ay = readMPU16(0x3D) / 16384.0f;
  float az = readMPU16(0x3F) / 16384.0f;

  float gx = readMPU16(0x43) / 131.0f;
  float gy = readMPU16(0x45) / 131.0f;
  float gz = readMPU16(0x47) / 131.0f;

  float accelMagnitude = sqrt(ax * ax + ay * ay + az * az);

  // Simple impact-based fall latch for prototype/demo
  if (!fallDetected && accelMagnitude > IMPACT_THRESHOLD) {
    fallDetected = true;
    digitalWrite(BUZZER_PIN, HIGH);
    Serial.println("!!! FALL / IMPACT DETECTED !!!");
  }

  // BMP280
  float temperature = bmp.readTemperature();
  float pressure = bmp.readPressure() / 100.0f;
  float altitude = bmp.readAltitude(1013.25f);

  // MAX3010x heart-rate estimate
  long irValue = maxSensor.getIR();
  long redValue = maxSensor.getRed();
  bool fingerDetected = irValue > 50000;

  if (fingerDetected && checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    if (delta > 0) {
      float currentBPM = 60.0f / (delta / 1000.0f);
      if (currentBPM > 35.0f && currentBPM < 220.0f) {
        heartRate = (heartRate == 0.0f)
          ? currentBPM
          : (heartRate * 0.7f + currentBPM * 0.3f);
      }
    }
  }

  if (!fingerDetected) heartRate = 0.0f;

  // Send telemetry twice per second
  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL) {
    lastTelemetry = millis();

    bool gpsFix = gps.location.isValid() && gps.location.age() < 3000;
    double latitude = gpsFix ? gps.location.lat() : 0.0;
    double longitude = gpsFix ? gps.location.lng() : 0.0;
    int satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;

    String packet;
    packet.reserve(240);

    packet += "AX=" + String(ax, 2);
    packet += ",AY=" + String(ay, 2);
    packet += ",AZ=" + String(az, 2);
    packet += ",AM=" + String(accelMagnitude, 2);
    packet += ",GX=" + String(gx, 1);
    packet += ",GY=" + String(gy, 1);
    packet += ",GZ=" + String(gz, 1);
    packet += ",T=" + String(temperature, 1);
    packet += ",P=" + String(pressure, 1);
    packet += ",ALT=" + String(altitude, 1);
    packet += ",IR=" + String(irValue);
    packet += ",RED=" + String(redValue);
    packet += ",HR=" + String(heartRate, 0);
    packet += ",F=" + String(fallDetected ? 1 : 0);
    packet += ",GPS=" + String(gpsFix ? 1 : 0);
    packet += ",LAT=" + String(latitude, 6);
    packet += ",LON=" + String(longitude, 6);
    packet += ",SAT=" + String(satellites);

    LoRa.beginPacket();
    LoRa.print(packet);
    LoRa.endPacket();

    Serial.println();
    Serial.println("===== TREKKER =====");
    Serial.println(packet);
  }
}
