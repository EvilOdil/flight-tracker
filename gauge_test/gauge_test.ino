// Gauge bench test — flashes to the ESP32-S3 ONLY.
//
// Exercises all five needles once: the speed needle directly (it is wired to
// this board) and the other four by pushing "T" frames down the UART link to
// the gauge controller. The controller firmware is NOT modified and NOT
// reflashed — this sketch is just a different client speaking the same
// protocol the real firmware speaks (device/gauge_link_protocol.md).
//
// Deliberately standalone: it does NOT include config.h / stepper.h from
// device/flight_tracker/. A test that reuses the code under test cannot fail
// when that code is wrong, and the pin map is exactly the thing being checked.
// The constants below are therefore copied, and must match
// device/flight_tracker/config.h — if you change a pin there, change it here.
//
// Board: "ESP32S3 Dev Module", USB CDC On Boot = Enabled.
//   ~/bin/arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc gauge_test
//
// Wiring it expects (device/PINOUT.md):
//   S3 GPIO21 -> WROOM GPIO16 (RX)      S3 GPIO14 <- WROOM GPIO17 (TX)
//   common GND between the boards       speed motor on S3 GPIO 1/2/42/41
//
// What "pass" looks like: each needle, one at a time, sweeps 0 -> 90 -> 180 ->
// 270 -> back to 0 while the serial monitor names it. Any needle that stays
// still, buzzes without turning, or turns while a DIFFERENT one is named is a
// wiring fault at that motor's ULN2003 header.

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Copied from device/flight_tracker/config.h — keep in sync.
// ---------------------------------------------------------------------------
#define SPD_IN1 1
#define SPD_IN2 2
#define SPD_IN3 42
#define SPD_IN4 41

#define GAUGE_LINK_TX    21
#define GAUGE_LINK_RX    14
#define GAUGE_LINK_BAUD  115200

#define STEPS_PER_REV    2048
#define STEP_INTERVAL_US 2500

#define GAUGE_MOTOR_COUNT 5

// Slot order is the link frame's needle order — do not reorder.
static const char* MOTOR_NAME[GAUGE_MOTOR_COUNT] = {
  "speed (S3)", "heading", "alt 100s", "alt 1000s", "alt 10000s",
};

// The four quarter-turn stops each needle visits, ending back at zero.
static const long SWEEP[] = {STEPS_PER_REV / 4, STEPS_PER_REV / 2,
                             (STEPS_PER_REV * 3) / 4, 0};
static const uint8_t SWEEP_LEN = sizeof(SWEEP) / sizeof(SWEEP[0]);

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

static bool sawReady = false;   // controller announced "R" (homed)
static bool sawPong  = false;   // controller answered "P" with "O"
static char rxBuf[96];
static uint8_t rxLen = 0;

static void handleLine(const char* line) {
  switch (line[0]) {
    case 'R': sawReady = true; Serial.println("  <- R  controller homed and ready"); break;
    case 'O': sawPong  = true; Serial.println("  <- O  pong (link is good in BOTH directions)"); break;
    case 'E': Serial.printf("  <- %s  homing failed on that needle (switch never tripped)\n", line); break;
    case '#': Serial.printf("  <- [gc]%s\n", line + 1); break;
    default:  Serial.printf("  <- %s\n", line); break;
  }
}

// Call this often — including inside every motion loop, so the controller's
// log lines appear as they happen instead of in a burst at the end.
static void pumpLink() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = c;
      continue;
    }
    rxBuf[rxLen] = '\0';
    uint8_t len = rxLen;
    rxLen = 0;
    if (len) handleLine(rxBuf);
  }
}

// Every frame carries all five slots; -1 means "no value for this needle", so
// naming one needle leaves the other four exactly where they are.
static void sendTargets(long s0, long s1, long s2, long s3, long s4) {
  char out[64];
  int n = snprintf(out, sizeof(out), "T %ld %ld %ld %ld %ld\n", s0, s1, s2, s3, s4);
  Serial1.write((const uint8_t*)out, n);
  Serial.printf("  -> %.*s\n", n - 1, out);
}

static void sendSlot(uint8_t slot, long steps) {
  long v[GAUGE_MOTOR_COUNT] = {-1, -1, -1, -1, -1};
  v[slot] = steps;
  sendTargets(v[0], v[1], v[2], v[3], v[4]);
}

static void sendVerb(const char* verb) {
  Serial1.printf("%s\n", verb);
  Serial.printf("  -> %s\n", verb);
}

// Bounded wait that keeps the link drained. Returns true if *flag went true.
static bool waitFor(bool* flag, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    pumpLink();
    if (*flag) return true;
    delay(2);
  }
  return false;
}

static void waitMs(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { pumpLink(); delay(2); }
}

// ---------------------------------------------------------------------------
// Speed needle — driven straight off this board's pins.
//
// Blocking on purpose: a bench test has nothing else to do, and one needle at
// a time is what makes a wrong-header fault visible.
// ---------------------------------------------------------------------------

static const uint8_t SPD_PINS[4] = {SPD_IN1, SPD_IN2, SPD_IN3, SPD_IN4};
// Full-step 4-phase sequence in the same coil order the firmware uses.
static const uint8_t PHASE[4][4] = {
  {1, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 1}, {1, 0, 0, 1},
};
static long spdPos = 0;

static void spdRelease() {
  for (uint8_t i = 0; i < 4; i++) digitalWrite(SPD_PINS[i], LOW);
}

static void spdMoveTo(long target) {
  while (spdPos != target) {
    spdPos += (target > spdPos) ? 1 : -1;
    uint8_t ph = (uint8_t)(((spdPos % 4) + 4) % 4);
    for (uint8_t i = 0; i < 4; i++) digitalWrite(SPD_PINS[i], PHASE[ph][i]);
    delayMicroseconds(STEP_INTERVAL_US);
    if ((spdPos & 0x3F) == 0) pumpLink();
  }
  spdRelease();   // coils off: the 28BYJ-48 gear train holds position unpowered
}

// ---------------------------------------------------------------------------
// The test
// ---------------------------------------------------------------------------

static void testMotor(uint8_t slot) {
  Serial.printf("\n[%u/%u] %s — 0 -> 90 -> 180 -> 270 -> 0 deg\n",
                slot + 1, GAUGE_MOTOR_COUNT, MOTOR_NAME[slot]);
  if (slot == 4) {
    Serial.println("      note: only moves if ALT3_ENABLED is 1 in "
                   "device/gauge_controller/config.h");
  }

  long from = 0;
  for (uint8_t i = 0; i < SWEEP_LEN; i++) {
    long to = SWEEP[i];
    if (slot == 0) {
      spdMoveTo(to);
    } else {
      sendSlot(slot, to);
      // No position feedback in the protocol, so wait out the worst-case
      // travel: a wrapping needle takes the short way round, never more than
      // half a revolution, plus slack for the controller's own loop.
      long steps = labs(to - from);
      if (steps > STEPS_PER_REV / 2) steps = STEPS_PER_REV - steps;
      waitMs((uint32_t)(steps * (STEP_INTERVAL_US / 1000.0f)) + 400);
    }
    from = to;
  }
  Serial.printf("      done — %s should be back at zero\n", MOTOR_NAME[slot]);
}

static void runAll() {
  Serial.println("\n=== sweeping all five needles, one at a time ===");
  for (uint8_t i = 0; i < GAUGE_MOTOR_COUNT; i++) testMotor(i);
  Serial.println("\n=== sweep complete ===");
  Serial.println("Every needle should have moved exactly once, in the order "
                 "printed above, and finished at zero.");
  printMenu();
}

static void printMenu() {
  Serial.println("\nCommands (type one character, then Enter):");
  Serial.println("  a    sweep all five needles again");
  Serial.println("  0-4  sweep one needle (0 speed, 1 heading, 2 alt100, "
                 "3 alt1k, 4 alt10k)");
  Serial.println("  h    ask the controller to re-home its four needles");
  Serial.println("  p    ping the controller (proves the link both ways)");
  Serial.println("  z    send every needle to zero");
}

static void console() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') continue;
    switch (c) {
      case 'a': runAll(); break;
      case '0': case '1': case '2': case '3': case '4':
        testMotor((uint8_t)(c - '0'));
        printMenu();
        break;
      case 'h':
        Serial.println("\nre-homing the controller's needles "
                       "(this takes a few seconds per needle) ...");
        sawReady = false;
        sendVerb("H");
        if (!waitFor(&sawReady, 60000)) Serial.println("  !! no R came back");
        break;
      case 'p':
        sawPong = false;
        sendVerb("P");
        if (!waitFor(&sawPong, 1000)) Serial.println("  !! no O came back");
        break;
      case 'z':
        Serial.println("\nall needles -> zero");
        spdMoveTo(0);
        sendTargets(-1, 0, 0, 0, 0);
        break;
      default:
        Serial.printf("unknown command '%c'\n", c);
        printMenu();
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Iron rule: without this, Serial.print blocks whenever no monitor is
  // attached and the board looks frozen.
  Serial.setTxTimeoutMs(0);
#endif
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  Serial1.begin(GAUGE_LINK_BAUD, SERIAL_8N1, GAUGE_LINK_RX, GAUGE_LINK_TX);

  for (uint8_t i = 0; i < 4; i++) {
    pinMode(SPD_PINS[i], OUTPUT);
    digitalWrite(SPD_PINS[i], LOW);
  }

  Serial.println("\n\n=== flight-tracker gauge bench test (S3 side) ===");
  Serial.printf("speed motor : GPIO %d/%d/%d/%d\n", SPD_IN1, SPD_IN2, SPD_IN3, SPD_IN4);
  Serial.printf("link        : TX GPIO%d -> WROOM RX, RX GPIO%d <- WROOM TX, "
                "%d baud\n", GAUGE_LINK_TX, GAUGE_LINK_RX, GAUGE_LINK_BAUD);
  Serial.println("the controller board is NOT reflashed for this test\n");

  // The controller homes its needles at ITS power-on and ignores the link
  // while it does, so find out which side of that we are on: a pong means it
  // is already up, otherwise it may still be sweeping.
  Serial.println("checking the link ...");
  sendVerb("P");
  if (!waitFor(&sawPong, 1000)) {
    Serial.println("  no pong yet — it may be mid-homing, waiting for R "
                   "(up to 60 s) ...");
    if (waitFor(&sawReady, 60000)) {
      sawPong = false;
      sendVerb("P");
      waitFor(&sawPong, 1000);
    }
  }
  if (!sawPong) {
    Serial.println("  !! NO REPLY from the gauge controller.");
    Serial.println("     Check: S3 GPIO21 -> WROOM GPIO16, S3 GPIO14 <- WROOM "
                   "GPIO17, common GND,");
    Serial.println("     WROOM powered and flashed with device/gauge_controller.");
    Serial.println("     The speed needle is on this board and will be tested "
                   "anyway.");
  }

  runAll();
}

void loop() {
  pumpLink();
  console();
}
