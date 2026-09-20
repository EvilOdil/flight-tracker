// UART link from the S3 to the gauge controller (ESP32-WROOM-32).
//
// Line-oriented ASCII so the whole link can be read with a serial monitor
// during bring-up. Full contract: device/gauge_link_protocol.md — it is a
// compatibility contract between two separately-flashed boards, so only ADD
// optional trailing fields; never rename or repurpose one.
//
//   S3 -> controller   T <s0> <s1> <s2> <s3> <s4>   needle targets, steps
//                      H                            re-home every owned needle
//                      P                            ping
//   controller -> S3   R                            homed and ready
//                      E <motor> <code>             homing failed on a needle
//                      O                            pong
//                      # <text>                     log line, relayed to USB
//
// The frame always carries all five needle slots regardless of which board
// drives which motor; -1 means "no value". Moving a motor between boards is
// therefore a pin-map change on two config.h files, not a protocol change.
#pragma once
#include <Arduino.h>
#include "config.h"

#define GAUGE_MOTOR_COUNT 5   // 0 speed, 1 heading, 2 alt100, 3 alt1k, 4 alt10k

class GaugeLink {
 public:
  void begin() {
    Serial1.begin(GAUGE_LINK_BAUD, SERIAL_8N1, GAUGE_LINK_RX, GAUGE_LINK_TX);
  }

  void sendTargets(const long* steps) {
    char out[64];
    int n = snprintf(out, sizeof(out), "T %ld %ld %ld %ld %ld\n",
                     steps[0], steps[1], steps[2], steps[3], steps[4]);
    Serial1.write((const uint8_t*)out, n);
  }

  void sendRehome() { Serial1.write((const uint8_t*)"H\n", 2); }

  // Call every loop(). Returns true when the controller has just announced
  // itself ready — it (re)booted and re-homed, so the caller must re-send the
  // current needle targets or the gauges sit at zero until the next flight
  // update. Cheap: only touches the UART when bytes are waiting.
  bool poll() {
    bool ready = false;
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c != '\n') {
        if (len_ < sizeof(buf_) - 1) buf_[len_++] = c;
        continue;
      }
      buf_[len_] = '\0';
      if (len_) ready |= handleLine(buf_);
      len_ = 0;
    }
    return ready;
  }

 private:
  bool handleLine(const char* line) {
    switch (line[0]) {
      case 'R':
        Serial.println("[gauge] controller ready (needles homed)");
        return true;
      case 'E':
        Serial.printf("[gauge] homing FAILED: %s\n", line);
        return false;
      case '#':
#if GAUGE_SERIAL_LOG
        Serial.printf("[gauge]%s\n", line + 1);
#endif
        return false;
      default:
        return false;
    }
  }

  char buf_[96];
  uint8_t len_ = 0;
};
