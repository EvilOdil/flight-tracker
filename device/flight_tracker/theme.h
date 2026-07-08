// Runtime UI theme: accent colour pushed from the web app via the cfg frame
// ("th": "#rrggbb"). Everything that used the hardcoded sky-blue accent
// (COLOR_LABEL / GFX_ACCENT) reads uiAccent565 instead.
#pragma once
#include <Arduino.h>

// RGB888 -> RGB565 (same packing as TFT_eSPI::color565, minus the instance).
inline uint16_t themeRgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Default matches the original hardcoded 120,200,255 accent; overwritten as
// soon as a cfg frame carries a theme.
static uint16_t uiAccent565 = themeRgb565(120, 200, 255);

// Parse "#rrggbb" and set the accent. The web app shows its accent on filled
// buttons but lightens it 40% toward white for text (--dim); labels on the
// black TFT are text, so apply the same 40% lighten here — the default web
// blue (#2ea8ff) then lands on the device's original sky-blue.
inline void setThemeColor(const char* hex) {
  if (!hex || hex[0] != '#' || strlen(hex) != 7) return;
  for (int i = 1; i < 7; i++) if (!isxdigit((unsigned char)hex[i])) return;
  long v = strtol(hex + 1, nullptr, 16);
  int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  r += (255 - r) * 2 / 5;
  g += (255 - g) * 2 / 5;
  b += (255 - b) * 2 / 5;
  uiAccent565 = themeRgb565(r, g, b);
}
