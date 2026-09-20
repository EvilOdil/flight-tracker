// TFT UI: the tested 4-block flight-info layout from display/display.ino,
// plus status screens for setup / connecting / waiting states.
// Display pin mapping lives in the TFT_eSPI library's User_Setup.h.
#pragma once
#include <TFT_eSPI.h>
#include "config.h"
#include "theme.h"
#include "bitmaps.h"
#include "photo.h"
#if UI_CREATIVE_SCREENS
#include "graphics.h"
#endif

extern TFT_eSPI tft;

#define COLOR_LABEL uiAccent565
#define COLOR_VALUE TFT_WHITE
#define COLOR_LINE  tft.color565(30, 40, 50)
#define COLOR_DIM   tft.color565(110, 125, 140)

// Which screen is currently animating (driven by displayTick from loop()).
enum UiAnim { UI_ANIM_NONE, UI_ANIM_CONNECTING, UI_ANIM_RADAR };
static UiAnim uiAnim_ = UI_ANIM_NONE;

// Identity of the flight currently painted, so a repeated identical "flight"
// frame is a no-op instead of a full-screen repaint (blank-flash + ~tens of ms
// blocking on the parallel bus). Cleared by every other screen so returning to
// the same flight still repaints once.
static String lastFlightKey_;

inline void displayBegin() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
}

inline void showBootScreen() {
#if UI_CREATIVE_SCREENS
  gfxBootScreen();
#endif
}

// Flight-screen layout, chosen from the web app (cfg "layout"):
//   0 = standard — text only, enlarged and spread over the full height.
//   1 = classic  — text packed at the top, aircraft photo (fetched from the
//                  backend; silhouette bitmap as fallback) in the lower panel.
static uint8_t flightLayout_ = 0;
inline void setDisplayLayout(uint8_t n) { flightLayout_ = n ? 1 : 0; }

// Largest classic text size (6 px/char at size 1) that keeps `s` on one line.
inline uint8_t fitTextSize(const String& s, int maxWidthPx, uint8_t want) {
  uint8_t sz = want;
  while (sz > 1 && (int)s.length() * 6 * sz > maxWidthPx) sz--;
  return sz;
}

// Two-line word wrap for long values (airline / aircraft names): largest text
// size <= `want` at which `s` fits maxWidthPx as at most two word-wrapped
// lines. *split = index of the space to break at, -1 = fits on one line.
// Preferred over fitTextSize where vertical room for a second line exists —
// a wrapped size-3 line beats an unreadable single size-1 line.
inline uint8_t wrapPlan(const String& s, int maxWidthPx, uint8_t want, int* split) {
  for (uint8_t sz = want; sz >= 1; sz--) {
    int maxChars = maxWidthPx / (6 * sz);
    *split = -1;
    if ((int)s.length() <= maxChars) return sz;  // one line at this size
    int cut = -1;
    for (int i = min((int)s.length() - 1, maxChars); i > 0; i--)
      if (s.charAt(i) == ' ') { cut = i; break; }
    if (cut > 0 && (int)s.length() - cut - 1 <= maxChars) { *split = cut; return sz; }
    if (sz == 1) return 1;  // pathological: single line, library edge-wrap
  }
  return 1;  // unreachable
}

// Draw a value planned by wrapPlan. BOTH lines start at the same x so a
// wrapped second line keeps the left column alignment of every other field.
// Returns the pixel height consumed (one or two 8*size lines).
inline int drawWrappedValue(const String& s, int x, int y, uint8_t sz, int split) {
  tft.setTextSize(sz);
  int lineH = 8 * sz;
  tft.setCursor(x, y);
  if (split < 0) { tft.println(s); return lineH; }
  tft.println(s.substring(0, split));
  tft.setCursor(x, y + lineH + 2);
  tft.println(s.substring(split + 1));
  return 2 * lineH + 2;
}

// Nearest-neighbour draw of a 1-bpp MSB-first bitmap scaled to fit a dst rect,
// preserving aspect ratio and centering. Only set bits are drawn (transparent
// background). Used to blow the aircraft silhouette up for the big-image layout.
inline void drawBitmapFit(const unsigned char* bmp, int sw, int sh,
                          int dstX, int dstY, int dstW, int dstH, uint16_t color) {
  float sc = min((float)dstW / sw, (float)dstH / sh);
  int w = (int)(sw * sc), h = (int)(sh * sc);
  int ox = dstX + (dstW - w) / 2, oy = dstY + (dstH - h) / 2;
  int byteW = (sw + 7) / 8;
  for (int y = 0; y < h; y++) {
    const unsigned char* row = bmp + (int)(y / sc) * byteW;
    for (int x = 0; x < w; x++) {
      int sx = (int)(x / sc);
      if (row[sx >> 3] & (0x80 >> (sx & 7))) tft.drawPixel(ox + x, oy + y, color);
    }
  }
}

// Layout 0 "Standard": text only — the original label/value/divider style,
// enlarged and spread over the full height freed by dropping the image.
// Airline/aircraft names wrap to two left-aligned lines instead of shrinking:
// each section has room for two size-3 lines before its divider (airline
// ends <= 0.475H vs the 0.50H divider; aircraft <= 0.974H vs screen bottom).
inline void drawFlightStandard(const String& flight, const String& airline,
                               const String& route, const String& aircraft) {
  int W = tft.width(), H = tft.height();
  int mx = W * 0.06, endX = W - mx, maxW = W - 2 * mx;
  int split;
  uint8_t sz;

  tft.setCursor(mx, H * 0.05);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("FLIGHT");
  tft.setCursor(mx, H * 0.11);
  tft.setTextColor(COLOR_VALUE); tft.setTextSize(fitTextSize(flight, maxW, 5)); tft.println(flight);
  tft.drawLine(mx, H * 0.25, endX, H * 0.25, COLOR_LINE);

  tft.setCursor(mx, H * 0.31);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("AIRLINE");
  tft.setTextColor(COLOR_VALUE);
  sz = wrapPlan(airline, maxW, 3, &split);
  drawWrappedValue(airline, mx, H * 0.37, sz, split);
  tft.drawLine(mx, H * 0.50, endX, H * 0.50, COLOR_LINE);

  tft.setCursor(mx, H * 0.56);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("ROUTE");
  tft.setCursor(mx, H * 0.62);
  tft.setTextColor(COLOR_VALUE); tft.setTextSize(fitTextSize(route, maxW, 4)); tft.println(route);
  tft.drawLine(mx, H * 0.75, endX, H * 0.75, COLOR_LINE);

  tft.setCursor(mx, H * 0.81);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("AIRCRAFT");
  tft.setTextColor(COLOR_VALUE);
  sz = wrapPlan(aircraft, maxW, 3, &split);
  drawWrappedValue(aircraft, mx, H * 0.87, sz, split);
}

// Layout 1 "Classic": text packed tight at the top; real aircraft photo
// (backend-fetched, resized server-side) fills the lower panel, with the
// silhouette bitmap as fallback when no photo exists. This is the ONLY place
// a photo is requested — Standard mode never touches the network.
//
// The text block flows: airline/aircraft may wrap to a second left-aligned
// line (long names used to library-edge-wrap to x=0 and crowd the divider),
// and everything below a wrapped value shifts down to keep the gaps. The
// divider and photo box stay PINNED at 0.47H/0.49H — shrinking the photo box
// below the backend's 217 px target would silently knock tall photos back to
// the silhouette (drawAircraftPhoto rejects jh > bh). Section gaps absorb the
// extra lines instead: uniform when everything is short, compressed (min
// 5 px, incl. above the divider) in the worst wrapped case.
inline void drawFlightClassic(const String& flight, const String& airline,
                              const String& route, const String& aircraft,
                              const unsigned char* planeBitmap, const String& imgUrl) {
  int W = tft.width(), H = tft.height();
  int mx = W * 0.06, endX = W - mx, maxW = W - 2 * mx;

  int aSplit, cSplit;
  uint8_t aSz = wrapPlan(airline, maxW, 2, &aSplit);
  uint8_t cSz = wrapPlan(aircraft, maxW, 2, &cSplit);
  uint8_t rSz = fitTextSize(route, maxW, 2);   // routes are short: single line
  int aH = (aSplit < 0) ? 8 * aSz : 16 * aSz + 2;
  int cH = (cSplit < 0) ? 8 * cSz : 16 * cSz + 2;

  const int labelH = 16, labelGap = H * 0.010;
  int divY = H * 0.47, topY = H * 0.03;
  int content = (labelH + labelGap) * 4 + 24 + aH + 8 * rSz + cH;
  int gap = (divY - topY - (int)(H * 0.02) - content) / 3;
  gap = constrain(gap, 5, (int)(H * 0.03));

  int y = topY;
  tft.setCursor(mx, y); tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("FLIGHT");
  y += labelH + labelGap;
  tft.setCursor(mx, y); tft.setTextColor(COLOR_VALUE); tft.setTextSize(3); tft.println(flight);
  y += 24 + gap;

  tft.setCursor(mx, y); tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("AIRLINE");
  y += labelH + labelGap;
  tft.setTextColor(COLOR_VALUE);
  y += drawWrappedValue(airline, mx, y, aSz, aSplit) + gap;

  tft.setCursor(mx, y); tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("ROUTE");
  y += labelH + labelGap;
  tft.setCursor(mx, y); tft.setTextColor(COLOR_VALUE); tft.setTextSize(rSz); tft.println(route);
  y += 8 * rSz + gap;

  tft.setCursor(mx, y); tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("AIRCRAFT");
  y += labelH + labelGap;
  tft.setTextColor(COLOR_VALUE);
  drawWrappedValue(aircraft, mx, y, cSz, cSplit);

  tft.drawLine(mx, divY, endX, divY, COLOR_LINE);

  int by = H * 0.49, bw = W - 2 * mx, bh = H - by - 18;  // 18 px for the credit
  String credit;
  if (drawAircraftPhoto(imgUrl, mx, by, bw, bh, &credit)) {
    if (credit.length()) {
      tft.setTextDatum(BC_DATUM);
      tft.setTextColor(COLOR_DIM, TFT_BLACK); tft.setTextSize(1);
      tft.drawString("photo: " + credit + " / planespotters.net", W / 2, H - 5);
      tft.setTextDatum(TL_DATUM);
    }
  } else {
    drawBitmapFit(planeBitmap, 200, 50, mx, by, bw, bh, COLOR_LABEL);
  }
}

// Dynamic flight renderer. Dispatches to the layout picked from the web app.
inline void showFlightInfo(const String& flight, const String& airline,
                           const String& route, const String& aircraft,
                           const unsigned char* planeBitmap, const String& imgUrl) {
  // Skip the repaint when nothing visible changed (backend may resend the same
  // "flight" frame). Layout and theme are part of the key so switching either
  // redraws. Gauges are updated separately by the caller.
  String key = String((int)flightLayout_) + '\x1f' + String((unsigned)uiAccent565)
             + '\x1f' + flight + '\x1f' + airline
             + '\x1f' + route + '\x1f' + aircraft;
  if (uiAnim_ == UI_ANIM_NONE && key == lastFlightKey_) return;
  lastFlightKey_ = key;

  uiAnim_ = UI_ANIM_NONE;
  tft.fillScreen(TFT_BLACK);
  if (flightLayout_ == 1) drawFlightClassic(flight, airline, route, aircraft, planeBitmap, imgUrl);
  else                    drawFlightStandard(flight, airline, route, aircraft);
}

// Generic status screen: big title + up to 5 detail lines.
inline void showStatus(const String& title, const String* lines, int nLines) {
  lastFlightKey_ = String();  // leaving the flight screen
  tft.fillScreen(TFT_BLACK);
  int marginLeft = tft.width() * 0.06;
  tft.setCursor(marginLeft, tft.height() * 0.08);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(3);
  tft.println(title);
  tft.drawLine(marginLeft, tft.height() * 0.18, tft.width() - marginLeft,
               tft.height() * 0.18, COLOR_LINE);
  tft.setTextSize(2);
  int y = tft.height() * 0.24;
  for (int i = 0; i < nLines && i < 5; i++) {
    tft.setCursor(marginLeft, y);
    tft.setTextColor(i == 0 ? COLOR_VALUE : COLOR_DIM);
    tft.println(lines[i]);
    y += tft.height() * 0.09;
  }
}

inline void showSetupScreen(const String& apName, const String& deviceId) {
  lastFlightKey_ = String();
  uiAnim_ = UI_ANIM_NONE;  // portal loop blocks; setup screen is static
#if UI_CREATIVE_SCREENS
  gfxSetupScreen(apName);
#else
  String lines[] = {
    "1. On your phone join",
    "   Wi-Fi: " + apName,
    "2. Open 192.168.4.1",
    "3. Enter home Wi-Fi +",
    "   server address",
  };
  showStatus("SETUP", lines, 5);
#endif
}

inline void showConnecting(const String& what) {
  lastFlightKey_ = String();
#if UI_CREATIVE_SCREENS
  gfxConnectingScreen(what);
  uiAnim_ = UI_ANIM_CONNECTING;
#else
  uiAnim_ = UI_ANIM_NONE;
  String lines[] = { what };
  showStatus("CONNECTING", lines, 1);
#endif
}

inline void showWaiting(const String& deviceId, const String& modeLine) {
  lastFlightKey_ = String();
#if UI_CREATIVE_SCREENS
  gfxWaitingScreen(deviceId, modeLine);
  uiAnim_ = UI_ANIM_RADAR;
#else
  uiAnim_ = UI_ANIM_NONE;
  String lines[] = {
    "Device ID: " + deviceId,
    modeLine,
    "Configure at the",
    "tracker web app.",
  };
  showStatus("WATCHING SKY", lines, 4);
#endif
}

// Call from loop(): advances whichever animation is on screen. No-op when
// UI_CREATIVE_SCREENS is 0 or a static screen is showing.
inline void displayTick() {
#if UI_CREATIVE_SCREENS
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 60) return;
  lastMs = millis();
  if (uiAnim_ == UI_ANIM_CONNECTING) gfxConnectingTick();
  else if (uiAnim_ == UI_ANIM_RADAR) gfxRadarTick();
#endif
}
