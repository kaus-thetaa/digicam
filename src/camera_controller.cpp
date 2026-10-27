#include "digicam/camera_controller.h"
#include "digicam/config.h"
#include <esp_heap_caps.h>
namespace dc {
static const framesize_t kSizes[] = { FRAMESIZE_VGA, FRAMESIZE_QVGA };
static constexpr uint16_t kWidths[] = {640,320};
static constexpr uint16_t kHeights[] = {480,240};
bool CameraController::beginAt(framesize_t fs){
  camera_config_t c{};
  c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=cfg::CAMERA_Y2; c.pin_d1=cfg::CAMERA_Y3; c.pin_d2=cfg::CAMERA_Y4; c.pin_d3=cfg::CAMERA_Y5;
  c.pin_d4=cfg::CAMERA_Y6; c.pin_d5=cfg::CAMERA_Y7; c.pin_d6=cfg::CAMERA_Y8; c.pin_d7=cfg::CAMERA_Y9;
  c.pin_xclk=cfg::CAMERA_XCLK; c.pin_pclk=cfg::CAMERA_PCLK; c.pin_vsync=cfg::CAMERA_VSYNC; c.pin_href=cfg::CAMERA_HREF;
  c.pin_sccb_sda=cfg::CAMERA_SIOD; c.pin_sccb_scl=cfg::CAMERA_SIOC;
  c.pin_pwdn=cfg::CAMERA_PWDN; c.pin_reset=cfg::CAMERA_RESET; c.xclk_freq_hz=cfg::XCLK_HZ;
  c.pixel_format=PIXFORMAT_YUV422; c.frame_size=fs; c.jpeg_quality=12; c.fb_count=1;
  c.fb_location=psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  c.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
  return esp_camera_init(&c)==ESP_OK;
}
bool CameraController::validateFrame(){
  camera_fb_t* fb=esp_camera_fb_get();
  bool ok=fb && fb->format==PIXFORMAT_YUV422 && fb->width==kWidths[resolutionIndex_] && fb->height==kHeights[resolutionIndex_] && fb->len >= (size_t)fb->width*fb->height*2U;
  if(fb) esp_camera_fb_return(fb);
  return ok;
}
bool CameraController::begin(){
  if(ready_) return true;
  // Actual user's known-good mode is VGA. Keep it first; retry at QVGA only if validation fails.
  for(uint8_t idx=0;idx<2;idx++){
    resolutionIndex_=idx;
    if(beginAt(kSizes[idx])){
      if(validateFrame()) {ready_=true; maxResolutionIndex_=idx; return true;}
      esp_camera_deinit();
    }
  }
  ready_=false; pinMode(cfg::CAMERA_PWDN,OUTPUT); digitalWrite(cfg::CAMERA_PWDN,HIGH); return false;
}
void CameraController::end(){ if(ready_) esp_camera_deinit(); ready_=false; pinMode(cfg::CAMERA_PWDN,OUTPUT); digitalWrite(cfg::CAMERA_PWDN,HIGH); delay(10); }
bool CameraController::setResolution(uint8_t index){
  if(!ready_ || index>1 || index<maxResolutionIndex_) return false;
  if(index==resolutionIndex_) return true;
  sensor_t* sensor=esp_camera_sensor_get(); if(!sensor || !sensor->set_framesize) return false;
  if(sensor->set_framesize(sensor,kSizes[index])!=0) return false;
  resolutionIndex_=index;
  camera_fb_t* drop=esp_camera_fb_get(); if(drop) esp_camera_fb_return(drop);
  return true;
}
camera_fb_t* CameraController::acquire(){return ready_?esp_camera_fb_get():nullptr;}
void CameraController::release(camera_fb_t* fb){if(fb) esp_camera_fb_return(fb);}
bool CameraController::exposureCompensationSupported() const {
  sensor_t* s=ready_?esp_camera_sensor_get():nullptr; return s && s->set_ae_level;
}
bool CameraController::sensorAutoExposureAvailable() const {
  sensor_t* s=ready_?esp_camera_sensor_get():nullptr; return s && s->set_exposure_ctrl;
}
void CameraController::enableAutoExposure(){
  sensor_t* s=ready_?esp_camera_sensor_get():nullptr; if(!s)return;
  if(s->set_exposure_ctrl)s->set_exposure_ctrl(s,1);
  if(s->set_gain_ctrl)s->set_gain_ctrl(s,1);
  if(s->set_whitebal)s->set_whitebal(s,1);
  if(s->set_awb_gain)s->set_awb_gain(s,1);
}
bool CameraController::exposureLockSupported() const {
  sensor_t* s=ready_?esp_camera_sensor_get():nullptr; return s && s->set_exposure_ctrl && s->set_gain_ctrl;
}
bool CameraController::lockExposure(){
  sensor_t* s=ready_?esp_camera_sensor_get():nullptr;
  if(!s || !s->set_exposure_ctrl || !s->set_gain_ctrl)return false;
  // Freeze the sensor's current exposure/gain registers after metering. White balance stays automatic.
  bool ok=(s->set_exposure_ctrl(s,0)==0)&&(s->set_gain_ctrl(s,0)==0);
  if(s->set_whitebal)s->set_whitebal(s,1);
  if(s->set_awb_gain)s->set_awb_gain(s,1);
  return ok;
}
bool CameraController::setExposureCompensation(int8_t ev){
  if(ev < -2 || ev > 2 || !ready_) return false;
  sensor_t* s=esp_camera_sensor_get();
  // Fail closed: a declared callback is the only generic capability signal available.
  if(!s || !s->set_ae_level) return false;
  return s->set_ae_level(s,ev)==0;
}
}
