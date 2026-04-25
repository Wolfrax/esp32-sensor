# ESP32 Sensor Web Server – Code Description

## Overview

This firmware runs on an **ESP32-C6 DevKitC-1** and provides:

- Wi-Fi station or fallback access point mode
- Time synchronization via SNTP
- Periodic **BME280** sensor sampling
- Persistent CSV logging in flash
- A small HTTP web server
- A browser dashboard for live and historical data
- A debug page for node info, logs, files, and Wi-Fi setup
- **Async SSE (Server-Sent Events)** for multiple simultaneous live clients

The design separates:

- **sampling and storage**
- **web serving**
- **live event broadcasting**

so that multiple web clients can receive live updates without blocking the HTTP server.

---

## Main Responsibilities

### 1. Wi-Fi handling
The firmware tries to load a previously saved SSID/password from NVS.

- If credentials exist, it starts in **STA mode**
- If no credentials exist, it starts in **AP mode** with a setup SSID

This allows the device to either join an existing Wi-Fi network or provide its own configuration hotspot.
When no credentials exists, it will create the WiFi SSID "ESP32-Setup" with password "12345678".
The ESP32 device will have IP-address 192.168.4.1
Connect to that WiFi and browse to "http://192.1684.1/debug.html", then using that page apply
crendtials in the WiFi Setup box, do Save WiFi and reboot. The ESP32 should now connect to the wanted WiFi.

### 2. Time synchronization
The device uses **SNTP** to obtain current time and sets the Sweden time zone.

This is used for:

- readable startup time
- timestamps in CSV log files
- timestamps in live SSE updates

### 3. Sensor sampling
A background task periodically reads the BME280 values:

- temperature
- humidity
- pressure

Each sample is:

- timestamped
- appended to a daily CSV file
- published as the latest live sample for SSE clients

### 4. File storage
Data is stored in flash using FATFS under:

- `/storage/data` for sensor CSV files
- `/storage/www` for web pages such as `index.html` and `debug.html`

Old data files older than 7 days are automatically deleted.

### 5. HTTP server
The HTTP server serves:

- dashboard HTML
- debug HTML
- historical data as JSON
- node metadata as JSON
- file listing and file download
- logs
- Wi-Fi setup/reset APIs
- live SSE stream

### 6. Async SSE broadcasting
Live sensor events are distributed to multiple browser clients using:

- **async HTTP request handling**
- a list of connected SSE clients
- a background broadcaster task
- queued work executed in HTTPD context

This avoids the blocking behavior of a traditional long-running synchronous handler.

---

## Core Data Types

### `sample_t`
Represents one sensor sample:

- `ts` – timestamp
- `t` – temperature
- `h` – humidity
- `p` – pressure

### `sse_client_t`
Represents one connected live SSE client:

- `active` – whether the slot is in use
- `sockfd` – socket descriptor
- `req_async` – async HTTP request handle
- `last_sent_seq` – last sample sequence sent

### `sse_work_t`
Work item queued to HTTP server:

- target client
- keepalive or data
- sample payload
- sequence number

---

## Live Data Flow

1. Sensor sampled in background task
2. Stored to CSV
3. Latest sample updated
4. Broadcast task detects new data
5. Work queued to HTTP server
6. SSE sent to clients
7. Browser updates UI

---

## Summary

This firmware provides a complete:

- sensor logger
- web dashboard
- debug interface

with a robust async SSE architecture suitable for multiple clients.
