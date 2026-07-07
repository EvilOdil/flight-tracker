// Non-blocking cooperative driver for five 28BYJ-48 gauges.
//
// All motors advance at most one step per run() call (call run() every loop()).
// Coils are de-energized once a needle reaches its target — the 28BYJ-48 gear
// train holds position unpowered, saving ~1.2 W per motor.
//
// Homing: needles are assumed to sit on their zero mark at power-on; positions
// are tracked in software from there (PLAN.md §3).
#pragma once
#include <Arduino.h>
#include "config.h"

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
  }

  // Target as a fraction of one full revolution [0..1) for wrapping gauges,
  // clamped [0..1] for end-stop gauges.
  void moveToFraction(float frac) {
    if (wraps_) {
      frac -= floorf(frac);
      long t = lroundf(frac * STEPS_PER_REV) % STEPS_PER_REV;
      // Shortest path from current (mod one rev) position.
      long cur = ((pos_ % STEPS_PER_REV) + STEPS_PER_REV) % STEPS_PER_REV;
      long diff = t - cur;
      if (diff > STEPS_PER_REV / 2) diff -= STEPS_PER_REV;
      if (diff < -STEPS_PER_REV / 2) diff += STEPS_PER_REV;
      target_ = pos_ + diff;
    } else {
      frac = constrain(frac, 0.0f, 1.0f);
      target_ = lroundf(frac * STEPS_PER_REV);
    }
  }

  bool moving() const { return pos_ != target_; }

  // Advance at most one step if due. Returns true if it stepped.
  bool run(unsigned long nowUs) {
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

  uint8_t pins_[4] = {0};
  bool wraps_ = false;
  bool energized_ = false;
  long pos_ = 0, target_ = 0;
  unsigned long lastStepUs_ = 0;
};

class Gauges {
 public:
  void begin() {
    speed_.begin(SPD_IN1, SPD_IN2, SPD_IN3, SPD_IN4, false);
    heading_.begin(HDG_IN1, HDG_IN2, HDG_IN3, HDG_IN4, true);
    alt100_.begin(ALT1_IN1, ALT1_IN2, ALT1_IN3, ALT1_IN4, true);
    alt1k_.begin(ALT2_IN1, ALT2_IN2, ALT2_IN3, ALT2_IN4, true);
    alt10k_.begin(ALT3_IN1, ALT3_IN2, ALT3_IN3, ALT3_IN4, false);
  }

  void set(long altFt, int gsKt, int trackDeg) {
    altFt = constrain(altFt, 0L, (long)ALT_MAX_FT);
    // Speed: linear over the printed sweep. Fraction of full motor rev.
    float spdFrac = (float)(constrain(gsKt, SPEED_MIN_KT, SPEED_MAX_KT) - SPEED_MIN_KT)
                    / (SPEED_MAX_KT - SPEED_MIN_KT) * (SPEED_SWEEP_DEG / 360.0f);
    speed_.moveToFraction(spdFrac);
    heading_.moveToFraction(trackDeg / 360.0f);
    // Classic 3-needle sensitive altimeter.
    alt100_.moveToFraction((altFt % 1000) / 1000.0f);
    alt1k_.moveToFraction((altFt % 10000) / 10000.0f);
    alt10k_.moveToFraction(altFt / 100000.0f);
  }

  void zero() { set(0, 0, 0); }

  void run() {
    unsigned long nowUs = micros();
    speed_.run(nowUs);
    heading_.run(nowUs);
    alt100_.run(nowUs);
    alt1k_.run(nowUs);
    alt10k_.run(nowUs);
  }

 private:
  GaugeStepper speed_, heading_, alt100_, alt1k_, alt10k_;
};
