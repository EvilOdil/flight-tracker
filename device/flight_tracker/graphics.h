// Creative vector-drawn screens: boot splash, setup scene, animated
// "connecting" flight path, and a radar sweep for the waiting screen.
// Drawn entirely with TFT_eSPI primitives (no bitmaps, ~zero flash cost).
//
// Everything here is used only when UI_CREATIVE_SCREENS is 1 (config.h).
// Set it to 0 there to fall back to the plain text screens.
#pragma once
#include <TFT_eSPI.h>
#include "config.h"
#include "theme.h"
#include "qr.h"

extern TFT_eSPI tft;

// The SoftAP captive-portal address (WiFi.softAPIP() default).
#define SETUP_PORTAL_IP  "192.168.4.1"

#define GFX_ACCENT   uiAccent565                    // matches COLOR_LABEL
#define GFX_DIMTXT   tft.color565(110, 125, 140)
#define GFX_FAINT    tft.color565(22, 34, 46)
#define GFX_TRAIL    tft.color565(50, 70, 90)
#define GFX_RADAR    tft.color565(40, 210, 120)
#define GFX_RADAR_MD tft.color565(24, 120, 70)
#define GFX_RADAR_DK tft.color565(12, 60, 36)

// Top-down aircraft silhouette pointing right, built from triangles.
inline void gfxPlaneRight(int x, int y, float s, uint16_t c) {
  tft.fillTriangle(x - 10 * s, y - 2 * s, x - 10 * s, y + 2 * s, x + 10 * s, y, c);  // fuselage
  tft.fillTriangle(x + 1 * s, y, x - 5 * s, y - 8 * s, x - 5 * s, y, c);             // wing up
  tft.fillTriangle(x + 1 * s, y, x - 5 * s, y + 8 * s, x - 5 * s, y, c);             // wing down
  tft.fillTriangle(x - 10 * s, y - 5 * s, x - 10 * s, y + 5 * s, x - 6 * s, y, c);   // tail
}

// ---------------------------------------------------------------------------
// Boot splash: plane over faint range rings, title, sweeping loading bar.
// Blocks ~0.6 s — only ever runs once at power-up.
// ---------------------------------------------------------------------------
inline void gfxBootScreen() {
  tft.fillScreen(TFT_BLACK);
  int w = tft.width(), h = tft.height();
  int cx = w / 2, cy = h * 2 / 5;

  for (int r = 40; r <= 140; r += 50) tft.drawCircle(cx, cy, r, GFX_FAINT);
  // dotted contrail behind the plane
  for (int x = cx - 130; x < cx - 50; x += 10) tft.drawFastHLine(x, cy, 4, GFX_TRAIL);
  gfxPlaneRight(cx, cy, 4.0f, GFX_ACCENT);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(4);
  tft.drawString("FLIGHT", cx, h * 0.60);
  tft.drawString("TRACKER", cx, h * 0.67);
  tft.setTextSize(1);
  tft.setTextColor(GFX_DIMTXT, TFT_BLACK);
  tft.drawString("fw " FW_VERSION, cx, h * 0.75);
  tft.setTextDatum(TL_DATUM);

  int bw = w * 0.6, bx = (w - bw) / 2, by = h * 0.82;
  tft.drawRoundRect(bx - 3, by - 3, bw + 6, 14, 5, GFX_TRAIL);
  for (int i = 0; i < bw - 4; i += 6) {
    tft.fillRect(bx + i, by, 4, 8, GFX_ACCENT);
    delay(18);
  }
}

// Standard "WIFI:" QR payload that phone cameras parse to auto-join a network
// ("Join Network" prompt on iOS/Android). Escapes the MECARD special chars.
inline String wifiJoinPayload(const String& ssid, const String& pass) {
  auto esc = [](const String& s) {
    String o;
    for (uint16_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
      o += c;
    }
    return o;
  };
  return "WIFI:T:WPA;S:" + esc(ssid) + ";P:" + esc(pass) + ";;";
}

// ---------------------------------------------------------------------------
// Setup scene (all centered): scan the QR to JOIN the device Wi-Fi, then the
// captive portal auto-opens the setup page. Wi-Fi name/password and the portal
// IP are printed below as fallbacks for anyone who can't scan or whose page
// doesn't pop up on its own. Static (the portal loop blocks — no animation).
// ---------------------------------------------------------------------------
inline void gfxSetupScreen(const String& apName) {
  tft.fillScreen(TFT_BLACK);
  int w = tft.width(), h = tft.height();
  int cx = w / 2;
  tft.setTextDatum(TC_DATUM);  // draw everything about the horizontal centre

  tft.setTextColor(GFX_ACCENT, TFT_BLACK);
  tft.setTextSize(3);
  tft.drawString("SETUP", cx, h * 0.035);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Scan to join Wi-Fi", cx, h * 0.105);

  // QR encodes the Wi-Fi credentials -> phone joins the hotspot on scan.
  drawQR(wifiJoinPayload(apName, AP_PASSWORD), cx, h * 0.40, 5);

  // Fallbacks below the code, centered.
  int y = h * 0.635, dy = h * 0.05;
  tft.setTextColor(GFX_ACCENT, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("Wi-Fi: " + apName, cx, y);
  tft.setTextColor(GFX_DIMTXT, TFT_BLACK);
  tft.drawString("Password: " AP_PASSWORD, cx, y += dy);

  tft.setTextSize(1);
  tft.drawString("Setup page opens automatically -", cx, y += dy * 1.05);
  tft.setTextColor(GFX_ACCENT, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("else open " SETUP_PORTAL_IP, cx, y += dy * 0.7);
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------------
// Connecting screen: plane flies along a dotted route between two waypoints.
// gfxConnectingTick() animates it from displayTick().
// ---------------------------------------------------------------------------
static int gfxConnX, gfxConnPathY;

inline void gfxConnectingScreen(const String& what) {
  tft.fillScreen(TFT_BLACK);
  int w = tft.width(), h = tft.height();

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.drawString("CONNECTING", w / 2, h * 0.12);
  tft.setTextColor(GFX_DIMTXT, TFT_BLACK);
  tft.setTextSize(what.length() > 24 ? 1 : 2);
  tft.drawString(what, w / 2, h * 0.20);
  tft.setTextDatum(TL_DATUM);

  gfxConnPathY = h * 0.45;
  gfxConnX = w * 0.14;
}

inline void gfxConnectingTick() {
  int w = tft.width();
  int x0 = w * 0.08, x1 = w * 0.92;

  tft.fillRect(gfxConnX - 24, gfxConnPathY - 16, 48, 32, TFT_BLACK);
  for (int x = x0 + 10; x < x1 - 8; x += 10)          // restore route dots
    if (x >= gfxConnX - 24 && x <= gfxConnX + 24) tft.drawFastHLine(x, gfxConnPathY, 3, GFX_TRAIL);
  tft.fillCircle(x0, gfxConnPathY, 4, GFX_ACCENT);    // waypoints
  tft.fillCircle(x1, gfxConnPathY, 4, GFX_ACCENT);

  gfxConnX += 3;
  if (gfxConnX > w * 0.84) gfxConnX = w * 0.16;
  gfxPlaneRight(gfxConnX, gfxConnPathY, 1.6f, TFT_WHITE);
}

// ---------------------------------------------------------------------------
// Waiting screen: radar scope with rotating sweep and fading contact blips.
// gfxRadarTick() animates it from displayTick().
// ---------------------------------------------------------------------------
static int gfxRadCX, gfxRadCY, gfxRadR;
static float gfxSweepDeg;
struct GfxBlip { float ang, rfrac; };
static const GfxBlip GFX_BLIPS[3] = {{40, 0.55f}, {160, 0.80f}, {265, 0.35f}};
static uint8_t gfxBlipGlow[3];

inline void gfxRadarBase() {
  tft.drawCircle(gfxRadCX, gfxRadCY, gfxRadR, GFX_RADAR);
  tft.drawCircle(gfxRadCX, gfxRadCY, gfxRadR * 2 / 3, GFX_RADAR_DK);
  tft.drawCircle(gfxRadCX, gfxRadCY, gfxRadR / 3, GFX_RADAR_DK);
  tft.drawFastHLine(gfxRadCX - gfxRadR, gfxRadCY, gfxRadR * 2, GFX_RADAR_DK);
  tft.drawFastVLine(gfxRadCX, gfxRadCY - gfxRadR, gfxRadR * 2, GFX_RADAR_DK);
}

inline void gfxWaitingScreen(const String& deviceId, const String& modeLine) {
  tft.fillScreen(TFT_BLACK);
  int w = tft.width(), h = tft.height();

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(GFX_ACCENT, TFT_BLACK);
  tft.setTextSize(3);
  tft.drawString("WATCHING SKY", w / 2, h * 0.05);

  gfxRadCX = w / 2;
  gfxRadCY = h * 0.42;
  gfxRadR  = w * 0.38;
  gfxRadarBase();
  gfxSweepDeg = 0;
  for (int i = 0; i < 3; i++) gfxBlipGlow[i] = 1;

  int y = gfxRadCY + gfxRadR + 24;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("ID: " + deviceId, w / 2, y);
  tft.setTextColor(GFX_DIMTXT, TFT_BLACK);
  tft.drawString(modeLine, w / 2, y + 26);
  tft.setTextSize(1);
  tft.drawString("Set location & mode in the web app", w / 2, y + 54);
  tft.setTextDatum(TL_DATUM);
}

inline void gfxRadarTick() {
  auto sweepLine = [](float deg, uint16_t c) {
    float r = deg * DEG_TO_RAD;
    tft.drawLine(gfxRadCX, gfxRadCY,
                 gfxRadCX + (int)(cosf(r) * (gfxRadR - 2)),
                 gfxRadCY + (int)(sinf(r) * (gfxRadR - 2)), c);
  };

  sweepLine(gfxSweepDeg, TFT_BLACK);       // erase previous sweep + trail
  sweepLine(gfxSweepDeg - 6, TFT_BLACK);
  gfxSweepDeg += 4;
  if (gfxSweepDeg >= 360) gfxSweepDeg -= 360;
  gfxRadarBase();                          // heal the rings the erase crossed
  sweepLine(gfxSweepDeg - 6, GFX_RADAR_DK);
  sweepLine(gfxSweepDeg, GFX_RADAR);

  static uint8_t fadeDiv = 0;
  bool fade = (++fadeDiv >= 7);
  if (fade) fadeDiv = 0;
  for (int i = 0; i < 3; i++) {
    float d = gfxSweepDeg - GFX_BLIPS[i].ang;
    while (d < 0) d += 360;
    if (d < 12) gfxBlipGlow[i] = 4;
    else if (fade && gfxBlipGlow[i] > 1) gfxBlipGlow[i]--;
    uint16_t c = (gfxBlipGlow[i] >= 4) ? GFX_RADAR
               : (gfxBlipGlow[i] == 3) ? GFX_RADAR_MD
               : (gfxBlipGlow[i] == 2) ? GFX_RADAR_DK : GFX_FAINT;
    float a = GFX_BLIPS[i].ang * DEG_TO_RAD;
    tft.fillCircle(gfxRadCX + (int)(cosf(a) * gfxRadR * GFX_BLIPS[i].rfrac),
                   gfxRadCY + (int)(sinf(a) * gfxRadR * GFX_BLIPS[i].rfrac), 3, c);
  }
}
