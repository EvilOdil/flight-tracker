// Hardware + behaviour configuration for the flight-tracker device.
#pragma once

#define FW_VERSION "1.0.0"

// ---------------------------------------------------------------------------
// Stepper pin map (28BYJ-48 + ULN2003, IN1..IN4 per motor).
//
// IMPORTANT: your 3.5" display in 8-bit parallel mode claims a large set of
// pins (see TFT_eSPI User_Setup.h). Make sure NONE of the pins below collide
// with the display pins — adjust to match your actual wiring.
// ---------------------------------------------------------------------------

// Speed gauge (matches the tested stepper_test wiring)
#define SPD_IN1 4
#define SPD_IN2 5
#define SPD_IN3 6
#define SPD_IN4 7

// Heading gauge
#define HDG_IN1 15
#define HDG_IN2 16
#define HDG_IN3 17
#define HDG_IN4 18

// Altimeter needle 1 — 100s of feet (1 rev = 1,000 ft)
#define ALT1_IN1 8
#define ALT1_IN2 3
#define ALT1_IN3 46
#define ALT1_IN4 9

// Altimeter needle 2 — 1,000s of feet (1 rev = 10,000 ft)
#define ALT2_IN1 10
#define ALT2_IN2 11
#define ALT2_IN3 12
#define ALT2_IN4 13

// Altimeter needle 3 — 10,000s of feet (1 rev = 100,000 ft)
#define ALT3_IN1 14
#define ALT3_IN2 21
#define ALT3_IN3 47
#define ALT3_IN4 48

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
// Networking
// ---------------------------------------------------------------------------
#define AP_PASSWORD       "flight123"   // setup-portal AP password (min 8 chars)
#define WIFI_TIMEOUT_MS   20000
#define WS_PATH           "/ws/device"
#define WS_RECONNECT_MS   5000
