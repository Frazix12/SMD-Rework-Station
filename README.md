<div align="center">

# 🔧 SMD Rework Station

**Arduino-powered hot air rework station with PID control, desktop app & web UI**

[![Firmware](https://img.shields.io/badge/Firmware-v3-blueviolet?style=for-the-badge&logo=arduino)](SMD.ino)
[![Protocol](https://img.shields.io/badge/Serial%20Protocol-v2%20%40%20115200-blue?style=for-the-badge)](SMD.ino)
[![Platform](https://img.shields.io/badge/Platform-Arduino%20Nano%20%2F%20Uno-00979D?style=for-the-badge&logo=arduino&logoColor=white)](https://arduino.cc)
[![License](https://img.shields.io/badge/License-MIT-green?style=for-the-badge)](LICENSE)

[![App](https://img.shields.io/badge/Desktop%20App-Download-orange?style=for-the-badge&logo=github)](https://github.com/Frazix12/SMD-Rework-Station/releases)
[![WebApp](https://img.shields.io/badge/Web%20App-Live-success?style=for-the-badge&logo=vercel)](https://smd-station.vercel.app/)

</div>

---

## ✨ Features

| | Feature |
|---|---|
| 🌡️ | PID heater control via SSR output |
| 💨 | PWM fan control with calibrated airflow table |
| 🔌 | MAX6675 thermocouple input |
| 📟 | 16×2 I2C LCD with big temperature digits |
| 🔘 | 3-button interface — temp, airflow & calibration |
| 😴 | Sleep mode with fan cooldown protection |
| 🔔 | Buzzer feedback for events & transitions |
| 💾 | EEPROM persistence for all settings |
| 🖥️ | Serial protocol v2 for desktop/web companion app |

---

## ⚡ Quick Start

> Requires [`arduino-cli`](https://arduino.github.io/arduino-cli/) on Linux.

```bash
# 1. Set your board (once)
./flash.sh --set-board

# 2. Compile & upload
./flash.sh
```

**Other options:**

```bash
./flash.sh --compile-only          # just compile
./flash.sh --upload-only           # just upload
./flash.sh -p /dev/ttyUSB0         # specify port
./flash.sh -b                      # open serial monitor
```

Board/port config is saved at `~/.config/smd-flash/config`.

---

## 🗺️ Hardware Pin Map

| Function | Pin | Notes |
|---|---|---|
| MAX6675 SO | `D12` | Thermocouple data |
| MAX6675 CS | `D10` | Chip select |
| MAX6675 SCK | `D13` | Clock |
| Heater SSR | `D9` | PID output |
| Fan PWM | `D3` | Fan drive |
| Buzzer | `D2` | Audio feedback |
| Sleep Input | `D4` | Active LOW |
| Up Button | `D5` | Active LOW + pull-up |
| OK Button | `D6` | Active LOW + pull-up |
| Down Button | `D7` | Active LOW + pull-up |
| LCD I2C | `A4` / `A5` | SDA / SCL |

---

## 🎮 Controls

- **UP / DOWN** — adjust temperature setpoint
- **OK** — toggle between temperature & fan adjustment
- **Hold UP + DOWN** for ~2s — enter calibration mode
- **Sleep pin LOW** — stop heating, run fan cooldown

---

## 🔌 Serial Protocol v2

The firmware speaks machine-readable packets and plain-text commands at `115200` baud.

**Packet types:** `@BOOT` · `@STATE` · `@EVENT` · `@ACK` · `@ERR`

| Command | What it does |
|---|---|
| `SET <val>` | Target temperature |
| `FAN <val>` | Fan display % |
| `FANMIN <val>` | Actual minimum fan % |
| `OFFSET <val>` | Thermocouple offset |
| `KP / KI / KD <val>` | PID tuning |
| `HZ <val>` | PID update rate |
| `CALEN / CALDIS` | Airflow calibration on/off |
| `CALROW <f> <t> <v>` | Set one calibration table cell |
| `STATUS / INFO / HELP` | Diagnostics |

---

## 🌬️ Airflow Calibration

Use the [Desktop App](https://github.com/Frazix12/SMD-Rework-Station/releases) or [Web App](https://smd-station.vercel.app/) for easy calibration.

<details>
<summary>Manual calibration points</summary>

**Fan levels:** `30%` · `60%` · `90%`

**Temperature points:** `100°C` through `500°C` in 50° steps

The current calibration data is stored in `data.txt`. Update the arrays in `SMD.ino` or use serial `CALROW` commands for live tuning.

</details>

---

## 📁 Project Files

| File | Purpose |
|---|---|
| `SMD.ino` | Main full-featured firmware |
| `SMD_basic/SMD_basic.ino` | Simpler fallback sketch |
| `flash.sh` | Linux compile/upload helper |
| `data.txt` | Airflow calibration reference |

---

## ⚙️ Runtime Limits

| Setting | Value |
|---|---|
| Temp range | `100°C – 500°C` |
| Fan range | `10% – 100%` |
| Default fan minimum | `30%` |
| Telemetry rate | `4 Hz` |

---

<div align="center">

Made with ❤️ and a soldering iron

</div>
