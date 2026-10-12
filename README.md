# Trekker Telemetry Mini Project

ESP32 + 433 MHz LoRa trekker telemetry system.

## Hardware

### Trekker
- ESP32
- Ai-Thinker RA-02 / SX1278 433 MHz LoRa
- MPU6500 (`0x68`)
- BMP280 (`0x76`)
- MAX3010x (`0x57`)
- GPS UART at 9600 baud
- Buzzer on GPIO25
- Active-low reset button on GPIO35 (external pull-up required)

### Base
- ESP32-WROOM-32D
- Ai-Thinker RA-02 / SX1278 433 MHz LoRa
- BLE link to the browser dashboard

## Firmware
- `firmware/trekker/trekker.ino`
- `firmware/base/base.ino`

## Dashboard
The root `index.html`, `styles.css`, and `app.js` form a static GitHub Pages dashboard. It connects to the base ESP32 with Web Bluetooth and displays live LoRa telemetry.

> Web Bluetooth works best in Chrome/Edge on desktop and Chrome on Android. iOS Safari does not currently provide standard Web Bluetooth support.
