#pragma once
#include <Arduino.h>
#include <time.h>
#include "types.h"
#include "settings_store.h"
#include "camera_controller.h"
#include "storage_manager.h"
#include "image_processor.h"
#include "display_ui.h"
#include "button_input.h"
#include "network_manager.h"
namespace dc {
class DigiCamApp {
 public:
  void begin();
  void loop();
 private:
  enum class MenuPage : uint8_t { Root=0, Capture=1, Image=2, Display=3, Connectivity=4, Settings=5 };
  void handleButton(ButtonEvent e);
  void openMenu();
  void closeMenu();
  void selectMenu();
  void backMenu();
  void persistSettings();
  void refreshScreen();
  void startNetwork(NetworkMode mode);
  void stopNetwork();
  void startTimer(uint8_t seconds);
  void cancelTimer();
  void capturePhoto();
  void applyStoredSettings();
  void enterOrientationWizard();
  void updateClockFromNetwork();
  bool makeTimestamp(char* out,size_t len) const;
  const char* rotationLabel() const;
  static void onOtaProgress(int percent);
  Settings settings_{};
  SettingsStore settingsStore_{};
  CameraController camera_{};
  StorageManager storage_{};
  ImageProcessor imageProcessor_{};
  DisplayUI display_{};
  ButtonInput button_{};
  NetworkManager network_{};
  AppState state_=AppState::Booting;
  NetworkMode networkMode_=NetworkMode::Off;
  MenuPage menuPage_=MenuPage::Root;
  uint8_t menuIndex_=0;
  bool menuOpen_=false,ledState_=false,timerActive_=false,orientationWizard_=false;
  bool clockValid_=false;
  bool timestampEnablePending_=false;
  int16_t utcOffsetMinutes_=0;
  uint32_t timerDeadline_=0,nextBlinkAt_=0,ledOffAt_=0,lastPreviewAt_=0,lastScreenAt_=0,previewResultUntil_=0;
  String message_="Booting";
  bool exposureSupported_=false;
  static DigiCamApp* instance_;
};
}
