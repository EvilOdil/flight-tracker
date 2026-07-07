// Flight Tracker device — ESP32-S3
//
// 3.5" TFT (TFT_eSPI, pins in the library's User_Setup.h) + five 28BYJ-48
// steppers (speed, heading, altimeter x3). Receives compact JSON push frames
// from the backend over WebSocket; all tracking config is done from the
// phone web app served by the backend. See ../../PLAN.md.
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
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* t = doc["t"] | "";

  if (!strcmp(t, "flight")) {
    tracking = true;
    String fam = doc["fam"] | "GENERIC";
    showFlightInfo(doc["cs"] | "------", doc["al"] | "", doc["rt"] | "",
                   doc["ac"] | "", bitmapForFamily(fam));
    gauges.set(doc["alt"] | 0L, doc["gs"] | 0, doc["trk"] | 0);

  } else if (!strcmp(t, "state")) {
    gauges.set(doc["alt"] | 0L, doc["gs"] | 0, doc["trk"] | 0);

  } else if (!strcmp(t, "clear")) {
    tracking = false;
    gauges.zero();
    showWaiting(deviceId, modeLine);

  } else if (!strcmp(t, "cfg")) {
    const char* mode = doc["mode"] | "radius";
    if (!strcmp(mode, "flight")) {
      modeLine = "Mode: flight " + String(doc["cs"] | "?");
    } else {
      modeLine = "Mode: radius " + String(doc["radiusKm"] | 0) + " km";
    }
    if (!tracking) showWaiting(deviceId, modeLine);
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
  deviceId = makeDeviceId();

  displayBegin();
  gauges.begin();
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
