// TFT UI: the tested 4-block flight-info layout from display/display.ino,
// plus status screens for setup / connecting / waiting states.
// Display pin mapping lives in the TFT_eSPI library's User_Setup.h.
#pragma once
#include <TFT_eSPI.h>
#include "config.h"
#include "bitmaps.h"
#if UI_CREATIVE_SCREENS
#include "graphics.h"
#endif

extern TFT_eSPI tft;

#define COLOR_LABEL tft.color565(120, 200, 255)
#define COLOR_VALUE TFT_WHITE
#define COLOR_LINE  tft.color565(30, 40, 50)
#define COLOR_DIM   tft.color565(110, 125, 140)

// Which screen is currently animating (driven by displayTick from loop()).
enum UiAnim { UI_ANIM_NONE, UI_ANIM_CONNECTING, UI_ANIM_RADAR };
static UiAnim uiAnim_ = UI_ANIM_NONE;

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

// Tested dynamic flight renderer (proportional layout, 200x50 bitmap).
inline void showFlightInfo(const String& flight, const String& airline,
                           const String& route, const String& aircraft,
                           const unsigned char* planeBitmap) {
  uiAnim_ = UI_ANIM_NONE;
  tft.fillScreen(TFT_BLACK);

  int screenW = tft.width();
  int screenH = tft.height();
  int marginLeft = screenW * 0.06;
  int lineEndX = screenW - marginLeft;

  int yFlightLbl = screenH * 0.05, yFlightVal = screenH * 0.11, yLine1 = screenH * 0.23;
  int yAirLbl    = screenH * 0.28, yAirVal    = screenH * 0.34, yLine2 = screenH * 0.43;
  int yRouteLbl  = screenH * 0.48, yRouteVal  = screenH * 0.54, yLine3 = screenH * 0.65;
  int yCraftLbl  = screenH * 0.70, yCraftVal  = screenH * 0.76;

  tft.setCursor(marginLeft, yFlightLbl);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("FLIGHT");
  tft.setCursor(marginLeft, yFlightVal);
  tft.setTextColor(COLOR_VALUE); tft.setTextSize(4); tft.println(flight);
  tft.drawLine(marginLeft, yLine1, lineEndX, yLine1, COLOR_LINE);

  tft.setCursor(marginLeft, yAirLbl);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("AIRLINE");
  tft.setCursor(marginLeft, yAirVal);
  tft.setTextColor(COLOR_VALUE); tft.setTextSize(2); tft.println(airline);
  tft.drawLine(marginLeft, yLine2, lineEndX, yLine2, COLOR_LINE);

  tft.setCursor(marginLeft, yRouteLbl);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("ROUTE");
  tft.setCursor(marginLeft, yRouteVal);
  tft.setTextColor(COLOR_VALUE); tft.setTextSize(3); tft.println(route);
  tft.drawLine(marginLeft, yLine3, lineEndX, yLine3, COLOR_LINE);

  tft.setCursor(marginLeft, yCraftLbl);
  tft.setTextColor(COLOR_LABEL); tft.setTextSize(2); tft.println("AIRCRAFT");
  tft.setCursor(marginLeft, yCraftVal);
  tft.setTextColor(COLOR_VALUE); tft.setTextSize(2); tft.println(aircraft);

  int planeX = (screenW - 200) / 2;
  int planeY = screenH - 60;
  tft.drawBitmap(planeX, planeY, planeBitmap, 200, 50, COLOR_LABEL);
}

// Generic status screen: big title + up to 5 detail lines.
inline void showStatus(const String& title, const String* lines, int nLines) {
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
