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
unsigned long lastTelemetry = 0;

const float IMPACT_THRESHOLD = 2.5f;
const unsigned long TELEMETRY_INTERVAL = 500;

// ---------------- Heart-rate state ----------------
long lastBeat = 0;
float heartRate = 0.0f;
float avgBpm = 0.0f;

#define BPM_AVG_COUNT 5
float bpmBuffer[BPM_AVG_COUNT] = {0};
int bpmIndex = 0;
int bpmSamples = 0;

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

  // MAX3010x - tuned settings that worked in standalone BPM test
  if (!maxSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX3010x FAILED");
    while (1) delay(10);
  }

  maxSensor.setup(
    50,      // LED brightness
    4,       // averaging
    2,       // RED + IR
    400,     // sample rate
    411,     // pulse width
    4096     // ADC range
  );

  maxSensor.setPulseAmplitudeRed(50);
  maxSensor.setPulseAmplitudeIR(50);
  maxSensor.setPulseAmplitudeGreen(0);

  Serial.println("MAX3010x OK");

  // GPS: module confirmed at 9600 baud, TX connected to GPIO32
  GPS.setRxBufferSize(1024);
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
    delay(200);
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

  // MAX3010x tuned BPM detection
  long irValue = maxSensor.getIR();
  long redValue = maxSensor.getRed();
  bool fingerDetected = irValue >= 5000;

  if (!fingerDetected) {
    heartRate = 0.0f;
    avgBpm = 0.0f;
    lastBeat = 0;
    bpmIndex = 0;
    bpmSamples = 0;
  } else if (checkForBeat(irValue)) {
    long now = millis();

    if (lastBeat != 0) {
      long delta = now - lastBeat;

      // Accept roughly 40-200 BPM
      if (delta >= 300 && delta <= 1500) {
        float currentBpm = 60000.0f / delta;

        bpmBuffer[bpmIndex] = currentBpm;
        bpmIndex++;
        if (bpmIndex >= BPM_AVG_COUNT) bpmIndex = 0;

        if (bpmSamples < BPM_AVG_COUNT) bpmSamples++;

        float total = 0.0f;
        for (int i = 0; i < bpmSamples; i++) {
          total += bpmBuffer[i];
        }

        avgBpm = total / bpmSamples;
        heartRate = avgBpm;

        Serial.print("BEAT | BPM: ");
        Serial.print(currentBpm, 1);
        Serial.print(" | AVG: ");
        Serial.println(avgBpm, 1);
      }
    }

    lastBeat = now;
  }

  // Send telemetry twice per second
  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL) {
    lastTelemetry = millis();

    bool gpsFix = gps.location.isValid() && gps.location.age() < 3000;
    double latitude = gpsFix ? gps.location.lat() : 0.0;
    double longitude = gpsFix ? gps.location.lng() : 0.0;
    int satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;

    String packet;
    packet.reserve(260);

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
    packet += ",FINGER=" + String(fingerDetected ? 1 : 0);
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

    Serial.print("GPS chars: ");
    Serial.print(gps.charsProcessed());
    Serial.print(" | Satellites: ");
    Serial.print(satellites);
    Serial.print(" | GPS fix: ");
    Serial.println(gpsFix ? "YES" : "NO");
  }
}
