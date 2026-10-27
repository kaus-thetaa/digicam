#include "digicam/app.h"
#include "digicam/config.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>

namespace dc {
DigiCamApp* DigiCamApp::instance_ = nullptr;
namespace {
static const char* kRootMenu[] = {"Capture", "Image", "Display", "Connectivity", "Settings", "Orientation test", "Close menu"};
static const char* kCaptureMenu[] = {"Capture mode", "Fill LED", "Timer LED blink", "Take photo now", "Back"};
static const char* kImageMenu[] = {"Resolution", "Rotation", "Exposure comp.", "Picture style", "Film grain", "Date stamp", "Back"};
static const char* kDisplayMenu[] = {"OLED live preview", "Back"};
static const char* kConnectivityMenu[] = {"Set date & time", "Transfer photos", "Update firmware", "Back"};
static const char* kSettingsMenu[] = {"Orientation test", "Reset settings", "Back"};
static const char* kModeNames[] = {"Auto", "Exp lock", "Timer 3s", "Timer 5s", "Timer 10s", "Low light", "Ghost"};
static const char* kResolutionNames[] = {"VGA 640x480", "QVGA 320x240"};
static const char* kStyleNames[] = {"Normal", "Vintage", "Warm", "Noir", "Cinema"};
static const char* kRotationNames[] = {"0 deg", "90 CW", "180 deg", "90 CCW"};
static constexpr uint8_t kTimerSeconds[] = {0, 0, 3, 5, 10, 0, 0};
}

void DigiCamApp::onOtaProgress(int p) { if (instance_) instance_->display_.otaProgress(p); }

void DigiCamApp::begin() {
  instance_ = this;
  pinMode(cfg::FLASH_GPIO, OUTPUT);
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);

  display_.begin();
  display_.splash("Starting modules");
  button_.begin(cfg::BUTTON_GPIO);

  // SD initializes before SettingsStore: all user preferences live on the removable card.
  const bool sdOk = storage_.begin();
  settingsStore_.begin(sdOk);
  const bool settingsLoaded = settingsStore_.load(settings_);
  if (!settingsLoaded && settingsStore_.available()) {
    settingsStore_.save(settings_);  // Seed the default SD settings file on first boot or after recovery.
  }

  message_ = "Camera init";
  display_.splash("Camera init / VGA");
  const bool camOk = camera_.begin();
  if (!camOk) message_ = "Camera unavailable";
  if (camOk) {
    applyStoredSettings();
    exposureSupported_ = camera_.exposureCompensationSupported();
    if (exposureSupported_ && !camera_.setExposureCompensation(settings_.ev)) exposureSupported_ = false;
    if (settings_.mode == static_cast<uint8_t>(CaptureMode::ExposureLock) && !camera_.exposureLockSupported()) {
      settings_.mode = static_cast<uint8_t>(CaptureMode::Auto);
      persistSettings();
    }
  }

  network_.setProgressCallback(&DigiCamApp::onOtaProgress);
  // Only confirm an OTA-pending image after the essential hardware and SD storage are available.
  // Full rollback still depends on bootloader rollback support; see docs/OTA_UPDATES.md.
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t otaState;
  if (running && esp_ota_get_state_partition(running, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
    if (camera_.ready() && display_.ready() && storage_.ready()) esp_ota_mark_app_valid_cancel_rollback();
  }

  state_ = camera_.ready() ? AppState::Ready : AppState::Error;
  if (!camera_.ready()) message_ = "Camera error";
  else if (!storage_.ready()) message_ = "Insert SD card";
  else message_ = "Ready";

  if (camera_.ready() && display_.ready() && !settings_.orientationConfirmed) enterOrientationWizard();
  else refreshScreen();
}

void DigiCamApp::applyStoredSettings() {
  if (settings_.resolution > 1) settings_.resolution = 0;
  if (settings_.resolution == 1) {
    if (!camera_.setResolution(1)) settings_.resolution = camera_.resolutionIndex();
  } else {
    settings_.resolution = camera_.resolutionIndex();
  }
  if (camera_.exposureCompensationSupported()) camera_.setExposureCompensation(settings_.ev);
}

void DigiCamApp::persistSettings() {
  if (!settingsStore_.available() || !settingsStore_.save(settings_)) {
    message_ = "Settings not saved / SD";
  }
}

const char* DigiCamApp::rotationLabel() const {
  return kRotationNames[settings_.rotation <= 3 ? settings_.rotation : 3];
}

bool DigiCamApp::makeTimestamp(char* out, size_t len) const {
  if (!out || len < 18 || !clockValid_) return false;
  const time_t now = time(nullptr);
  if (now < 1760000000) return false;
  const time_t local = now + static_cast<time_t>(utcOffsetMinutes_) * 60;
  struct tm tmLocal{};
  if (!gmtime_r(&local, &tmLocal)) return false;
  return strftime(out, len, "%Y-%m-%d %H:%M", &tmLocal) > 0;
}

void DigiCamApp::updateClockFromNetwork() {
  time_t epoch = 0;
  int16_t offset = 0;
  if (!network_.consumeTimeSync(epoch, offset)) return;
  struct timeval tv{};
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) == 0) {
    utcOffsetMinutes_ = offset;
    clockValid_ = true;  // Deliberately session-only; must be synced again after power-off.
    if (timestampEnablePending_) {
      settings_.timestampEnabled = true;
      timestampEnablePending_ = false;
      if (settingsStore_.available() && settingsStore_.save(settings_)) message_ = "Time set / stamp enabled";
      else message_ = "Time set / settings not saved";
    } else {
      message_ = "Time synchronized";
    }
    display_.network(NetworkMode::TimeSetup, "TIME SET / HOLD EXIT");
  } else {
    message_ = "Clock sync failed";
    display_.network(NetworkMode::TimeSetup, "SYNC FAILED / RETRY");
  }
}

void DigiCamApp::refreshScreen() {
  if (orientationWizard_) return;
  if (network_.active()) {
    const char* status = (networkMode_ == NetworkMode::TimeSetup && clockValid_) ? "TIME SET / HOLD EXIT" : "192.168.4.1";
    display_.network(networkMode_, status);
    return;
  }
  if (timerActive_) {
    const uint32_t remain = timerDeadline_ > millis() ? timerDeadline_ - millis() : 0;
    display_.timer(static_cast<uint8_t>((remain + 999) / 1000), ledState_);
    return;
  }
  if (menuOpen_) {
    const char* const* base = nullptr;
    uint8_t count = 0;
    const char* title = "MENU";
    switch (menuPage_) {
      case MenuPage::Root: base = kRootMenu; count = sizeof(kRootMenu) / sizeof(kRootMenu[0]); title = "MENU"; break;
      case MenuPage::Capture: base = kCaptureMenu; count = sizeof(kCaptureMenu) / sizeof(kCaptureMenu[0]); title = "CAPTURE"; break;
      case MenuPage::Image: base = kImageMenu; count = sizeof(kImageMenu) / sizeof(kImageMenu[0]); title = "IMAGE"; break;
      case MenuPage::Display: base = kDisplayMenu; count = sizeof(kDisplayMenu) / sizeof(kDisplayMenu[0]); title = "DISPLAY"; break;
      case MenuPage::Connectivity: base = kConnectivityMenu; count = sizeof(kConnectivityMenu) / sizeof(kConnectivityMenu[0]); title = "CONNECTIVITY"; break;
      case MenuPage::Settings: base = kSettingsMenu; count = sizeof(kSettingsMenu) / sizeof(kSettingsMenu[0]); title = "SETTINGS"; break;
    }
    String textRows[8];
    const char* rowPtrs[8] = {};
    for (uint8_t i = 0; i < count; ++i) {
      textRows[i] = base[i];
      if (menuPage_ == MenuPage::Root) {
        if (i == 5) textRows[i] += settings_.orientationConfirmed ? "  / saved" : "  / setup";
      } else if (menuPage_ == MenuPage::Capture) {
        switch (i) {
          case 0: textRows[i] += String("  / ") + kModeNames[settings_.mode <= 6 ? settings_.mode : 0]; break;
          case 1: textRows[i] += settings_.flashEnabled ? "  / ON" : "  / OFF"; break;
          case 2: textRows[i] += settings_.timerBlink ? "  / ON" : "  / OFF"; break;
          case 3: textRows[i] = "Capture now"; break;
        }
      } else if (menuPage_ == MenuPage::Image) {
        switch (i) {
          case 0: textRows[i] += String("  / ") + kResolutionNames[settings_.resolution <= 1 ? settings_.resolution : 0]; break;
          case 1: textRows[i] += String("  / ") + rotationLabel(); break;
          case 2: textRows[i] += exposureSupported_ ? String("  / EV ") + (settings_.ev > 0 ? "+" : "") + String(settings_.ev) : String("  / N/A"); break;
          case 3: textRows[i] += String("  / ") + kStyleNames[settings_.style <= 4 ? settings_.style : 0]; break;
          case 4: textRows[i] += settings_.grainEnabled ? "  / ON" : "  / OFF"; break;
          case 5:
            if (!clockValid_) textRows[i] += settings_.timestampEnabled ? "  / ON, SET TIME" : "  / SET TIME";
            else textRows[i] += settings_.timestampEnabled ? "  / ON" : "  / OFF";
            break;
        }
      } else if (menuPage_ == MenuPage::Display) {
        if (i == 0) textRows[i] += settings_.oledPreview ? "  / ON" : "  / OFF";
      } else if (menuPage_ == MenuPage::Connectivity) {
        if (i == 0) textRows[i] += clockValid_ ? "  / SET" : "  / REQUIRED";
      }
      rowPtrs[i] = textRows[i].c_str();
    }
    display_.menu(title, menuIndex_, rowPtrs, count);
    return;
  }
  if (settings_.oledPreview && camera_.ready()) return;
  display_.home(settings_, message_.c_str(), camera_.ready(), storage_.ready(), exposureSupported_, clockValid_, storage_.freeBytes());
}

void DigiCamApp::openMenu() {
  menuOpen_ = true;
  menuPage_ = MenuPage::Root;
  state_ = AppState::Menu;
  menuIndex_ = 0;
  refreshScreen();
}

void DigiCamApp::closeMenu() {
  menuOpen_ = false;
  menuPage_ = MenuPage::Root;
  state_ = camera_.ready() ? AppState::Ready : AppState::Error;
  message_ = camera_.ready() ? "Ready" : "Camera error";
  refreshScreen();
}

void DigiCamApp::backMenu() {
  if (menuPage_ != MenuPage::Root) {
    menuPage_ = MenuPage::Root;
    menuIndex_ = 0;
    refreshScreen();
  } else {
    closeMenu();
  }
}

void DigiCamApp::enterOrientationWizard() {
  orientationWizard_ = true;
  menuOpen_ = false;
  state_ = AppState::Preview;
  message_ = "Orientation test";
  if (display_.ready()) display_.message("ORIENTATION TEST", "Tap: next / Hold: keep");
}

void DigiCamApp::handleButton(ButtonEvent e) {
  if (e == ButtonEvent::None) return;
  if (orientationWizard_) {
    if (e == ButtonEvent::Single) settings_.rotation = static_cast<uint8_t>((settings_.rotation + 1U) % 4U);
    else if (e == ButtonEvent::Double) settings_.rotation = static_cast<uint8_t>((settings_.rotation + 3U) % 4U);
    else if (e == ButtonEvent::Long) {
      orientationWizard_ = false;
      settings_.orientationConfirmed = true;
      persistSettings();
      message_ = "Orientation saved";
      state_ = AppState::Ready;
      refreshScreen();
    }
    return;
  }
  if (network_.active()) {
    if (e == ButtonEvent::Double || e == ButtonEvent::Long) stopNetwork();
    return;
  }
  if (timerActive_) {
    if (e == ButtonEvent::Long || e == ButtonEvent::Double) cancelTimer();
    return;
  }
  if (menuOpen_) {
    if (e == ButtonEvent::Single) {
      menuIndex_ = static_cast<uint8_t>((menuIndex_ + 1U) %
        (menuPage_ == MenuPage::Root ? 7 : menuPage_ == MenuPage::Capture ? 5 :
         menuPage_ == MenuPage::Image ? 7 : menuPage_ == MenuPage::Display ? 2 :
         menuPage_ == MenuPage::Connectivity ? 4 : 3));
      refreshScreen();
    } else if (e == ButtonEvent::Long) selectMenu();
    else if (e == ButtonEvent::Double) backMenu();
    return;
  }
  if (e == ButtonEvent::Single) {
    const uint8_t mode = settings_.mode;
    if (mode >= 2 && mode <= 4) startTimer(kTimerSeconds[mode]);
    else capturePhoto();
  } else if (e == ButtonEvent::Double || e == ButtonEvent::Long) {
    openMenu();
  }
}

void DigiCamApp::selectMenu() {
  const bool onRoot = menuPage_ == MenuPage::Root;
  if (onRoot) {
    switch (menuIndex_) {
      case 0: menuPage_ = MenuPage::Capture; menuIndex_ = 0; refreshScreen(); return;
      case 1: menuPage_ = MenuPage::Image; menuIndex_ = 0; refreshScreen(); return;
      case 2: menuPage_ = MenuPage::Display; menuIndex_ = 0; refreshScreen(); return;
      case 3: menuPage_ = MenuPage::Connectivity; menuIndex_ = 0; refreshScreen(); return;
      case 4: menuPage_ = MenuPage::Settings; menuIndex_ = 0; refreshScreen(); return;
      case 5: enterOrientationWizard(); return;
      case 6: closeMenu(); return;
    }
  }

  bool shouldPersist = true;
  switch (menuPage_) {
    case MenuPage::Capture:
      switch (menuIndex_) {
        case 0: {
          uint8_t next = static_cast<uint8_t>((settings_.mode + 1U) % 7U);
          if (next == static_cast<uint8_t>(CaptureMode::ExposureLock) && !camera_.exposureLockSupported()) next = static_cast<uint8_t>(CaptureMode::Timer3);
          settings_.mode = next;
          message_ = String("Mode / ") + kModeNames[settings_.mode];
          break;
        }
        case 1: settings_.flashEnabled = !settings_.flashEnabled; message_ = settings_.flashEnabled ? "Fill LED on" : "Fill LED off"; break;
        case 2: settings_.timerBlink = !settings_.timerBlink; message_ = settings_.timerBlink ? "Timer signal on" : "Timer signal off"; break;
        case 3: menuOpen_ = false; menuPage_ = MenuPage::Root; capturePhoto(); return;
        case 4: backMenu(); return;
      }
      break;

    case MenuPage::Image:
      switch (menuIndex_) {
        case 0: {
          uint8_t target = settings_.resolution == 0 ? 1 : 0;
          if (target < camera_.maxResolutionIndex()) target = camera_.maxResolutionIndex();
          if (camera_.setResolution(target)) {
            settings_.resolution = target;
            message_ = target == 0 ? "Resolution VGA" : "Resolution QVGA";
          } else { message_ = "Resolution unavailable"; shouldPersist = false; }
          break;
        }
        case 1: settings_.rotation = static_cast<uint8_t>((settings_.rotation + 1U) % 4U); message_ = String("Rotation / ") + rotationLabel(); break;
        case 2:
          if (camera_.exposureCompensationSupported()) {
            int8_t nextEv = static_cast<int8_t>(settings_.ev + 1);
            if (nextEv > 2) nextEv = -2;
            if (camera_.setExposureCompensation(nextEv)) { settings_.ev = nextEv; message_ = String("Exposure EV ") + (nextEv > 0 ? "+" : "") + String(nextEv); }
            else { exposureSupported_ = false; message_ = "Exposure not supported"; shouldPersist = false; }
          } else { message_ = "Exposure not supported"; shouldPersist = false; }
          break;
        case 3: settings_.style = static_cast<uint8_t>((settings_.style + 1U) % 5U); message_ = String("Style / ") + kStyleNames[settings_.style]; break;
        case 4: settings_.grainEnabled = !settings_.grainEnabled; message_ = settings_.grainEnabled ? "Grain on" : "Grain off"; break;
        case 5:
          if (!clockValid_ && !settings_.timestampEnabled) {
            // Starting time setup here is the fast path. A successful sync enables
            // stamping; leaving the AP without syncing leaves the preference OFF.
            timestampEnablePending_ = true;
            startNetwork(NetworkMode::TimeSetup);
            return;
          }
          if (!clockValid_ && settings_.timestampEnabled) {
            // Preserve the saved preference across power cycles, but never emit a
            // fabricated timestamp. The UI shows SET TIME until sync succeeds.
            message_ = "Connect Wi-Fi / set time";
            shouldPersist = false;
            startNetwork(NetworkMode::TimeSetup);
            return;
          }
          settings_.timestampEnabled = !settings_.timestampEnabled;
          message_ = settings_.timestampEnabled ? "Date stamp on" : "Date stamp off";
          break;
        case 6: backMenu(); return;
      }
      break;

    case MenuPage::Display:
      if (menuIndex_ == 0) { settings_.oledPreview = !settings_.oledPreview; message_ = settings_.oledPreview ? "OLED preview on" : "OLED preview off"; }
      else { backMenu(); return; }
      break;

    case MenuPage::Connectivity:
      if (menuIndex_ == 0) { startNetwork(NetworkMode::TimeSetup); return; }
      if (menuIndex_ == 1) { startNetwork(NetworkMode::PhotoTransfer); return; }
      if (menuIndex_ == 2) { startNetwork(NetworkMode::FirmwareUpdate); return; }
      backMenu(); return;

    case MenuPage::Settings:
      if (menuIndex_ == 0) { enterOrientationWizard(); return; }
      if (menuIndex_ == 1) {
        settings_ = Settings{};
        // A reset deliberately returns to the orientation wizard because the mount may differ.
        persistSettings();
        applyStoredSettings();
        enterOrientationWizard();
        return;
      }
      backMenu(); return;

    case MenuPage::Root: return;
  }
  if (shouldPersist) persistSettings();
  refreshScreen();
}

void DigiCamApp::startNetwork(NetworkMode mode) {
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  camera_.end();  // Stop sensor clock, DMA/frame buffers and assert PWDN before Wi-Fi starts.
  if (network_.begin(mode, storage_.ready())) {
    networkMode_ = mode;
    state_ = AppState::Network;
    menuOpen_ = false;
    message_ = "Wi-Fi mode active";
    display_.network(mode, "192.168.4.1");
  } else {
    if (mode == NetworkMode::TimeSetup && !clockValid_) timestampEnablePending_ = false;
    networkMode_ = NetworkMode::Off;
    message_ = "Wi-Fi AP failed";
    const bool ok = camera_.begin();
    if (ok) applyStoredSettings();
    state_ = ok ? AppState::Ready : AppState::Error;
    refreshScreen();
  }
}

void DigiCamApp::stopNetwork() {
  network_.end();
  if (!clockValid_) timestampEnablePending_ = false;
  networkMode_ = NetworkMode::Off;
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  message_ = "Restarting camera";
  display_.splash("Restarting camera");
  const bool ok = camera_.begin();
  if (ok) {
    applyStoredSettings();
    exposureSupported_ = camera_.exposureCompensationSupported();
    if (exposureSupported_ && !camera_.setExposureCompensation(settings_.ev)) exposureSupported_ = false;
    message_ = "Camera ready";
    state_ = AppState::Ready;
  } else {
    message_ = "Camera restart failed";
    state_ = AppState::Error;
  }
  menuOpen_ = false;
  refreshScreen();
}

void DigiCamApp::startTimer(uint8_t seconds) {
  if (!camera_.ready() || !storage_.ready()) {
    message_ = !camera_.ready() ? "Camera unavailable" : "SD unavailable";
    refreshScreen(); return;
  }
  timerActive_ = true;
  timerDeadline_ = millis() + static_cast<uint32_t>(seconds) * 1000UL;
  nextBlinkAt_ = millis();
  ledOffAt_ = 0;
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  message_ = "Timer running";
  refreshScreen();
}

void DigiCamApp::cancelTimer() {
  timerActive_ = false;
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  message_ = "Timer cancelled";
  refreshScreen();
}

void DigiCamApp::capturePhoto() {
  if (network_.active()) { message_ = "Camera off in network mode"; refreshScreen(); return; }
  if (!camera_.ready() || !storage_.ready()) {
    message_ = !camera_.ready() ? "Camera unavailable" : "SD unavailable";
    refreshScreen(); return;
  }
  state_ = AppState::Capturing;
  menuOpen_ = false;
  timerActive_ = false;
  const bool fill = settings_.flashEnabled || settings_.mode == static_cast<uint8_t>(CaptureMode::LowLight);
  const bool wantExposureLock = settings_.mode == static_cast<uint8_t>(CaptureMode::ExposureLock);
  bool lockFallback = false;
  char stamp[24] = {};
  const bool stampRequested = settings_.timestampEnabled;
  const bool stampApplied = stampRequested && makeTimestamp(stamp, sizeof(stamp));
  if (stampRequested && !stampApplied) message_ = "Set time for stamp";

  camera_.enableAutoExposure();
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  const size_t expected = static_cast<size_t>(camera_.resolutionIndex() == 0 ? 640 : 320) *
                          static_cast<size_t>(camera_.resolutionIndex() == 0 ? 480 : 240) * 2U;
  uint8_t* previous = nullptr;
  size_t previousLen = 0;
  if (settings_.mode == static_cast<uint8_t>(CaptureMode::Ghost)) {
    camera_fb_t* first = camera_.acquire();
    if (first && first->format == PIXFORMAT_YUV422 && first->len >= expected) {
      previous = static_cast<uint8_t*>(heap_caps_malloc(expected, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (previous) { memcpy(previous, first->buf, expected); previousLen = expected; }
    }
    camera_.release(first);
    delay(120);
  }
  if (fill) {
    digitalWrite(cfg::FLASH_GPIO, HIGH); ledState_ = true;
    display_.message("FILL LIGHT", "Metering / settling");
    delay(450);
    for (uint8_t i = 0; i < 3; ++i) { camera_fb_t* d = camera_.acquire(); camera_.release(d); }
  }
  if (wantExposureLock) {
    for (uint8_t i = 0; i < 3; ++i) { camera_fb_t* d = camera_.acquire(); camera_.release(d); }
    if (!camera_.lockExposure()) { lockFallback = true; camera_.enableAutoExposure(); }
  }
  display_.message("CAPTURING", "Hold steady");
  camera_fb_t* fb = camera_.acquire();
  digitalWrite(cfg::FLASH_GPIO, LOW);
  ledState_ = false;
  if (!fb) {
    if (previous) free(previous);
    message_ = "Frame capture failed";
    state_ = AppState::Ready;
    refreshScreen(); return;
  }
  ImageDimensions dims{};
  size_t processedLen = 0;
  uint8_t* processed = ImageProcessor::transform(fb, previous, previousLen,
      static_cast<Rotation>(settings_.rotation), static_cast<Style>(settings_.style),
      settings_.grainEnabled, dims, processedLen, stampApplied ? stamp : nullptr);
  if (previous) free(previous);
  String saved;
  const bool ok = processed && storage_.saveJpegFromYuv(processed, processedLen, dims.width, dims.height, saved);
  if (processed) free(processed);
  camera_.release(fb);
  if (ok) {
    message_ = String(lockFallback ? "Saved / lock N/A" : "Photo saved");
    if (stampRequested && !stampApplied) message_ = "Saved / set time for stamp";
  } else {
    message_ = "Photo failed / retry";
  }
  state_ = AppState::Ready;
  if (settings_.oledPreview) {
    previewResultUntil_ = millis() + 1000;
    display_.message(ok ? "PHOTO SAVED" : "PHOTO FAILED",
                     ok ? saved.substring(saved.lastIndexOf('/') + 1).c_str() : "Check SD / retry");
  } else {
    refreshScreen();
  }
}

void DigiCamApp::loop() {
  const uint32_t now = millis();
  handleButton(button_.poll(now));
  if (network_.active()) {
    network_.loop();
    updateClockFromNetwork();
    delay(2);
    return;
  }
  if (orientationWizard_ && camera_.ready()) {
    if (now - lastPreviewAt_ >= 220) {
      lastPreviewAt_ = now;
      camera_fb_t* fb = camera_.acquire();
      if (fb) {
        static uint8_t bitmap[128 * 64 / 8];
        uint8_t x = 0, y = 0, w = 0, h = 0;
        if (ImageProcessor::makePreview(fb, static_cast<Rotation>(settings_.rotation), bitmap, sizeof(bitmap), x, y, w, h))
          display_.orientationPreview(bitmap, kRotationNames[settings_.rotation <= 3 ? settings_.rotation : 0]);
        camera_.release(fb);
      }
    }
    delay(2);
    return;
  }
  if (timerActive_) {
    if (settings_.timerBlink) {
      if (ledState_ && now >= ledOffAt_) { digitalWrite(cfg::FLASH_GPIO, LOW); ledState_ = false; }
      if (now >= nextBlinkAt_ && !ledState_) {
        digitalWrite(cfg::FLASH_GPIO, HIGH); ledState_ = true;
        ledOffAt_ = now + 65; nextBlinkAt_ = now + 450;
      }
    }
    if (now >= timerDeadline_) {
      timerActive_ = false;
      digitalWrite(cfg::FLASH_GPIO, LOW); ledState_ = false;
      capturePhoto();
    } else if (now - lastScreenAt_ > 100) {
      lastScreenAt_ = now; refreshScreen();
    }
    delay(2);
    return;
  }
  if (settings_.oledPreview && !menuOpen_ && now < previewResultUntil_) { delay(2); return; }
  if (settings_.oledPreview && !menuOpen_ && camera_.ready() && now - lastPreviewAt_ >= 400) {
    lastPreviewAt_ = now;
    camera_fb_t* fb = camera_.acquire();
    if (fb) {
      static uint8_t bitmap[128 * 64 / 8];
      uint8_t x = 0, y = 0, w = 0, h = 0;
      if (ImageProcessor::makePreview(fb, static_cast<Rotation>(settings_.rotation), bitmap, sizeof(bitmap), x, y, w, h))
        display_.preview(bitmap, x, y, w, h);
      camera_.release(fb);
    }
  } else if (!settings_.oledPreview && now - lastScreenAt_ > 500) {
    lastScreenAt_ = now;
    refreshScreen();
  }
  delay(2);
}
}  // namespace dc
