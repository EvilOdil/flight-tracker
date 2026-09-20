# Flight Tracker

A physical flight tracker. Five analog aircraft gauges and a 3.5" TFT show a real
aircraft flying near you — live altitude on a three-needle altimeter, ground speed on
a 0–500 kt dial, track on a compass rose, and the flight number, route, aircraft type
and a real photo of the actual airframe on the screen.

Data comes from free, key-free APIs (adsb.lol + adsbdb.com + planespotters.net) through
a small Node backend you host yourself. No accounts, no API keys, no subscriptions.

```
adsb.lol / adsbdb / planespotters
        │  (HTTPS poll, cached)
        ▼
   Node backend  ──REST──▶  phone web app (map, radius, theme, layout)
        │
        │  WebSocket push (device dials OUT over wss://)
        ▼
    ESP32-S3  ── display, Wi-Fi, LED strips, speed needle
        │
        │  UART link, 115200 8N1
        ▼
  ESP32-WROOM-32  ── heading + three altimeter needles
```

The device dials **out** to the hosted backend, so it works on any home Wi-Fi with no
port forwarding and no shared network. The phone app talks only to the backend, never
directly to the device.

---

## Repository layout

```
device/flight_tracker/      ESP32-S3 firmware — display, Wi-Fi/WS, LEDs, speed gauge
device/gauge_controller/    ESP32-WROOM-32 firmware — heading + 3 altimeter needles
device/gauge_link_protocol.md   UART contract between the two boards
device/PINOUT.md            Full pinout for both boards, generated from the configs
backend/                    Node server (express + ws + sharp)
backend/lib/tracker.js      Device registry, per-device config, WS protocol, poll loop
backend/lib/upstream.js     All external API calls + caching
backend/server.js           HTTP API + photo pipeline
backend/public/index.html   The entire web app — one file, vanilla JS, no build step
render.yaml                 Render blueprint (deploys from origin/main)
gauge_test/                 Bench sketch: motor wiring + homing checks
stepper_test/               Original 28BYJ-48 validation sketch (reference)
display/                    Original TFT validation sketch (reference)
display_image_test/         Photo-fidelity + panel calibration bench sketch
```

---

## Hardware

| Part | Qty | Notes |
|---|---|---|
| ESP32-S3 dev board | 1 | Needs a native-USB S3; PSRAM stays **disabled** (see LED note) |
| ESP32-WROOM-32 devkit | 1 | 38-pin. Exists purely for pins, not compute |
| 3.5" parallel TFT shield | 1 | 8-bit bus, **ILI9488** controller |
| 28BYJ-48 stepper + ULN2003 | 5 | One per needle |
| Limit switch (normally-open) | 5 | One per needle, for homing |
| 10 kΩ resistor | 4 | External pull-ups, WROOM side only — see below |
| WS2812 LED strip | 3 × 11 | Accent lighting, needs its own 5 V supply |

### Why two boards

The S3's display bus owns GPIO 4–7, 9–12 and 15–18; USB owns 19/20; UART0 owns 43/44;
flash/PSRAM owns 26–37. That leaves **16 clean GPIOs, and five ULN2003 motors need 20**
before a single limit switch. So heading and the three altimeter needles moved to a
WROOM-32; the speed needle stayed on the S3, where it was already wired.

The split is a *pin-map* fact, not a protocol fact. The S3 computes all five needles'
step targets and sends **every one** over the link; each board applies the slots it owns
and ignores the rest. Moving a motor between boards is an edit to two `config.h` files,
never a protocol change. All dial-face maths lives on the S3 in `gauges.h`.

### Board-to-board link — 3 wires

| ESP32-S3 | | ESP32-WROOM-32 |
|---|---|---|
| GPIO 21 (TX) | → | GPIO 16 (RX) |
| GPIO 14 (RX) | ← | GPIO 17 (TX) |
| GND | — | GND |

Both boards are 3.3 V logic, so no level shifter. **The common ground is not optional** —
without it the link reads garbage or nothing at all. Each board keeps its own USB serial
monitor (S3: native CDC, WROOM: UART0 on GPIO 1/3).

Full frame spec: [`device/gauge_link_protocol.md`](device/gauge_link_protocol.md).
Full pin tables for both boards: [`device/PINOUT.md`](device/PINOUT.md).

### Two wiring traps that cost real debugging time

1. **WROOM limit switches need external pull-ups.** All four sit on GPIO 34–39, which are
   input-only and have **no internal pull-up**. Each needs a 10 kΩ from the pin to 3V3 or
   it floats — homing then either trips instantly or never trips at all. This is the most
   likely wiring mistake on that board; check it first if a needle homes to the wrong place.
2. **The LED strips need their own 5 V.** 33 LEDs at full-bright white is roughly 2 A,
   well past what a USB port supplies. Under-supply shows up as a brownout reboot loop or
   the far end of a strip going pink. Give them a separate 5 V feed with a common ground,
   or lower `LED_BRIGHTNESS` in `config.h`.

Also note the strips live on GPIO 35/36/37, which are the S3's octal-PSRAM lines. They
are free **only because this build has PSRAM disabled**. If PSRAM is ever enabled, all
three strips die at once — and it will look like a strip fault, not a build-option change.

---

## 1. Host the backend

Node.js ≥ 18. State is two small JSON files plus a photo cache on disk, so any host with
a persistent disk works.

```bash
cd backend
npm install
npm start            # listens on $PORT (default 8080)
```

**Deploy options**

- **Render (what this repo is set up for):** `render.yaml` is a blueprint — *New →
  Blueprint*, point it at this repo, done. HTTPS + WSS are automatic. Pushing to
  `main` redeploys, so don't push a broken `main`.
- **Any PaaS (Railway, Fly.io):** root `backend/`, start command `npm start`.
- **VPS + Caddy:** `caddy reverse_proxy localhost:8080` gives automatic HTTPS/WSS.
- **LAN only, for bench testing:** run it on your laptop, give the device
  `http://<laptop-ip>:8080`.

The device speaks `wss://` when the server URL is `https://` and `ws://` for `http://`.
WebSocket upgrades on `/ws/device` pass through all of the above unchanged.

**No API keys anywhere** — adsb.lol and adsbdb.com are free without registration, and
planespotters' public photo API only wants a descriptive User-Agent. That was a
deliberate architecture choice; please keep it.

### HTTP API

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/devices` | Registry listing (also the health check) |
| `GET` | `/api/devices/:id/status` | What the device is showing right now |
| `GET` | `/api/devices/:id/config` | Stored per-device config |
| `PUT` | `/api/devices/:id/config` | Update mode / location / radius / callsign / layout / theme |
| `POST` | `/api/devices/:id/reset-network` | Push the device back to its setup portal (`409` if offline) |
| `GET` | `/api/aircraft/:hex/photo` | Resized JPEG for the device, or `404` → silhouette fallback |
| `WS` | `/ws/device` | Device link |

### Polling and caching

Everything here is shaped by free-tier frugality — the backend never polls when no
device is connected.

| | Interval / TTL |
|---|---|
| Poll while tracking an aircraft | 8 s |
| Poll while idle (nothing in range) | 30 s |
| Backoff cap after repeated failures / 429s | 5 min |
| Callsign mode grace before giving up | 5 min |
| Route + aircraft lookups (adsbdb) | 24 h |
| Photo lookups (planespotters) | 7 days |
| Confirmed not-found | 6 h |
| Network / 5xx errors | **never cached** |

That last row matters: caching a transient failure as a negative would permanently blank
a photo that actually exists.

### Photo pipeline

Lookup → fetch thumbnail → `sharp` resize to 282×217 fit-inside → JPEG q88, **4:4:4**,
**baseline**. Three constraints are load-bearing: 4:2:0 smears hues at this size,
TJpg_Decoder on the device cannot decode progressive JPEG, and the disk cache is keyed
`<hex>.v4.jpg` — bump that version on any processing change or stale renders get served.

The path fails soft at every step: a missing or broken photo falls back to an on-device
silhouette bitmap and may never blank the screen or stall the device loop.

`PHOTO_CAL=1` (in `render.yaml`) serves a raw panel test pattern instead of every photo,
for diagnosing the display. Leave it `"0"` in production.

---

## 2. Build and flash the firmware

Arduino IDE or `arduino-cli`. Libraries: **TFT_eSPI**, **ArduinoJson** (v7),
**WebSockets** by Markus Sattler (links2004), **Adafruit NeoPixel**, **TJpg_Decoder**.

**The TFT driver lives outside this repo.** Set `ILI9488_DRIVER` in the library's
`~/Arduino/libraries/TFT_eSPI/User_Setup.h`, along with the parallel-bus pin map. This
is worth stating loudly: the panel is an ILI9488, and running it as ILI9486 *mostly*
works but produces a subtly wrong gamma/tone response that is very easy to misdiagnose
as a photo-processing problem. HX8357D gives a dead-black screen.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc device/flight_tracker
arduino-cli compile --fqbn esp32:esp32:esp32              device/gauge_controller
```

Compile **both** after any firmware change — `stepper.h` is duplicated byte-identically
in the two sketch directories, because Arduino cannot share headers across sketches. Edit
one, copy it to the other, recompile both.

Current flash use: S3 **94%** of the default 1.3 MB app partition, controller **21%**. If
the S3 crosses ~98%, switch to the `huge_app` partition scheme rather than cutting
features.

All pins, gauge ranges and feature flags (`UI_CREATIVE_SCREENS`, `GAUGE_SERIAL_LOG`,
`ALT3_ENABLED`) live in the two `config.h` files and nowhere else.

**Before first power-on**, either wire the limit switches or set the needles to their
zero marks by hand — see homing below.

---

## 3. First-time device setup (phone)

1. Power the device. It shows **SETUP** with its AP name, which is also its device ID
   (e.g. `FT-A1B2C3`), plus a QR code.
2. Scan the QR to join, or manually join Wi-Fi `FT-A1B2C3` (password `flight123`) and
   open `192.168.4.1`. The portal serves the form directly on any URL, so most phones
   pop it open automatically on joining.
3. Enter your home Wi-Fi credentials and the server URL.
4. The device reboots, joins Wi-Fi, dials out to the backend and shows **WATCHING SKY**.

Hold **BOOT** for 3 s at any time to wipe the config and return to setup.

---

## 4. Use it (phone web app)

Open the hosted URL, enter the device ID from the screen.

- **Radius watch** — tap the map or use your location, drag the radius slider (max
  100 km). The device tracks the aircraft closest to your point, switching targets only
  when a new one is meaningfully closer, so the needles don't twitch between two planes.
- **Track a flight** — enter an ICAO callsign (e.g. `UAL203`, `BAW117`).
- **Theme** — pick an accent colour; it applies to the web app and the device screen
  together.
- **Layout** — `0 Standard` is text only, spread over the full screen and never touches
  the network. `1 Classic` packs the text tight and adds a real photo of the airframe.
  Photos are fetched **only** in Classic, which saves device bandwidth and the free
  photo-API quota.
- **Change network** — pushes the device back to its setup portal, behind a confirm
  modal.

A presentation-only change (theme, layout) re-sends the current flight for redraw
instead of restarting flight tracking — switching layout never costs you the aircraft
you're watching.

---

## 5. Gauge faces, homing and calibration

Print or source faces to match these ranges — they're set in `config.h` if yours differ:

- **Speed** — 0–500 kt over a **270°** sweep. (A C172-style 40–200 kt dial is too small
  for airliner ground speeds.)
- **Altimeter** — standard three-needle sensitive altimeter: 1 rev = 1,000 ft / 10,000 ft
  / 100,000 ft. Altitude is clamped at 45,000 ft.
- **Heading** — standard 0–360° compass rose.

**Homing.** Each needle seeks its limit switch at power-on, then travels a per-needle
offset to reach the printed zero — because in practice a switch gets mounted wherever it
physically fits, not at zero. Both the seek direction and the offset are mechanical facts
of one individual gauge; measure them on the bench using the procedure in
`gauge_link_protocol.md` ("Calibrating a needle"), and set them in that board's `config.h`.

Homing is deliberately **fail-soft**: a switch that never trips within about one
revolution gives up, leaves its needle where it is and treats that as zero. A dead switch
may degrade one gauge — never the whole device. Set a home pin to `-1` to skip homing for
that needle entirely (power-on position is then assumed to be zero).

The third altimeter needle (10,000s) is off by default; set `ALT3_ENABLED` to `1` in
**both** configs once it's physically wired.

---

## 6. Adding aircraft bitmaps

`device/flight_tracker/bitmaps.h` ships tested B737 and A330 silhouettes (200×50, 1-bit).
The backend maps ICAO type codes to family keys — `B737, B747, A320, A330, RJ, PROP, GA,
GENERIC` — in `bitmapFamily()` in `backend/lib/tracker.js`. To add art: convert a 200×50
monochrome image with image2cpp, add the array to `bitmaps.h`, register it in
`AIRCRAFT_BITMAPS`. Prefer flash-cheap solutions; the S3 is at 94%.

---

## 7. When something looks wrong

**The display shows garbage or freezes.** In this project's actual order of historical
frequency:

1. A GPIO conflict with the parallel bus — a stepper or peripheral pin overlapping the
   display's. The classic symptom is a freeze *after* the boot splash.
2. Blocking serial with no monitor attached. `Serial.setTxTimeoutMs(0)` in `setup()`
   prevents it; without it the device appears frozen whenever no monitor is connected,
   and works perfectly whenever you attach one to debug it.
3. Byte-phase slip from block pixel pushes. Photos are drawn per-pixel on purpose;
   `pushImage`/`pushPixels` produce pastel garbage on this clone parallel bus.
4. The wrong TFT driver profile. If colours and tones are *systematically* wrong rather
   than random, suspect the driver before writing any correction code.

Check those four before inventing new theories. Note that `invertDisplay`/MADCTL state
survives a soft reset — hard power-cycle before trusting a "broken" panel.

**A needle homes to the wrong place or never homes.** External pull-ups on the WROOM
switches first, then the per-needle direction and offset.

**The device brownouts or reboots when the LEDs light.** Power, not firmware. See above.

**The whole device reboots into download mode.** On the WROOM, a ULN2003 input on GPIO0
clamps the boot pull-up below threshold. GPIO0 and GPIO12 are never coil pins.

---

## Status and limitations

This is a working prototype, not a product.

- **No authentication.** Anyone who knows a device ID can reconfigure that device. Fine
  on a private deployment; add a per-device pairing token before making it public.
- **Single-user scale.** No database, no framework — state is JSON files on one small
  instance, sized for the free tier. Frugality *is* the reliability strategy here: dedup
  keys, demand-driven fetches, layered caches and fail-soft fallbacks at every remote
  boundary.
- The WebSocket protocol and the UART gauge link are both **compatibility contracts**.
  Devices in the field are flashed separately from the backend, and the two boards are
  flashed separately from each other, so only ever ADD optional fields — never rename or
  repurpose one.
