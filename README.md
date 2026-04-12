# SMD Rework Station

## Finished Project Image
<p align="center"><em>Add the final assembled project photo here.</em></p>
<p align="center"><br><br><br><br></p>

## Overview
This repository contains the firmware for an Arduino-based SMD hot air rework
station. The main sketch, `SMD.ino`, handles heater control, fan control,
thermocouple feedback, LCD output, button input, buzzer feedback, sleep mode,
EEPROM persistence, and the serial connection used by the desktop companion
app.

Use `SMD.ino` for the current full-featured build. The
`SMD_basic/SMD_basic.ino` sketch is a simpler fallback version.

## Desktop App Screenshots
<table>
  <tr>
    <td align="center"><strong>Main Control View</strong><br><br><br><br></td>
    <td align="center"><strong>Live Telemetry / Tuning</strong><br><br><br><br></td>
  </tr>
  <tr>
    <td align="center"><strong>Calibration Screen</strong><br><br><br><br></td>
    <td align="center"><strong>Extra App Screenshot</strong><br><br><br><br></td>
  </tr>
</table>

## Current Features
- PID-based heater control through an SSR output.
- PWM fan control with separate display percentage and calibrated actual
  minimum airflow.
- MAX6675 thermocouple input with raw, sensor, corrected, and display
  temperature handling.
- 16x2 I2C LCD output with large temperature digits for runtime display.
- Three-button interface for temperature, airflow, and calibration control.
- Sleep input that shuts down heating and keeps the fan running until cooldown
  completes.
- Buzzer feedback for button presses, sleep transitions, and target reached.
- EEPROM storage for setpoint, fan level, sensor offset, and fan minimum.
- Serial protocol v2 at `115200` baud for desktop app control and telemetry.
- Built-in airflow calibration table plus serial commands for live tuning.

## Default Runtime Limits
| Setting | Value |
| --- | --- |
| Firmware version | `3` |
| Serial protocol | `2` |
| Temperature setpoint range | `100 C` to `500 C` |
| Fan command range | `10%` to `100%` |
| Default actual fan minimum | `30%` |
| State telemetry rate | `4 Hz` |

## Hardware Pin Map
| Function | Pin | Notes |
| --- | --- | --- |
| MAX6675 SO | `D12` | Thermocouple data output |
| MAX6675 CS | `D10` | Thermocouple chip select |
| MAX6675 SCK | `D13` | Thermocouple clock |
| Heater SSR | `D9` | PID-driven heater output |
| Fan PWM | `D3` | PWM fan drive |
| Buzzer | `D2` | Audible feedback |
| Sleep input | `D4` | Active LOW sleep switch/input |
| Up button | `D5` | Active LOW with pull-up |
| OK button | `D6` | Active LOW with pull-up |
| Down button | `D7` | Active LOW with pull-up |
| LCD I2C | `A4` / `A5` | SDA / SCL on Arduino Uno/Nano |

## Controls
- `UP` / `DOWN` in normal mode change the temperature setpoint in steps.
- Press `OK` to switch between temperature adjustment and airflow adjustment.
- Hold buttons to repeat adjustments faster.
- Hold `UP` + `DOWN` for about `2s` to enter calibration mode.
- Pull the sleep input LOW to stop heating and run cooldown protection.

## Serial Protocol v2
The firmware reports machine-readable packets for a desktop companion app and
also accepts plain-text commands over serial.

### Packet types
- `@BOOT`
- `@STATE`
- `@EVENT`
- `@ACK`
- `@ERR`

### Supported commands
| Command | Purpose |
| --- | --- |
| `SET <value>` | Set target temperature |
| `FAN <value>` | Set display fan percentage |
| `FANMIN <value>` | Set actual minimum fan percentage |
| `OFFSET <value>` | Set thermocouple offset |
| `KP <value>` | Update PID `Kp` |
| `KI <value>` | Update PID `Ki` |
| `KD <value>` | Update PID `Kd` |
| `HZ <value>` | Update PID update rate |
| `CALEN` | Enable calibration |
| `CALDIS` | Disable calibration |
| `CALROW <fanIdx> <tempIdx> <value>` | Update one airflow table cell |
| `STATUS` | Print one current state packet |
| `INFO` | Print one boot/info packet |
| `HELP` | Print command help |

## Airflow Calibration
The main sketch includes a built-in lookup table for converting sensor-domain
temperature into a more realistic output temperature at different airflow
levels.

Current default fan calibration points:
- `30%`
- `60%`
- `90%`

Current default temperature points:
- `100 C`
- `150 C`
- `200 C`
- `250 C`
- `300 C`
- `350 C`
- `400 C`
- `450 C`
- `500 C`

The current calibration dataset is also reflected in `data.txt`. If your
hardware behaves differently, update the arrays in `SMD.ino` or tune values
through the serial calibration commands.

## Build and Flash
The firmware is mostly self-contained. It includes in-sketch replacements for
the PID controller, MAX6675 interface, and I2C LCD support, so you mainly need
the normal Arduino core environment.

### Quick start with `flash.sh`
1. Install `arduino-cli`.
2. Select your target board once:
   `./flash.sh --set-board`
3. Compile and upload:
   `./flash.sh`

Useful options:
- `./flash.sh --compile-only`
- `./flash.sh --upload-only`
- `./flash.sh -p /dev/ttyUSB0`
- `./flash.sh -b`

The flash script stores the selected board and last used port in:
- `~/.config/smd-flash/config`

## Project Files
| Path | Purpose |
| --- | --- |
| `SMD.ino` | Main full-featured firmware |
| `SMD_basic/SMD_basic.ino` | Simpler alternate firmware |
| `flash.sh` | Compile/upload helper script |
| `data.txt` | Airflow calibration reference data |

## Component List
| Component | Quantity | Notes |
| --- | --- | --- |
|  |  |  |
|  |  |  |
|  |  |  |
|  |  |  |
|  |  |  |

## Schematic Image
<p align="center"><em>Add the wiring / schematic image here.</em></p>
<p align="center"><br><br><br><br></p>
