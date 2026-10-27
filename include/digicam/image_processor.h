#pragma once
#include <Arduino.h>
#include <esp_camera.h>
#include "types.h"
namespace dc {
class ImageProcessor {
 public:
  // Returns a PSRAM-backed YUV422 output frame. Caller must free() it.
  // Input frame is never modified. The optional previous frame is used by Ghost mode.
  static uint8_t* transform(const camera_fb_t* current, const uint8_t* previous, size_t previousLen,
                            Rotation rotation, Style style, bool grain,
                            ImageDimensions& outputDims, size_t& outputLen, const char* timestampText=nullptr);
  // A mono composition aid; uses the same output-coordinate transform as saved photos.
  static bool makePreview(const camera_fb_t* frame, Rotation rotation,
                          uint8_t* bitmap, size_t bitmapLen,
                          uint8_t& imageX, uint8_t& imageY, uint8_t& imageW, uint8_t& imageH);
 private:
  static uint8_t luma(const uint8_t* src, uint16_t w, uint16_t x, uint16_t y);
  static uint8_t chromaU(const uint8_t* src, uint16_t w, uint16_t x, uint16_t y);
  static uint8_t chromaV(const uint8_t* src, uint16_t w, uint16_t x, uint16_t y);
};
}
