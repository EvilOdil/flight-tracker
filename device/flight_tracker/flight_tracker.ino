// Flight Tracker device — ESP32-S3
//
// 3.5" TFT (TFT_eSPI; pins AND driver in the library's User_Setup.h — must
// be ILI9488_DRIVER: the clone panel renders correct colors only with the
// 9488 init profile, confirmed on-glass 2026-07-09; ILI9486_DRIVER "worked"
// but with a broken tone response) + the speed gauge stepper. The other four
// 28BYJ-48 needles (heading, altimeter x3) live on an ESP32-WROOM-32 gauge
// controller (device/gauge_controller/) over a UART link — the S3 ran out of
// GPIOs. Receives compact JSON push frames from the backend over WebSocket;
// all tracking config is done from the phone web app served by the backend.
// See ../../PLAN.md and ../gauge_link_protocol.md.
//
// Libraries (Library Manager): TFT_eSPI, ArduinoJson (v7),
//                              "WebSockets" by Markus Sattler (links2004).

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "config.h"
// NOTE: wifi_portal.h (WebServer.h) must come before display_ui.h (TFT_eSPI):
// TFT_eSPI defines FS_NO_GLOBALS, which breaks WebServer.h if included first.
#include "wifi_portal.h"
#include "gauges.h"
#include "display_ui.h"
#include "leds.h"

TFT_eSPI tft = TFT_eSPI();
Gauges gauges;
WifiPortal portal;
WebSocketsClient ws;
NetConfig net;

String deviceId;          // "FT-" + last 3 MAC bytes, shown on screen + web app
bool wsConnected = false;
bool tracking = false;
String modeLine = "Mode: not configured";
unsigned long resetBtnDownAt = 0;

// Passed to gauges.home(): homing blocks for seconds, so keep the boot
// animation moving instead of freezing a half-drawn screen.
void pumpDisplay() { displayTick(); }

// Bench calibration console on the USB serial monitor. Same verbs as the
// gauge link, so one monitor drives all five needles — the speed needle
// locally, the other four forwarded to the gauge controller:
//   T <s0> <s1> <s2> <s3> <s4>   jog needles to raw step positions (-1 = skip)
//   H                            re-home the speed needle, then the controller
// Homing blocks for seconds; that is fine for a deliberate bench command.
void serialConsoleTick() {
  static char line[80];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = '\0';
    if (len && line[0] == 'T') {
      long t[GAUGE_MOTOR_COUNT];
      char* end;
      const char* p = line + 1;
      for (uint8_t i = 0; i < GAUGE_MOTOR_COUNT; i++) {
        t[i] = strtol(p, &end, 10);
        if (end == p) t[i] = -1; else p = end;
      }
      Serial.printf("[cal] jog %ld %ld %ld %ld %ld\n", t[0], t[1], t[2], t[3], t[4]);
      gauges.setSteps(t);
    } else if (len && line[0] == 'H') {
      Serial.println("[cal] re-homing");
      gauges.home(&pumpDisplay);
      gauges.rehomeController();
    }
    len = 0;
  }
}

String makeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[16];
  snprintf(buf, sizeof(buf), "FT-%02X%02X%02X",
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  return String(buf);
}

// ---------------------------------------------------------------------------
// WebSocket message handling
// ---------------------------------------------------------------------------

void handleMessage(uint8_t* payload, size_t len) {
  Serial.printf("[ws] rx: %.*s\n", (int)len, (const char*)payload);
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* t = doc["t"] | "";

  if (!strcmp(t, "flight")) {
    tracking = true;
    Serial.printf("[flight] %s | %s | %s | alt=%ld ft gs=%d kt trk=%d\n",
                  doc["cs"] | "?", doc["rt"] | "?", doc["ac"] | "?",
                  (long)(doc["alt"] | 0L), (int)(doc["gs"] | 0), (int)(doc["trk"] | 0));
    String fam = doc["fam"] | "GENERIC";
    // Absolute photo URL for the Classic layout; the fetch itself only
    // happens inside drawFlightClassic (Standard mode never requests it).
    String imgUrl;
    const char* img = doc["img"] | "";
    if (img[0]) {
      imgUrl = String(net.tls ? "https://" : "http://") + net.host + ":" + net.port + img;
    }
    showFlightInfo(doc["cs"] | "------", doc["al"] | "", doc["rt"] | "",
                   doc["ac"] | "", bitmapForFamily(fam), imgUrl);
    gauges.set(doc["alt"] | 0L, doc["gs"] | 0, doc["trk"] | 0);

  } else if (!strcmp(t, "state")) {
    gauges.set(doc["alt"] | 0L, doc["gs"] | 0, doc["trk"] | 0);

  } else if (!strcmp(t, "clear")) {
    tracking = false;
    gauges.zero();
    showWaiting(deviceId, modeLine);

  } else if (!strcmp(t, "cfg")) {
    if (doc["th"].is<const char*>()) setThemeColor(doc["th"] | "");
    if (doc["layout"].is<int>()) setDisplayLayout(doc["layout"] | 0);
    const char* mode = doc["mode"] | "radius";
    if (!strcmp(mode, "flight")) {
      modeLine = "Mode: flight " + String(doc["cs"] | "?");
    } else {
      modeLine = "Mode: radius " + String(doc["radiusKm"] | 0) + " km";
    }
    if (!tracking) showWaiting(deviceId, modeLine);

  } else if (!strcmp(t, "netreset")) {
    // "Change network" from the web app: wipe Wi-Fi creds and reboot into the
    // setup portal (same path as holding BOOT for 3 s).
    Serial.println("[net] reset requested -> setup portal");
    portal.wipe();
    delay(200);
    ESP.restart();
  }
}

void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      wsConnected = true;
      JsonDocument doc;
      doc["t"] = "hello";
      doc["id"] = deviceId;
      doc["fw"] = FW_VERSION;
      String out;
      serializeJson(doc, out);
      ws.sendTXT(out);
      break;
    }
    case WStype_DISCONNECTED:
      if (wsConnected) {  // show once, not on every retry
        wsConnected = false;
        tracking = false;
        gauges.zero();
        showConnecting("server " + net.host);
      }
      break;
    case WStype_TEXT:
      handleMessage(payload, len);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // ESP32-S3 native USB-CDC: with no serial monitor attached the TX buffer
  // fills and Serial.print() blocks until a timeout, stalling loop() (looks
  // like a display "freeze" once the monitor is closed). Never block on TX.
  // (Only USB-CDC Serial has this; a UART build has no such hazard.)
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  deviceId = makeDeviceId();

  displayBegin();
  showBootScreen();  // no-op when UI_CREATIVE_SCREENS is 0
  gauges.begin();
  ledsBegin();
  // Drive the speed needle onto its limit switch, then to its printed zero.
  // Blocking, but this is setup() — nothing else is running yet, and the
  // gauge controller homes its own four needles in parallel on its own boot.
  showConnecting("calibrating gauges");
  gauges.home(&pumpDisplay);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  net = portal.load();
  if (!net.valid()) {
    showSetupScreen(deviceId, deviceId);  // AP name == device ID
    portal.runPortal(deviceId);           // blocks, reboots after save
  }

  showConnecting("Wi-Fi " + net.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceId.c_str());
  WiFi.begin(net.ssid.c_str(), net.pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(200);

  if (WiFi.status() != WL_CONNECTED) {
    // Wrong credentials or network gone: fall back to the setup portal.
    showSetupScreen(deviceId, deviceId);
    portal.runPortal(deviceId);
  }

  Serial.printf("[wifi] %s -> %s\n", deviceId.c_str(), WiFi.localIP().toString().c_str());
  showConnecting("server " + net.host);

  // Hosted backend -> wss:// (TLS, no cert pinning for the prototype);
  // local/LAN backend -> plain ws://.
  if (net.tls) ws.beginSSL(net.host.c_str(), net.port, WS_PATH);
  else ws.begin(net.host.c_str(), net.port, WS_PATH);
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(WS_RECONNECT_MS);
  ws.enableHeartbeat(20000, 5000, 2);  // ping every 20 s, 2 misses -> reconnect
}

void loop() {
  ws.loop();
  gauges.run();
  serialConsoleTick();
  displayTick();  // animates connecting / radar screens (UI_CREATIVE_SCREENS)

  // Hold BOOT for 3 s -> wipe config, back to setup portal.
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    if (resetBtnDownAt == 0) resetBtnDownAt = millis();
    else if (millis() - resetBtnDownAt > 3000) {
      portal.wipe();
      ESP.restart();
    }
  } else {
    resetBtnDownAt = 0;
  }
}
