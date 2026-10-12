#include <SPI.h>
#include <LoRa.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define LORA_SS    5
#define LORA_RST   14
#define LORA_DIO0  26

// Must match app.js
#define BLE_SERVICE_UUID        "7d7a0001-4b52-4b52-8a6e-4d494e495052"
#define BLE_TELEMETRY_UUID      "7d7a0002-4b52-4b52-8a6e-4d494e495052"

BLECharacteristic *telemetryCharacteristic;
bool bleClientConnected = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleClientConnected = true;
    Serial.println("BLE client connected");
  }

  void onDisconnect(BLEServer *server) override {
    bleClientConnected = false;
    Serial.println("BLE client disconnected");
    BLEDevice::startAdvertising();
  }
};

String getValue(const String &data, const String &key) {
  int start = data.indexOf(key);
  if (start < 0) return "--";

  start += key.length();
  int end = data.indexOf(',', start);
  if (end < 0) end = data.length();

  return data.substring(start, end);
}

void sendBleLine(const String &line) {
  if (!bleClientConnected) return;

  // Small chunks make this robust even with the default BLE MTU.
  // Browser app reassembles chunks until it sees '\n'.
  const size_t CHUNK_SIZE = 18;
  String framed = line + "\n";

  for (size_t i = 0; i < framed.length(); i += CHUNK_SIZE) {
    String chunk = framed.substring(i, min(i + CHUNK_SIZE, framed.length()));
    telemetryCharacteristic->setValue((uint8_t *)chunk.c_str(), chunk.length());
    telemetryCharacteristic->notify();
    delay(8);
  }
}

void printTelemetry(const String &packet, int rssi, float snr) {
  String ax   = getValue(packet, "AX=");
  String ay   = getValue(packet, "AY=");
  String az   = getValue(packet, "AZ=");
  String am   = getValue(packet, "AM=");
  String gx   = getValue(packet, "GX=");
  String gy   = getValue(packet, "GY=");
  String gz   = getValue(packet, "GZ=");
  String temp = getValue(packet, "T=");
  String pres = getValue(packet, "P=");
  String alt  = getValue(packet, "ALT=");
  String hr   = getValue(packet, "HR=");
  String fall = getValue(packet, "F=");
  String gps  = getValue(packet, "GPS=");
  String lat  = getValue(packet, "LAT=");
  String lon  = getValue(packet, "LON=");
  String sat  = getValue(packet, "SAT=");

  Serial.println();
  Serial.println("========================================");
  Serial.println("           TREKKER TELEMETRY");
  Serial.println("========================================");

  Serial.print("Accel      : X "); Serial.print(ax);
  Serial.print("  Y "); Serial.print(ay);
  Serial.print("  Z "); Serial.print(az); Serial.println(" g");

  Serial.print("Magnitude  : "); Serial.print(am); Serial.println(" g");
  Serial.print("Gyro       : X "); Serial.print(gx);
  Serial.print("  Y "); Serial.print(gy);
  Serial.print("  Z "); Serial.println(gz);

  Serial.print("Temperature: "); Serial.print(temp); Serial.println(" C");
  Serial.print("Pressure   : "); Serial.print(pres); Serial.println(" hPa");
  Serial.print("Altitude   : "); Serial.print(alt); Serial.println(" m");
  Serial.print("Heart Rate : "); Serial.print(hr); Serial.println(" BPM");

  if (gps == "1") {
    Serial.print("GPS        : "); Serial.print(lat); Serial.print(", "); Serial.println(lon);
    Serial.print("Satellites : "); Serial.println(sat);
  } else {
    Serial.println("GPS        : NO FIX");
  }

  Serial.print("Fall       : ");
  Serial.println(fall == "1" ? "*** FALL DETECTED ***" : "NORMAL");

  Serial.print("LoRa       : RSSI "); Serial.print(rssi);
  Serial.print(" dBm, SNR "); Serial.print(snr, 1); Serial.println(" dB");
  Serial.println("========================================");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // LoRa
  SPI.begin(18, 19, 23, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa FAILED");
    while (1) delay(10);
  }

  LoRa.enableCrc();
  Serial.println("LoRa OK");

  // BLE
  BLEDevice::init("Trekker-Base");

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(BLE_SERVICE_UUID);

  telemetryCharacteristic = service->createCharacteristic(
    BLE_TELEMETRY_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );

  telemetryCharacteristic->addDescriptor(new BLE2902());
  telemetryCharacteristic->setValue("BASE_READY\n");
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.println("BLE advertising as Trekker-Base");
  Serial.println("BASE READY");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String packet;
  packet.reserve(260);

  while (LoRa.available()) {
    packet += (char)LoRa.read();
  }

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  printTelemetry(packet, rssi, snr);

  // Add base-side radio quality before forwarding to the browser.
  String browserPacket = packet;
  browserPacket += ",RSSI=" + String(rssi);
  browserPacket += ",SNR=" + String(snr, 1);

  sendBleLine(browserPacket);
}
