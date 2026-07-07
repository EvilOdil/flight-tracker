// Wi-Fi onboarding: credentials in NVS; when absent (or after a reset-button
// wipe) the device raises a SoftAP + captive portal where the phone enters
// home Wi-Fi credentials and the backend server address.
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

struct NetConfig {
  String ssid, pass, host;
  uint16_t port = 443;
  bool tls = true;   // hosted backends sit behind HTTPS -> wss://
  bool valid() const { return ssid.length() > 0 && host.length() > 0; }
};

class WifiPortal {
 public:
  NetConfig load() {
    prefs_.begin("ftracker", true);
    NetConfig c;
    c.ssid = prefs_.getString("ssid", "");
    c.pass = prefs_.getString("pass", "");
    c.host = prefs_.getString("host", "");
    c.port = prefs_.getUShort("port", 443);
    c.tls = prefs_.getBool("tls", true);
    prefs_.end();
    return c;
  }

  void save(const NetConfig& c) {
    prefs_.begin("ftracker", false);
    prefs_.putString("ssid", c.ssid);
    prefs_.putString("pass", c.pass);
    prefs_.putString("host", c.host);
    prefs_.putUShort("port", c.port);
    prefs_.putBool("tls", c.tls);
    prefs_.end();
  }

  void wipe() {
    prefs_.begin("ftracker", false);
    prefs_.clear();
    prefs_.end();
  }

  // Blocking portal: serves the config form until saved, then reboots.
  void runPortal(const String& apName) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str(), AP_PASSWORD);
    dns_.start(53, "*", WiFi.softAPIP());

    server_.on("/", [this]() { server_.send(200, "text/html", formHtml()); });
    server_.on("/save", HTTP_POST, [this]() { handleSave(); });
    server_.onNotFound([this]() {  // captive-portal redirect
      server_.sendHeader("Location", "http://192.168.4.1/", true);
      server_.send(302, "text/plain", "");
    });
    server_.begin();

    while (true) {
      dns_.processNextRequest();
      server_.handleClient();
      if (saved_) { delay(1500); ESP.restart(); }
      delay(2);
    }
  }

 private:
  void handleSave() {
    NetConfig c;
    c.ssid = server_.arg("ssid");
    c.pass = server_.arg("pass");
    c.ssid.trim();

    // Server URL: "https://tracker.example.com", "http://192.168.1.50:8080",
    // "tracker.example.com" or "host:port". Scheme decides TLS; a bare host
    // defaults to HTTPS/443 (hosted backend); a bare host:port to plain ws.
    String url = server_.arg("host");
    url.trim();
    if (url.startsWith("https://")) { c.tls = true;  c.port = 443; url = url.substring(8); }
    else if (url.startsWith("http://")) { c.tls = false; c.port = 80; url = url.substring(7); }
    else { c.tls = true; c.port = 443; }
    int slash = url.indexOf('/');
    if (slash >= 0) url = url.substring(0, slash);
    int colon = url.indexOf(':');
    if (colon >= 0) {
      c.port = url.substring(colon + 1).toInt();
      url = url.substring(0, colon);
      if (!server_.arg("host").startsWith("https://") && c.port != 443) c.tls = false;
    }
    c.host = url;
    if (!c.valid() || c.port == 0) { server_.send(400, "text/html", "<h3>Missing Wi-Fi name or server address.</h3><a href='/'>Back</a>"); return; }
    save(c);
    saved_ = true;
    server_.send(200, "text/html",
      "<h2 style='font-family:sans-serif'>Saved &#10003;</h2>"
      "<p style='font-family:sans-serif'>The tracker is restarting and will join your Wi-Fi.</p>");
  }

  String formHtml() {
    String h = F(
      "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Flight Tracker Setup</title><style>"
      "body{font-family:sans-serif;background:#0d1117;color:#e6edf3;padding:20px;max-width:420px;margin:auto}"
      "h2{color:#78d0ff}label{display:block;margin:14px 0 4px;font-size:.85rem;color:#78d0ff}"
      "input{width:100%;padding:10px;border-radius:8px;border:1px solid #1e2833;background:#0a0e13;color:#e6edf3;font-size:1rem}"
      "button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;background:#2ea8ff;color:#fff;font-size:1rem;font-weight:600}"
      "</style></head><body><h2>&#9992; Flight Tracker</h2>"
      "<form method='POST' action='/save'>"
      "<label>Home Wi-Fi name (SSID)</label><input name='ssid' required>"
      "<label>Wi-Fi password</label><input name='pass' type='password'>"
      "<label>Server URL</label><input name='host' value='");
    h += F(DEFAULT_SERVER_URL);
    h += F("' required>"
      "<div style='font-size:.75rem;color:#7d8b99;margin-top:4px'>Leave as-is &mdash; only change for local testing (http://192.168.1.50:8080)</div>"
      "<button type='submit'>Save &amp; restart</button></form></body></html>");
    return h;
  }

  Preferences prefs_;
  WebServer server_{80};
  DNSServer dns_;
  bool saved_ = false;
};
