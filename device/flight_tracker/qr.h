// QR rendering onto the TFT (black modules on a white quiet-zone so phone
// cameras lock on). Used for the setup-portal join code and the web-app link.
#pragma once
#include <TFT_eSPI.h>
#include "qrcodegen.h"   // vendored ricmoo QR generator (avoids clashing with
                         // the ESP32 core's own espressif__qrcode/qrcode.h)

extern TFT_eSPI tft;

// Draw `text` as a QR centered at (cx,cy). `scale` = pixels per module.
// version 4 (33x33 modules) holds up to ~78 bytes — plenty for a URL or a
// Wi-Fi join string. Returns the drawn square's side length (px), 0 on error.
inline int drawQR(const String& text, int cx, int cy, int scale) {
  const uint8_t version = 4;             // 33x33 modules
  QRCode qr;
  uint8_t data[200];                     // version 4 needs 137 B; 200 is safe
  if (qrcode_initText(&qr, data, version, ECC_LOW, text.c_str()) < 0) return 0;

  const int quiet = 3;                       // quiet-zone modules each side
  int side = (qr.size + 2 * quiet) * scale;
  int x0 = cx - side / 2, y0 = cy - side / 2;
  tft.fillRect(x0, y0, side, side, TFT_WHITE);  // background + quiet zone
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        tft.fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale,
                     scale, scale, TFT_BLACK);
  return side;
}
