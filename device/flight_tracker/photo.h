// Aircraft photo for the Classic layout: downloads the backend-resized JPEG
// (already scaled to fit the panel, always baseline-encoded) and pushes it to
// the TFT via TJpg_Decoder. Any failure returns false and the caller falls
// back to the vector silhouette bitmaps.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <TJpg_Decoder.h>
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

#define PHOTO_MAX_BYTES 65536      // backend thumbs are ~15-30 KB
#define PHOTO_FETCH_TIMEOUT_MS 10000

static bool photoPushBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // Draw pixel-by-pixel instead of one pushImage stream. The decoded output
  // is verified correct (host-decoded with the same tjpgd.c); the corruption
  // seen on hardware (blocky pastel/pink garbage) is a byte-phase slip in
  // long back-to-back parallel-bus pixel streams. Per-pixel addressing
  // re-syncs the bus at every pixel so a slip can never propagate. The photo
  // draws once per flight, so the extra ~0.5 s is invisible in practice.
  for (uint16_t j = 0; j < h; j++)
    for (uint16_t i = 0; i < w; i++)
      tft.drawPixel(x + i, y + j, bitmap[j * w + i]);
  return true;
}

// Fetch `url` and draw the JPEG centered inside the (x,y,w,h) box. Fills
// `photographer` (from the x-photographer header) for the credit line.
inline bool drawAircraftPhoto(const String& url, int x, int y, int w, int h,
                              String* photographer = nullptr) {
  if (url.length() == 0 || WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  NetworkClientSecure tlsClient;
  NetworkClient plainClient;
  bool tls = url.startsWith("https://");
  if (tls) tlsClient.setInsecure();  // prototype: no cert pinning (matches WS)
  if (!(tls ? http.begin(tlsClient, url) : http.begin(plainClient, url))) return false;
  const char* hdrs[] = { "x-photographer" };
  http.collectHeaders(hdrs, 1);
  http.setTimeout(PHOTO_FETCH_TIMEOUT_MS);

  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len <= 0 || len > PHOTO_MAX_BYTES) { http.end(); return false; }

  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { http.end(); return false; }
  NetworkClient* stream = http.getStreamPtr();
  int got = 0;
  uint32_t t0 = millis();
  while (got < len && millis() - t0 < PHOTO_FETCH_TIMEOUT_MS) {
    int n = stream->read(buf + got, len - got);
    if (n > 0) got += n;
    else delay(5);
  }
  if (photographer) *photographer = http.header("x-photographer");
  http.end();
  if (got != len) { free(buf); return false; }

  uint16_t jw = 0, jh = 0;
  TJpgDec.setJpgScale(1);            // backend already sized it to the box
  // Native byte order: drawPixel takes plain RGB565 like every text colour.
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(photoPushBlock);
  if (TJpgDec.getJpgSize(&jw, &jh, buf, len) != JDR_OK
      || jw == 0 || jw > w || jh > h) { free(buf); return false; }
  bool ok = TJpgDec.drawJpg(x + (w - jw) / 2, y + (h - jh) / 2, buf, len) == JDR_OK;
  free(buf);
  return ok;
}
