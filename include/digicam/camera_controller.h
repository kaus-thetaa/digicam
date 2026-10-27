#pragma once
#include <Arduino.h>
#include <esp_camera.h>
#include "types.h"
namespace dc {
class CameraController {
 public:
  bool begin();
  void end();
  bool ready() const { return ready_; }
  bool setResolution(uint8_t index);
  uint8_t resolutionIndex() const { return resolutionIndex_; }
  uint8_t maxResolutionIndex() const { return maxResolutionIndex_; }
  camera_fb_t* acquire();
  void release(camera_fb_t* fb);
  bool setExposureCompensation(int8_t ev);
  void enableAutoExposure();
  bool lockExposure();
  bool exposureLockSupported() const;
  bool exposureCompensationSupported() const;
  bool sensorAutoExposureAvailable() const;
 private:
  bool beginAt(framesize_t size);
  bool validateFrame();
  bool ready_=false;
  uint8_t resolutionIndex_=0;
  uint8_t maxResolutionIndex_=0;
};
}
