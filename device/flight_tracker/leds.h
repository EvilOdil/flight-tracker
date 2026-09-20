// Accent LED strips — 3 independent WS2812-style NeoPixel strips on their own
// data pins (see config.h for pins, lengths, and the power/PSRAM caveats).
//
// Fixed full-brightness white, set once at boot: no animation, so nothing here
// runs in loop(). Strips are separate objects rather than one long chain
// because each has its own data line — a chain would need them daisy-chained
// physically.
#pragma once

#include <Adafruit_NeoPixel.h>
#include "config.h"

static Adafruit_NeoPixel ledStrip1(LED_STRIP_1_COUNT, LED_STRIP_1_PIN, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel ledStrip2(LED_STRIP_2_COUNT, LED_STRIP_2_PIN, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel ledStrip3(LED_STRIP_3_COUNT, LED_STRIP_3_PIN, NEO_GRB + NEO_KHZ800);

// Pointers, not an array of objects: Adafruit_NeoPixel owns a malloc'd pixel
// buffer, so copying one into an array slot is a trap.
static Adafruit_NeoPixel* const ledStrips[] = { &ledStrip1, &ledStrip2, &ledStrip3 };
#define LED_STRIP_N (sizeof(ledStrips) / sizeof(ledStrips[0]))

inline void ledsBegin() {
  for (uint8_t i = 0; i < LED_STRIP_N; i++) {
    ledStrips[i]->begin();
    ledStrips[i]->setBrightness(LED_BRIGHTNESS);
    ledStrips[i]->fill(ledStrips[i]->Color(255, 255, 255));
    ledStrips[i]->show();
  }
}
