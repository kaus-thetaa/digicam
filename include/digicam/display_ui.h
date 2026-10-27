#pragma once
#include <Arduino.h>
#include <U8g2lib.h>
#include "types.h"
namespace dc {
class DisplayUI {
 public:
  bool begin();
  bool ready() const { return ready_; }
  void splash(const char* line);
  void home(const Settings& s,const char* message,bool cameraOk,bool sdOk,bool evSupported,bool clockValid,uint32_t freeBytes);
  void menu(const char* title,uint8_t selected,const char* const* rows,uint8_t count);
  void preview(const uint8_t* bitmap,uint8_t x,uint8_t y,uint8_t w,uint8_t h);
  void orientationPreview(const uint8_t* bitmap,const char* rotation);
  void timer(uint8_t seconds,bool ledOn);
  void network(NetworkMode mode,const char* line);
  void message(const char* title,const char* detail);
  void otaProgress(int percent);
 private:
  bool ready_=false;
  U8G2_SH1106_128X64_NONAME_F_HW_I2C oled_{U8G2_R0,U8X8_PIN_NONE};
};
}
