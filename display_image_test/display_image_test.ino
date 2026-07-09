// display_image_test — bench sketch for the flight-tracker's 3.5" TFT.
//
// Same board/wiring as device/flight_tracker/ (ESP32-S3 + TFT_eSPI, driver
// config in the library's User_Setup.h — untouched here, see CLAUDE.md Iron
// Rule 6). Two modes, cycled with the BOOT button or Serial:
//
//   IMAGE mode — decodes and draws JPEGs through the *exact* path used in
//   production (device/flight_tracker/photo.h): TJpg_Decoder, scale 1,
//   native byte order, per-pixel drawPixel callback — never pushImage /
//   pushPixels (Iron Rule 4, avoids the byte-phase-slip garbage on this
//   panel's clone parallel bus). Drawn inside the same photo-panel rect as
//   display_ui.h's Classic layout, so proportions match the real device.
//   This is a driver/library fidelity test only: it does NOT run the
//   backend's PANEL_TONE_GAMMA / PANEL_TONE_MAX / PANEL_GREEN_CURVE LUT
//   (backend/server.js) — colors are raw decode output, not the tuned
//   production look. That's deliberate; tune the backend LUT separately.
//
//   CAL mode — RGB565 primary/secondary swatches, a grayscale ramp, and
//   smooth per-channel gradients, all drawn raw (no LUT) to characterize
//   the panel + driver path in isolation from any photo content.
//
// Setup:
//   1. Library Manager: TFT_eSPI, TJpg_Decoder (same versions as
//      device/flight_tracker — see its top-of-file comment).
//   2. Get test photos into images/, either:
//        - drop your own JPEGs in directly, or
//        - `python3 fetch_test_photos.py <hex> [<hex> ...]` to pull real
//          aircraft photos straight from planespotters.net by ICAO24 hex
//          (same API + User-Agent contract as backend/lib/upstream.js).
//      Real-world photos are almost always progressive-encoded and larger
//      than the 282x217 panel target — TJpg_Decoder can only decode
//      baseline JPEG (same constraint as production). convert_images.py
//      auto resize+re-encodes anything that doesn't already fit when
//      Pillow is installed; without Pillow it skips non-conforming files
//      with instructions instead.
//   3. Run `python3 convert_images.py` from this folder. It (re)writes
//      images_data.h, a PROGMEM byte array per photo. Re-run it whenever
//      images/ changes, then re-upload the sketch.
//
// Controls:
//   BOOT button (GPIO0 — same pin as the production reset button; unused
//   here since nothing else is wired):
//     short press  -> next image (IMAGE mode) / next page (CAL mode)
//     long press (>800 ms) -> toggle IMAGE <-> CAL
//   Serial (115200) mirrors this: 'n' = next, 'm' = toggle mode.

#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "images_data.h"

TFT_eSPI tft = TFT_eSPI();

#define BTN_PIN 0
#define LONG_PRESS_MS 800

enum Mode { MODE_IMAGE, MODE_CAL };
static Mode mode_ = MODE_IMAGE;
static int imgIdx_ = 0;
static int calPage_ = 0;
static const int CAL_PAGE_COUNT = 3;

// ---------------------------------------------------------------------------
// IMAGE mode — identical decode/draw path to device/flight_tracker/photo.h
// ---------------------------------------------------------------------------

static bool photoPushBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  for (uint16_t j = 0; j < h; j++)
    for (uint16_t i = 0; i < w; i++)
      tft.drawPixel(x + i, y + j, bitmap[j * w + i]);
  return true;
}

// Mirrors display_ui.h drawFlightClassic's photo-panel rect exactly (by =
// H*0.49, mx = W*0.06, 18px credit-line reserve) so the panel proportions
// on this bench sketch match the real device.
static void photoPanelRect(int& mx, int& by, int& bw, int& bh) {
  int W = tft.width(), H = tft.height();
  mx = W * 0.06;
  by = H * 0.49;
  bw = W - 2 * mx;
  bh = H - by - 18;
}

static void drawTestImage(int idx) {
  tft.fillScreen(TFT_BLACK);
  int mx, by, bw, bh;
  photoPanelRect(mx, by, bw, bh);
  tft.drawRect(mx - 1, by - 1, bw + 2, bh + 2, tft.color565(30, 40, 50));

  if (TEST_IMAGE_COUNT == 0) {
    tft.setTextColor(TFT_WHITE); tft.setTextSize(2);
    tft.setCursor(mx, 10);  tft.println("No images embedded.");
    tft.setTextSize(1);
    tft.setCursor(mx, 40);  tft.println("Drop baseline JPEGs into images/,");
    tft.setCursor(mx, 55);  tft.println("run convert_images.py, reflash.");
    return;
  }

  const TestImage& im = TEST_IMAGES[idx];
  uint16_t jw = 0, jh = 0;
  TJpgDec.setJpgScale(1);          // no scaling: draw at native decoded size
  TJpgDec.setSwapBytes(false);     // drawPixel takes plain RGB565, like photo.h
  TJpgDec.setCallback(photoPushBlock);
  bool ok = false;
  if (TJpgDec.getJpgSize(&jw, &jh, im.data, im.len) == JDR_OK && jw > 0 && jw <= bw && jh <= bh) {
    ok = TJpgDec.drawJpg(mx + (bw - jw) / 2, by + (bh - jh) / 2, im.data, im.len) == JDR_OK;
  }

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(1);
  String label = String(idx + 1) + "/" + String(TEST_IMAGE_COUNT) + ": " + im.name;
  if (!ok) label += "  [DECODE FAIL - check size/baseline]";
  tft.drawString(label, tft.width() / 2, tft.height() - 5);
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------------
// CAL mode — raw panel/driver characterization, no backend LUT involved.
// ---------------------------------------------------------------------------

struct Swatch { const char* label; uint8_t r, g, b; };
static const Swatch SWATCHES[] = {
  {"RED",     255, 0,   0},
  {"GREEN",   0,   255, 0},
  {"BLUE",    0,   0,   255},
  {"WHITE",   255, 255, 255},
  {"BLACK",   0,   0,   0},
  {"CYAN",    0,   255, 255},
  {"MAGENTA", 255, 0,   255},
  {"YELLOW",  255, 255, 0},
};
static const int SWATCH_COUNT = sizeof(SWATCHES) / sizeof(SWATCHES[0]);

static void calFooter(const char* text) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(1);
  tft.setCursor(4, tft.height() - 10);
  tft.print(text);
}

// Solid single-color fills: byte-symmetric, safe on this panel's parallel
// bus per Iron Rule 4 (only variable-pixel block pushes cause the slip).
static void drawSwatchGrid() {
  tft.fillScreen(TFT_BLACK);
  int W = tft.width(), H = tft.height();
  int cols = 2, rows = (SWATCH_COUNT + 1) / 2;
  int cw = W / cols, ch = (H - 20) / rows;
  for (int i = 0; i < SWATCH_COUNT; i++) {
    int col = i % cols, row = i / cols;
    int x = col * cw, y = row * ch;
    uint16_t c = tft.color565(SWATCHES[i].r, SWATCHES[i].g, SWATCHES[i].b);
    tft.fillRect(x + 4, y + 4, cw - 8, ch - 24, c);
    tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(1);
    tft.setCursor(x + 4, y + ch - 18);
    tft.print(SWATCHES[i].label); tft.print(" 0x"); tft.print(c, HEX);
  }
  calFooter("CAL 1/3: primary/secondary swatches");
}

static void drawGrayRamp() {
  tft.fillScreen(TFT_BLACK);
  int W = tft.width(), H = tft.height();
  int steps = 8;
  int barH = (H - 20) / steps;
  for (int step = 0; step < steps; step++) {
    uint8_t v = step * 255 / (steps - 1);
    uint16_t c = tft.color565(v, v, v);
    int y = step * barH;
    tft.fillRect(0, y, W, barH - 2, c);
    tft.setTextColor(v > 128 ? TFT_BLACK : TFT_WHITE);
    tft.setCursor(4, y + 2);
    tft.print(v);
  }
  calFooter("CAL 2/3: grayscale ramp (0-255, 8 steps)");
}

// Pixel value varies along x, so this is drawn per-pixel with drawPixel —
// same primitive photo.h's decode callback uses, never a block/DMA push.
static void drawChannelGradients() {
  tft.fillScreen(TFT_BLACK);
  int W = tft.width(), H = tft.height();
  int barH = (H - 20) / 3;
  for (int ch = 0; ch < 3; ch++) {
    int y0 = ch * barH;
    for (int x = 0; x < W; x++) {
      uint8_t v = (uint32_t)x * 255 / (W - 1);
      uint16_t c = (ch == 0) ? tft.color565(v, 0, 0)
                 : (ch == 1) ? tft.color565(0, v, 0)
                             : tft.color565(0, 0, v);
      for (int y = y0; y < y0 + barH - 2; y++) tft.drawPixel(x, y, c);
    }
  }
  calFooter("CAL 3/3: R/G/B smooth gradients (0-255)");
}

static void drawCalPage(int page) {
  if (page == 0) drawSwatchGrid();
  else if (page == 1) drawGrayRamp();
  else drawChannelGradients();
}

// ---------------------------------------------------------------------------
// Input: BOOT button (short = next, long = toggle mode) + Serial mirror
// ---------------------------------------------------------------------------

static unsigned long btnDownAt_ = 0;
static bool btnWasDown_ = false;

static void nextItem() {
  if (mode_ == MODE_IMAGE) {
    if (TEST_IMAGE_COUNT > 0) imgIdx_ = (imgIdx_ + 1) % TEST_IMAGE_COUNT;
    drawTestImage(imgIdx_);
  } else {
    calPage_ = (calPage_ + 1) % CAL_PAGE_COUNT;
    drawCalPage(calPage_);
  }
}

static void toggleMode() {
  mode_ = (mode_ == MODE_IMAGE) ? MODE_CAL : MODE_IMAGE;
  if (mode_ == MODE_IMAGE) drawTestImage(imgIdx_);
  else drawCalPage(calPage_);
}

static void pollButton() {
  bool down = digitalRead(BTN_PIN) == LOW;
  if (down && !btnWasDown_) btnDownAt_ = millis();
  if (!down && btnWasDown_) {
    unsigned long held = millis() - btnDownAt_;
    if (held >= LONG_PRESS_MS) toggleMode();
    else nextItem();
  }
  btnWasDown_ = down;
}

static void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'n') nextItem();
    else if (c == 'm') toggleMode();
  }
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // never block loop() on an unattached monitor
#endif
  pinMode(BTN_PIN, INPUT_PULLUP);

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  Serial.println("[display_image_test] 'n' = next, 'm' = toggle image/cal mode (BOOT button mirrors this)");
  Serial.printf("[display_image_test] %d image(s) embedded\n", TEST_IMAGE_COUNT);

  drawTestImage(imgIdx_);
}

void loop() {
  pollButton();
  pollSerial();
}
