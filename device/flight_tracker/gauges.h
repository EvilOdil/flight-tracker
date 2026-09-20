// Gauge facade for the S3 side of the two-board split.
//
// Owns the dial-face maths for ALL five needles (PLAN.md §3 lives here, in one
// place), drives the speed needle directly, and pushes every needle's target
// over the UART link. The gauge controller applies the ones it owns.
//
// Needle indices are the link protocol's slot order and must not be reordered:
//   0 speed | 1 heading | 2 alt 100s | 3 alt 1,000s | 4 alt 10,000s
#pragma once
#include <Arduino.h>
#include "config.h"
#include "stepper.h"
#include "gauge_link.h"

class Gauges {
 public:
  void begin() {
    link_.begin();
    speed_.begin(SPD_IN1, SPD_IN2, SPD_IN3, SPD_IN4, false);
    speed_.attachHome(SPD_HOME_PIN, SPD_HOME_DIR, SPD_HOME_OFFSET,
                      SPD_HOME_MAX_STEPS);
  }

  // Drive the speed needle onto its zero mark. Blocking (worst case ~8 s at
  // HOME_STEP_INTERVAL_US), so call it from setup() only; pump() is called
  // once per step to keep the boot screen animating. The controller homes its
  // own four needles in parallel with this, on its own power-on.
  bool home(void (*pump)() = nullptr) {
    bool ok = speed_.home(pump);
    if (!ok) Serial.println("[gauges] speed needle: limit switch never tripped");
    return ok;
  }

  void set(long altFt, int gsKt, int trackDeg) {
    altFt = constrain(altFt, 0L, (long)ALT_MAX_FT);
    gsKt = constrain(gsKt, SPEED_MIN_KT, SPEED_MAX_KT);

    // Speed: linear over the printed sweep, as a fraction of a full motor rev.
    float spdFrac = (float)(gsKt - SPEED_MIN_KT) / (SPEED_MAX_KT - SPEED_MIN_KT)
                    * (SPEED_SWEEP_DEG / 360.0f);
    long t[GAUGE_MOTOR_COUNT];
    t[0] = lroundf(spdFrac * STEPS_PER_REV);
    t[1] = lroundf(trackDeg / 360.0f * STEPS_PER_REV);
    // Classic 3-needle sensitive altimeter.
    t[2] = lroundf((altFt % 1000) / 1000.0f * STEPS_PER_REV);
    t[3] = lroundf((altFt % 10000) / 10000.0f * STEPS_PER_REV);
    t[4] = lroundf(altFt / 100000.0f * STEPS_PER_REV);

    apply(t);
    memcpy(last_, t, sizeof(last_));
    haveLast_ = true;

    // Bench-testing aid: what each needle should show, in dial terms and in
    // motor steps from zero (2048/rev) — compare against the real needles and
    // calibrate faces/ranges from this.
#if GAUGE_SERIAL_LOG
    Serial.printf("[gauges] alt=%ld ft gs=%d kt trk=%d deg\n", altFt, gsKt, trackDeg);
    Serial.printf("[gauges]   speed  %3d kt   -> %4ld steps (%5.1f deg on 270 sweep)  [S3]\n",
                  gsKt, t[0], spdFrac * 360.0f);
    Serial.printf("[gauges]   heading %3d deg -> %4ld steps  [ctrl]\n", trackDeg, t[1]);
    Serial.printf("[gauges]   alt 100s=%4ld  1000s=%4ld  10k=%4ld steps  [ctrl]%s\n",
                  t[2], t[3], t[4], ALT3_ENABLED ? "" : " (needle 3 disabled)");
#endif
  }

  void zero() { set(0, 0, 0); }

  void run() {
    speed_.run(micros());
    // The controller reboots independently (its own power rail, its own reset
    // button). When it comes back it re-homes and announces itself; re-send the
    // current needle targets or its four gauges would sit at zero until the
    // next flight update arrives from the backend.
    if (link_.poll() && haveLast_) {
      Serial.println("[gauges] controller rebooted -> re-sending needle targets");
      link_.sendTargets(last_);
    }
  }

  // Bench calibration: drive needles to raw step positions, bypassing the
  // dial-face maths. -1 leaves a needle where it is. Used by the serial
  // console to measure each needle's post-switch offset — see
  // ../gauge_link_protocol.md, "Calibrating a needle".
  void setSteps(const long* t) {
    if (t[0] >= 0) speed_.moveToSteps(t[0]);
    link_.sendTargets(t);
    memcpy(last_, t, sizeof(last_));
    haveLast_ = true;
  }

  // Ask the controller to re-home its four needles (the speed needle homes
  // from setup() only — re-homing it mid-flight would block the display loop).
  void rehomeController() { link_.sendRehome(); }

 private:
  void apply(const long* t) {
    speed_.moveToSteps(t[0]);
    link_.sendTargets(t);
  }

  GaugeStepper speed_;
  GaugeLink link_;
  long last_[GAUGE_MOTOR_COUNT] = {0};
  bool haveLast_ = false;
};
