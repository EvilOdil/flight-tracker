// Hardware + behaviour configuration for the flight-tracker device.
#pragma once

#define FW_VERSION "1.1.0"

// ---------------------------------------------------------------------------
// Two-board split (2026-08-18)
//
// The S3 owns the display, Wi-Fi/WebSocket, the LED strip, and the SPEED gauge
// only. Heading + the three altimeter needles moved to an ESP32-WROOM-32
// devkit ("gauge controller", device/gauge_controller/) reached over a UART
// link, because the S3 has no pins left: the 8-bit display bus plus USB plus
// flash/PSRAM leave 16 clean GPIOs, and five ULN2003 motors alone need 20.
//
// The speed motor stays here because the S3 has room for exactly one motor
// plus its switch plus the link. Its coils moved from 1/2/3/8 to 1/2/42/41 on
// 2026-08-19 to match the physical harness; 3 and 8 are free again. Motor
// ownership is a pin-map fact on each board, NOT a protocol fact — the link
// frame always carries all five needle targets and each board applies the ones
// it owns, so a motor can be moved across the boundary by editing two configs.
// Protocol contract: device/gauge_link_protocol.md.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Speed gauge stepper (28BYJ-48 + ULN2003, IN1..IN4) — the only motor on the S3.
//
// The 3.5" display's 8-bit parallel bus owns GPIO 4,5,6,7,15,16,17,18 (data)
// and 9,10,11,12 (RST/CS/DC/WR) — see TFT_eSPI User_Setup.h. NONE of the pins
// below may touch those, or the display freezes after the boot splash.
// Also reserved: 0 (BOOT btn), 19/20 (USB), 43/44 (UART0 serial monitor),
// 26..37 (flash/PSRAM, except 35 knowingly overridden for the LED strip).
// ---------------------------------------------------------------------------
#define SPD_IN1 1
#define SPD_IN2 2
#define SPD_IN3 42
#define SPD_IN4 41

// Speed needle limit switch: normally-open to GND, internal pull-up, so
// closed reads LOW. Set to -1 to disable homing (needle assumed at zero on
// power-on, the pre-2026-08 behaviour).
#define SPD_HOME_PIN     13
// Which way to rotate while seeking the switch, and how far past the switch
// the printed zero sits. Both are mechanical facts of THIS gauge — measure on
// the bench (see device/gauge_link_protocol.md "Calibrating a needle").
#define SPD_HOME_DIR     (-1)
#define SPD_HOME_OFFSET  0
// Give-up threshold: the speed needle only sweeps 270 deg, so a full rev plus
// margin means the switch is missing or dead.
#define SPD_HOME_MAX_STEPS (STEPS_PER_REV + STEPS_PER_REV / 8)

// ---------------------------------------------------------------------------
// UART link to the gauge controller (ESP32-WROOM-32).
//
// Cross-connect: S3 TX -> WROOM RX (GPIO16), S3 RX <- WROOM TX (GPIO17), and
// a common GND between the boards. Both are 3.3 V logic — no level shifter.
// Do NOT use 43/44 here: that is UART0 / the USB serial monitor.
// ---------------------------------------------------------------------------
#define GAUGE_LINK_TX    21
#define GAUGE_LINK_RX    14
#define GAUGE_LINK_BAUD  115200

// Free S3 GPIOs after the above: 3, 8, 22, 23, 24, 25, 38, 39, 40, 45, 46, 47, 48.

// Hold BOOT (GPIO0) for 3 s while running to wipe Wi-Fi config -> setup portal.
#define RESET_BTN_PIN 0

// ---------------------------------------------------------------------------
// Accent LED strips — 3 independent WS2812-style 5V NeoPixel strips, one data
// pin each, fixed full-bright white (no animation, no per-frame updates).
//
// PIN BAND: 35/36/37 are the ESP32-S3's octal-PSRAM lines (SPIIO6, SPIIO7,
// SPIDQS). They are free ONLY because this build has PSRAM disabled — the
// default for this FQBN, verified with `arduino-cli board details`. GPIO35 has
// been driving a strip on this board since 2026-08-18, so the band is proven
// good here. If PSRAM is ever switched on (e.g. for a bigger photo buffer) the
// OPI bus takes all three pins and all three strips die at once — that will
// look like a strip fault, not a build-option change. Unconditionally-free
// alternatives, physically adjacent on the header: 38, 39, 40.
//
// POWER: 33 LEDs at full-bright white is ~2 A at 5 V (60 mA/LED). That is well
// past what a USB port will supply — the strips need their own 5 V feed with a
// common ground, or LED_BRIGHTNESS must come down. Symptom of under-supply is
// a brownout reboot loop or the far end of a strip going pink/white-ish.
// ---------------------------------------------------------------------------
#define LED_STRIP_1_PIN   35
#define LED_STRIP_1_COUNT 11
#define LED_STRIP_2_PIN   36
#define LED_STRIP_2_COUNT 11
#define LED_STRIP_3_PIN   37
#define LED_STRIP_3_COUNT 11
// 0-255, applied to every strip. Lower this before suspecting anything else if
// the device browns out once the strips are lit.
#define LED_BRIGHTNESS    255

// ---------------------------------------------------------------------------
// Gauge scales (see PLAN.md §3)
//
// All five needles' step targets are computed HERE, on the S3, and pushed over
// the link — the gauge controller knows only pins, homing offsets and
// STEPS_PER_REV. One source of truth for the dial faces.
// ---------------------------------------------------------------------------
#define STEPS_PER_REV     2048      // 28BYJ-48 full-step (validated in stepper_test)
#define STEP_INTERVAL_US  2500      // ~2.9 ms/step is the tested 10 rpm; 2.5 ms is safe

// Homing runs slower than normal motion: a needle may be driving toward a hard
// mechanical stop, and a missed switch read at speed overshoots further.
#define HOME_STEP_INTERVAL_US   3500
#define HOME_BACKOFF_MAX_STEPS  300

#define SPEED_MIN_KT      0
#define SPEED_MAX_KT      500       // print a 0-500 kt face; C172 dial (40-200) is too small
#define SPEED_SWEEP_DEG   270.0

#define ALT_MAX_FT        45000     // clamp; commercial ceiling ~FL430

// Altimeter needle 3 (10,000s of feet) lives on the gauge controller and is
// off until its motor is physically wired. Keep this in sync with the
// ALT3_ENABLED in device/gauge_controller/config.h: this side only decides
// whether to log it, the controller side decides whether to drive it.
#define ALT3_ENABLED 0

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
// Vector-drawn boot splash + setup scene + animated connecting/radar screens
// (graphics.h). Set to 0 to fall back to the plain text status screens —
// no other change needed anywhere.
#define UI_CREATIVE_SCREENS 1

// Per-frame gauge target logging to serial (bench-testing aid). Also relays
// the gauge controller's own log lines to this board's USB serial, since the
// controller's UART0 stays free for its own monitor but is rarely attached.
// Set to 0 for production — it runs on the WS hot path.
#define GAUGE_SERIAL_LOG 1

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
#define AP_PASSWORD       "flight123"   // setup-portal AP password (min 8 chars)
// Pre-filled in the setup portal's server field; users normally only enter
// Wi-Fi credentials. Change/clear it in the form for bench testing on a LAN.
#define DEFAULT_SERVER_URL "https://flight-tracker-uid0.onrender.com"
#define WIFI_TIMEOUT_MS   20000
#define WS_PATH           "/ws/device"
#define WS_RECONNECT_MS   5000
