# Air-Regulated-IoT-Embedded-System-ARIES-

An ESP32-based smart hostel/home room prototype that automates lighting and ventilation based on real occupancy and environmental conditions, while measuring genuine electrical power draw and modeling it through Malaysia's actual TNB RP4 residential electricity tariff — including live tariff-tier-aware automation, a perpetual meter register, and a resident-facing energy awareness display.

Built for Engineering Team Project (ETP) at Universiti Teknologi PETRONAS (UTP).

<img width="1280" height="960" alt="photo_2026-09-27_19-59-19" src="https://github.com/user-attachments/assets/afee4497-8d59-49ff-b092-ca24539aab6e" />


---

## Features

- **Dual-sensor occupancy fusion** — PIR motion sensor combined with an LD2410 mmWave radar, so stationary occupants (not just moving ones) are correctly detected — a common failure point of PIR-only systems.
- **Real measured power, not assumed wattage** — an INA219 current/voltage sensor measures genuine electrical draw, scaled to simulate a household-level load for demonstration purposes.
- **TNB RP4 tariff engine (post-July 2025 structure)** — models unbundled energy/capacity/network charges, the Energy Efficiency Incentive (EEI) rebate bracket table, retail charge, AFA, the KWTBB renewable energy fund levy, and SST — plus a parallel Time-of-Use (ToU) bill calculation for comparison.
- **Tariff-tier-aware automation** — as this billing cycle's usage crosses real TNB pricing thresholds (200 / 300 / 600 kWh), the automation becomes progressively stricter — the occupancy hold time halves per tier and the fan's temperature threshold rises — since each additional kWh gets more expensive past those points.
- **Perpetual meter register** — the lifetime energy meter never resets, mirroring how a real utility meter works; billing is calculated from the difference between two readings across a billing cycle, not from a naive running total.
- **Temperature-differential fan control** — dual DHT11 sensors (indoor + outdoor) drive the fan only when outdoor air is meaningfully cooler than indoor, with a hysteresis threshold to prevent rapid on/off flapping on marginal readings.
- **3-state manual override** — every automated load can be forced `ON`, forced `OFF` or left in `AUTOMATION`, using a quick-tap vs. hold-down gesture on a single physical switch.
- **Energy-saving counter** — tracks today's actual consumption against a continuous-run baseline to show real avoided energy (Wh) and cost (RM).
- **In-room OLED awareness display** — a live dashboard physically inside the room, addressing the occupant-awareness gap identified in hostel energy audit literature
- **Local WiFi dashboard** — the ESP32 hosts its own Access Point and a read-only live web dashboard
- **NVS persistence** — the meter reading and billing cycle state survive power loss and reboot.
- **Real supply-state sensing + demo-triggered outage simulation** — actual 5V-rail voltage is sensed to distinguish backup-battery-only operation from normal supply (so reported LED/fan state always reflects reality); a separate OUTAGE/RESTORE command, sent over the same USB link, lets the dashboard stage a load-shedding demonstration on demand — the fan (high-draw channel) is shed while the battery-backed LED stays on essential automation.
**USB serial telemetry** — the ESP32 streams a compact JSON line of live sensor, automation, and billing state over USB once per second, consumed by a companion dashboard application built by a teammate

---

## System Architecture

```
  [PIR] [LD2410 radar] [DHT11 x2] [INA219]
        │       │          │         │
        └───────┴────┬─────┴─────────┘
                      │
                  [ESP32]
         (occupancy + billing +
          tariff-tier automation)
                      │
        ┌─────────────┼─────────────┐
        │              │             │
  [OLED Display]  [Relay Module]  [USB Serial — JSON
   (in-room)       → LED / Fan     telemetry, 1/sec] →
                                    [Teammate's Dashboard App]
```



---

## Hardware Used

| Component | Approx. Cost (RM) |
|---|---|
| ESP32 microcontroller | 35 |
| PIR motion sensor | included in sensor suite |
| LD2410 mmWave radar sensor | included in sensor suite |
| DHT11 temperature/humidity sensors (x2) | included in sensor suite |
| Sensor suite subtotal | 70 |
| Relay control module (2-channel used) | 15 |
| DC cooling fan | 20 |
| INA219 current/voltage sensor | 10 |
| SSD1306 OLED display | 20 |
| Buck converter + DC jack adapter | 20 |
| Acrylic enclosure/casing | 60 |
| Power adapters & wiring | 40 |
| Breadboard + jumper wires | 10 |
| **Total prototype BOM** | **~RM 300–400** |

---

## Tech Stack

- **Firmware:** Arduino(software) used to program the ESP32 with a C Programming Language code
- **Key libraries:** [`ld2410`](https://github.com/ncmreynolds/ld2410), Adafruit `INA219`, Adafruit `SSD1306` / `GFX`, Adafruit `DHT`, `Preferences` (NVS), `WiFi.h` / `WebServer.h`
**Telemetry:** structured JSON streamed over USB serial (115200 baud), one line per second
**Companion dashboard app:** built and maintained by a teammate; parses the USB serial telemetry to render a live dashboard

---

## Design Highlights (why this differs from a typical student energy-monitoring project)

- Power figures come from a live current sensor, not a hardcoded wattage assumption.
- The billing model isn't a toy counter — it implements the unbundled post-July-2025 RP4 structure (energy + capacity + network charges, the EEI rebate bracket table, KWTBB, AFA, SST) closely enough to survive scrutiny from someone who has actually read a current TNB bill, and includes a parallel Time-of-Use comparison.
- The meter/billing-cycle separation specifically mirrors real utility metering practice (delta-based billing, non-resetting register) rather than the common "just reset everything monthly" shortcut.
- Automation conservation level responds to actual tariff economics (real EEI/KWTBB/SST thresholds), not just a fixed timer.
- Reported device state is gated on real sensed supply voltage, not just relay command state, so the telemetry never claims a load is running when it has no power.
---

## Repository Structure

```
├── firmware/              ESP32 Arduino sketch (main automation + billing logic)
├── docs/                   Circuit diagrams, architecture diagrams, demo photos
├── report/                 Final project report (optional)
└── media/                  Demo GIF/video
```

---

## Setup & Usage

1. Install the Arduino IDE and the ESP32 board package.
2. Install required libraries via Library Manager: `ld2410`, `Adafruit INA219`, `Adafruit SSD1306`, `Adafruit GFX`, `Adafruit Unified Sensor`, `DHT sensor library`.
3. Open `firmware/smart_room_main/smart_room_main.ino`.
4. Update pin definitions if your wiring differs (see pinout table below).
5. Flash to an ESP32 dev board then keep it connected over USB.
6. Open Serial Monitor at 115200 baud to see human-readable status output, or use the companion dashboard app to parse the JSON telemetry line streamed once per second and render it live.
7. To stage a demo outage: send the text command OUTAGE over the same serial connection to shed the fan while the LED stays on essential automation; send RESTORE to return to normal.
### Pin Reference

| Function | GPIO |
|---|---|
| PIR sensor | 27 |
| LD2410 radar (RX/TX) | 16 / 17 |
| DHT11 outdoor / indoor | 4 / 5 |
| Relay 1 (LED) / Relay 2 (Fan) | 25 / 26 |
| Manual switches 1 / 2 | 32 / 33 |
| I2C (OLED + INA219) | 21 / 22 |

---

## Known Limitations & Future Work

- Outage load-shedding is currently triggered by an explicit OUTAGE/RESTORE serial command from the dashboard for reliable live demonstration, rather than autonomous mains-side current sensing (e.g. an ACS712 ahead of the buck converter) — a natural next step for a fully autonomous version.
- Demonstration uses accelerated simulated time and a power scale factor (prototype watts × 90, tuned to represent a believable single-room share of load) to make effects visible in minutes rather than a real billing month — displayed figures represent the mechanism, not literal live household consumption.
- Current build is breadboard-based; a soldered/PCB revision would remove the intermittent connection issues encountered during development.
- Planned but not yet implemented: per-branch current sensing for true (not attribution-based) per-appliance monitoring, multi-room ESP-NOW mesh scaling, and a maintenance-safety digital interlock.

See the full report in /report for the complete Sustainability & Future Recommendations discussion.

**Team & Contributions**

This was a team project. To keep contributions clear:

Elektro19 — Levinesh: Firmware and system integration — occupancy fusion logic, TNB billing engine, tariff-tier-aware automation, sensor/relay/power hardware integration and debugging, OLED and local web dashboard, Sustainability & Future Recommendations report section.
Natalie: Designed and built a companion dashboard application that connects to the ESP32 over USB serial to visualize live telemetry. This component is maintained separately and is not included in this repository.

**License**

This project is licensed under the MIT License — see LICENSE for details.
