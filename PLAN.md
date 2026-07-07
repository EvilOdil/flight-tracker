# Flight Tracker — System Plan

Physical flight-tracker: ESP32-S3 drives 3 analog gauges (altimeter ×3 needles, heading,
speed — five 28BYJ-48 steppers total) + a 3.5" TFT showing flight info, fed live data
from commercial/free flight APIs through a hostable backend.

```
┌─────────────┐   HTTPS (poll, cached)   ┌──────────────┐   WSS (device dials   ┌────────────┐
│ Flight APIs  │ ───────────────────────► │   Backend    │ ◄──── out, push) ───► │  ESP32-S3  │
│ adsb.lol     │                          │  Node.js     │   compact JSON        │  device on │
│ adsbdb.com   │                          │  express+ws  │                       │ user Wi-Fi │
│ (OpenSky fb) │                          │ (internet-   │                       │ 5 steppers │
└─────────────┘                           │  hosted)  ▲  │                       │ 3.5" TFT   │
                                          └───────────┼──┘                       └────────────┘
                                                      │ HTTPS (config web app, Leaflet map)
                                               ┌──────┴───────┐
                                               │ Mobile phone │
                                               └──────────────┘
```

The backend is **internet-hosted** (PaaS or VPS behind HTTPS). Devices sit on the
user's own Wi-Fi and dial **out** to the backend over `wss://` — no port-forwarding,
NAT traversal, or shared network required. Plain `ws://` remains available for
bench testing against a LAN server.

## 1. API strategy (free tiers)

| Need | Primary | Notes |
|---|---|---|
| Live positions in radius | **adsb.lol** `GET /v2/point/{lat}/{lon}/{radius}` | Free, no key, radius in **NM** (100 km = 54 NM), dynamic rate limits. Drop-in ADSBx-compatible. |
| Track one callsign | **adsb.lol** `GET /v2/callsign/{cs}` | Free, no key. |
| Route (origin → destination) | **adsbdb.com** `GET /v0/callsign/{cs}` | Free, no key. Returns origin/destination airports + airline. **Static per flight → cache hard.** |
| Aircraft type / registration | **adsbdb.com** `GET /v0/aircraft/{hex}` | Free. Type, ICAO type code, manufacturer. **Static forever → cache forever.** |
| Fallback live data | airplanes.live `/v2/point/...`, OpenSky `/api/states/all` | Same shape (airplanes.live); OpenSky needs OAuth2, ~4000 credits/day registered. |

### Efficient API usage
- Backend polls upstream **only while at least one device is connected and configured**.
- **One poll per unique area**, not per device: devices sharing a location/radius share a poll.
- Radius mode: poll every **8 s** (adaptive: back off to 30 s when no aircraft in range, on HTTP 429 exponential backoff up to 5 min).
- Callsign mode: poll `/v2/callsign` every **8 s**.
- adsbdb lookups: in-memory + disk cache. Aircraft-by-hex cached **forever**, callsign routes **24 h**, negative results **6 h**. Each new flight costs at most 2 adsbdb calls, ever.

## 2. Message transport (backend → device)

WebSocket (`/ws/device`), server-push, compact single-letter-key JSON (ArduinoJson-friendly, <200 B/frame):

- `{"t":"flight","cs":"UAL203","al":"UNITED AIRLINES","rt":"SFO -> LAX","ac":"Boeing 737-900","fam":"B737","alt":34000,"gs":450,"trk":278}` — sent once when the tracked flight changes (static metadata + first state). `fam` selects the PROGMEM bitmap on-device.
- `{"t":"state","alt":34200,"gs":452,"trk":279}` — sent **only when a value moves past a threshold** (alt ±100 ft, gs ±5 kt, trk ±2°). Typical rate: a frame every few seconds, often none.
- `{"t":"clear"}` — no aircraft in range / flight lost (5 min grace on callsign mode).
- `{"t":"cfg",...}` — echo of current device config (mode, radius…).
- WS ping/pong keepalive every 20 s; device auto-reconnects with backoff.

Device → backend: `{"t":"hello","id":"<deviceId>","fw":"1.0.0"}` on connect. All config comes from the phone web app, not the device.

**Radius-mode selection rule:** among aircraft inside the radius, track the one **closest to the configured location** (with hysteresis: keep current target unless another is 20% closer) so needles don't thrash between planes.

## 3. Gauge ranges & stepper math

28BYJ-48 + ULN2003, **2048 full-steps/rev** (as validated in `stepper_test/`). 5.69 steps/degree.

| Gauge | Range | Mapping | Steps |
|---|---|---|---|
| **Speed** (ground speed) | **0–500 kt** over a **270°** sweep | linear | 0–1536 steps (3.07 steps/kt). The C172 dial (40–200 KIAS) is too small for airliners at 400–500 kt GS; print a 0–500 face. |
| **Heading** | 0–360°, full continuous circle | 1° = 5.689 steps | shortest-path move, wraps through 0 |
| **Altimeter needle 1** (100s ft) | 1 rev = 1,000 ft | steps = (alt mod 1000)/1000 × 2048 | classic 3-needle sensitive altimeter |
| **Altimeter needle 2** (1,000s ft) | 1 rev = 10,000 ft | steps = (alt mod 10000)/10000 × 2048 | |
| **Altimeter needle 3** (10,000s ft) | 1 rev = 100,000 ft | steps = alt/100000 × 2048 | 0–45,000 ft covers all commercial traffic (ceiling ≈ FL430) |

- Needles homed by assuming **power-on position = 0** (align needles to zero before boot); positions tracked in software. (Optional later: hall-sensor homing.)
- Custom non-blocking driver (`GaugeStepper`) steps all 5 motors cooperatively from `loop()` — no blocking `Stepper.step()` calls, coils de-energized when idle (28BYJ-48 gearbox holds position, saves ~1.2 W/motor).

## 4. Device firmware (`device/flight_tracker/`, Arduino)

- **Wi-Fi onboarding:** on first boot (or button hold) device starts SoftAP `FlightTracker-XXXX` + captive portal (DNSServer); phone connects, portal page takes home Wi-Fi SSID/password + backend URL; stored in NVS (`Preferences`). Then joins Wi-Fi and opens the WebSocket.
- **Display:** TFT_eSPI (config in library `User_Setup.h`, as in `display/display.ino`), same 4-block GUI + 200×50 PROGMEM bitmap; adds status screens (AP mode instructions with device ID, connecting, "waiting for flights"). Bitmap table keyed by `fam` (B737 / A330 shipped from the tested sketch; unknown types get a generic silhouette slot — add more bitmaps to `bitmaps.h`).
- **Libraries:** TFT_eSPI, ArduinoJson, WebSockets (links2004/arduinoWebSockets). Steppers use a built-in driver, no lib.

## 5. Backend (`backend/`, Node.js — hostable anywhere with Node ≥ 18)

- `express` serves the mobile web app + REST config API; `ws` serves `/ws/device`.
- Device registry in `data/devices.json` (id → config); flight caches in `data/cache.json`.
- REST: `GET /api/devices`, `GET/PUT /api/devices/:id/config` (mode `radius`|`flight`, lat, lon, radiusKm ≤ 100, callsign), `GET /api/devices/:id/status` (online, current flight — powers live view in the app).
- Poll scheduler: per-area tick → adsb.lol → pick target → enrich via adsbdb (cached) → threshold-diff → push to sockets.

## 6. Web app (`backend/public/`, served by backend, mobile-first)

- Enter/select device by ID (shown on the device's screen).
- Mode toggle: **Radius** — Leaflet interactive map (OpenStreetMap tiles), tap/drag marker to set location, drag/slider for radius with live circle overlay, clamped to 100 km, "use my location" via browser geolocation. **Flight** — callsign input.
- Live status card: what the device is currently showing.

## 7. Phases

1. **This pass:** plan + firmware + backend + web app, backend smoke-tested locally against live APIs.
2. Hardware bring-up: pin mapping to your ULN2003 boards, print 0–500 kt speed face, calibrate.
3. Later: hall-sensor homing, more aircraft bitmaps, OpenSky fallback wiring, auth for public hosting.
