// Hardware + behaviour configuration for the gauge controller (ESP32-WROOM-32).
#pragma once

#define GC_VERSION "1.0.0"

// ---------------------------------------------------------------------------
// Pin budget (38-pin ESP32-WROOM-32 devkit)
//
// Output-capable and broken out: 0,1,2,3,4,5,12..19,21,22,23,25,26,27,32,33
// (22 pins). Input-only: 34,35,36,39 — usable for limit switches and UART RX,
// never for a coil. GPIO6..11 are the SPI flash.
//
// Deliberately NOT used for coils:
//   GPIO0   devkit boot pull-up + a ULN2003 input clamps it to ~1.8 V, below
//           the 2.475 V HIGH threshold -> the board boots into download mode.
//   GPIO12  MTDI strap: must read LOW at boot or the flash runs at 1.8 V.
//   GPIO1/3 UART0 — kept free so this board keeps a USB serial monitor.
//
// Budget with the speed gauge moved to the S3: 16 coils + 2 UART = 18 of the
// 19 safe output pins, and all four limit switches land on the input-only
// pins that can't drive coils anyway. Fits with one output pin spare.
// ---------------------------------------------------------------------------

// Heading gauge — continuous 0..360 deg.
#define HDG_IN1 13
#define HDG_IN2 14
#define HDG_IN3 18
#define HDG_IN4 19

// Altimeter needle 1 — 100s of feet (1 rev = 1,000 ft)
#define ALT1_IN1 21
#define ALT1_IN2 22
#define ALT1_IN3 23
#define ALT1_IN4 25

// Altimeter needle 2 — 1,000s of feet (1 rev = 10,000 ft)
#define ALT2_IN1 27
#define ALT2_IN2 26
#define ALT2_IN3 33
#define ALT2_IN4 32

// Altimeter needle 3 — 10,000s of feet (1 rev = 100,000 ft).
// Not wired yet. GPIO2 and GPIO15 are strapping pins parked on this motor on
// purpose: it is the one that may never exist. Both are safe with a ULN2003
// (its input floats low when idle, which is what each of those pins wants at
// boot — 15 low only silences the ROM boot log). Set to 1 once it is wired,
// and set ALT3_ENABLED to 1 in the S3's config.h too so the logs agree.
#define ALT3_ENABLED 0
#define ALT3_IN1 4
#define ALT3_IN2 5
#define ALT3_IN3 15
#define ALT3_IN4 2

// ---------------------------------------------------------------------------
// Limit switches — one per needle, normally-open to GND, so closed reads LOW.
//
// All four sit on input-only pins (34..39). Those have NO internal pull-up:
// each needs an EXTERNAL 10k from the pin to 3V3 or it floats and homing
// either trips instantly or never. This is the single most likely wiring
// mistake on this board — check it first if a needle homes to the wrong spot.
//
// Set a pin to -1 to disable homing for that needle (assumed at zero on
// power-on, the pre-limit-switch behaviour).
// ---------------------------------------------------------------------------
#define GAUGE_HOME_INTERNAL_PULLUP 0

#define HDG_HOME_PIN  34
#define ALT1_HOME_PIN 35
#define ALT2_HOME_PIN 36
#define ALT3_HOME_PIN 39

// Which way each needle rotates while seeking its switch (+1 / -1), and how
// many steps it must then travel to land on the printed zero. Every one of
// these is a mechanical fact of ONE gauge — the switch is mounted wherever it
// physically fit, not at zero. Measure them on the bench; the procedure is in
// ../gauge_link_protocol.md ("Calibrating a needle").
#define HDG_HOME_DIR      1
#define HDG_HOME_OFFSET   0
#define ALT1_HOME_DIR     1
#define ALT1_HOME_OFFSET  0
#define ALT2_HOME_DIR     1
#define ALT2_HOME_OFFSET  0
#define ALT3_HOME_DIR     1
#define ALT3_HOME_OFFSET  0

// Give-up threshold: a full revolution plus margin. A switch that has not
// tripped by then is missing, miswired, or dead — never spin a needle forever.
#define HOME_MAX_STEPS (STEPS_PER_REV + STEPS_PER_REV / 8)

// ---------------------------------------------------------------------------
// UART link to the ESP32-S3 (Serial2 defaults).
//
// Cross-connect: WROOM RX (16) <- S3 TX (21), WROOM TX (17) -> S3 RX (14),
// plus a common GND between the boards. Both are 3.3 V logic — no level
// shifter. Protocol contract: ../gauge_link_protocol.md.
// ---------------------------------------------------------------------------
#define GAUGE_LINK_RX    16
#define GAUGE_LINK_TX    17
#define GAUGE_LINK_BAUD  115200

// Needle slots in the link frame. Order is protocol, not preference — it must
// match the S3's gauge_link.h. Slot 0 (speed) is driven by the S3 and is
// always ignored here.
#define GAUGE_MOTOR_COUNT 5
#define MOTOR_SPEED   0
#define MOTOR_HEADING 1
#define MOTOR_ALT100  2
#define MOTOR_ALT1K   3
#define MOTOR_ALT10K  4

// ---------------------------------------------------------------------------
// Motion (must match the S3's config.h — the two boards share stepper.h)
// ---------------------------------------------------------------------------
#define STEPS_PER_REV     2048      // 28BYJ-48 full-step (validated in stepper_test)
#define STEP_INTERVAL_US  2500      // ~2.9 ms/step is the tested 10 rpm; 2.5 ms is safe

// Homing runs slower than normal motion: a needle may be driving toward a hard
// mechanical stop, and a missed switch read at speed overshoots further.
#define HOME_STEP_INTERVAL_US   3500
#define HOME_BACKOFF_MAX_STEPS  300

// Echo link traffic and homing progress to this board's own USB serial
// monitor. Log lines also go up the link as "# ..." frames, which the S3
// reprints — so a monitor here is optional during bring-up.
#define GC_SERIAL_LOG 1
