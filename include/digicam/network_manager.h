#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "types.h"
namespace dc {
class NetworkManager {
 public:
  bool begin(NetworkMode mode,bool sdAvailable);
  void loop();
  void end();
  bool active() const { return mode_!=NetworkMode::Off; }
  NetworkMode mode() const { return mode_; }
  String ipAddress() const { return WiFi.softAPIP().toString(); }
  void setProgressCallback(void (*callback)(int)) { progressCallback_=callback; }
  bool consumeTimeSync(time_t& epochSeconds, int16_t& utcOffsetMinutes);
 private:
  void registerRoutes();
  bool authenticated();
  void sendHome();
  void sendFile();
  void updateUpload();
  void handleTimeSync();
  WebServer server_{80};
  NetworkMode mode_=NetworkMode::Off;
  bool sdAvailable_=false,serverStarted_=false,routesRegistered_=false;
  bool uploadStarted_=false,uploadFailed_=false,updateComplete_=false;
  bool timeSyncPending_=false;
  time_t timeSyncEpoch_=0;
  int16_t timeSyncUtcOffsetMinutes_=0;
  String uploadError_;
  size_t uploadBytesWritten_=0;
  void (*progressCallback_)(int)=nullptr;
};
}
