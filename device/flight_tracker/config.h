// Hardware + behaviour configuration for the flight-tracker device.
#pragma once

#define FW_VERSION "1.0.0"

// ---------------------------------------------------------------------------
// Stepper pin map (28BYJ-48 + ULN2003, IN1..IN4 per motor).
//
// The 3.5" display's 8-bit parallel bus owns GPIO 4,5,6,7,15,16,17,18 (data)
// and 9,10,11,12 (RST/CS/DC/WR) — see TFT_eSPI User_Setup.h. NONE of the
// pins below may touch those, or the display freezes after the boot splash.
// Also reserved: 0 (BOOT btn), 19/20 (USB), 43/44 (UART0 serial monitor),
// 26..37 (flash/PSRAM). That leaves exactly 16 clean GPIOs -> 4 steppers.
// ---------------------------------------------------------------------------

// Speed gauge
#define SPD_IN1 1
#define SPD_IN2 2
#define SPD_IN3 3
#define SPD_IN4 8

// Heading gauge
#define HDG_IN1 13
#define HDG_IN2 14
#define HDG_IN3 21
#define HDG_IN4 38

// Altimeter needle 1 — 100s of feet (1 rev = 1,000 ft)
#define ALT1_IN1 39
#define ALT1_IN2 40
#define ALT1_IN3 41
#define ALT1_IN4 42

// Altimeter needle 2 — 1,000s of feet (1 rev = 10,000 ft)
// (45/46 are strapping pins: fine driving ULN2003 inputs, which float at boot)
#define ALT2_IN1 45
#define ALT2_IN2 46
#define ALT2_IN3 47
#define ALT2_IN4 48

// Altimeter needle 3 — 10,000s of feet (1 rev = 100,000 ft).
// No clean GPIOs remain for it. Set ALT3_ENABLED to 1 only after wiring it
// to pins you can spare: 43/44 kill the serial monitor, 19/20 kill native
// USB — or add an I2C GPIO expander (e.g. PCF8574) later.
#define ALT3_ENABLED 0
#define ALT3_IN1 43
#define ALT3_IN2 44
#define ALT3_IN3 19
#define ALT3_IN4 20

// Hold BOOT (GPIO0) for 3 s while running to wipe Wi-Fi config -> setup portal.
#define RESET_BTN_PIN 0

// ---------------------------------------------------------------------------
// Gauge scales (see PLAN.md §3)
// ---------------------------------------------------------------------------
#define STEPS_PER_REV     2048      // 28BYJ-48 full-step (validated in stepper_test)
#define STEP_INTERVAL_US  2500      // ~2.9 ms/step is the tested 10 rpm; 2.5 ms is safe

#define SPEED_MIN_KT      0
#define SPEED_MAX_KT      500       // print a 0-500 kt face; C172 dial (40-200) is too small
#define SPEED_SWEEP_DEG   270.0

#define ALT_MAX_FT        45000     // clamp; commercial ceiling ~FL430

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
// Vector-drawn boot splash + setup scene + animated connecting/radar screens
// (graphics.h). Set to 0 to fall back to the plain text status screens —
// no other change needed anywhere.
#define UI_CREATIVE_SCREENS 1

// Per-frame gauge target logging to serial (bench-testing aid before the
// steppers are wired). Set to 0 for production — it runs on the WS hot path.
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
