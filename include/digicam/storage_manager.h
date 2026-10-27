#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include "types.h"
namespace dc {
class StorageManager {
 public:
  bool begin();
  bool ready() const { return ready_; }
  // The ESP32 img_converters API declares fmt2jpg_cb source as uint8_t* (non-const).
  // This method accepts the mutable transformed YUV buffer created by ImageProcessor.
  bool saveJpegFromYuv(uint8_t* yuv,size_t len,uint16_t width,uint16_t height,String& savedPath);
  uint32_t freeBytes() const;
 private:
  bool ensureDcim();
  String nextPath();
  static size_t writeCallback(void* arg,size_t index,const void* data,size_t len);
  bool ready_=false;
  uint32_t nextNumber_=1;
};
}
