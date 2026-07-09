# Flight Tracker — System Development Rules

Binding guidelines for ANY AI model or developer changing this codebase. These rules exist
because each one was paid for with a real debugging session on real hardware. Violating them
breaks the device, the deploy, or the look-and-feel in ways that are hard to trace back.

## 1. System map (do not restructure)

```
device/flight_tracker/   ESP32-S3 firmware (Arduino, header-per-module, single .ino)
backend/                 Node backend (express + ws), deployed on Render free tier
backend/public/          Web app — ONE self-contained index.html (vanilla JS, no build step)
backend/lib/tracker.js   Device registry, per-device config, WS protocol, flight push loop
backend/lib/upstream.js  All external API calls (adsb.lol, adsbdb, planespotters) + caching
backend/server.js        HTTP API, photo pipeline, panel-calibration LUTs
render.yaml              Render blueprint — deploys from origin/main
PLAN.md                  Original architecture plan (reference, not a spec to "fix")
```

- Data flow: adsb.lol/adsbdb → backend poll loop → WS push → device. Web app talks REST to
  the backend and never talks to the device directly.
- The device dials OUT (wss://) to the internet-hosted backend; there is no inbound path to
  the device except its own captive portal in setup mode.
- Keep this topology. Do not add device-side polling of external APIs, do not add a build
  step to the web app, do not split the firmware into libraries.

## 2. Iron rules (never do)

1. **Never assign stepper/peripheral GPIOs from the display bus**: 4,5,6,7,15,16,17,18
   (data) + 9,10,11,12 (RST/CS/DC/WR). Also reserved: 0, 19/20 (USB), 43/44 (UART0),
   26–37 (flash/PSRAM). Violations freeze the display *after* the boot splash — the
   nastiest possible symptom. Pin map + rationale live in `config.h`.
2. **Never include TFT_eSPI (via display_ui.h/graphics.h) before WebServer.h** in the .ino.
   TFT_eSPI defines `FS_NO_GLOBALS` and breaks the ESP32 core build. Include order:
   `wifi_portal.h` FIRST, display headers after.
3. **Never remove `Serial.setTxTimeoutMs(0)`** (CDC-guarded, in setup()). Without it,
   `Serial.print` blocks when no monitor is attached and the whole device appears frozen —
   while "working fine" whenever you attach a monitor to debug it.
4. **Never draw decoded photo pixels with block/DMA pushes** (`pushImage`, `pushPixels`).
   The clone parallel bus slips byte phase on long variable-pixel streams → pastel garbage.
   `photo.h` draws per-pixel via `drawPixel` deliberately. Full-screen fills are safe only
   because they are byte-symmetric colors.
5. **Never touch the panel calibration constants casually** (`PANEL_TONE_GAMMA`,
   `PANEL_TONE_MAX=148`, `PANEL_GREEN_CURVE='0:0,8:3,57:29,68:68,80:80'`, 4:4:4 q88
   baseline JPEG in server.js). These are *measured on the physical glass* over multiple
   on-device tuning rounds — the clone panel has a non-monotonic (folded) brightness
   response that no amount of code reasoning can re-derive. Tuning goes through env vars,
   never by editing defaults, and every tuning value MUST stay part of the photo cache
   filename so stale renders can't serve.
6. **Never switch the TFT driver in code review**. The driver define lives OUTSIDE the repo
   in `~/Arduino/libraries/TFT_eSPI/User_Setup.h` (currently `ILI9486_DRIVER`). HX8357D was
   tried and produced a dead-black screen. Untried candidates (ILI9481, ST7796, R61581,
   RM68140) are hardware experiments requiring the user at the device — never a casual edit.
7. **Never re-add touch input** without an external XPT2046 module plan. The shield's bare
   4-wire resistive panel shares LCD pins; reading it via ADC froze the display. Full
   post-mortem is in project memory. Also: never name any macro `TOUCH_CS` (TFT_eSPI hijacks
   it and #errors in parallel mode).
8. **Never call external APIs without the descriptive User-Agent** in `upstream.js`
   (`USER_AGENT`, includes contact mailto). planespotters.net 403s generic UAs. Route ALL
   upstream HTTP through `upstream.js` so this can't be bypassed.
9. **Never cache transient upstream failures as negatives.** Cache policy: positive photo
   lookups 7 d, confirmed not-found 6 h, network/5xx errors NOT cached. Breaking this either
   hammers free APIs or permanently blanks a photo that exists.
10. **Never let a presentation-only config change reset flight tracking.** `setConfig` in
    tracker.js computes `trackingChanged` from mode/lat/lon/radius/callsign only; theme and
    layout changes re-send the current flight for redraw without a re-poll. Preserve this
    split when adding config fields.
11. **Never commit secrets, `backend/data/`, or the photo cache.** `.gitignore` already
    covers `backend/data/`. There are no API keys in this system by design — keep it that
    way (adsb.lol and adsbdb are key-free; that was a deliberate architecture choice).
12. **Never force-push or flash the device unless the user asks.** Flashing overwrites a
    known-good state on physical hardware; a bad flash bricked the display once already.

## 3. Firmware rules (`device/flight_tracker/`)

**Structure & style**
- One `.ino` (setup/loop/message dispatch) + single-purpose headers (`display_ui.h`,
  `graphics.h`, `gauges.h`, `wifi_portal.h`, `photo.h`, `theme.h`, `qr.h`, `config.h`,
  `bitmaps.h`). New capability = new header, not a bigger .ino. `qrcodegen.c/.h` is
  vendored (ESP32 core ships a conflicting `qrcode.h`) — do not "upgrade" it to a library.
- All hardware constants, pin maps, and feature flags (`UI_CREATIVE_SCREENS`,
  `UI_TOUCH_ENABLED`, `GAUGE_SERIAL_LOG`, `ALT3_ENABLED`) live in `config.h` only.
- Verify EVERY firmware change with
  `~/bin/arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc device/flight_tracker`
  before declaring it done. When a feature flag exists, compile-check both states.

**Flash budget** — currently ~93% of the default partition. Report the new percentage after
any firmware change; if it crosses ~98%, the escape valve is the `huge_app` partition scheme
(FQBN option), not deleting features. Prefer flash-cheap solutions (fonts over bitmaps,
computed graphics over stored images).

**Rendering discipline** (this is where "feel" lives)
- The display never flickers because nothing repaints without cause: `showFlightInfo` keys a
  dedup hash over ALL visible state (flight fields + layout + accent color). Any new visible
  element MUST join that key, or switching it won't repaint / not switching it will.
- No `delay()`-based animation in `loop()`; steppers, WS, and display share one cooperative
  loop. Long-running work (photo fetch+decode) is acceptable only at flight-change events.
- Text sizing goes through `fitTextSize()` (adaptive to string length); never hardcode a
  text size for variable-length data. Layouts: 0 = Standard (text-only, spread over full
  height, dividers at fixed fractions), 1 = Classic (compact text, photo panel + credit
  line, silhouette-bitmap fallback). New layouts follow the same proportional (fraction-of-
  H/W) geometry — no absolute pixel coordinates.
- All accent-colored UI goes through `uiAccent565` (`theme.h`) — never a literal color for
  anything thematic. The 40% lighten-toward-white on the web hex is intentional (dark
  background legibility); don't remove it.
- Photos are fetched by the device ONLY in Classic layout (demand-driven; saves device
  bandwidth AND backend/planespotters quota). Keep every new resource fetch demand-driven
  the same way.

**Setup / provisioning flow** — the captive portal serves the form directly (HTTP 200,
no-cache) from a catch-all `onNotFound` + wildcard DNS, which is what makes phones auto-open
the page on join. Do not "clean it up" into a 302 redirect. The setup screen's QR is a
Wi-Fi-JOIN code (`WIFI:T:WPA;...` via `wifiJoinPayload()`, MECARD-escaped), with SSID/pass/
IP printed as fallback — keep all three signup paths working.

## 4. Backend rules (`backend/`)

- Plain Node + express + ws + sharp. No framework migrations, no TypeScript conversion, no
  ORM, no database — state is `cache.json` + `data/photos/` on disk, sized for a single-user
  Render free instance. Scalability here means *frugality*: cache every upstream response,
  fetch only on demand, never add polling that runs when no device is connected.
- **WS protocol is a compatibility contract.** Frames: `cfg` (carries `layout`, `th`),
  flight frames (carry `img` relative URL), `netreset`. Deployed devices in the field speak
  this protocol — only ADD optional fields; never rename or repurpose existing ones. The
  firmware builds absolute photo URLs from its stored server host + the relative `img` path;
  keep photo URLs relative in frames.
- **Photo pipeline** (server.js): lookup via upstream.js → fetch thumb → sharp resize to
  282×217 fit-inside → per-pixel calibration LUTs → JPEG q88, 4:4:4, **baseline**
  (TJpg_Decoder cannot decode progressive JPEG). Disk cache keyed
  `<hex>.v3.<full-calibration-tag>.jpg`. 404 → device falls back to silhouette bitmaps —
  the photo path must ALWAYS fail soft; a missing/broken photo may never blank the screen
  or stall the device loop.
- `PHOTO_CAL` env (render.yaml): 0 = real photos (production), 1/2/3 = calibration test
  patterns. Leave "0" unless running a calibration session with the user at the device.
- REST endpoints return proper status codes the UI depends on (e.g. 409 when
  `reset-network` targets an offline device → UI alert). Match that pattern for new
  endpoints; errors are structured, never 200-with-error-body.
- Config validation lives in tracker.js `setConfig` (theme `#rrggbb` lowercased, layout
  enum). Every new config field gets validated there, persisted per-device, and echoed in
  the `cfg` frame — one source of truth for device config.

## 5. Web app rules (`backend/public/index.html`)

- ONE file: inline CSS + vanilla JS + Leaflet from CDN. No bundler, no framework, no npm
  frontend deps. It must stay directly servable by express.static.
- **Look and feel**: dark UI, device-synced accent theme (default `#2ea8ff`), rounded cards,
  the existing spacing scale. New controls go inside the existing settings sheet pattern
  (gear → overlay) — do not add new top-level chrome to the main screen. Destructive
  actions (Change network) always get the styled confirm modal, never `confirm()`.
- **Theme changes** flow through `applyTheme(hex, push)`: CSS variables + localStorage
  fallback + debounced (350 ms) PUT to the device config. Device's saved theme wins on
  load. Never introduce a second theme mechanism.
- **Two CSS landmines already hit — don't reintroduce them:**
  - Overlay visibility: `.overlay.hidden{display:none}` must out-specify any later
    `.overlay{display:flex}` (equal-specificity cascade order bug showed settings at boot).
  - Leaflet stacking: `#map{position:relative;z-index:0}` contains Leaflet's z-index
    400–1000 panes so overlays paint above the map. Do not remove; do not "fix" overlay
    conflicts by hiding/showing the map (causes invalidateSize churn).
- Verify UI changes by actually loading the page: run the backend locally
  (`node server.js`, kill stale servers on the port first — a stale process once served
  old code for a whole debugging round) and screenshot with headless Firefox (snap Firefox
  cannot write /tmp; screenshot into the home dir).

## 6. Cross-cutting workflow

- **Definition of done** for any change: firmware compiles (both flag states if flagged),
  backend starts clean, web page screenshots correctly, and the change is exercised
  end-to-end where possible. "It should work" is not done.
- **Deploy**: Render auto-deploys from origin/main via render.yaml. Push = deploy. So never
  push a broken main; verify locally first. Ops toggles are render.yaml `envVars`, not code
  edits.
- **Commits**: small, scoped, message states the user-visible effect. Anything device-
  visible needs the user to confirm on the physical glass before the thread is closed —
  the calibration saga proved the emulator-free truth lives only on the panel.
- **When the display shows garbage**, the cause ranking from this project's history:
  (1) GPIO conflict with the parallel bus, (2) blocking Serial with no monitor,
  (3) byte-phase slip from block pixel pushes, (4) the panel's folded response — in that
  order. Check these before inventing new theories.
- **Free-tier mindset**: every upstream call, every device fetch, every repaint must
  justify itself. The system's reliability comes from doing less: dedup keys, demand-driven
  fetches, layered caches, and fail-soft fallbacks at every remote boundary.
