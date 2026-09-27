# Air-Regulated-IoT-Embedded-System-ARIES-

An ESP32-based smart hostel/home room prototype that automates lighting and ventilation based on real occupancy and environmental conditions, while measuring genuine electrical power draw and modeling it through Malaysia's actual TNB RP4 residential electricity tariff — including live tariff-tier-aware automation, a perpetual meter register, and a resident-facing energy awareness display.

Built for Engineering Team Project (ETP) at Universiti Teknologi PETRONAS (UTP).

<img width="1280" height="960" alt="photo_2026-09-27_19-59-19" src="https://github.com/user-attachments/assets/afee4497-8d59-49ff-b092-ca24539aab6e" />


---

## Features

- **Dual-sensor occupancy fusion** — PIR motion sensor combined with an LD2410 mmWave radar, so stationary occupants (not just moving ones) are correctly detected — a common failure point of PIR-only systems.
- **Real measured power, not assumed wattage** — an INA219 current/voltage sensor measures genuine electrical draw, scaled to simulate a household-level load for demonstration purposes.
- **TNB RP4 tariff engine** — models the real tiered rate structure, minimum charge, retail charge, Automatic Fuel Adjustment (AFA) surcharge, and 8% SST.
- **Tariff-tier-aware automation** — as simulated billing-cycle consumption crosses TNB's real tariff boundaries (600 kWh / 1500 kWh), the automation itself becomes stricter — shorter occupancy hold time, tighter temperature threshold — since each additional kWh costs more once a boundary is crossed.
- **Perpetual meter register** — the lifetime energy meter never resets, mirroring how a real utility meter works; billing is calculated from the difference between two readings across a billing cycle, not from a naive running total.
- **Temperature-differential fan control** — dual DHT11 sensors (indoor + outdoor) drive the fan only when outdoor air is meaningfully cooler than indoor, with a hysteresis threshold to prevent rapid on/off flapping on marginal readings.
- **3-state manual override** — every automated load can be forced `ON`, forced `OFF` or left in `AUTOMATION`, using a quick-tap vs. hold-down gesture on a single physical switch.
- **Energy-saving counter** — tracks today's actual consumption against a continuous-run baseline to show real avoided energy (Wh) and cost (RM).
- **In-room OLED awareness display** — a live dashboard physically inside the room, addressing the occupant-awareness gap identified in hostel energy audit literature
- **Local WiFi dashboard** — the ESP32 hosts its own Access Point and a read-only live web dashboard
- **NVS persistence** — the meter reading and billing cycle state survive power loss and reboot.
- **Outage-aware load shedding** — a battery-backed rail keeps the low-power/essential load (LED) running during a mains outage, while the high-draw load (fan) is automatically shed, detected via current sensing on the mains-side supply.
- **Companion USB-connected dashboard** — a separate local web dashboard application, developed by a teammate, that connects to the ESP32 over USB serial to visualize live telemetry. 

---

## System Architecture

```
 [PIR] [LD2410 radar] [DHT11 x2] [INA219]
        │       │          │         │
        └───────┴────┬─────┴─────────┘
                      │
                  [ESP32]
             (occupancy + billing +
              automation logic)
                      │
        ┌─────────────┼─────────────┬──────────────┐
        │              │             │              │
   [OLED Display] [Relay Module] [Local Web AP] [USB Serial →
    (in-room)      → LED / Fan     Dashboard      Teammate's
                                                    Dashboard App]
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
**Local dashboard:** self-hosted HTML/JS served directly from the ESP32 over its own WiFi Access Point
**Companion dashboard app:** built and maintained by a teammate; connects to the ESP32 over USB serial.

---

## Design Highlights (why this differs from a typical student energy-monitoring project)

- Power figures come from a live current sensor, not a hardcoded wattage assumption.
- The billing model isn't a toy counter — it follows the real TNB RP4 structure closely enough to survive scrutiny from someone who has actually read a TNB bill.
- The meter/billing-cycle separation specifically mirrors real utility metering practice (delta-based billing, non-resetting register) rather than the common "just reset everything monthly" shortcut.
- Automation strictness responds to actual tariff economics, not just a fixed timer.

---

## Repository Structure

```
├── firmware/              ESP32 Arduino sketch (main automation + billing logic)
├── web_dashboard/          Local dashboard served by the ESP32
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
5. Flash to an ESP32 dev board.
6. On boot, the device creates its own WiFi Access Point (SmartRoom_Demo by default) — connect and browse to the IP shown in Serial Monitor to view this repository's built-in live dashboard.

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

- Radar sensitivity requires site-specific calibration to reliably separate stationary human presence from environmental motion.
- Demonstration uses accelerated simulated time and a power scale factor to make effects visible in minutes rather than a real billing month — displayed figures represent the mechanism, not literal live household consumption.
- Current build is breadboard-based; a soldered/PCB revision would remove the intermittent connection issues encountered during development.
- Planned but not yet implemented: per-branch current sensing for true (not attribution-based) per-appliance monitoring, multi-room ESP-NOW mesh scaling, and a maintenance-safety digital interlock.

See the full report in /report for the complete Sustainability & Future Recommendations discussion.

Team & Contributions

This was a team project. To keep contributions clear:

Elektro19 — Levinesh: Firmware and system integration — occupancy fusion logic, TNB billing engine, tariff-tier-aware automation, sensor/relay/power hardware integration and debugging, OLED and local web dashboard, Sustainability & Future Recommendations report section.
Natalie: Designed and built a companion dashboard application that connects to the ESP32 over USB serial to visualize live telemetry. This component is maintained separately and is not included in this repository.
