// Non-blocking cooperative driver for one 28BYJ-48 gauge needle, with
// limit-switch homing.
//
// MIRROR FILE — device/flight_tracker/stepper.h and
// device/gauge_controller/stepper.h must stay byte-identical. Arduino sketches
// cannot share headers across sketch directories, so this file is duplicated
// deliberately (same rationale as the vendored qrcodegen.c). Edit one, copy to
// the other, recompile BOTH sketches.
//
// Motion: at most one step per run() call, so five needles share one loop()
// with the display and WebSocket. Coils are de-energized once a needle reaches
// its target — the 28BYJ-48 gear train holds position unpowered (~1.2 W saved
// per motor).
//
// Homing: each needle has its own limit switch, mounted wherever it physically
// fits on that gauge, so the switch position is NOT the printed zero. home()
// seeks the switch, then walks a per-motor offset to the zero mark. Homing is
// blocking by design (it only ever runs at boot or on an explicit re-home
// request) but pumps a caller-supplied callback so the host stays responsive.
#pragma once
#include <Arduino.h>
#include "config.h"

// Switch wiring contract: normally-open, one side to the pin, other to GND.
// Closed (needle at the switch) therefore reads LOW.
#define HOME_ACTIVE_LEVEL LOW

class GaugeStepper {
 public:
  // wraps=true  -> circular scale (heading, altimeter 100s/1000s needles):
  //                moves the shortest way around, position is modulo one rev.
  // wraps=false -> end-stop scale (speed, altimeter 10,000s needle).
  void begin(uint8_t in1, uint8_t in2, uint8_t in3, uint8_t in4, bool wraps) {
    pins_[0] = in1; pins_[1] = in2; pins_[2] = in3; pins_[3] = in4;
    wraps_ = wraps;
    for (uint8_t p : pins_) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
    pos_ = target_ = 0;
    begun_ = true;
  }

  // pin < 0 disables homing for this needle: position is assumed 0 at
  // power-on (the pre-limit-switch behaviour) and home() succeeds trivially.
  //   dir         +1 / -1, which way to rotate while seeking the switch.
  //   offsetSteps signed steps to travel AFTER the switch trips, to land the
  //               needle on its printed zero. Per motor — the switch sits
  //               wherever the mechanics allowed.
  //   maxSteps    give-up threshold; a missing or dead switch must never spin
  //               a needle forever.
  //   pullup      false for ESP32 input-only pins 34-39, which have no
  //               internal pull-up — those need an external 10k to 3V3.
  void attachHome(int8_t pin, int8_t dir, long offsetSteps, long maxSteps,
                  bool pullup = true) {
    homePin_ = pin;
    homeDir_ = dir >= 0 ? 1 : -1;
    homeOffset_ = offsetSteps;
    homeMaxSteps_ = maxSteps;
    if (homePin_ >= 0) pinMode(homePin_, pullup ? INPUT_PULLUP : INPUT);
  }

  // Seek the limit switch, then offset to the printed zero. Returns false if
  // the switch never tripped within maxSteps — the needle is left where it is
  // and treated as zero, so a missing switch degrades to the old assume-zero
  // behaviour instead of bricking the gauge.
  bool home(void (*pump)() = nullptr) {
    if (!begun_) return true;
    if (homePin_ < 0) { pos_ = target_ = 0; homed_ = true; return true; }

    // Needle parked on its switch: back off until it releases, or homing
    // would "succeed" instantly at the wrong place.
    for (long i = 0; i < HOME_BACKOFF_MAX_STEPS && switchClosed(); i++) {
      stepBlocking(-homeDir_, pump);
    }

    bool found = false;
    for (long i = 0; i < homeMaxSteps_; i++) {
      stepBlocking(homeDir_, pump);
      if (switchClosed()) { found = true; break; }
    }

    if (found) {
      int8_t d = homeOffset_ >= 0 ? 1 : -1;
      for (long i = 0, n = labs(homeOffset_); i < n; i++) stepBlocking(d, pump);
    }

    release();
    pos_ = target_ = 0;
    homed_ = found;
    return found;
  }

  bool homed() const { return homed_; }

  // Absolute target in steps within one revolution [0..STEPS_PER_REV-1].
  // Wrapping needles resolve the shortest path from where they are; end-stop
  // needles clamp. This is the ONLY motion primitive, so a motor behaves
  // identically whichever board owns it.
  void moveToSteps(long stepsInRev) {
    if (!begun_) return;
    if (wraps_) {
      long t = ((stepsInRev % STEPS_PER_REV) + STEPS_PER_REV) % STEPS_PER_REV;
      long cur = ((pos_ % STEPS_PER_REV) + STEPS_PER_REV) % STEPS_PER_REV;
      long diff = t - cur;
      if (diff > STEPS_PER_REV / 2) diff -= STEPS_PER_REV;
      if (diff < -STEPS_PER_REV / 2) diff += STEPS_PER_REV;
      target_ = pos_ + diff;
    } else {
      target_ = constrain(stepsInRev, 0L, (long)STEPS_PER_REV);
    }
  }

  bool moving() const { return pos_ != target_; }

  // Advance at most one step if due. Returns true if it stepped.
  bool run(unsigned long nowUs) {
    if (!begun_) return false;  // disabled gauge: never touch its pins
    if (pos_ == target_) {
      if (energized_) { release(); }
      return false;
    }
    if (nowUs - lastStepUs_ < STEP_INTERVAL_US) return false;
    lastStepUs_ = nowUs;
    pos_ += (target_ > pos_) ? 1 : -1;
    writePhase(((pos_ % 4) + 4) % 4);
    energized_ = true;
    return true;
  }

 private:
  // Full-step 4-phase sequence, same coil order as the tested
  // Stepper(IN1, IN3, IN2, IN4) wiring.
  void writePhase(uint8_t ph) {
    static const uint8_t seq[4][4] = {
      {1, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 1}, {1, 0, 0, 1},
    };
    for (uint8_t i = 0; i < 4; i++) digitalWrite(pins_[i], seq[ph][i]);
  }
  void release() {
    for (uint8_t p : pins_) digitalWrite(p, LOW);
    energized_ = false;
  }

  // Three agreeing reads reject contact bounce and bus noise on the harness;
  // any disagreement returns "open" immediately (fail-safe: keep seeking).
  bool switchClosed() {
    for (uint8_t i = 0; i < 3; i++) {
      if (digitalRead(homePin_) != HOME_ACTIVE_LEVEL) return false;
      delayMicroseconds(200);
    }
    return true;
  }

  // One step at homing speed. pump() runs once per step so the host can keep
  // its screen animating / serial drained while a needle sweeps.
  void stepBlocking(int8_t dir, void (*pump)()) {
    pos_ += dir;
    writePhase(((pos_ % 4) + 4) % 4);
    energized_ = true;
    if (pump) pump();
    unsigned long t0 = micros();
    while (micros() - t0 < HOME_STEP_INTERVAL_US) { /* spin */ }
  }

  uint8_t pins_[4] = {0};
  bool begun_ = false;
  bool wraps_ = false;
  bool energized_ = false;
  bool homed_ = false;
  long pos_ = 0, target_ = 0;
  unsigned long lastStepUs_ = 0;

  int8_t homePin_ = -1;
  int8_t homeDir_ = 1;
  long homeOffset_ = 0;
  long homeMaxSteps_ = 0;
};
