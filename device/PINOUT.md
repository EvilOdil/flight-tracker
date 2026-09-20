# Flight Tracker — full pinout (both boards)

Generated from the source of truth, not from memory:
`~/Arduino/libraries/TFT_eSPI/User_Setup.h` (LCD), `device/flight_tracker/config.h` (S3),
`device/gauge_controller/config.h` (WROOM). If you change a pin, change it there — no
GPIO number is hardcoded anywhere else in the firmware.

Board roles: the **ESP32-S3** runs the display, Wi-Fi/WebSocket, the LED strips and the
speed gauge. The **ESP32-WROOM-32** runs the other four needles. Split rationale and the
link protocol: `gauge_link_protocol.md`.

---

## 1. Board-to-board link (3 wires)

| ESP32-S3 | | ESP32-WROOM-32 |
|---|---|---|
| GPIO 21 (TX) | → | GPIO 16 (RX) |
| GPIO 14 (RX) | ← | GPIO 17 (TX) |
| GND | — | GND |

115200 8N1. Both boards are 3.3 V logic — no level shifter. The **common ground is not
optional**; without it the link reads garbage or nothing at all.

---

## 2. ESP32-S3

### 2.1 LCD — 12 pins, do not reuse for anything

3.5" parallel shield, `ILI9488_DRIVER`. These live in TFT_eSPI's `User_Setup.h`, OUTSIDE
this repo. Touching one freezes the display *after* the boot splash — the nastiest
symptom this project has.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| TFT_D0 | 4 | | TFT_D4 | 15 |
| TFT_D1 | 5 | | TFT_D5 | 16 |
| TFT_D2 | 6 | | TFT_D6 | 17 |
| TFT_D3 | 7 | | TFT_D7 | 18 |
| TFT_RST | 9 | | TFT_DC | 11 |
| TFT_CS | 10 | | TFT_WR | 12 |

`TFT_RD` is `-1` (not wired). No backlight pin, no touch pin — the bare resistive touch
panel is deliberately unused (it shares LCD pins and froze the display).

### 2.2 Speed gauge — 28BYJ-48 + ULN2003

| Signal | GPIO | Notes |
|---|---|---|
| ULN2003 IN1 | 1 | |
| ULN2003 IN2 | 2 | |
| ULN2003 IN3 | 42 | JTAG MTMS — free, S3 routes JTAG through internal USB by default |
| ULN2003 IN4 | 41 | JTAG MTDI — same |
| limit switch | 13 | NO to GND, **internal** pull-up — no external resistor |

ULN2003 board also needs **5 V** and **GND** (shared with the S3's ground). The motor
runs off 5 V, never 3.3 V.

### 2.3 LED strips — 3x WS2812, 11 LEDs each

| Strip | Data GPIO | LEDs |
|---|---|---|
| 1 | 35 | 11 |
| 2 | 36 | 11 |
| 3 | 37 | 11 |

Each strip: data pin + 5 V + GND common with the S3. 35/36/37 are the octal-PSRAM lines
(SPIIO6/SPIIO7/SPIDQS), free **only** because this build has `PSRAM=disabled`. Enabling
PSRAM kills all three strips at once. Unconditionally-free alternatives, adjacent on the
header: 38, 39, 40.

### 2.4 Other

| Signal | GPIO | Notes |
|---|---|---|
| BOOT button | 0 | hold 3 s while running → wipe Wi-Fi config, back to setup portal |

### 2.5 S3 pins NOT available

| GPIO | Why |
|---|---|
| 19, 20 | native USB D-/D+ |
| 26–32 | SPI flash |
| 33, 34 | octal-PSRAM band (free while `PSRAM=disabled`, same caveat as 35–37) |
| 43, 44 | UART0 — the USB serial monitor |

**Free for future use:** 3, 8, 22, 23, 24, 25, 38, 39, 40, 45, 46, 47, 48
(3 and 45/46 are strapping pins — fine driving ULN2003 inputs, which float at boot).

---

## 3. ESP32-WROOM-32 (gauge controller)

### 3.1 Motors — 28BYJ-48 + ULN2003, IN1..IN4

| Needle | IN1 | IN2 | IN3 | IN4 |
|---|---|---|---|---|
| Heading (0–360°, continuous) | 13 | 14 | 18 | 19 |
| Altimeter 100s ft (1 rev = 1,000 ft) | 21 | 22 | 23 | 25 |
| Altimeter 1,000s ft (1 rev = 10,000 ft) | 27 | 26 | 33 | 32 |
| Altimeter 10,000s ft — **not wired yet** | 4 | 5 | 15 | 2 |

The 10,000s needle is off (`ALT3_ENABLED 0`). Its two strapping pins (15, 2) are parked
there on purpose: it is the motor that may never exist, and both are safe with a ULN2003
because that input floats low at boot, which is what each pin wants.

### 3.2 Limit switches — external pull-ups required

| Needle | GPIO | |
|---|---|---|
| Heading | 34 | **needs external 10 kΩ to 3V3** |
| Altimeter 100s | 35 | **needs external 10 kΩ to 3V3** |
| Altimeter 1,000s | 36 | **needs external 10 kΩ to 3V3** |
| Altimeter 10,000s | 39 | **needs external 10 kΩ to 3V3** |

All switches normally-open to GND, so closed reads LOW. GPIO 34–39 are input-only and
have **no internal pull-up** — this is the single most likely wiring mistake on this
board. Symptom: a needle homes instantly to the wrong place, or never homes at all
(`E <slot> nosw` on the serial monitor).

Contrast the S3's switch on GPIO 13, which does use an internal pull-up and needs no
resistor.

### 3.3 WROOM pins NOT available

| GPIO | Why |
|---|---|
| 0 | boot select — a ULN2003 input clamps the devkit pull-up to ~1.8 V and the board boots into download mode. **Never put a coil here.** |
| 1, 3 | UART0 — this board's USB serial monitor, kept free deliberately |
| 6–11 | SPI flash |
| 12 | MTDI strap — must read LOW at boot or the flash runs at 1.8 V |

**Free for future use:** 5 (once ALT3 is decided), and that's about it — this board is
nearly full at 16 coils + 2 link pins.

---

## 4. Power

| Load | Draw |
|---|---|
| 3 LED strips, 33 LEDs full-white | ~2.0 A @ 5 V |
| 28BYJ-48, per motor while stepping | ~0.24 A @ 5 V (coils de-energize at rest) |
| Two ESP32 boards + display | ~0.5 A @ 5 V |

Budget a **5 V supply of 3 A or more**, with every ground tied together (both ESP32s,
all five ULN2003 boards, all three LED strips). USB alone cannot feed the strips.

Homing runs one motor at a time, so peak current happens during normal flight updates
when several needles move together — not at boot.

Under-supply symptoms: brownout reboot loop, the far end of an LED strip washing out
pink, or needles stalling and losing steps (which homing will silently correct at the
next reset, hiding the real cause). Drop `LED_BRIGHTNESS` in `config.h` as the first
diagnostic.
