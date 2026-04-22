

# ESP32 IoT Sensor Node (Temperature, Humidity, Pressure Logging System)



This project describes a **repeatable, scalable IoT sensor node design** based on ESP32, using ESP-IDF on Linux (Mint + VS Code).  

It is optimized for **reliability, low cost, and easy replication**.



---



# 📦 System Overview



Each device:

- Reads environmental data (temperature, humidity, pressure)

- Stores data in a **RAM ring buffer (~10,800 samples)**

- Periodically flushes data to **flash storage**

- Exposes data via a **simple HTTP server (WiFi)**

- Uses **NTP (internet time)** for timestamps

- Powered via **USB (power bank or adapter)**



---



# 🧠 Recommended Hardware



## 🔌 Microcontroller (choose one)



### Recommended (best balance)

- ESP32 DevKitC V4 (ESP32-WROOM-32)



### Cost-optimized for replication

- DOIT ESP32 DevKit V1



Both are fully supported in ESP-IDF and stable for production-like deployments.



---



## 🌡️ Sensor



### Recommended sensor module

- BME280 (I²C temperature, humidity, pressure sensor)



Key features:

- Single chip for all measurements

- Widely supported in ESP-IDF

- Stable and easy to wire



⚠️ Ensure it is **BME280, not BMP280 (missing humidity)**



---



## 📦 Enclosure



- ABS project box (~100×60×25 mm recommended)

- Must include ventilation holes near sensor

- USB-C cable passthrough



---



## 🔋 Power



- USB power bank (USB-C preferred)

- ESP32 powered directly via onboard regulator



Notes:

- Ensure stable cable and power bank (avoid auto-shutdown models)



---



# 🔌 Wiring Standard (IMPORTANT: keep identical across all units)



## ESP32 GPIO mapping



| Function | GPIO | Color (recommended) |

|----------|------|---------------------|

| 3.3V     | 3V3  | Red |

| GND      | GND  | Black |

| SDA      | GPIO21 | Green |

| SCL      | GPIO22 | Yellow |



---



## BME280 wiring (I²C)



| BME280 Pin | ESP32 |

|------------|------|

| VCC        | 3.3V |

| GND        | GND |

| SDA        | GPIO21 |

| SCL        | GPIO22 |



---



## Physical layout recommendation

[ USB power cable ]

|

[ ESP32 board ]

|

(short I²C wires)

|

[ BME280 sensor near ventilation holes ]

---



# 🏠 Enclosure Design Guidelines



## Must-have rules:

- Sensor must have **direct airflow exposure**

- Do NOT fully seal sensor inside enclosure

- Keep sensor away from heat sources (ESP32 regulator)



## Recommended setup:

- Sensor mounted near enclosure edge

- Ventilation holes above sensor area

- ESP32 mounted centrally



---



# 💾 Flash Storage Strategy



## Goals:

- Avoid flash wear-out

- Ensure data persistence

- Support long-term logging



---



## Architecture



### 1. RAM ring buffer

- Stores ~10,800 samples

- Fast acquisition layer



---



### 2. Flash write strategy



- Flush RAM → flash in **blocks (e.g. 300–1000 samples)**

- Avoid per-sample writes



---



### 3. Storage system



Recommended:

- LittleFS (preferred for simplicity + wear leveling)



Alternative:

- Raw flash circular logging (advanced control)



---



## Data format optimization



To reduce flash usage:

- Store scaled integers instead of floats  

  (e.g. 23.45°C → 2345)



Benefits:

- Less storage

- Faster writes

- Lower flash wear



---



## Logging model



Circular buffer in flash:

Sector 1 → Sector 2 → Sector 3 → … → wrap around

Each block contains:

- Timestamp

- Sample count

- Sensor data array



---



# 🌐 Networking



## WiFi

- ESP32 built-in WiFi

- Used for HTTP + NTP



## Time synchronization

- SNTP (Simple Network Time Protocol)

- Required for timestamping all measurements



---



## HTTP Server



Provides access to:

- Current buffer (RAM)

- Historical data (flash)



Recommended:

- ESP-IDF `esp_http_server`

- JSON or chunked responses for large datasets



---



# 🧪 Replication Guidelines



To ensure consistent device behavior:



## Hardware consistency

- Use same ESP32 board model across all units

- Use identical BME280 module type

- Standardize wiring colors and GPIO mapping



## Firmware consistency

- Same ESP-IDF version

- Same partition table

- Same sensor driver version



## Assembly consistency

- Same enclosure model

- Same sensor placement rules

- Same cable lengths (recommended <20 cm for I²C)



---



# ⚙️ ESP-IDF Development Setup



- Linux Mint (recommended)

- VS Code with Espressif extension

- ESP-IDF v5.x or newer



---



# 🚀 Summary Architecture

[BME280 Sensor]

↓ (I²C)

[ESP32]

├── RAM Ring Buffer

├── Flash Logging (LittleFS / circular)

├── SNTP Time Sync

└── HTTP Server (WiFi)

↓

Client Access

---



# ✅ Design Priorities



1. Stability over minimal cost

2. Replication consistency

3. Flash longevity

4. Simple wiring and assembly

5. Easy debugging and maintenance



---



# 📌 Notes



- This system is optimized for **multi-device deployment**

- Avoid overly cheap components for scaling reliability

- Keep wiring and firmware identical across all units









