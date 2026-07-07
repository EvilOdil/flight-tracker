# Flight Tracker

Physical flight tracker: an ESP32-S3 with three analog gauges (3-needle altimeter,
heading, speed — five 28BYJ-48 steppers) and a 3.5" TFT, fed live flight data from
free APIs (adsb.lol + adsbdb.com) through a small hostable backend.

Read `PLAN.md` for the architecture, API strategy, gauge ranges and message protocol.

```
backend/                 Node.js server (APIs -> WebSocket push) + mobile web app
device/flight_tracker/   Arduino firmware for the ESP32-S3
display/, stepper_test/  original tested hardware samples (kept as reference)
```

## 1. Host the backend

The backend runs on the internet — devices on any home Wi-Fi dial **out** to it
over WebSocket, so users never need port-forwarding or a shared network.
Needs Node.js ≥ 18; state is two small JSON files, so any host with a
persistent disk works.

```bash
cd backend
npm install
npm start            # listens on $PORT (default 8080)
```

Deployment options (pick one):

- **PaaS (Render / Railway / Fly.io):** point it at `backend/`, start command
  `npm start`. They provide HTTPS + WSS automatically. Attach a persistent
  volume (or accept that `data/` resets on redeploys — configs are re-enterable,
  caches rebuild themselves).
- **VPS + Caddy:** `caddy reverse_proxy localhost:8080` gives automatic
  HTTPS/WSS for your domain; run the server under systemd or pm2.
- **LAN only (bench testing):** run it on your laptop and give the device
  `http://<laptop-ip>:8080`.

The device speaks `wss://` when the server URL is `https://` and plain `ws://`
for `http://` — WebSocket upgrades on `/ws/device` pass through all the proxies
above unchanged.

No API keys needed — adsb.lol and adsbdb.com are free without registration.
Device configs persist in `backend/data/devices.json`, API caches in
`backend/data/cache.json`.

> Prototype security note: there is no auth yet — anyone who knows a device ID
> can reconfigure that device. Fine for a prototype; add a per-device pairing
> token before offering this publicly.

## 2. Build & flash the firmware

Arduino IDE (or `arduino-cli`), board **ESP32S3 Dev Module**, libraries:

- **TFT_eSPI** — display pins/driver are configured in the library's
  `User_Setup.h`, exactly as with the tested `display/display.ino` sketch.
- **ArduinoJson** (v7)
- **WebSockets** by Markus Sattler (links2004)

Open `device/flight_tracker/flight_tracker.ino`. Set the stepper pins in
`config.h` to match your wiring (defaults keep the tested `4/5/6/7` for the
speed gauge) — make sure they don't collide with the display's parallel-bus pins.

The sketch uses ~89% of the default 1.3 MB app partition; pick a larger
partition scheme in Tools → Partition Scheme (e.g. "Huge App") — the S3 module
has flash to spare — before adding more aircraft bitmaps.

**Before powering on, set all five needles to their zero marks** — homing is
positional (power-on = 0). Verified compile: `arduino-cli compile --fqbn esp32:esp32:esp32s3 device/flight_tracker`.

## 3. First-time device setup (phone)

1. Power the device — it shows **SETUP** with its AP name (= device ID, e.g. `FT-A1B2C3`).
2. On your phone, join Wi-Fi `FT-A1B2C3` (password `flight123`), open `192.168.4.1`.
3. Enter your home Wi-Fi credentials and the server URL
   (`https://tracker.example.com`, or `http://<laptop-ip>:8080` for bench testing).
4. Device reboots, joins Wi-Fi, connects out to the hosted backend over WSS,
   shows **WATCHING SKY** with its device ID.

Hold the **BOOT** button 3 s at any time to wipe the config and return to setup.

## 4. Configure tracking (phone web app)

Open the hosted URL (e.g. `https://tracker.example.com`) on your phone, enter
the device ID from the screen:

- **Radius watch** — tap the map (or "use my location"), drag the radius slider
  (max 100 km). The device tracks the aircraft closest to your point.
- **Track a flight** — enter an ICAO callsign (e.g. `UAL203`, `BAW117`).

The status card shows live what the device is displaying.

## 5. Gauge faces

- **Speed**: print a **0–500 kt** face with a **270°** sweep (the C172 40–200 kt
  dial in the reference project is too small for airliner ground speeds).
- **Altimeter**: standard 3-needle sensitive altimeter face (100 ft / 1,000 ft /
  10,000 ft per rev). The Thingiverse `Altimeter for Flight Simulator` face works.
- **Heading**: standard 0–360° compass rose.

Sweep/range constants live in `device/flight_tracker/config.h` if your printed
faces differ.

## 6. Adding aircraft bitmaps

`device/flight_tracker/bitmaps.h` ships the tested B737 + A330 silhouettes
(200×50 px, 1-bit). The backend maps ICAO type codes to family keys
(`B737, B747, A320, A330, RJ, PROP, GA, GENERIC` — see `bitmapFamily()` in
`backend/lib/tracker.js`). To add art: convert a 200×50 monochrome image with
image2cpp, add the array to `bitmaps.h`, and register it in `AIRCRAFT_BITMAPS`.
