#include "digicam/display_ui.h"
#include "digicam/config.h"
#include <Wire.h>

namespace dc {
namespace {
static const char* kRot[] = {"0 DEG", "90 CW", "180 DEG", "90 CCW"};
static const char* kRes[] = {"VGA", "QVGA"};
static const char* kStyle[] = {"Normal", "Vintage", "Warm", "Noir", "Cinema"};
static const char* kMode[] = {"Auto", "Exp. lock", "Timer 3s", "Timer 5s", "Timer 10s", "Low light", "Ghost"};
static uint8_t bound(uint8_t v, uint8_t max) { return v > max ? max : v; }

// Truncate by actual rendered pixel width, not character count; this matters
// because Helvetica-style glyphs have variable widths on the 128px display.
static String fitText(U8G2& oled, const String& input, uint8_t maxWidth) {
  if (oled.getStrWidth(input.c_str()) <= maxWidth) return input;
  String out = input;
  const String suffix = "...";
  while (out.length() > 0 && oled.getStrWidth((out + suffix).c_str()) > maxWidth) {
    out = out.substring(0, out.length() - 1U);
  }
  return out + suffix;
}

void drawShutter(U8G2& o, uint8_t cx, uint8_t cy) {
  o.drawCircle(cx, cy, 10, U8G2_DRAW_ALL);
  o.drawCircle(cx, cy, 6, U8G2_DRAW_ALL);
  o.drawLine(cx - 3, cy - 8, cx + 3, cy - 3);
  o.drawLine(cx + 8, cy - 2, cx + 3, cy + 3);
  o.drawLine(cx + 2, cy + 8, cx - 3, cy + 3);
  o.drawLine(cx - 8, cy + 1, cx - 3, cy - 3);
}
}  // namespace

bool DisplayUI::begin() {
  Wire.begin(cfg::OLED_SDA, cfg::OLED_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  delay(20);
  uint8_t addr = cfg::OLED_ADDRESS;
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() != 0) {
    addr = (addr == 0x3C) ? 0x3D : 0x3C;
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) return false;
  }
  oled_.setI2CAddress(static_cast<uint8_t>(addr << 1));
  ready_ = oled_.begin();
  if (ready_) oled_.setContrast(180);
  return ready_;
}

void DisplayUI::splash(const char* line) {
  if (!ready_) return;
  // A short, low-cost four-frame loader: no sprites, heap allocations or long delays.
  for (uint8_t frame = 0; frame < 4; ++frame) {
    oled_.clearBuffer();
    oled_.drawFrame(0, 0, 128, 64);
    oled_.setFont(u8g2_font_helvB12_tf);
    oled_.drawStr(10, 22, "DigiCam");
    oled_.setFont(u8g2_font_helvR08_tf);
    oled_.drawStr(11, 35, "V12  /  OFFLINE CAMERA");
    oled_.drawHLine(10, 43, 108);
    const uint8_t segments = static_cast<uint8_t>(frame + 1);
    for (uint8_t i = 0; i < 8; ++i) {
      const uint8_t x = static_cast<uint8_t>(12 + i * 13);
      if (i < segments) oled_.drawBox(x, 48, 8, 3);
      else oled_.drawFrame(x, 48, 8, 3);
    }
    oled_.setFont(u8g2_font_5x8_tf);
    String text = line ? String(line) : String("Starting");
    if (text.length() > 22) text = text.substring(0, 22);
    oled_.drawStr(10, 60, text.c_str());
    oled_.sendBuffer();
    delay(38);
  }
}

void DisplayUI::home(const Settings& s, const char* msg, bool cam, bool sd,
                     bool ev, bool clockValid, uint32_t freeBytes) {
  if (!ready_) return;
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_helvB08_tf);
  oled_.drawStr(0, 9, "DIGICAM");
  oled_.setFont(u8g2_font_5x8_tf);
  oled_.drawStr(103, 9, "V12");
  oled_.drawHLine(0, 13, 128);

  String heading;
  if (msg && *msg && strcmp(msg, "Ready") != 0 && strcmp(msg, "Camera ready") != 0) heading = msg;
  else heading = cam ? "READY" : "CAMERA ERROR";
  oled_.setFont(u8g2_font_helvB12_tf);
  heading = fitText(oled_, heading, 96);
  oled_.drawStr(0, 30, heading.c_str());
  drawShutter(oled_, 111, 27);

  oled_.setFont(u8g2_font_helvR08_tf);
  String first = String(kMode[bound(s.mode, 6)]) + "  /  " + kRes[bound(s.resolution, 1)] + "  /  " + kRot[bound(s.rotation, 3)];
  first = fitText(oled_, first, 128);
  oled_.drawStr(0, 41, first.c_str());

  oled_.drawHLine(0, 46, 128);
  oled_.setFont(u8g2_font_5x8_tf);
  String status = String("SD ") + (sd ? "OK" : "--") + "   TIME " + (clockValid ? "SET" : "--");
  status = fitText(oled_, status, 128);
  oled_.drawStr(0, 56, status.c_str());
  String bottom = String("STAMP ") + (s.timestampEnabled ? (clockValid ? "ON" : "SET TIME") : "OFF");
  if (ev) bottom += String("   EV ") + (s.ev > 0 ? "+" : "") + String(s.ev);
  bottom = fitText(oled_, bottom, 128);
  oled_.drawStr(0, 63, bottom.c_str());
  oled_.sendBuffer();
  (void)freeBytes;  // Capacity is available in the SD menu/status page if needed later.
}

void DisplayUI::menu(const char* title, uint8_t selected, const char* const* rows, uint8_t count) {
  if (!ready_) return;
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_helvB08_tf);
  String header = title ? String(title) : String("MENU");
  if (header.length() > 20) header = header.substring(0, 20);
  oled_.drawStr(0, 9, header.c_str());
  char page[12];
  snprintf(page, sizeof(page), "%u/%u", (unsigned)(selected + 1), (unsigned)count);
  oled_.setFont(u8g2_font_5x8_tf);
  int pageWidth = oled_.getStrWidth(page);
  oled_.drawStr(127 - pageWidth, 9, page);
  oled_.drawHLine(0, 13, 128);

  const uint8_t visible = 4;
  uint8_t first = selected > 1 ? static_cast<uint8_t>(selected - 1) : 0;
  if (count > visible && first + visible > count) first = static_cast<uint8_t>(count - visible);
  for (uint8_t row = 0; row < visible && first + row < count; ++row) {
    const uint8_t idx = static_cast<uint8_t>(first + row);
    const uint8_t y = static_cast<uint8_t>(16 + row * 10);
    if (idx == selected) {
      oled_.drawBox(0, y, 128, 10);
      oled_.setDrawColor(0);
    }
    oled_.setFont(u8g2_font_helvR08_tf);
    const String text = fitText(oled_, String(rows[idx] ? rows[idx] : ""), 120);
    oled_.drawStr(4, static_cast<uint8_t>(y + 8), text.c_str());
    if (idx == selected) oled_.setDrawColor(1);
  }
  oled_.drawHLine(0, 57, 128);
  oled_.setFont(u8g2_font_4x6_tf);
  oled_.drawStr(1, 63, "TAP NEXT   HOLD OK   DBL BACK");
  oled_.sendBuffer();
}

void DisplayUI::preview(const uint8_t* bitmap, uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  if (!ready_ || !bitmap) return;
  oled_.clearBuffer();
  oled_.drawXBMP(0, 0, 128, 64, bitmap);
  oled_.drawFrame(x, y, w, h);
  oled_.sendBuffer();
}

void DisplayUI::orientationPreview(const uint8_t* bitmap, const char* rotation) {
  if (!ready_ || !bitmap) return;
  oled_.clearBuffer();
  oled_.drawXBMP(0, 0, 128, 64, bitmap);
  oled_.setDrawColor(0);
  oled_.drawBox(0, 49, 128, 15);
  oled_.setDrawColor(1);
  oled_.drawHLine(0, 49, 128);
  oled_.setFont(u8g2_font_helvB08_tf);
  oled_.drawStr(3, 60, rotation ? rotation : "ORIENTATION");
  oled_.setFont(u8g2_font_4x6_tf);
  oled_.drawStr(80, 60, "TAP NEXT");
  oled_.sendBuffer();
}

void DisplayUI::timer(uint8_t seconds, bool ledOn) {
  if (!ready_) return;
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_helvB08_tf);
  oled_.drawStr(0, 9, "SELF TIMER");
  oled_.drawHLine(0, 13, 128);
  oled_.setFont(u8g2_font_logisoso32_tn);
  char value[4];
  snprintf(value, sizeof(value), "%u", seconds);
  int w = oled_.getStrWidth(value);
  oled_.drawStr((128 - w) / 2, 48, value);
  oled_.setFont(u8g2_font_helvR08_tf);
  oled_.drawStr(0, 61, ledOn ? "STATUS  /  SIGNAL" : "HOLD OR DBL  /  CANCEL");
  oled_.sendBuffer();
}

void DisplayUI::network(NetworkMode mode, const char* line) {
  if (!ready_) return;
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_helvB08_tf);
  const char* title = mode == NetworkMode::FirmwareUpdate ? "FIRMWARE UPDATE" :
                      mode == NetworkMode::TimeSetup ? "DATE & TIME" : "PHOTO TRANSFER";
  oled_.drawStr(0, 9, title);
  oled_.drawHLine(0, 13, 128);
  oled_.setFont(u8g2_font_helvB10_tf);
  const char* heading = mode == NetworkMode::FirmwareUpdate ? "CAMERA OFF" :
                        mode == NetworkMode::TimeSetup ? "SYNC CLOCK" : "TRANSFER ONLY";
  oled_.drawStr(0, 29, heading);
  oled_.setFont(u8g2_font_helvR08_tf);
  const char* ssid = mode == NetworkMode::FirmwareUpdate ? "DigiCam-V12-Update" :
                     mode == NetworkMode::TimeSetup ? "DigiCam-Time" : "DigiCam-Transfer";
  oled_.drawStr(0, 41, ssid);
  String s = fitText(oled_, line ? String(line) : String("192.168.4.1"), 128);
  oled_.drawStr(0, 51, s.c_str());
  oled_.drawHLine(0, 55, 128);
  oled_.setFont(u8g2_font_4x6_tf);
  oled_.drawStr(1, 62, "OPEN 192.168.4.1   HOLD EXIT");
  oled_.sendBuffer();
}

void DisplayUI::message(const char* title, const char* detail) {
  if (!ready_) return;
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_helvB10_tf);
  String t = fitText(oled_, title ? String(title) : String("NOTICE"), 128);
  oled_.drawStr(0, 14, t.c_str());
  oled_.drawHLine(0, 18, 128);
  oled_.setFont(u8g2_font_helvR08_tf);
  String d = fitText(oled_, detail ? String(detail) : String(""), 128);
  oled_.drawStr(0, 34, d.c_str());
  oled_.setFont(u8g2_font_4x6_tf);
  oled_.drawStr(0, 62, "HOLD: SELECT / DBL: BACK");
  oled_.sendBuffer();
}

void DisplayUI::otaProgress(int percent) {
  if (!ready_) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_helvB08_tf);
  oled_.drawStr(0, 10, "FIRMWARE UPDATE");
  oled_.drawHLine(0, 14, 128);
  oled_.drawFrame(7, 27, 114, 12);
  const uint8_t fill = static_cast<uint8_t>((110UL * static_cast<uint32_t>(percent)) / 100UL);
  if (fill > 0) oled_.drawBox(9, 29, fill, 8);
  char text[24];
  snprintf(text, sizeof(text), "Writing image  %d%%", percent);
  oled_.setFont(u8g2_font_helvR08_tf);
  oled_.drawStr(7, 54, fitText(oled_, String(text), 114).c_str());
  oled_.sendBuffer();
}
}  // namespace dc
