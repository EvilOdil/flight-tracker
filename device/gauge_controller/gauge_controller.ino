// Flight Tracker gauge controller — ESP32-WROOM-32
//
// Drives four of the five 28BYJ-48 gauge needles (heading + altimeter x3),
// each with its own limit switch for homing. The fifth needle (speed) is
// driven by the ESP32-S3, which also owns the display, Wi-Fi and all dial-face
// maths — this board is a dumb motor server that knows only pins, homing
// offsets and STEPS_PER_REV.
//
// The split exists because the S3's 8-bit display bus, USB and flash/PSRAM
// leave 16 clean GPIOs, and five ULN2003 motors need 20.
//
// Link: UART to the S3, line-oriented ASCII. Contract in
// ../gauge_link_protocol.md — it is a compatibility contract between two
// separately-flashed boards, so only ADD optional trailing fields.
//
// Libraries: none. Board: "ESP32 Dev Module".

#include <Arduino.h>
#include "config.h"
#include "stepper.h"

struct MotorCfg {
  const char* name;
  uint8_t in1, in2, in3, in4;
  bool wraps;        // circular scale (shortest-path) vs end-stop scale
  int8_t homePin;    // -1 = no switch, assume zero at power-on
  int8_t homeDir;
  long homeOffset;
  bool owned;        // false = another board drives this needle
};

// Indexed by the link frame's needle slots — do not reorder.
static const MotorCfg MOTORS[GAUGE_MOTOR_COUNT] = {
  {"speed",   0, 0, 0, 0,                                 false, -1,            1,                0,                false},
  {"heading", HDG_IN1,  HDG_IN2,  HDG_IN3,  HDG_IN4,      true,  HDG_HOME_PIN,  HDG_HOME_DIR,  HDG_HOME_OFFSET,  true},
  {"alt100",  ALT1_IN1, ALT1_IN2, ALT1_IN3, ALT1_IN4,     true,  ALT1_HOME_PIN, ALT1_HOME_DIR, ALT1_HOME_OFFSET, true},
  {"alt1k",   ALT2_IN1, ALT2_IN2, ALT2_IN3, ALT2_IN4,     true,  ALT2_HOME_PIN, ALT2_HOME_DIR, ALT2_HOME_OFFSET, true},
  {"alt10k",  ALT3_IN1, ALT3_IN2, ALT3_IN3, ALT3_IN4,     false, ALT3_HOME_PIN, ALT3_HOME_DIR, ALT3_HOME_OFFSET, ALT3_ENABLED},
};

// Slot 0 is never begun, so its run()/moveToSteps() are no-ops and it can
// never touch a pin — the speed needle's GPIOs live on the other board.
static GaugeStepper motors[GAUGE_MOTOR_COUNT];

static char rxBuf[96];
static uint8_t rxLen = 0;

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

static void linkSend(const char* line) {
  Serial2.print(line);
  Serial2.print('\n');
}

// Log lines travel up the link as "# ..." so they surface on the S3's USB
// serial monitor; this board's own monitor is optional during bring-up.
static void linkLog(const char* fmt, ...) {
  char msg[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  Serial2.printf("# %s\n", msg);
#if GC_SERIAL_LOG
  Serial.printf("[gc] %s\n", msg);
#endif
}

static void applyTargets(const char* args) {
  char* end;
  const char* p = args;
  for (uint8_t i = 0; i < GAUGE_MOTOR_COUNT; i++) {
    long v = strtol(p, &end, 10);
    if (end == p) break;   // short frame: apply what arrived, ignore the rest
    p = end;
    // -1 means "no value for this needle"; a needle this board does not own
    // is ignored even when a value is present, so both boards can be handed
    // the identical frame.
    if (v >= 0 && MOTORS[i].owned) motors[i].moveToSteps(v);
  }
}

// Drain the link without acting on it. Used during a homing sweep: the S3 may
// keep pushing targets, but a needle mid-homing has no valid position yet, and
// the S3 re-sends everything when this board announces itself ready.
static void drainLink() {
  while (Serial2.available()) Serial2.read();
}

// ---------------------------------------------------------------------------
// Homing
// ---------------------------------------------------------------------------

// One needle at a time: keeps peak current down and makes a wrong-switch
// wiring fault obvious (only one needle should ever be moving).
static void homeAll() {
  for (uint8_t i = 0; i < GAUGE_MOTOR_COUNT; i++) {
    if (!MOTORS[i].owned) continue;
    linkLog("homing %s ...", MOTORS[i].name);
    if (motors[i].home(&drainLink)) {
      linkLog("homing %s ok", MOTORS[i].name);
    } else {
      // Fail soft: the needle stays where it is and is treated as zero, so a
      // missing switch degrades to the old assume-zero behaviour rather than
      // taking the gauge out of service.
      Serial2.printf("E %u nosw\n", i);
#if GC_SERIAL_LOG
      Serial.printf("[gc] homing %s FAILED: switch never tripped (check the "
                    "external 10k pull-up on GPIO%d)\n",
                    MOTORS[i].name, MOTORS[i].homePin);
#endif
    }
  }
  drainLink();
  linkSend("R");   // ready — the S3 answers by re-sending current targets
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial2.begin(GAUGE_LINK_BAUD, SERIAL_8N1, GAUGE_LINK_RX, GAUGE_LINK_TX);
#if GC_SERIAL_LOG
  Serial.printf("\n[gc] flight-tracker gauge controller %s\n", GC_VERSION);
#endif

  for (uint8_t i = 0; i < GAUGE_MOTOR_COUNT; i++) {
    if (!MOTORS[i].owned) continue;
    motors[i].begin(MOTORS[i].in1, MOTORS[i].in2, MOTORS[i].in3, MOTORS[i].in4,
                    MOTORS[i].wraps);
    motors[i].attachHome(MOTORS[i].homePin, MOTORS[i].homeDir,
                         MOTORS[i].homeOffset, HOME_MAX_STEPS,
                         GAUGE_HOME_INTERNAL_PULLUP);
  }

  homeAll();
}

// One line parser for both inputs: the link, and this board's own USB serial
// monitor (so the four needles can be jogged during bench calibration without
// the S3 attached — see ../gauge_link_protocol.md).
static void handleByte(char c) {
  if (c == '\r') return;
  if (c != '\n') {
    if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = c;
    return;
  }
  rxBuf[rxLen] = '\0';
  uint8_t len = rxLen;
  rxLen = 0;
  if (!len) return;
  switch (rxBuf[0]) {
    case 'T': applyTargets(rxBuf + 1); break;
    case 'H': homeAll(); break;
    case 'P': linkSend("O"); break;
    default:  break;   // unknown verb: ignore, never stall the link
  }
}

void loop() {
  while (Serial2.available()) handleByte((char)Serial2.read());
  while (Serial.available())  handleByte((char)Serial.read());

  unsigned long nowUs = micros();
  for (uint8_t i = 0; i < GAUGE_MOTOR_COUNT; i++) motors[i].run(nowUs);
}
