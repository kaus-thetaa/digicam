#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <SD_MMC.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include "digicam_config.h"

#if DIGICAM_I2C_ENABLED
#include <Wire.h>
#include <U8g2lib.h>
#endif
#if DIGICAM_I2C_ENABLED && DIGICAM_ENABLE_JOYSTICK
#include <Adafruit_ADS1X15.h>
#endif

static WebServer server(80);
static bool cameraReady = false;
static bool sdReady = false;
static bool flashEnabled = false;
static bool oledReady = false;
static uint8_t detectedOledAddress = 0;
static bool adsReady = false;
static bool pcfReady = false;
static bool i2cEnabled = false;
static bool otaReady = false;
static bool wifiEnabled = true;
static bool standaloneMenuActive = false;
static bool mpuReady = false;
static uint8_t activeMpuAddress = 0x68;
static float mpuRollDegrees = 0.0f;
static float mpuPitchDegrees = 0.0f;
static uint32_t lastMpuPollMs = 0;
static uint32_t lastLocalViewMs = 0;
static uint8_t localMenuIndex = 0;
static bool localMenuActionPending = false;
static bool localButtonWasLow = false;
static uint32_t localButtonDownMs = 0;
static bool localLongPressHandled = false;
static uint32_t localLastNavMs = 0;
static uint32_t photoCounter = 1;
static uint32_t lastOledUpdate = 0;
static float batteryVoltage = NAN; // Battery monitor intentionally omitted until its hardware is added.
static float lastFlashAverage = NAN;
static float lastFlashClippedPct = NAN;
static int8_t lastFlashAeLevel = 0;
static uint8_t jpegQuality = DIGICAM_JPEG_QUALITY;
static uint8_t digitalZoom = 1; // 1x preserves full lens field of view; 2x/3x are center crops.
static uint8_t grainAmount = 0; // Clean photo baseline; users can opt into grain.
static int8_t exposureCompensation = 0;
// The selected sensor frame size is shared by preview and still capture. The browser
// downsizes this same frame for the 160x120 viewfinder, preventing crop/zoom disagreement.
static framesize_t activeCameraFrameSize = DIGICAM_FRAME_SIZE;
static framesize_t selectedCameraFrameSize = DIGICAM_STILL_FRAME_SIZE; // Chosen resolution is shared by live view + stills.
// We reinitialize esp_camera when changing frame size; changing only sensor registers
// while the driver keeps VGA DMA buffers caused the GC2145 stream to stall on some builds.
static bool sensorFrameSizeSwitchSupported = false;
static bool nativePreviewFallback = false; // true means VGA camera + software downscale.
static bool cameraRecoveryAttempted = false;
static uint32_t lastCameraRecoveryMs = 0;
static uint32_t frameGetFailures = 0;
static uint32_t lastFrameAtMs = 0;
static uint32_t lastFrameBytes = 0;
static uint16_t lastFrameWidth = 0;
static uint16_t lastFrameHeight = 0;
static uint32_t resolutionChangeCount = 0;
static uint32_t lastResolutionChangeMs = 0;
static bool lastCaptureAtStillResolution = false;
static bool exposureHardwareApplied = false;
static int16_t activeGc2145AecTarget = 0x40;
static uint32_t lastJpegEncodeMs = 0;
static uint32_t lastCaptureDurationMs = 0;
static uint32_t captureCount = 0;
static uint32_t captureFailures = 0;
static uint32_t lastStillJpegBytes = 0;
static bool lastJpegValid = false; // validity of the most recent encoded output (preview or still)
static bool lastStillJpegValid = false; // validity of the last requested still photo
static bool lastJpegOutputLimitFailure = false;
static float lastStillAverageLuma = NAN;
static float lastStillClippedPercent = NAN;
static String lastAction = "Boot";

enum class PhotoFilter : uint8_t { Normal, Mono, Warm, Cool, Negative };
enum class PhotoMode : uint8_t { Auto, Portrait, Night, Action, Retro };
static PhotoMode currentMode = PhotoMode::Auto;
static const char *MODE_NAMES[] = {"auto", "portrait", "night", "action", "retro"};
static bool joystickReady = false;
#if DIGICAM_I2C_ENABLED && DIGICAM_ENABLE_JOYSTICK
static Adafruit_ADS1115 joystickAdc;
static bool physicalCapturePending = false;
static uint32_t lastJoystickPoll = 0;
static uint32_t lastJoystickAction = 0;
static uint32_t lastButtonPoll = 0;
static bool previousCaptureButtonHigh = true;
static uint32_t buttonDownAtMs = 0;
static bool buttonLongHandled = false;
#endif
static PhotoFilter currentFilter = PhotoFilter::Normal;
static const char *FILTER_NAMES[] = {"normal", "mono", "warm", "cool", "negative"};

#if DIGICAM_I2C_ENABLED
static U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif

static uint8_t clampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

static const char *filterName(PhotoFilter f) {
  return FILTER_NAMES[static_cast<uint8_t>(f)];
}

static bool parseFilter(const String &value, PhotoFilter &out) {
  for (uint8_t i = 0; i < 5; ++i) {
    if (value.equalsIgnoreCase(FILTER_NAMES[i])) {
      out = static_cast<PhotoFilter>(i);
      return true;
    }
  }
  return false;
}

static bool parsePhotoMode(const String &value, PhotoMode &out) {
  for (uint8_t i = 0; i < 5; ++i) {
    if (value.equalsIgnoreCase(MODE_NAMES[i])) { out = static_cast<PhotoMode>(i); return true; }
  }
  return false;
}

static const char *photoModeName(PhotoMode m) { return MODE_NAMES[static_cast<uint8_t>(m)]; }

// GC2145's stock sensor driver intentionally maps set_ae_level() to a no-op.
// Its own register table identifies page 1, register 0x13 as the AEC Y target.
// Adjust only that documented target while keeping the sensor's AEC enabled.
// EV steps are conservative target levels bounded by the driver's 0x20..0x50 limits.
static bool setGc2145AecTarget(sensor_t *sensor, int level) {
  if (!sensor || sensor->id.PID != 0x2145 || !sensor->get_reg || !sensor->set_reg || level < -2 || level > 2) return false;
  static const uint8_t targetForEv[5] = {0x28, 0x34, 0x40, 0x48, 0x50};
  int previousPage = sensor->get_reg(sensor, 0xFE, 0xFF);
  if (previousPage < 0 || previousPage > 3) previousPage = 0;
  if (sensor->set_reg(sensor, 0xFE, 0xFF, 0x01) != 0) {
    (void)sensor->set_reg(sensor, 0xFE, 0xFF, previousPage);
    return false;
  }
  const int writeResult = sensor->set_reg(sensor, 0x13, 0xFF, targetForEv[level + 2]);
  const int restoreResult = sensor->set_reg(sensor, 0xFE, 0xFF, previousPage);
  if (writeResult != 0 || restoreResult != 0) return false;
  activeGc2145AecTarget = targetForEv[level + 2];
  return true;
}

static bool applySensorExposureLevel(sensor_t *sensor, int level) {
  if (!sensor || level < -2 || level > 2) return false;
  if (sensor->id.PID == 0x2145) return setGc2145AecTarget(sensor, level);
  return sensor->set_ae_level && sensor->set_ae_level(sensor, level) == 0;
}

static bool setExposureCompensation(int level) {
  if (level < -2 || level > 2) return false;
  exposureCompensation = static_cast<int8_t>(level);
  sensor_t *sensor = esp_camera_sensor_get();
  exposureHardwareApplied = applySensorExposureLevel(sensor, level);
  return exposureHardwareApplied;
}

static bool setPhotoMode(PhotoMode m) {
  currentMode = m;
  int ev = 0;
  switch (m) {
    case PhotoMode::Auto:    currentFilter = PhotoFilter::Normal; grainAmount = 0; ev = 0; break;
    case PhotoMode::Portrait:currentFilter = PhotoFilter::Warm;   grainAmount = 1; ev = 1; break;
    case PhotoMode::Night:   currentFilter = PhotoFilter::Normal; grainAmount = 0; ev = 2; break;
    case PhotoMode::Action:  currentFilter = PhotoFilter::Normal; grainAmount = 0; ev = -1; break;
    case PhotoMode::Retro:   currentFilter = PhotoFilter::Warm;   grainAmount = 4; ev = 0; break;
  }
  const bool applied = setExposureCompensation(ev);
  lastAction = String("Mode: ") + photoModeName(m) + (applied ? "" : " (EV hardware control unavailable)");
  return applied;
}

struct ResolutionOption {
  const char *id;
  const char *label;
  framesize_t size;
  uint16_t width;
  uint16_t height;
};

// GC2145 advertises a maximum of UXGA (1600x1200). Larger frame-size enums
// are for other sensors and are intentionally not offered here.
static const ResolutionOption RESOLUTION_OPTIONS[] = {
  {"96x96",    "96 × 96",                  FRAMESIZE_96X96,    96,   96},
  {"128x128",  "128 × 128",                FRAMESIZE_128X128, 128,  128},
  {"qqvga",    "QQVGA · 160 × 120",        FRAMESIZE_QQVGA,   160,  120},
  {"qcif",     "QCIF · 176 × 144",         FRAMESIZE_QCIF,    176,  144},
  {"hqvga",    "HQVGA · 240 × 176",        FRAMESIZE_HQVGA,   240,  176},
  {"240x240",  "240 × 240",                FRAMESIZE_240X240, 240,  240},
  {"qvga",     "QVGA · 320 × 240 · Default", FRAMESIZE_QVGA,   320,  240},
  {"320x320",  "320 × 320",                FRAMESIZE_320X320, 320,  320},
  {"cif",      "CIF · 400 × 296",          FRAMESIZE_CIF,     400,  296},
  {"hvga",     "HVGA · 480 × 320",         FRAMESIZE_HVGA,    480,  320},
  {"vga",      "VGA · 640 × 480",          FRAMESIZE_VGA,     640,  480},
  {"svga",     "SVGA · 800 × 600",         FRAMESIZE_SVGA,    800,  600},
  {"xga",      "XGA · 1024 × 768",         FRAMESIZE_XGA,    1024,  768},
  {"hd",       "HD · 1280 × 720",          FRAMESIZE_HD,     1280,  720},
  {"sxga",     "SXGA · 1280 × 1024",       FRAMESIZE_SXGA,   1280, 1024},
  {"uxga",     "UXGA · 1600 × 1200 · Max", FRAMESIZE_UXGA,   1600, 1200}
};
static constexpr size_t RESOLUTION_OPTION_COUNT = sizeof(RESOLUTION_OPTIONS) / sizeof(RESOLUTION_OPTIONS[0]);

static const ResolutionOption *resolutionById(const String &id) {
  for (size_t i = 0; i < RESOLUTION_OPTION_COUNT; ++i) {
    if (id.equalsIgnoreCase(RESOLUTION_OPTIONS[i].id)) return &RESOLUTION_OPTIONS[i];
  }
  return nullptr;
}
static const ResolutionOption *resolutionBySize(framesize_t size) {
  for (size_t i = 0; i < RESOLUTION_OPTION_COUNT; ++i) {
    if (RESOLUTION_OPTIONS[i].size == size) return &RESOLUTION_OPTIONS[i];
  }
  return nullptr;
}
static uint16_t frameWidth(framesize_t size) {
  const ResolutionOption *option = resolutionBySize(size);
  return option ? option->width : 160;
}
static uint16_t frameHeight(framesize_t size) {
  const ResolutionOption *option = resolutionBySize(size);
  return option ? option->height : 120;
}
static const char *resolutionId(framesize_t size) {
  const ResolutionOption *option = resolutionBySize(size);
  return option ? option->id : "qqvga";
}
static const char *resolutionLabel(framesize_t size) {
  const ResolutionOption *option = resolutionBySize(size);
  return option ? option->label : "QQVGA · 160 × 120";
}

// initCamera takes the requested size so the driver DMA/framebuffer geometry always
// matches the mode. This forward declaration permits mode changes to restart the camera.
static bool initCamera(framesize_t frameSize);

// GC2145 set_framesize() can report success while subsequent fb_get() calls stall if the
// driver is switched in-place. Resolution changes therefore deinitialize and reinitialize
// the camera driver, then probe a real frame before the mode is accepted.
static bool restartCameraAt(framesize_t requested) {
  if (cameraReady) {
    const esp_err_t deinitResult = esp_camera_deinit();
    cameraReady = false;
    if (deinitResult != ESP_OK) {
      lastAction = String("Camera deinit failed: ") + String(static_cast<int>(deinitResult));
      return false;
    }
  }
  delay(80);
  if (!initCamera(requested)) {
    cameraReady = false;
    lastAction = String("Camera init failed at ") + String(frameWidth(requested)) + "x" + String(frameHeight(requested));
    return false;
  }
  activeCameraFrameSize = requested;
  delay(70); // let the first complete frame reach the freshly initialized queue
  (void)setExposureCompensation(exposureCompensation);
  lastAction = String("Camera restarted at ") + String(frameWidth(requested)) + "x" + String(frameHeight(requested));
  return true;
}

static bool setActiveCameraFrameSize(framesize_t requested) {
  if (cameraReady && activeCameraFrameSize == requested) return true;
  return restartCameraAt(requested);
}

static bool resolutionMemoryFits(const ResolutionOption &option, String &reason) {
  // YUV422 requires 2 bytes/pixel. Budget for the framebuffer, a conservative JPEG
  // output allowance, and Wi-Fi/HTTP/encoder working headroom. Very large enums are
  // present in the sensor driver but may not fit this original ESP32 + 4MB PSRAM setup.
  const uint64_t pixels = static_cast<uint64_t>(option.width) * option.height;
  const uint64_t rawBytes = pixels * 2ULL;
  // This sensor does not output hardware JPEG in the current driver. YUV422 with
  // multiple camera framebuffers is specifically discouraged by Espressif because
  // it increases PSRAM pressure and image loss when Wi-Fi is enabled. Budget one FB.
  // Reserve extra space for Wi-Fi/HTTP when connected; standalone mode can use more PSRAM,
  // but still leaves room for JPEG output, fragmentation, stack and SD buffers.
  const uint64_t fbCount = 1ULL;
  const uint64_t reserveBytes = wifiEnabled ? (1280ULL * 1024ULL) : (768ULL * 1024ULL);
  const uint64_t needBytes = rawBytes * fbCount + rawBytes / 2ULL + reserveBytes;
  const uint64_t freeBytes = ESP.getFreePsram();
  if (!psramFound() || needBytes > freeBytes) {
    reason = String(option.label) + " needs about " + String(static_cast<unsigned long>(needBytes / 1024ULL)) +
             " KB of safe working memory; currently free PSRAM is " +
             String(static_cast<unsigned long>(freeBytes / 1024ULL)) + " KB. Choose a smaller resolution.";
    return false;
  }
  return true;
}

static bool probeCameraFrame(uint16_t expectedWidth, uint16_t expectedHeight, String *reason = nullptr) {
  uint16_t observedWidth = 0, observedHeight = 0;
  size_t observedBytes = 0;
  pixformat_t observedFormat = PIXFORMAT_RGB565;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      observedWidth = fb->width;
      observedHeight = fb->height;
      observedBytes = fb->len;
      observedFormat = fb->format;
      const size_t expectedBytes = static_cast<size_t>(expectedWidth) * expectedHeight * 2U;
      const bool valid = fb->buf != nullptr && fb->width == expectedWidth &&
                         fb->height == expectedHeight && fb->format == PIXFORMAT_YUV422 &&
                         fb->len >= expectedBytes;
      esp_camera_fb_return(fb);
      if (valid) return true;
    }
    delay(120);
  }
  ++frameGetFailures;
  if (reason) {
    *reason = String("Frame probe failed for ") + String(expectedWidth) + "x" + String(expectedHeight) +
              "; last frame was " + String(observedWidth) + "x" + String(observedHeight) +
              ", " + String(static_cast<unsigned long>(observedBytes)) + " bytes, format=" +
              String(static_cast<int>(observedFormat)) + ". The driver may not support this mode reliably.";
  }
  return false;
}

static bool selectCameraResolution(const ResolutionOption &option, String &reason) {
  const framesize_t oldSelected = selectedCameraFrameSize;
  const framesize_t oldActive = activeCameraFrameSize;
  if (cameraReady && activeCameraFrameSize == option.size) {
    if (probeCameraFrame(option.width, option.height, &reason)) {
      selectedCameraFrameSize = option.size;
      nativePreviewFallback = false;
      sensorFrameSizeSwitchSupported = true;
      return true;
    }
    const String probeError = reason;
    if (restartCameraAt(oldSelected) &&
        probeCameraFrame(frameWidth(oldSelected), frameHeight(oldSelected), &reason)) {
      activeCameraFrameSize = oldSelected;
      selectedCameraFrameSize = oldSelected;
      reason = probeError + " Previous selected mode was restarted.";
    } else {
      cameraReady = false;
      reason = probeError + " Existing mode could not be restored; use Recover camera.";
    }
    lastAction = reason;
    return false;
  }
  if (!resolutionMemoryFits(option, reason)) return false;

  String requestedFailure;
  const bool requestedOk = restartCameraAt(option.size) &&
      probeCameraFrame(option.width, option.height, &requestedFailure);
  if (!requestedOk) {
    if (requestedFailure.length() == 0) requestedFailure = lastAction;
    if (cameraReady) { (void)esp_camera_deinit(); cameraReady = false; }

    // Prefer restoring the previous *selected* mode so the UI, preview and still stay in sync.
    String rollbackError;
    if (restartCameraAt(oldSelected) &&
        probeCameraFrame(frameWidth(oldSelected), frameHeight(oldSelected), &rollbackError)) {
      selectedCameraFrameSize = oldSelected;
      activeCameraFrameSize = oldSelected;
      reason = requestedFailure + String(" Restored previous resolution ") +
               String(frameWidth(oldSelected)) + "x" + String(frameHeight(oldSelected)) + ".";
    } else if (oldActive != oldSelected && restartCameraAt(oldActive) &&
               probeCameraFrame(frameWidth(oldActive), frameHeight(oldActive), &rollbackError)) {
      // Last resort if the old driver mode differed from the last selected mode.
      selectedCameraFrameSize = oldActive;
      activeCameraFrameSize = oldActive;
      reason = requestedFailure + String(" Previous selection failed; restored verified mode ") +
               String(frameWidth(oldActive)) + "x" + String(frameHeight(oldActive)) + ".";
    } else {
      cameraReady = false;
      reason = requestedFailure + " Camera could not restore a verified mode; use Recover camera.";
    }
    lastAction = reason;
    return false;
  }

  selectedCameraFrameSize = option.size;
  activeCameraFrameSize = option.size;
  sensorFrameSizeSwitchSupported = true;
  ++resolutionChangeCount;
  lastResolutionChangeMs = millis();
  nativePreviewFallback = false;
  cameraRecoveryAttempted = false;
  lastCaptureAtStillResolution = true;
  lastAction = String("Resolution set to ") + String(option.width) + "x" + String(option.height) + " (preview + still)" +
               ((option.width > 320 || option.height > 240) ? "; experimental YUV mode on original ESP32" : "");
  return true;
}


static void applyYuvFilter(uint8_t *buf, size_t len, PhotoFilter f) {
  // YUV422 is processed in Y0-U-Y1-V groups. A still may be processed directly in the
  // currently leased frame buffer; it is not returned to the driver until JPEG encoding ends.
  if (!buf || f == PhotoFilter::Normal) return;
  for (size_t i = 0; i + 3 < len; i += 4) {
    uint8_t &y0 = buf[i + 0];
    uint8_t &u  = buf[i + 1];
    uint8_t &y1 = buf[i + 2];
    uint8_t &v  = buf[i + 3];
    switch (f) {
      case PhotoFilter::Mono:
        u = 128;
        v = 128;
        break;
      case PhotoFilter::Warm:
        y0 = clampByte(static_cast<int>(y0) + 5);
        y1 = clampByte(static_cast<int>(y1) + 5);
        u = clampByte(static_cast<int>(u) - 10);
        v = clampByte(static_cast<int>(v) + 12);
        break;
      case PhotoFilter::Cool:
        u = clampByte(static_cast<int>(u) + 10);
        v = clampByte(static_cast<int>(v) - 10);
        break;
      case PhotoFilter::Negative:
        y0 = static_cast<uint8_t>(255 - y0);
        y1 = static_cast<uint8_t>(255 - y1);
        u  = static_cast<uint8_t>(255 - u);
        v  = static_cast<uint8_t>(255 - v);
        break;
      case PhotoFilter::Normal:
      default:
        break;
    }
  }
}

struct LumaStats {
  float average = 0.0f;
  float clippedPct = 0.0f;
  float darkPct = 0.0f;
  uint32_t samples = 0;
};

// GC2145 is running YUV422 in this build (Y0-U-Y1-V). Sampling only Y bytes
// keeps metering cheap and avoids modifying the driver's framebuffer.
static bool measureYuv422(const uint8_t *buf, size_t len, LumaStats &stats) {
  if (!buf || len < 4) return false;
  uint64_t sum = 0;
  uint32_t clipped = 0, dark = 0, count = 0;
  for (size_t i = 0; i + 3 < len; i += 4) {
    const uint8_t y0 = buf[i];
    const uint8_t y1 = buf[i + 2];
    sum += y0; sum += y1; count += 2;
    if (y0 >= 245) ++clipped;
    if (y1 >= 245) ++clipped;
    if (y0 <= 28) ++dark;
    if (y1 <= 28) ++dark;
  }
  if (!count) return false;
  stats.average = static_cast<float>(sum) / static_cast<float>(count);
  stats.clippedPct = 100.0f * static_cast<float>(clipped) / static_cast<float>(count);
  stats.darkPct = 100.0f * static_cast<float>(dark) / static_cast<float>(count);
  stats.samples = count;
  return true;
}

// Flash metering changes the AE target before the final frame. Sensor drivers
// may not implement set_ae_level; a failed/unsupported setter is safely ignored.
static bool prepareFlashExposure(int8_t originalAeLevel, int8_t &selectedAeLevel) {
  sensor_t *sensor = esp_camera_sensor_get();
  selectedAeLevel = originalAeLevel;
  if (!sensor) return false;

  bool changed = false;
  for (uint8_t pass = 0; pass < 2; ++pass) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;
    LumaStats stats;
    const bool measured = fb->format == PIXFORMAT_YUV422 && measureYuv422(fb->buf, fb->len, stats);
    esp_camera_fb_return(fb);
    if (!measured) break;

    lastFlashAverage = stats.average;
    lastFlashClippedPct = stats.clippedPct;
    int delta = 0;
    if (stats.clippedPct >= 8.0f || stats.average >= 170.0f) delta = -2;
    else if (stats.clippedPct >= 2.5f || stats.average >= 132.0f) delta = -1;
    else if (stats.average <= 48.0f && stats.clippedPct < 0.5f) delta = 1;
    else if (stats.average <= 70.0f && stats.clippedPct < 1.0f) delta = 1;

    const int requested = constrain(static_cast<int>(originalAeLevel) + delta, -2, 2);
    if (requested == selectedAeLevel) break;
    if (!applySensorExposureLevel(sensor, requested)) break;
    selectedAeLevel = static_cast<int8_t>(requested);
    changed = true;
    lastFlashAeLevel = selectedAeLevel;
    delay(55); // allow the camera's automatic exposure controller to react to the new AEC target
  }
  return changed;
}

// Compact flash-specific tone curve: restrained exposure compensation plus a
// soft highlight shoulder. It cannot recover clipped sensor detail, so the
// pre-flash AE adjustment above is the primary protection against blow-outs.
static void applyFlashToneMap(uint8_t *buf, size_t len, const LumaStats &stats) {
  if (!buf || len < 4) return;
  float gain = 1.0f;
  if (stats.clippedPct >= 2.5f || stats.average >= 155.0f) {
    gain = constrain(145.0f / fmaxf(stats.average, 1.0f), 0.70f, 0.94f);
  } else if (stats.average < 82.0f) {
    gain = constrain(88.0f / fmaxf(stats.average, 1.0f), 1.0f, 1.18f);
  }
  for (size_t i = 0; i + 3 < len; i += 4) {
    for (uint8_t sample = 0; sample < 2; ++sample) {
      const size_t offset = sample == 0 ? 0 : 2;
      float y = static_cast<float>(buf[i + offset]) * gain;
      if (y > 184.0f) y = 184.0f + (y - 184.0f) * 0.58f;
      if (y < 20.0f) y = 20.0f + (y - 20.0f) * 0.88f;
      buf[i + offset] = clampByte(static_cast<int>(y + 0.5f));
    }
  }
}

// GC2145 builds may not implement sensor-side AE compensation. In that case, apply
// a restrained software luma exposure curve so the UI slider still visibly affects
// preview/stills. This is post-processing, not a substitute for sensor exposure control.
static void applySoftwareExposure(uint8_t *buf, size_t len, int8_t ev) {
  if (!buf || len < 4 || ev < -2 || ev > 2 || ev == 0) return;
  static const float gainByEv[5] = {0.50f, 0.72f, 1.0f, 1.38f, 1.75f};
  const float gain = gainByEv[ev + 2];
  for (size_t i = 0; i + 3 < len; i += 4) {
    for (uint8_t offset = 0; offset <= 2; offset += 2) {
      float y = static_cast<float>(buf[i + offset]) * gain;
      if (y > 220.0f) y = 220.0f + (y - 220.0f) * 0.45f;
      buf[i + offset] = clampByte(static_cast<int>(y + 0.5f));
    }
  }
}

// Low-amplitude film grain on stills only. Modifies luma (Y0/Y1) without
// perturbing shared chroma, so color stays stable. A local LCG avoids per-pixel
// hardware RNG calls and keeps CPU cost predictable.
static void applyFilmGrain(uint8_t *buf, size_t len, uint8_t amount) {
  if (!buf || len < 4 || amount == 0) return;
  uint32_t state = esp_random() | 1U;
  for (size_t i = 0; i + 3 < len; i += 4) {
    state = state * 1664525U + 1013904223U;
    const int noise0 = static_cast<int>((state >> 28) % (2U * amount + 1U)) - amount;
    state = state * 1664525U + 1013904223U;
    const int noise1 = static_cast<int>((state >> 28) % (2U * amount + 1U)) - amount;
    buf[i] = clampByte(static_cast<int>(buf[i]) + noise0);
    buf[i + 2] = clampByte(static_cast<int>(buf[i + 2]) + noise1);
  }
}

// Sample a full-resolution YUV422 frame down to a small viewfinder frame without
// changing the sensor mode or camera driver's DMA buffer geometry. This avoids
// unsafe live sensor-size switching on camera drivers whose DMA size is fixed at init.
static uint8_t *downscaleYuv422(const uint8_t *src, size_t srcLen,
                                uint16_t srcW, uint16_t srcH,
                                uint16_t outW, uint16_t outH, size_t &outLen) {
  if (!src || srcW < outW || srcH < outH || (outW & 1U) || srcW < 2 ||
      srcLen < static_cast<size_t>(srcW) * srcH * 2U) return nullptr;
  outLen = static_cast<size_t>(outW) * outH * 2U;
  uint8_t *dst = static_cast<uint8_t *>(heap_caps_malloc(outLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!dst) dst = static_cast<uint8_t *>(malloc(outLen));
  if (!dst) return nullptr;
  for (uint16_t y = 0; y < outH; ++y) {
    const uint16_t sy = static_cast<uint16_t>((static_cast<uint32_t>(y) * srcH) / outH);
    const uint8_t *srcRow = src + static_cast<size_t>(sy) * srcW * 2U;
    uint8_t *dstRow = dst + static_cast<size_t>(y) * outW * 2U;
    for (uint16_t x = 0; x < outW; x += 2) {
      uint16_t sx0 = static_cast<uint16_t>(((static_cast<uint32_t>(x) * srcW) / outW) & ~1U);
      uint16_t sx1 = static_cast<uint16_t>(((static_cast<uint32_t>(x + 1) * srcW) / outW) & ~1U);
      if (sx0 >= srcW) sx0 = srcW - 2;
      if (sx1 >= srcW) sx1 = srcW - 2;
      const uint8_t *p0 = srcRow + static_cast<size_t>(sx0) * 2U;
      const uint8_t *p1 = srcRow + static_cast<size_t>(sx1) * 2U;
      const size_t o = static_cast<size_t>(x) * 2U;
      dstRow[o] = p0[0];
      dstRow[o + 1] = static_cast<uint8_t>((static_cast<unsigned>(p0[1]) + p1[1]) / 2U);
      dstRow[o + 2] = p1[0];
      dstRow[o + 3] = static_cast<uint8_t>((static_cast<unsigned>(p0[3]) + p1[3]) / 2U);
    }
  }
  return dst;
}

// Center crop packed YUV422 (Y0 U Y1 V); 1x preserves the full sensor frame.
static uint8_t *cropYuv422Center(const uint8_t *src, size_t srcLen, uint16_t srcW, uint16_t srcH, uint8_t zoom, uint16_t &outW, uint16_t &outH, size_t &outLen) {
  if (!src || srcW < 4 || srcH < 4 || zoom <= 1) { outW=srcW; outH=srcH; outLen=srcLen; return nullptr; }
  outW = static_cast<uint16_t>((srcW / zoom) & ~1U); outH = static_cast<uint16_t>(srcH / zoom);
  if (outW < 4 || outH < 4 || srcLen < static_cast<size_t>(srcW)*srcH*2U) { outW=srcW; outH=srcH; outLen=srcLen; return nullptr; }
  const uint16_t x0=static_cast<uint16_t>(((srcW-outW)/2U)&~1U), y0=static_cast<uint16_t>((srcH-outH)/2U);
  outLen=static_cast<size_t>(outW)*outH*2U;
  uint8_t *dst=static_cast<uint8_t *>(heap_caps_malloc(outLen,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if (!dst) dst=static_cast<uint8_t *>(malloc(outLen));
  if (!dst) { outW=srcW; outH=srcH; outLen=srcLen; return nullptr; }
  for (uint16_t y=0;y<outH;++y) memcpy(dst+static_cast<size_t>(y)*outW*2U,src+(static_cast<size_t>(y+y0)*srcW+x0)*2U,static_cast<size_t>(outW)*2U);
  return dst;
}

// Dynamic callback buffer. fmt2jpg() in esp32-camera uses a fixed 128 KiB buffer
// and may truncate larger output; fmt2jpg_cb lets us grow to a safe memory cap instead.
struct JpegAccumulator {
  uint8_t *data = nullptr;
  size_t length = 0;
  size_t capacity = 0;
  size_t maximum = 0;
  bool psram = true;
  bool failed = false;
};

static size_t appendJpegChunk(void *arg, size_t index, const void *data, size_t len) {
  JpegAccumulator *out = static_cast<JpegAccumulator *>(arg);
  if (!out || out->failed) return 0;
  if (!data || len == 0) return 0; // Encoder's end-of-image marker.
  if (index != out->length || len > out->maximum || index > out->maximum - len) {
    out->failed = true;
    return 0;
  }

  const size_t needed = index + len;
  if (needed > out->capacity) {
    size_t nextCapacity = out->capacity ? out->capacity : 16384U;
    while (nextCapacity < needed && nextCapacity < out->maximum) {
      const size_t doubled = nextCapacity > out->maximum / 2U ? out->maximum : nextCapacity * 2U;
      if (doubled <= nextCapacity) break;
      nextCapacity = doubled;
    }
    if (nextCapacity < needed) {
      out->failed = true;
      return 0;
    }

    void *grown = nullptr;
    if (out->data && out->psram) {
      grown = heap_caps_realloc(out->data, nextCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!grown) {
        // A failed realloc leaves the original allocation valid. Try a fresh PSRAM
        // block (to avoid fragmentation) then standard heap before giving up.
        grown = heap_caps_malloc(nextCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown) {
          memcpy(grown, out->data, out->length);
          free(out->data);
        } else {
          grown = malloc(nextCapacity);
          if (grown) {
            memcpy(grown, out->data, out->length);
            free(out->data);
            out->psram = false;
          }
        }
      }
    } else if (out->data) {
      grown = realloc(out->data, nextCapacity);
      if (!grown) {
        grown = heap_caps_malloc(nextCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown) {
          memcpy(grown, out->data, out->length);
          free(out->data);
          out->psram = true;
        }
      }
    } else {
      grown = heap_caps_malloc(nextCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!grown) {
        grown = malloc(nextCapacity);
        if (grown) out->psram = false;
      }
    }
    if (!grown) {
      out->failed = true;
      return 0;
    }
    out->data = static_cast<uint8_t *>(grown);
    out->capacity = nextCapacity;
  }

  memcpy(out->data + index, data, len);
  out->length = needed;
  return len;
}

static bool looksLikeCompleteJpeg(const uint8_t *data, size_t len) {
  return data && len >= 4 && data[0] == 0xFF && data[1] == 0xD8 &&
         data[len - 2] == 0xFF && data[len - 1] == 0xD9;
}

static bool makeJpeg(uint8_t **jpgOut, size_t *jpgLenOut, bool useCurrentFilter,
                     bool flashShot = false, bool previewFrame = false) {
  if (!jpgOut || !jpgLenOut || !cameraReady) return false;
  *jpgOut = nullptr;
  *jpgLenOut = 0;
  lastJpegValid = false;
  if (!previewFrame) {
    lastStillJpegValid = false;
    lastJpegOutputLimitFailure = false;
  }
  camera_fb_t *fb = nullptr;
  // A just-started/restarted sensor can miss the first VSYNC. Try one short retry before
  // treating it as a real camera failure; the HTTP handler has a slower recovery path too.
  const uint8_t maxFrameAttempts = previewFrame ? 1 : 2;
  for (uint8_t attempt = 0; attempt < maxFrameAttempts && !fb; ++attempt) {
    fb = esp_camera_fb_get();
    if (!fb && attempt + 1 < maxFrameAttempts) delay(35);
  }
  if (!fb) {
    ++frameGetFailures;
    lastAction = "Camera frame failed (fb_get timeout)";
    return false;
  }
  lastFrameAtMs = millis();
  lastFrameBytes = static_cast<uint32_t>(fb->len);
  lastFrameWidth = fb->width;
  lastFrameHeight = fb->height;
  if (fb->format == PIXFORMAT_YUV422) {
    const size_t expectedRawBytes = static_cast<size_t>(fb->width) * fb->height * 2U;
    if (!fb->buf || fb->width == 0 || fb->height == 0 || fb->len < expectedRawBytes) {
      ++frameGetFailures;
      lastAction = String("Short YUV422 frame: got ") + String(static_cast<unsigned long>(fb->len)) +
                   " bytes, expected at least " + String(static_cast<unsigned long>(expectedRawBytes));
      esp_camera_fb_return(fb);
      return false;
    }
  }

  bool ok = false;
  bool jpegOutputLimitFailure = false;
  if (fb->format == PIXFORMAT_JPEG) {
    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) copy = static_cast<uint8_t *>(malloc(fb->len));
    if (copy) { memcpy(copy, fb->buf, fb->len); *jpgOut = copy; *jpgLenOut = fb->len; ok = true; }
  } else if (fb->format == PIXFORMAT_YUV422) {
    uint8_t *raw = nullptr;
    size_t rawLen = 0;
    uint16_t rawW = fb->width, rawH = fb->height;
    bool rawOwned = true;

    if (previewFrame && fb->width <= 160 && fb->height <= 120 && digitalZoom == 1 &&
        currentFilter == PhotoFilter::Normal &&
        (exposureCompensation == 0 || exposureHardwareApplied)) {
      // Fast path: hardware is already outputting 160x120 YUV422. Encode directly from
      // the camera buffer; no full-frame copy or software downscale is needed.
      raw = fb->buf;
      rawLen = static_cast<size_t>(fb->width) * fb->height * 2U;
      rawOwned = false;
    } else if (previewFrame) {
      // Fit the complete selected frame inside 160x120 without cropping or stretching.
      // At resolutions below the viewport, do not upscale; this keeps the feed cheap.
      uint16_t previewW = fb->width;
      uint16_t previewH = fb->height;
      if (fb->width > 160 || fb->height > 120) {
        if (static_cast<uint32_t>(fb->width) * 120U >= static_cast<uint32_t>(fb->height) * 160U) {
          previewW = 160;
          previewH = static_cast<uint16_t>((static_cast<uint32_t>(fb->height) * 160U) / fb->width);
        } else {
          previewH = 120;
          previewW = static_cast<uint16_t>((static_cast<uint32_t>(fb->width) * 120U) / fb->height);
        }
      }
      previewW = static_cast<uint16_t>(previewW & ~1U);
      if (previewW < 2) previewW = 2;
      if (previewH < 1) previewH = 1;
      raw = downscaleYuv422(fb->buf, fb->len, fb->width, fb->height, previewW, previewH, rawLen);
      if (raw) { rawW = previewW; rawH = previewH; }
    } else {
      // The caller owns fb until this conversion returns. Processing the borrowed frame
      // in place avoids a second full-frame allocation at VGA/SVGA/XGA resolutions.
      // It is returned to the camera only after fmt2jpg() finishes below.
      rawLen = static_cast<size_t>(fb->width) * fb->height * 2U;
      raw = fb->buf;
      rawOwned = false;
    }

    if (raw) {
      if (!previewFrame) {
        LumaStats unprocessedStats;
        if (measureYuv422(raw, rawLen, unprocessedStats)) {
          lastStillAverageLuma = unprocessedStats.average;
          lastStillClippedPercent = unprocessedStats.clippedPct;
        }
      }
      if (!previewFrame && flashShot) {
        LumaStats stats;
        if (measureYuv422(raw, rawLen, stats)) {
          applyFlashToneMap(raw, rawLen, stats);
          lastFlashAverage = stats.average;
          lastFlashClippedPct = stats.clippedPct;
        }
      }
      // Only adjust pixels in owned memory. The camera driver's buffer must remain untouched.
      if ((!previewFrame || rawOwned) && !exposureHardwareApplied) applySoftwareExposure(raw, rawLen, exposureCompensation);
      if (useCurrentFilter && (!previewFrame || rawOwned)) applyYuvFilter(raw, rawLen, currentFilter);
      if (!previewFrame) applyFilmGrain(raw, rawLen, grainAmount);
      uint16_t outputW = rawW, outputH = rawH;
      size_t outputLen = rawLen;
      uint8_t *cropped = nullptr;
      if (digitalZoom > 1) cropped = cropYuv422Center(raw, rawLen, rawW, rawH, digitalZoom, outputW, outputH, outputLen);
      const uint8_t *encodeBuf = cropped ? cropped : raw;
      const uint8_t quality = previewFrame ? DIGICAM_VIEWFINDER_JPEG_QUALITY : jpegQuality;
      const uint32_t encodeStartedAt = millis();
      // Bound output by the raw payload, current free PSRAM (leave room for Wi-Fi/HTTP),
      // and 1.5 MiB. At high resolutions a failed allocation must fail the photo, not
      // silently truncate the JPEG and deliver a torn-looking file.
      const uint64_t freePsram = ESP.getFreePsram();
      const uint64_t rawBound = static_cast<uint64_t>(outputLen) * 2ULL;
      const uint64_t memoryBound = freePsram > (256ULL * 1024ULL) ? freePsram - (256ULL * 1024ULL) : 0ULL;
      const uint64_t maximum = rawBound < memoryBound ? rawBound : memoryBound;
      JpegAccumulator accumulator;
      accumulator.maximum = static_cast<size_t>(maximum < (1536ULL * 1024ULL) ? maximum : (1536ULL * 1024ULL));
      if (accumulator.maximum >= 16384U) {
        const bool encoderOk = fmt2jpg_cb(const_cast<uint8_t *>(encodeBuf), outputLen, outputW, outputH,
                                          PIXFORMAT_YUV422, quality, appendJpegChunk, &accumulator);
        ok = encoderOk && !accumulator.failed && accumulator.data && accumulator.length > 4;
      } else {
        ok = false;
      }
      lastJpegEncodeMs = millis() - encodeStartedAt;
      if (ok) {
        *jpgOut = accumulator.data;
        *jpgLenOut = accumulator.length;
      } else {
        if (accumulator.data) free(accumulator.data);
        jpegOutputLimitFailure = true;
        lastJpegOutputLimitFailure = true;
        lastAction = accumulator.failed
          ? "JPEG output limit/allocation failed; reduce resolution or quality"
          : "JPEG encoder failed; try QVGA with JPEG quality 85-92";
      }
      if (cropped) free(cropped);
      if (rawOwned) free(raw);
    }
  }
  esp_camera_fb_return(fb);
  if (ok && *jpgOut && *jpgLenOut > 0 && !looksLikeCompleteJpeg(*jpgOut, *jpgLenOut)) {
    ok = false;
    lastAction = "JPEG integrity check failed (SOI/EOI marker missing); image discarded";
  }
  if (!ok || !*jpgOut || *jpgLenOut == 0) {
    if (*jpgOut) free(*jpgOut);
    *jpgOut = nullptr; *jpgLenOut = 0;
    if (!jpegOutputLimitFailure && lastAction.indexOf("JPEG integrity check") < 0 &&
        lastAction.indexOf("JPEG encoder failed") < 0) lastAction = "JPEG conversion failed";
    lastJpegValid = false;
    if (!previewFrame) lastStillJpegValid = false;
    return false;
  }
  lastJpegValid = true;
  if (!previewFrame) lastStillJpegValid = true;
  return true;
}

static bool saveJpegToSd(const uint8_t *data, size_t len, String *savedPath = nullptr) {
  if (!sdReady || !data || len == 0) return false;
  if (!SD_MMC.exists("/DCIM")) {
    if (!SD_MMC.mkdir("/DCIM")) return false;
  }

  char path[32];
  bool found = false;
  for (uint32_t tries = 0; tries < 9999; ++tries) {
    snprintf(path, sizeof(path), "/DCIM/IMG_%04lu.JPG", static_cast<unsigned long>(photoCounter));
    photoCounter++;
    if (photoCounter > 9999) photoCounter = 1;
    if (!SD_MMC.exists(path)) {
      found = true;
      break;
    }
  }
  if (!found) return false;

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) return false;
  const size_t written = file.write(data, len);
  file.flush();
  file.close();
  if (written != len) return false;
  if (savedPath) *savedPath = String(path);
  return true;
}

static void sendJpegResponse(uint8_t *jpg, size_t len, bool attachment) {
  if (!jpg || len == 0) {
    server.send(500, "text/plain; charset=utf-8", "JPEG generation failed");
    return;
  }

  // WebServer owns the response socket. Use its length-aware binary send path;
  // do not keep a second client writer alive while the UI is polling status.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  if (attachment) {
    server.sendHeader("Content-Disposition", "attachment; filename=\"DigiCam.jpg\"");
  }
  server.setContentLength(len);
  server.send(200, "image/jpeg", "");
  server.sendContent(reinterpret_cast<const char *>(jpg), len);
  // Critical: restore WebServer state or the next JSON/HTML reply inherits the
  // JPEG Content-Length and browsers wait forever for bytes that will not come.
  server.setContentLength(CONTENT_LENGTH_NOT_SET);
}

static bool makeAndSendSnapshot() {
  uint8_t *jpg = nullptr;
  size_t len = 0;
  if (!makeJpeg(&jpg, &len, true, false, true)) return false;
  sendJpegResponse(jpg, len, false);
  free(jpg);
  return true;
}

static void handleSnapshot() {
  // Do not perform camera deinit/reinit inside the preview route. WebServer is synchronous,
  // so a recovery here blocks resolution changes, the shutter and every other HTTP command.
  // A single failed preview returns 503; the browser retries on the next cycle. Full recovery
  // remains available through a resolution change or device restart, both of which are explicit.
  if (!cameraReady) {
    server.sendHeader("Retry-After", "1");
    server.send(503, "text/plain; charset=utf-8", "Camera is not ready; check /status or restart DigiCam");
    return;
  }
  if (activeCameraFrameSize != selectedCameraFrameSize) {
    server.sendHeader("Retry-After", "1");
    server.send(503, "text/plain; charset=utf-8", "Camera mode transition in progress; retry shortly");
    return;
  }
  if (makeAndSendSnapshot()) return;
  server.sendHeader("Retry-After", "1");
  server.send(503, "text/plain; charset=utf-8", "Viewfinder frame unavailable; retrying without blocking recovery");
}

static bool captureStillToBuffer(uint8_t **jpg, size_t *len) {
  if (!jpg || !len) return false;
  *jpg = nullptr; *len = 0;
  if (activeCameraFrameSize != selectedCameraFrameSize &&
      !setActiveCameraFrameSize(selectedCameraFrameSize)) {
    lastCaptureAtStillResolution = false;
    lastAction = "Capture failed: selected resolution could not be activated";
    return false;
  }
  lastCaptureAtStillResolution = activeCameraFrameSize == selectedCameraFrameSize;

  const bool useFlash = flashEnabled;
  sensor_t *sensor = esp_camera_sensor_get();
  int8_t originalAeLevel = exposureCompensation, selectedAeLevel = exposureCompensation;
  bool aeChanged = false;
  if (useFlash) {
    digitalWrite(DIGICAM_FLASH_GPIO, HIGH);
    delay(45);
    aeChanged = prepareFlashExposure(originalAeLevel, selectedAeLevel);
    delay(30);
  }
  const bool captured = makeJpeg(jpg, len, true, useFlash, false);
  if (useFlash) digitalWrite(DIGICAM_FLASH_GPIO, LOW);
  if (aeChanged && sensor) {
    const bool restored = applySensorExposureLevel(sensor, originalAeLevel);
    if (!restored) lastAction = "Flash capture complete; warning: could not restore sensor AEC target";
  }
  lastFlashAeLevel = selectedAeLevel;
  return captured;
}

static void handleCapture() {
  const uint32_t captureStartedAt = millis();
  uint8_t *jpg = nullptr;
  size_t len = 0;
  lastStillJpegValid = false;
  lastStillJpegBytes = 0;
  if (!captureStillToBuffer(&jpg, &len)) {
    ++captureFailures;
    lastCaptureDurationMs = millis() - captureStartedAt;
    if (jpg) free(jpg);
    server.send(503, "text/plain; charset=utf-8", "Capture at selected resolution failed; see /status");
    return;
  }
  ++captureCount;
  lastCaptureDurationMs = millis() - captureStartedAt;
  lastStillJpegBytes = static_cast<uint32_t>(len);
  String saved;
  const bool savedOk = saveJpegToSd(jpg, len, &saved);
  lastAction = savedOk ? ("Saved " + saved) : (sdReady ? "Captured; SD write failed" : "Captured; no SD card");
  sendJpegResponse(jpg, len, true);
  free(jpg);
}

static String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    if (c == '\r') { out += "\\r"; continue; }
    out += c;
  }
  return out;
}

static void handleStatus() {
  String json;
  json.reserve(1536);
  json += "{\"device\":\"DigiCam\",\"firmwareBuild\":\"" DIGICAM_BUILD_ID "\",\"displayProfile\":";
  json += DIGICAM_I2C_ENABLED ? "true" : "false";
  json += ",\"i2cSda\":" + String(DIGICAM_I2C_SDA_GPIO) + ",\"i2cScl\":" + String(DIGICAM_I2C_SCL_GPIO) + ",\"cameraReady\":";
  json += cameraReady ? "true" : "false";
  json += ",\"sensorPid\":\"0x2145\",\"width\":" + String(frameWidth(selectedCameraFrameSize)) + ",\"height\":" + String(frameHeight(selectedCameraFrameSize)) + ",\"resolution\":\"" + String(resolutionId(selectedCameraFrameSize)) + "\",\"resolutionLabel\":\"" + String(frameWidth(selectedCameraFrameSize)) + "x" + String(frameHeight(selectedCameraFrameSize)) + "\",\"resolutionName\":\"" + String(resolutionLabel(selectedCameraFrameSize)) + "\",\"viewWidth\":160,\"viewHeight\":120,\"viewfinderActive\":true,\"framingConsistent\":" + String(activeCameraFrameSize == selectedCameraFrameSize ? "true" : "false") + ",\"pixelPipeline\":\"YUV422 selected shared resolution to software JPEG\",\"sensorViewfinderMode\":" + String(activeCameraFrameSize == selectedCameraFrameSize ? "true" : "false") + ",\"sensorModeSwitchSupported\":" + String(sensorFrameSizeSwitchSupported ? "true" : "false") + ",\"lastCaptureAtStillResolution\":" + String(lastCaptureAtStillResolution ? "true" : "false") + ",\"activeSensorWidth\":" + String(frameWidth(activeCameraFrameSize)) + ",\"activeSensorHeight\":" + String(frameHeight(activeCameraFrameSize)) + ",\"nativePreviewFallback\":" + String(nativePreviewFallback ? "true" : "false") + ",\"frameGetFailures\":" + String(frameGetFailures) + ",\"lastFrameAtMs\":" + String(lastFrameAtMs) + ",\"lastFrameBytes\":" + String(lastFrameBytes) + ",\"lastFrameWidth\":" + String(lastFrameWidth) + ",\"lastFrameHeight\":" + String(lastFrameHeight) + ",\"freePsram\":" + String(ESP.getFreePsram()) + ",\"frameBufferCount\":1,\"grabMode\":\"CAMERA_GRAB_WHEN_EMPTY\",\"lastJpegEncodeMs\":" + String(lastJpegEncodeMs) + ",\"lastCaptureDurationMs\":" + String(lastCaptureDurationMs) + ",\"captureCount\":" + String(captureCount) + ",\"captureFailures\":" + String(captureFailures) + ",\"lastStillJpegBytes\":" + String(lastStillJpegBytes) + ",\"lastJpegValid\":" + String(lastJpegValid ? "true" : "false") + ",\"lastStillJpegValid\":" + String(lastStillJpegValid ? "true" : "false") + ",\"jpegOutputLimitFailure\":" + String(lastJpegOutputLimitFailure ? "true" : "false") + ",\"lastStillAverageLuma\":";
  if (isnan(lastStillAverageLuma)) json += "null"; else json += String(lastStillAverageLuma, 1);
  json += ",\"lastStillClippedPercent\":";
  if (isnan(lastStillClippedPercent)) json += "null"; else json += String(lastStillClippedPercent, 2);
  json += ",\"activeGc2145AecTarget\":" + String(activeGc2145AecTarget) + ",\"highResolutionYuvRisk\":" + String((frameWidth(selectedCameraFrameSize) > 320 || frameHeight(selectedCameraFrameSize) > 240) ? "true" : "false") + ",\"format\":\"GC2145 YUV422 -> dynamic software JPEG\",\"sdReady\":";
  json += sdReady ? "true" : "false";
  json += ",\"wifiAp\":\"DigiCam-AP\",\"ip\":\"192.168.4.1\",\"otaReady\":";
  json += otaReady ? "true" : "false";
  json += ",\"filter\":\"" + String(filterName(currentFilter)) + "\",\"photoMode\":\"" + String(photoModeName(currentMode)) + "\",\"jpegQuality\":" + String(jpegQuality) + ",\"digitalZoom\":" + String(digitalZoom) + ",\"grainAmount\":" + String(grainAmount) + ",\"exposureCompensation\":" + String(exposureCompensation) + ",\"exposureSupported\":" + String(exposureHardwareApplied ? "true" : "false") + ",\"flashEnabled\":";
  json += flashEnabled ? "true" : "false";
  json += ",\"i2cEnabled\":";
  json += i2cEnabled ? "true" : "false";
  json += ",\"oledReady\":";
  json += oledReady ? "true" : "false";
  json += ",\"oledI2cAddress\":";
  if (detectedOledAddress) json += String(detectedOledAddress); else json += "null";
  json += ",\"adsReady\":";
  json += adsReady ? "true" : "false";
  json += ",\"pcfReady\":";
  json += pcfReady ? "true" : "false";
  json += ",\"joystickReady\":";
  json += joystickReady ? "true" : "false";
  json += ",\"wifiEnabled\":"; json += wifiEnabled ? "true" : "false";
  json += ",\"standaloneMode\":"; json += standaloneMenuActive ? "true" : "false";
  json += ",\"mpuReady\":"; json += mpuReady ? "true" : "false";
  json += ",\"mpuRollDegrees\":" + String(mpuRollDegrees, 1) + ",\"mpuPitchDegrees\":" + String(mpuPitchDegrees, 1);
  json += ",\"batteryVoltage\":";
  if (isnan(batteryVoltage)) json += "null";
  else json += String(batteryVoltage, 2);
  json += ",\"flashMeterAverage\":";
  if (isnan(lastFlashAverage)) json += "null"; else json += String(lastFlashAverage, 1);
  json += ",\"flashClippedPercent\":";
  if (isnan(lastFlashClippedPct)) json += "null"; else json += String(lastFlashClippedPct, 2);
  json += ",\"flashAeLevel\":" + String(lastFlashAeLevel);
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"freePsram\":" + String(ESP.getFreePsram());
  json += ",\"resolutionChangeCount\":" + String(resolutionChangeCount) + ",\"lastResolutionChangeMs\":" + String(lastResolutionChangeMs);
  json += ",\"lastAction\":\"" + jsonEscape(lastAction) + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

static const char INDEX_HTML[] PROGMEM = R"DGCAMHTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#30352a"><title>DigiCam D-2002</title>
<style>
:root{color-scheme:dark;--body:#ded3bc;--case:#30352a;--case2:#22271f;--caseEdge:#606047;--ink:#f2ead8;--muted:#c3b79d;--line:#555946;--brass:#c6a36a;--brass2:#e1c58f;--red:#d76b50;--redDark:#9f4636;--green:#bbc58b;--screen:#0b110d;--screenInk:#e8efc9}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 50% 0,#f0e7d4 0,#ded3bc 55%,#c9bda5 100%);color:var(--ink);font:14px/1.45 ui-sans-serif,system-ui,-apple-system,sans-serif}
.camera{max-width:570px;margin:0 auto;padding:24px 14px 32px}.shell{position:relative;background:linear-gradient(150deg,#414536 0,#303529 38%,#252a22 100%);border:1px solid #777459;border-radius:22px;padding:17px;box-shadow:0 20px 45px #392f2040,inset 0 1px #ffffff18,inset 0 -2px #0005}.shell:before{content:"";position:absolute;inset:5px;border:1px solid #ffffff09;border-radius:17px;pointer-events:none}
.top{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:4px 4px 15px;border-bottom:1px solid var(--line);margin-bottom:15px}.brand{font:700 21px/1 Georgia,'Times New Roman',serif;letter-spacing:.035em;color:#f7edda}.brand .model{color:var(--brass2);font-size:17px}.brand small{display:block;margin-top:6px;font:9px/1.2 ui-monospace,monospace;letter-spacing:.16em;text-transform:uppercase;color:var(--muted)}.badge{font:10px/1 ui-monospace,monospace;text-transform:uppercase;letter-spacing:.09em;padding:8px 10px;border:1px solid #79775e;border-radius:99px;color:#e8d8af;background:#252a22;white-space:nowrap}.badge.ok{color:#d8e3a4;border-color:#79875e}.badge.bad{color:#ffb6a2;border-color:#9b5144}
.lcdframe{position:relative;background:linear-gradient(140deg,#151911,#080b08);border:2px solid #a48a5a;border-radius:12px;padding:8px;box-shadow:inset 0 0 0 2px #000,inset 0 0 0 3px #66553b,0 2px #0008}.lcd{position:relative;aspect-ratio:4/3;min-height:180px;max-height:52vh;background:var(--screen);overflow:hidden;display:grid;place-items:center;color:var(--screenInk);border-radius:5px}.lcd img{position:relative;z-index:0;display:block;width:100%;height:100%;object-fit:contain}.lcd:after{content:"";position:absolute;inset:0;z-index:1;pointer-events:none;opacity:.13;background:repeating-linear-gradient(to bottom,transparent 0,transparent 3px,#d9e5ba 4px),radial-gradient(ellipse at center,transparent 52%,#000b 100%)}.osd{z-index:2;position:absolute;inset:8px;display:flex;justify-content:space-between;align-items:flex-start;pointer-events:none;text-shadow:1px 1px #000;background:linear-gradient(#0008,transparent 22%,transparent 75%,#0008);font:9px/1.2 ui-monospace,monospace;letter-spacing:.06em;color:#f0f5d7}.osd span{padding:4px 6px;background:#10170fc9;border:1px solid #ffffff22;border-radius:2px}.nosignal{padding:12px;text-align:center;color:#c9d4aa;font:11px/1.6 ui-monospace,monospace;letter-spacing:.08em}
.quickbar{display:grid;grid-template-columns:1fr 1fr auto;gap:8px;margin-top:12px}.mini-control{display:block;min-width:0;color:var(--muted);font:9px/1.2 ui-monospace,monospace;letter-spacing:.13em;text-transform:uppercase}.mini-control select{display:block;width:100%;margin-top:5px;padding:9px 27px 9px 9px;border:1px solid #62644d;border-radius:6px;background:#20251e;color:var(--ink);font:12px ui-sans-serif,system-ui}
.exposure-control{margin:12px 2px 2px;padding:10px 11px 8px;border:1px solid #555946;border-radius:8px;background:#252a22}.exposure-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:7px;font:10px ui-monospace,monospace;letter-spacing:.11em;text-transform:uppercase;color:#d2c5a7}.exposure-head output{color:var(--brass2);font-weight:700}.range{display:flex;align-items:center;gap:8px}.range input{min-width:0;flex:1;accent-color:var(--brass)}.range .small{font:9px ui-monospace,monospace;color:#b4aa91}.quick-actions{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px;margin-top:11px}
button,select,input{font:inherit}button{min-height:39px;padding:9px 11px;border:1px solid #65664f;border-radius:7px;background:linear-gradient(#424736,#303529);color:var(--ink);font-size:11px;font-weight:650;letter-spacing:.035em;cursor:pointer;transition:filter .12s,transform .12s,border-color .12s}button:hover{filter:brightness(1.1);border-color:#aaa17d}button:active{transform:translateY(1px)}button:focus-visible,select:focus-visible,input:focus-visible{outline:2px solid var(--brass2);outline-offset:2px}button.primary{border:1px solid #f1ad87;background:linear-gradient(#e27a5a,#be5540);color:#fff5e3;box-shadow:0 3px 0 #783c30,0 5px 10px #0003}button.active{border-color:var(--brass2);color:var(--brass2);background:#393d2d}button.danger{border-color:#9e5949;color:#ffb7a4}.flash-btn{white-space:nowrap}.shutter{display:flex;align-items:center;justify-content:center;gap:13px;width:100%;min-height:68px;margin-top:13px;border-radius:11px;padding:10px 15px}.shutter .lens-icon{display:grid;place-items:center;width:39px;height:39px;border-radius:50%;border:3px solid #f7c7a9;box-shadow:inset 0 0 0 3px #b44a38,0 0 0 1px #77372b;background:radial-gradient(circle,#e9a17b 0 16%,#a84332 18% 100%);font-size:0}.shutter-copy{text-align:left;display:block}.shutter-copy strong{display:block;font:700 16px/1.1 Georgia,'Times New Roman',serif;letter-spacing:.03em}.shutter-copy small{display:block;margin-top:4px;font:9px ui-monospace,monospace;letter-spacing:.13em;color:#ffdec9}.hint{font-size:11px;color:var(--muted);margin:10px 2px 0;min-height:1.2em}.countdown{color:var(--brass2)!important;text-align:center;font:12px ui-monospace,monospace;letter-spacing:.12em}.tabs{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin-top:12px}.tab{min-height:35px;background:transparent;color:#c8bea4}.tab.selected{color:var(--brass2);border-color:#a48e61;background:#303529}
.settings-details{margin-top:12px;border-top:1px solid #555946;padding-top:10px}.settings-details>summary,.diagnostics>summary{cursor:pointer;list-style:none;display:flex;align-items:center;justify-content:space-between;gap:10px;color:#e4d5b5;font-size:11px;letter-spacing:.06em}.settings-details>summary::-webkit-details-marker,.diagnostics>summary::-webkit-details-marker{display:none}.settings-details>summary:after,.diagnostics>summary:after{content:"＋";color:var(--brass2);font-size:15px}.settings-details[open]>summary:after,.diagnostics[open]>summary:after{content:"−"}.panel{border:1px solid #555946;border-radius:9px;padding:12px;margin-top:11px;background:#292e25}.panelhead{display:flex;justify-content:space-between;gap:8px;align-items:center;margin:0 0 10px;font-weight:700;text-transform:uppercase;letter-spacing:.12em;font-size:10px;color:#e5d8bc}.controls{display:grid;grid-template-columns:1fr 1fr;gap:8px}.control{min-width:0;border:1px solid #4a503e;background:#22271f;border-radius:7px;padding:10px}.control label{display:flex;justify-content:space-between;gap:8px;font:10px ui-monospace,monospace;letter-spacing:.025em;color:#ded2b8;margin-bottom:7px}.control select,.control input[type=range]{width:100%}.control select{padding:8px;background:#181d17;border:1px solid #555946;border-radius:5px;color:var(--ink);font-size:12px}.control input[type=range]{accent-color:var(--brass)}.small{font-size:10px;color:#aaa58f}.control .small{margin:7px 0 0;line-height:1.4}.control .range{margin-top:6px}.menu-note{color:var(--muted);font-size:12px;margin:0 0 10px}.filelist{display:grid;gap:7px}.file{display:grid;grid-template-columns:minmax(0,1fr) auto;align-items:center;gap:8px;padding:10px;border:1px solid #4a503e;background:#22271f;border-radius:6px}.filename{min-width:0;overflow-wrap:anywhere;font-size:12px}.filesize{display:block;color:var(--muted);font-size:10px;margin-top:3px}.fileactions{display:flex;gap:5px;flex-wrap:wrap}.fileactions button{min-height:31px;padding:5px 8px;font-size:10px}.empty{padding:18px 10px;border:1px dashed #555946;border-radius:6px;text-align:center;color:var(--muted)}.diagnostics{margin-top:13px;padding-top:10px;border-top:1px solid #555946}.diagnostics .stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:7px;margin-top:11px}.stat{padding:8px;background:#22271f;border:1px solid #4a503e;border-radius:6px;min-width:0}.stat b{display:block;font:9px ui-monospace,monospace;letter-spacing:.1em;color:#b7ab90;text-transform:uppercase}.stat span{display:block;overflow-wrap:anywhere;margin-top:4px;font-size:11px;color:#f0e7d2}.diagnostics pre{font-size:10px;white-space:pre-wrap;overflow-wrap:anywhere;margin-bottom:0;color:#d9d0b8}.footer{text-align:center;color:#b8ae96;font:9px ui-monospace,monospace;padding:13px 5px 0;letter-spacing:.1em}.status-button{font-size:10px;padding:5px 8px;min-height:28px}[hidden]{display:none!important}@media(max-width:480px){.camera{padding:12px 8px 24px}.shell{padding:12px;border-radius:17px}.top{padding-bottom:12px;margin-bottom:12px}.brand{font-size:19px}.lcd{min-height:150px}.quickbar{grid-template-columns:1fr 1fr auto;gap:6px}.mini-control select{padding:8px 18px 8px 7px;font-size:11px}.quick-actions{gap:5px}.quick-actions button{padding:8px 5px;font-size:10px}.controls{grid-template-columns:1fr 1fr}.control{padding:8px}.diagnostics .stats{grid-template-columns:1fr 1fr}.file{grid-template-columns:1fr}.fileactions button{flex:1}}
@media(prefers-reduced-motion:reduce){*{transition:none!important;scroll-behavior:auto!important}}
</style></head><body><main class="camera"><section class="shell"><header class="top"><div class="brand">DIGICAM <span class="model">D-2002</span><small>GC2145 · POCKET DIGITAL / 02</small></div><span class="badge" id="badge">BOOTING</span></header>
<div class="lcdframe"><div class="lcd"><img id="preview" alt="Camera preview" hidden><div class="nosignal" id="nosignal">READY WHEN YOU ARE<br><small>Connecting to DigiCam…</small></div><div class="osd"><span id="osdMode">LIVE VIEW · 160×120</span><span id="osdFlash">FLASH OFF</span></div><div class="osd" style="align-items:flex-end"><span id="osdFilter">NATURAL</span><span id="osdSd">SD —</span></div></div></div>
<div class="quickbar"><label class="mini-control" for="zoom">Zoom <output id="zoomOut">1×</output><select id="zoom"><option value="1">1× · Wide</option><option value="2">2×</option><option value="3">3×</option></select></label><label class="mini-control" for="timer">Self-timer<select id="timer"><option value="0">Off</option><option value="3">3 sec</option><option value="5">5 sec</option><option value="10">10 sec</option></select></label><button class="flash-btn" id="flash">Flash off</button></div>
<div class="exposure-control"><div class="exposure-head"><label for="exposure">Exposure</label><output id="exposureOut">EV 0</output></div><div class="range"><span class="small">−2</span><input id="exposure" type="range" min="-2" max="2" step="1" value="0"><span class="small">+2</span></div><p class="small" id="exposureNote" style="margin:6px 0 0">Adjusts exposure when supported by the sensor driver.</p></div>
<button class="primary shutter" id="capture"><span class="lens-icon" aria-hidden="true"></span><span class="shutter-copy"><strong>Take photo</strong><small>FULL-QUALITY STILL</small></span></button>
<div class="quick-actions"><button id="auto" class="active">Live view · On</button><button class="tab" id="tabCamera">Adjust</button><button class="tab" id="tabLibrary">Library <span id="fileCount"></span></button></div>
<p class="hint" id="msg" role="status">Connecting to DigiCam…</p><p class="hint countdown" id="countdown" hidden aria-live="polite"></p></section>
<details class="settings-details" id="settingsDetails"><summary>More camera settings</summary><section class="panel" id="cameraMenu"><div class="panelhead"><span>Picture settings</span><span class="small">D-2002</span></div><div class="controls"><div class="control"><label for="filter">Picture style</label><select id="filter"><option value="normal">Natural</option><option value="mono">Monochrome</option><option value="warm">Warm vintage</option><option value="cool">Cool</option><option value="negative">Negative</option></select></div><div class="control"><label for="mode">Shooting mode</label><select id="mode"><option value="auto">Auto</option><option value="portrait">Portrait</option><option value="night">Night</option><option value="action">Action</option><option value="retro">Retro film</option></select><p class="small">Presets set the starting look and exposure compensation where supported.</p></div><div class="control"><label for="grain">Film grain <output id="grainOut">0</output></label><div class="range"><span class="small">OFF</span><input id="grain" type="range" min="0" max="6" step="1" value="0"><span class="small">MORE</span></div></div><div class="control"><label for="quality">JPEG quality <output id="qualityOut">92</output></label><div class="range"><span class="small">SMALL</span><input id="quality" type="range" min="60" max="95" value="92"><span class="small">FINE</span></div></div><div class="control"><label for="resolution">Resolution · view + photo</label><select id="resolution"><option value="96x96">96 × 96</option><option value="128x128">128 × 128</option><option value="qqvga">QQVGA · 160 × 120</option><option value="qcif">QCIF · 176 × 144</option><option value="hqvga">HQVGA · 240 × 176</option><option value="240x240">240 × 240</option><option value="qvga" selected>QVGA · 320 × 240 · Default</option><option value="320x320">320 × 320</option><option value="cif">CIF · 400 × 296</option><option value="hvga">HVGA · 480 × 320</option><option value="vga">VGA · 640 × 480</option><option value="svga">SVGA · 800 × 600</option><option value="xga">XGA · 1024 × 768</option><option value="hd">HD · 1280 × 720</option><option value="sxga">SXGA · 1280 × 1024</option><option value="uxga">UXGA · 1600 × 1200 · Max</option></select><p class="small" id="resolutionNote">QVGA is the recommended clean baseline. Larger YUV422 modes are experimental on original ESP32 and may lose image data with Wi-Fi enabled.</p></div><div class="control"><label for="interval">Live-view speed</label><select id="interval"><option value="50" selected>Fast</option><option value="200">Balanced</option><option value="450">Battery saver</option><option value="0">Paused</option></select></div><div class="control"><label>Camera</label><p class="small">Use refresh if the preview stalls. Restart only if the camera stops responding.</p><div class="row"><button id="refresh">Refresh view</button><button id="reboot">Recover camera</button></div></div></div></section></details>
<section class="panel" id="libraryMenu" hidden><div class="panelhead"><span>Photo library</span><button id="libraryRefresh" style="min-height:30px;padding:5px 9px;font-size:10px">Scan card</button></div><p class="menu-note" id="libraryInfo">Looking for photos on the microSD card…</p><div class="filelist" id="fileList"><div class="empty">Your saved photos will appear here.</div></div><p class="hint" id="libraryMsg"></p></section>
<details class="diagnostics"><summary>Device status & diagnostics</summary><div style="margin-top:10px"><button id="status" class="status-button">Refresh status</button></div><div class="stats"><div class="stat"><b>Camera</b><span id="camera">checking</span></div><div class="stat"><b>MicroSD</b><span id="sd">checking</span></div><div class="stat"><b>OTA</b><span id="ota">checking</span></div><div class="stat"><b>Memory</b><span id="memory">checking</span></div><div class="stat"><b>Display</b><span id="display">optional</span></div><div class="stat"><b>Joystick</b><span id="joystick">optional</span></div><div class="stat"><b>Last action</b><span id="last">—</span></div><div class="stat"><b>JPEG encode</b><span id="encodeTime">—</span></div><div class="stat"><b>Last still</b><span id="stillInfo">—</span></div><div class="stat"><b>Capture errors</b><span id="captureErrors">—</span></div></div><details style="margin-top:10px"><summary>Raw diagnostics</summary><pre id="json">Hidden</pre></details></details><div class="footer">DIGICAM D-2002 · MADE FOR THE SIDE QUESTS</div></main>
<script>
(()=>{'use strict';const $=id=>document.getElementById(id);let flash=false,previewBusy=false,actionBusy=false,viewfinderOn=true,autoMs=50,tickTimer=0,countdownTimer=0,countdownAbort=false,lastStatusAt=0,cameraReady=false,pendingCommands=new Map(),viewObjectUrl=null,libraryLoaded=false,capturePending=false,previewController=null,lastFrameAt=0,previewFps=0,frameMisses=0;const setMsg=t=>$('msg').textContent=t;window.onerror=()=>{ $('badge').textContent='UI ERROR';$('badge').className='badge bad';setMsg('Interface error. Reload the page.'); };
async function fetchJson(path,timeout=5000,options={}){const c=new AbortController(),t=setTimeout(()=>c.abort(),timeout);try{const r=await fetch(path,{cache:'no-store',signal:c.signal,...options});if(!r.ok)throw new Error('HTTP '+r.status);return await r.json()}finally{clearTimeout(t)}}
function setBadge(text,ok){$('badge').textContent=text;$('badge').className='badge '+(ok?'ok':'bad')}
async function status(){const j=await fetchJson('/status');$('camera').textContent=j.cameraReady?`${j.width}×${j.height} · GC2145`:'not ready';if(j.resolution)$('resolution').value=j.resolution;$('resolutionNote').textContent=`Selected for viewfinder and still: ${j.width}×${j.height}. QVGA is the photo-quality baseline; larger YUV modes remain experimental on original ESP32.`;$('sd').textContent=j.sdReady?'READY · /DCIM':'NO CARD';$('ota').textContent=j.otaReady?'READY · 3232':'OFF';$('memory').textContent=`${Math.round(j.freeHeap/1024)} KB heap`; $('display').textContent=j.oledReady?'SH1106 READY':'not detected';$('joystick').textContent=j.joystickReady?'READY · ADS/PCF':'not detected';$('last').textContent=j.lastAction||'—';$('encodeTime').textContent=(j.lastJpegEncodeMs??'—')+' ms';$('stillInfo').textContent=j.lastStillJpegBytes?`${(j.lastStillJpegBytes/1024).toFixed(1)} KB · ${j.lastStillJpegValid?'JPEG OK':'NOT VALIDATED'} · Y ${j.lastStillAverageLuma??'—'} · clip ${j.lastStillClippedPercent??'—'}%`:'—';$('captureErrors').textContent=`${j.captureFailures??0} / ${j.captureCount??0}`;$('filter').value=j.filter;$('mode').value=j.photoMode||'auto';$('quality').value=j.jpegQuality||92;$('qualityOut').value=$('quality').value;$('zoom').value=String(j.digitalZoom||1);$('zoomOut').value=$('zoom').value+'×';$('exposure').value=j.exposureCompensation??0;$('exposureOut').value='EV '+$('exposure').value;$('grain').value=j.grainAmount??0;$('grainOut').value=$('grain').value;$('exposureNote').textContent=j.exposureSupported?'GC2145 auto-exposure target active.':'Sensor control unavailable; EV uses software luma correction.';cameraReady=!!j.cameraReady;flash=!!j.flashEnabled;$('flash').textContent=flash?'Flash armed':'Flash off';$('osdFlash').textContent='FLASH '+(flash?'ARMED':'OFF');$('osdSd').textContent=j.sdReady?'SD OK':'SD --';$('osdMode').textContent=`SENSOR ${j.activeSensorWidth||j.width}×${j.activeSensorHeight||j.height} · PHOTO ${j.width}×${j.height}`;if(j.nativePreviewFallback)setMsg('Camera is using a recovery fallback mode; preview is software-scaled to 160×120.');else if(j.framingConsistent===false)setMsg('Preview and still use different sensor modes; check camera recovery status.');setBadge(j.cameraReady?'READY':'CAMERA ERROR',!!j.cameraReady);return j}
function showImage(src,isViewfinder=false){$('preview').src=src;$('preview').hidden=false;$('nosignal').hidden=true;$('osdFilter').textContent=$('filter').selectedOptions[0].text.toUpperCase();if(isViewfinder){const old=viewObjectUrl;viewObjectUrl=src;if(old&&old!==src)setTimeout(()=>URL.revokeObjectURL(old),800)}else if(viewObjectUrl){URL.revokeObjectURL(viewObjectUrl);viewObjectUrl=null}}
async function image(){
 const controller=new AbortController();previewController=controller;
 const timer=setTimeout(()=>controller.abort(),7000);
 try{const r=await fetch('/snapshot?fast=1&t='+Date.now(),{cache:'no-store',signal:controller.signal});if(!r.ok)return false;const blob=await r.blob();if(!blob.size)return false;const url=URL.createObjectURL(blob);showImage(url,true);const now=performance.now();if(lastFrameAt){const instant=1000/(now-lastFrameAt);previewFps=previewFps?previewFps*0.65+instant*0.35:instant;}$('osdMode').textContent=`LIVE 160×120 · ${previewFps.toFixed(1)} FPS`;lastFrameAt=now;frameMisses=0;return true}
 catch(_){return false}
 finally{clearTimeout(timer);if(previewController===controller)previewController=null}
}
function scheduleCycle(){if(viewfinderOn&&autoMs>0&&!actionBusy){clearTimeout(tickTimer);tickTimer=setTimeout(cycle,autoMs)}}
async function cycle(){
 if(!viewfinderOn||autoMs<=0||actionBusy||previewBusy)return;
 previewBusy=true;
 try{
  if(Date.now()-lastStatusAt>15000&&!actionBusy){await status();lastStatusAt=Date.now()}
  if(viewfinderOn&&!actionBusy&&cameraReady){
   const ok=await image();
   if(!ok&&!actionBusy){
    frameMisses=Math.min(frameMisses+1,6);
    $('nosignal').hidden=false;
    $('nosignal').textContent=frameMisses>=3?'FRAME MISSED · use Recover camera':'FRAME MISSED · retrying';
    if(frameMisses>=3){
     viewfinderOn=false;clearTimeout(tickTimer);
     $('auto').textContent='Live view · Paused';$('auto').classList.remove('active');
     setMsg('Repeated frame misses. Preview paused so controls stay responsive; use Recover camera.');
    }else{
     setMsg('Viewfinder retrying…');autoMs=Math.min(50+frameMisses*150,950);
    }
   }else if(ok&&!actionBusy){
    autoMs=Number($('interval').value)||50;
    setMsg('Live view · shutter captures a full-resolution still.');
   }
  }
 }catch(e){if(!actionBusy){setBadge('OFFLINE',false);$('camera').textContent='no response';$('nosignal').hidden=false;$('nosignal').textContent='NO SIGNAL — check DigiCam Wi-Fi and reload.';setMsg('Could not contact camera. '+(e.name==='AbortError'?'Request timed out.':e.message))}}
 finally{previewBusy=false;if(!actionBusy)scheduleCycle()}
}
function commandKey(path){if(path.startsWith('/settings?'))return 'settings';if(path.startsWith('/filter?'))return 'filter';if(path.startsWith('/flash?'))return 'flash';if(path.startsWith('/capture'))return 'capture';return path.split('?')[0]}
function queueCommand(path,success){
 const key=commandKey(path);
 if(actionBusy){
  if(key==='settings'){
   const existing=pendingCommands.get(key);const params=new URLSearchParams(existing?existing.path.split('?')[1]||'':'');
   for(const [k,v] of new URLSearchParams(path.split('?')[1]||''))params.set(k,v);
   pendingCommands.set(key,{path:'/settings?'+params.toString(),success:success||'Settings updated.'});
  }else pendingCommands.set(key,{path,success});
  setMsg(key==='capture'?'Shutter waiting for the current camera operation…':'Saving the latest control setting…');return;
 }
 runCommand(path,success);
}
function nextQueuedCommand(){
 if(pendingCommands.has('capture')){const c=pendingCommands.get('capture');pendingCommands.delete('capture');return c}
 const first=pendingCommands.entries().next();if(first.done)return null;pendingCommands.delete(first.value[0]);return first.value[1];
}
async function runCommand(path,success){
 actionBusy=true;clearTimeout(tickTimer);lastStatusAt=Date.now();
 if(previewController)previewController.abort();
 setMsg(path.startsWith('/capture')?'Taking full-resolution photo…':'Applying setting…');
 let actionTimeout=0;
 try{
  const controller=new AbortController();
  const isResolutionChange=/[?&]resolution=/.test(path);
  actionTimeout=setTimeout(()=>controller.abort(),path.startsWith('/capture')?20000:(path.startsWith('/recover')?25000:(isResolutionChange?30000:(path.startsWith('/settings?')?10000:6000))));
  if(isResolutionChange){$('resolution').disabled=true;$('resolutionNote').textContent='Changing sensor mode. Preview is paused while the camera restarts; this can take several seconds.';}
  const r=await fetch(path,{cache:'no-store',signal:controller.signal});
  if(!r.ok){let why='HTTP '+r.status;try{const j=await r.json();if(j.error)why=j.error;if(j.resolution)$('resolution').value=j.resolution;if(j.width&&j.height)$('resolutionNote').textContent=`Kept ${j.width}×${j.height}; ${why}`}catch(_){}throw new Error(why)}
  if(path.startsWith('/capture')){const blob=await r.blob();if(!blob.size)throw new Error('Empty image');const u=URL.createObjectURL(blob),a=document.createElement('a');a.href=u;a.download='DigiCam.jpg';document.body.appendChild(a);a.click();a.remove();showImage(u);setTimeout(()=>URL.revokeObjectURL(u),15000);setMsg('Photo captured at '+$('osdMode').textContent+'.');}
  else{try{const j=await r.json();if(j.flashEnabled!==undefined){flash=!!j.flashEnabled;$('flash').textContent=flash?'Flash armed':'Flash off';$('osdFlash').textContent='FLASH '+(flash?'ARMED':'OFF')}if(j.resolution){$('resolution').value=j.resolution;$('resolutionNote').textContent=`Applied to both viewfinder and still: ${j.width}×${j.height}.`} }catch(_){}if(isResolutionChange||path.startsWith('/recover')){viewfinderOn=true;frameMisses=0;$('auto').textContent='Live view · On';$('auto').classList.add('active');}setMsg(success||'Setting saved.')}
 }catch(e){if(/[?&]resolution=/.test(path)&&!$('resolutionNote').textContent.startsWith('Kept '))$('resolutionNote').textContent='Change did not finish. Check Device status before retrying.';setMsg('Action failed: '+(e.name==='AbortError'?'request timed out':e.message))}
 finally{
  clearTimeout(actionTimeout);
  if(path.startsWith('/capture')){capturePending=false;$('capture').disabled=false;$('countdown').hidden=true;}
  if(/[?&]resolution=/.test(path))$('resolution').disabled=false;
  actionBusy=false;
  const next=nextQueuedCommand();
  if(next)setTimeout(()=>runCommand(next.path,next.success),0);
  else{lastStatusAt=0;scheduleCycle()}
 }
}
function command(path,success){queueCommand(path,success)}
function formatBytes(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB'}
function safeText(el,text){el.textContent=String(text??'')}
async function loadLibrary(){if($('libraryMenu').hidden)return;if(actionBusy||previewBusy){$('libraryMsg').textContent='Finishing the current camera request…';setTimeout(loadLibrary,120);return} $('libraryRefresh').disabled=true;$('libraryInfo').textContent='Scanning microSD /DCIM…';$('fileList').replaceChildren();$('libraryMsg').textContent='';try{const j=await fetchJson('/files',7000);if(!j.sdReady){$('libraryInfo').textContent='No microSD card detected. Insert a compatible card and restart DigiCam.';$('fileList').innerHTML='<div class="empty">NO CARD DETECTED</div>';$('fileCount').textContent='';libraryLoaded=true;return}const files=Array.isArray(j.files)?j.files:[];$('libraryInfo').textContent=`${files.length} photo${files.length===1?'':'s'} · folder /DCIM · card ${j.cardSize?formatBytes(j.cardSize):'ready'}`;$('fileCount').textContent=files.length?' · '+files.length:'';if(!files.length){$('fileList').innerHTML='<div class="empty">No JPG photos found in /DCIM yet.</div>';libraryLoaded=true;return}for(const f of files){const row=document.createElement('div');row.className='file';const meta=document.createElement('div');meta.className='filename';const name=document.createElement('strong');name.textContent=f.name;const size=document.createElement('span');size.className='filesize';size.textContent=formatBytes(f.size);meta.append(name,size);const actions=document.createElement('div');actions.className='fileactions';const view=document.createElement('button');view.textContent='VIEW';view.onclick=()=>window.open('/file?name='+encodeURIComponent(f.name),'_blank','noopener');const download=document.createElement('button');download.textContent='SAVE';download.onclick=()=>{const a=document.createElement('a');a.href='/file?download=1&name='+encodeURIComponent(f.name);a.download=f.name;document.body.appendChild(a);a.click();a.remove()};const del=document.createElement('button');del.textContent='DELETE';del.className='danger';del.onclick=async()=>{if(!confirm('Delete '+f.name+' from the SD card? This cannot be undone.'))return;del.disabled=true;try{const out=await fetchJson('/delete?name='+encodeURIComponent(f.name),5000,{method:'POST'});if(!out.ok)throw new Error(out.error||'Delete failed');row.remove();$('libraryMsg').textContent='Deleted '+f.name;const remaining=$('fileList').querySelectorAll('.file').length;$('fileCount').textContent=remaining?' · '+remaining:'';if(!remaining)$('fileList').innerHTML='<div class="empty">No photos left in /DCIM.</div>'}catch(e){$('libraryMsg').textContent='Delete failed: '+e.message;del.disabled=false}};actions.append(view,download,del);row.append(meta,actions);$('fileList').append(row)}libraryLoaded=true}catch(e){$('libraryInfo').textContent='Could not scan the card.';$('fileList').innerHTML='<div class="empty">Library request failed: '+String(e.message).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))+'</div>'}finally{$('libraryRefresh').disabled=false}}
async function shutterWithTimer(){if(capturePending)return;const seconds=Number($('timer').value)||0;capturePending=true;$('capture').disabled=true;if(seconds<=0){command('/capture','Capture complete.');return}clearInterval(countdownTimer);countdownAbort=false;let remaining=seconds;$('countdown').hidden=false;$('countdown').textContent=`SELF-TIMER · ${remaining}`;countdownTimer=setInterval(()=>{remaining--;if(remaining>0){$('countdown').textContent=`SELF-TIMER · ${remaining}`}else{clearInterval(countdownTimer);$('countdown').hidden=true;if(!countdownAbort)command('/capture','Self-timer capture complete.')}},1000);setMsg('Get ready…');}
$('capture').onclick=shutterWithTimer;$('zoom').onchange=e=>{$('zoomOut').value=e.target.value+'×';command('/settings?zoom='+e.target.value,'Digital zoom updated.');};$('refresh').onclick=()=>{if(!actionBusy){viewfinderOn=true;frameMisses=0;$('auto').textContent='Live view · On';$('auto').classList.add('active');lastStatusAt=0;clearTimeout(tickTimer);cycle()}};$('flash').onclick=()=>command('/flash?on='+(flash?'0':'1'),'Flash setting updated.');$('filter').onchange=e=>command('/filter?name='+encodeURIComponent(e.target.value),'Picture style updated.');$('mode').onchange=e=>command('/settings?mode='+encodeURIComponent(e.target.value),'Shooting mode updated.');$('exposure').oninput=e=>$('exposureOut').value='EV '+e.target.value;$('exposure').onchange=e=>command('/settings?exposure='+e.target.value,'Exposure compensation updated.');$('grain').oninput=e=>$('grainOut').value=e.target.value;$('grain').onchange=e=>command('/settings?grain='+e.target.value,'Still grain updated.');$('quality').oninput=e=>$('qualityOut').value=e.target.value;$('quality').onchange=e=>command('/settings?quality='+e.target.value,'JPEG quality updated.');$('resolution').onchange=e=>{const choice=e.target.value;$('resolutionNote').textContent='Reinitializing sensor… preview and still will use the selected size.';command('/settings?resolution='+encodeURIComponent(choice),'Resolution updated for viewfinder and still.');};$('auto').onclick=()=>{viewfinderOn=!viewfinderOn;if(viewfinderOn){$('auto').textContent='Live view · On';$('auto').classList.add('active');autoMs=Number($('interval').value)||50;if(Number($('interval').value)===0)$('interval').value='50';lastStatusAt=0;cycle()}else{clearTimeout(tickTimer);$('auto').textContent='Live view · Paused';$('auto').classList.remove('active');setMsg('Viewfinder paused; shutter capture remains available.')}};$('interval').onchange=e=>{autoMs=Number(e.target.value);if(autoMs>0){viewfinderOn=true;$('auto').textContent='Live view · On';$('auto').classList.add('active');cycle()}else{viewfinderOn=false;clearTimeout(tickTimer);$('auto').textContent='Live view · Paused';$('auto').classList.remove('active')}};$('status').onclick=async e=>{e.preventDefault();e.stopPropagation();const p=$('json');try{p.textContent=JSON.stringify(await fetchJson('/status'),null,2)}catch(e){p.textContent=e.message}p.parentElement.open=true;p.closest('details.diagnostics').open=true};$('reboot').onclick=()=>{if(confirm('Reinitialize the camera sensor at the selected resolution? The web page may pause for several seconds.'))command('/recover','Camera recovery completed.');};$('tabCamera').onclick=()=>{const details=$('settingsDetails');const opening=!details.open;$('libraryMenu').hidden=true;$('cameraMenu').hidden=false;details.open=opening;$('tabCamera').classList.toggle('selected',opening);$('tabLibrary').classList.remove('selected');if(!viewfinderOn){viewfinderOn=true;$('auto').textContent='Live view · On';$('auto').classList.add('active');lastStatusAt=0;cycle()}};$('tabLibrary').onclick=()=>{viewfinderOn=false;clearTimeout(tickTimer);if(previewController)previewController.abort();$('auto').textContent='Live view · Paused';$('auto').classList.remove('active');$('settingsDetails').open=false;$('cameraMenu').hidden=true;$('libraryMenu').hidden=false;$('tabCamera').classList.remove('selected');$('tabLibrary').classList.add('selected');loadLibrary()};$('libraryRefresh').onclick=loadLibrary;$('badge').textContent='STARTING';$('auto').textContent='Live view · On';cycle();
})();
</script></body></html>
)DGCAMHTML";

static bool isSafeJpegName(const String &name) {
  if (name.length() < 5 || name.length() > 40 || name.indexOf("..") >= 0 ||
      name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

static String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

static void handleFiles() {
  String json;
  json.reserve(1536);
  json += "{\"sdReady\":";
  json += sdReady ? "true" : "false";
  json += ",\"cardSize\":";
  char cardSizeBuffer[24];
  const unsigned long long cardBytes = sdReady ? static_cast<unsigned long long>(SD_MMC.cardSize()) : 0ULL;
  snprintf(cardSizeBuffer, sizeof(cardSizeBuffer), "%llu", cardBytes);
  json += cardSizeBuffer;
  json += ",\"files\":[";
  if (sdReady) {
    File dir = SD_MMC.open("/DCIM");
    if (dir && dir.isDirectory()) {
      bool first = true;
      uint16_t reported = 0;
      while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        if (!file.isDirectory()) {
          const String name = baseName(String(file.name()));
          if (isSafeJpegName(name) && reported < 128) {
            if (!first) json += ',';
            first = false;
            json += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String(static_cast<unsigned long>(file.size())) + "}";
            ++reported;
          }
        }
        file.close();
      }
      dir.close();
    }
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

static void handleFile() {
  if (!sdReady) { server.send(503, "text/plain; charset=utf-8", "No microSD card detected"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain; charset=utf-8", "Missing file name"); return; }
  const String name = server.arg("name");
  if (!isSafeJpegName(name)) { server.send(400, "text/plain; charset=utf-8", "Invalid JPEG name"); return; }
  const String path = String("/DCIM/") + name;
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) { if (file) file.close(); server.send(404, "text/plain; charset=utf-8", "Photo not found"); return; }
  server.sendHeader("Cache-Control", "no-store");
  String disposition = server.hasArg("download") && server.arg("download") == "1" ? "attachment" : "inline";
  server.sendHeader("Content-Disposition", disposition + "; filename=\"" + name + "\"");
  server.streamFile(file, "image/jpeg");
  file.close();
}

static void handleDeleteFile() {
  if (!sdReady) { server.send(503, "application/json", "{\"ok\":false,\"error\":\"No microSD card\"}"); return; }
  if (!server.hasArg("name") || !isSafeJpegName(server.arg("name"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JPEG name\"}"); return;
  }
  const String path = String("/DCIM/") + server.arg("name");
  if (!SD_MMC.exists(path)) { server.send(404, "application/json", "{\"ok\":false,\"error\":\"Photo not found\"}"); return; }
  if (!SD_MMC.remove(path)) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"Delete failed\"}"); return; }
  lastAction = String("Deleted ") + server.arg("name");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleSetFilter() {
  if (!server.hasArg("name")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing name\"}");
    return;
  }
  PhotoFilter next;
  if (!parseFilter(server.arg("name"), next)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid filter\"}");
    return;
  }
  currentFilter = next;
  lastAction = String("Filter: ") + filterName(currentFilter);
  server.send(200, "application/json", String("{\"ok\":true,\"filter\":\"") + filterName(currentFilter) + "\"}");
}

static void handleFlash() {
  bool newState = flashEnabled;
  if (server.hasArg("on")) newState = server.arg("on").toInt() != 0;
  flashEnabled = newState;
  digitalWrite(DIGICAM_FLASH_GPIO, LOW); // LED only lights during an actual capture.
  lastAction = String("Flash armed: ") + (flashEnabled ? "on" : "off");
  server.send(200, "application/json", String("{\"ok\":true,\"flashEnabled\":") + (flashEnabled ? "true}" : "false}"));
}


static void handleSettings() {
  if (server.hasArg("resolution")) {
    const String requestedId = server.arg("resolution");
    const ResolutionOption *option = resolutionById(requestedId);
    if (!option) {
      server.send(400, "application/json", String("{\"ok\":false,\"error\":\"unknown resolution id\",\"resolution\":\"") + resolutionId(selectedCameraFrameSize) + "\"}");
      return;
    }
    String reason;
    if (!selectCameraResolution(*option, reason)) {
      lastAction = reason;
      String error = String("{\"ok\":false,\"error\":\"") + jsonEscape(reason) + "\",\"resolution\":\"" + resolutionId(selectedCameraFrameSize) + "\",\"width\":" + String(frameWidth(selectedCameraFrameSize)) + ",\"height\":" + String(frameHeight(selectedCameraFrameSize)) + "}";
      server.send(409, "application/json", error);
      return;
    }
  }
  if (server.hasArg("mode")) {
    PhotoMode next;
    if (!parsePhotoMode(server.arg("mode"), next)) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid photo mode\"}"); return; }
    (void)setPhotoMode(next);
  }
  if (server.hasArg("quality")) {
    const int requested = server.arg("quality").toInt();
    if (requested < 60 || requested > 95) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"quality must be 60..95\"}");
      return;
    }
    jpegQuality = static_cast<uint8_t>(requested);
    lastAction = String("JPEG quality: ") + String(jpegQuality);
  }
  if (server.hasArg("zoom")) {
    const int requestedZoom = server.arg("zoom").toInt();
    if (requestedZoom < 1 || requestedZoom > 3) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"zoom must be 1..3\"}"); return; }
    digitalZoom = static_cast<uint8_t>(requestedZoom);
    lastAction = String("Digital zoom: ") + String(digitalZoom) + "x";
  }
  if (server.hasArg("grain")) {
    const int requestedGrain = server.arg("grain").toInt();
    if (requestedGrain < 0 || requestedGrain > 6) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"grain must be 0..6\"}"); return; }
    grainAmount = static_cast<uint8_t>(requestedGrain);
    lastAction = String("Still grain: ") + String(grainAmount);
  }
  if (server.hasArg("exposure")) {
    const int requestedExposure = server.arg("exposure").toInt();
    if (requestedExposure < -2 || requestedExposure > 2) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"exposure must be -2..2\"}"); return; }
    const bool applied = setExposureCompensation(requestedExposure);
    lastAction = String("Exposure EV ") + String(requestedExposure) + (applied ? " applied" : " saved; sensor control unsupported");
  }
  String json = String("{\"ok\":true,\"jpegQuality\":") + String(jpegQuality) +
                ",\"digitalZoom\":" + String(digitalZoom) + ",\"grainAmount\":" + String(grainAmount) +
                ",\"exposureCompensation\":" + String(exposureCompensation) + ",\"photoMode\":\"" +
                photoModeName(currentMode) + "\",\"resolution\":\"" + resolutionId(selectedCameraFrameSize) +
                "\",\"resolutionName\":\"" + String(resolutionLabel(selectedCameraFrameSize)) + "\",\"width\":" + String(frameWidth(selectedCameraFrameSize)) + ",\"height\":" + String(frameHeight(selectedCameraFrameSize)) +
                ",\"exposureSupported\":" + String(exposureHardwareApplied ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

static void handleCameraRecover() {
  const framesize_t target = selectedCameraFrameSize;
  String reason;
  bool recovered = false;
  const ResolutionOption *targetOption = resolutionBySize(target);
  if (targetOption && resolutionMemoryFits(*targetOption, reason)) {
    if (restartCameraAt(target))
      recovered = probeCameraFrame(frameWidth(target), frameHeight(target), &reason);
    else if (reason.length() == 0)
      reason = lastAction;
  }
  if (!recovered && reason.length() == 0) reason = lastAction;
  if (!recovered && target != FRAMESIZE_QVGA) {
    if (cameraReady) { (void)esp_camera_deinit(); cameraReady = false; }
    String fallbackReason;
    if (restartCameraAt(FRAMESIZE_QVGA) &&
        probeCameraFrame(frameWidth(FRAMESIZE_QVGA), frameHeight(FRAMESIZE_QVGA), &fallbackReason)) {
      selectedCameraFrameSize = FRAMESIZE_QVGA;
      activeCameraFrameSize = FRAMESIZE_QVGA;
      recovered = true;
      reason = String("Selected mode failed; recovered at QVGA. ") + reason;
    } else {
      reason += String(" QVGA fallback failed: ") + fallbackReason;
    }
  }
  if (!recovered) {
    cameraReady = false;
    lastAction = String("Camera recovery failed: ") + reason;
    server.send(503, "application/json", String("{\"ok\":false,\"error\":\"") + jsonEscape(lastAction) + "\"}");
    return;
  }
  cameraReady = true;
  activeCameraFrameSize = selectedCameraFrameSize;
  lastFrameAtMs = millis();
  cameraRecoveryAttempted = false;
  lastAction = String("Camera recovered at ") + String(frameWidth(selectedCameraFrameSize)) + "x" + String(frameHeight(selectedCameraFrameSize));
  server.send(200, "application/json", String("{\"ok\":true,\"resolution\":\"") + resolutionId(selectedCameraFrameSize) +
              "\",\"width\":" + String(frameWidth(selectedCameraFrameSize)) + ",\"height\":" +
              String(frameHeight(selectedCameraFrameSize)) + "}");
}

static void handleReboot() {
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(250);
  ESP.restart();
}

#if DIGICAM_I2C_ENABLED
static bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

#if DIGICAM_ENABLE_JOYSTICK
static void initOta();
static const char *LOCAL_MENU_ITEMS[7] = {"Wi-Fi radio", "Resolution", "Picture style", "Photo mode", "Flash", "Exposure EV", "Max safe res"};
static bool readPcf8574(uint8_t &value) {
  if (!pcfReady) return false;
  if (Wire.requestFrom(static_cast<uint8_t>(DIGICAM_PCF8574_ADDR), static_cast<uint8_t>(1)) != 1) return false;
  value = static_cast<uint8_t>(Wire.read());
  return true;
}
#endif

static void initPeripherals() {
#if DIGICAM_I2C_USES_UART
  // GPIO3=SDA and GPIO1=SCL are UART0. Leave the web/OTA stack running,
  // but release UART0 so the display can use those pins as I2C.
  Serial.flush();
  Serial.end();
#endif
  Wire.begin(DIGICAM_I2C_SDA_GPIO, DIGICAM_I2C_SCL_GPIO, 100000);
  Wire.setTimeOut(25);
  i2cEnabled = true;

  oledReady = false;
  detectedOledAddress = 0;
  // GME12864-78 1.3-inch modules are SH1106-compatible; scan both common I2C addresses.
  if (i2cDevicePresent(0x3C)) detectedOledAddress = 0x3C;
  else if (i2cDevicePresent(0x3D)) detectedOledAddress = 0x3D;
  if (detectedOledAddress != 0) {
    oled.setI2CAddress(static_cast<uint8_t>(detectedOledAddress << 1));
    oled.begin();
    oled.setPowerSave(0);
    oled.setFont(u8g2_font_5x8_tf);
    oled.clearBuffer();
    oled.drawStr(18, 20, "DIGICAM DISPLAY");
    oled.drawHLine(8, 25, 112);
    oled.drawStr(27, 43, "SH1106 / 128x64");
    oled.drawStr(34, 58, "BOOTING...");
    oled.sendBuffer();
    oledReady = true;
  }
  // Optional MPU6050 on the same I2C bus as the SH1106. Missing sensor is non-fatal.
  mpuReady = i2cDevicePresent(0x68) || i2cDevicePresent(0x69);
  if (mpuReady) {
    activeMpuAddress = i2cDevicePresent(0x68) ? 0x68 : 0x69;
    Wire.beginTransmission(activeMpuAddress); Wire.write(0x6B); Wire.write(0x00);
    mpuReady = Wire.endTransmission() == 0;
    if (mpuReady) { lastAction = "MPU6050 detected"; }
  }

#if DIGICAM_ENABLE_JOYSTICK
  if (i2cDevicePresent(DIGICAM_ADS1115_ADDR)) {
    joystickReady = joystickAdc.begin(DIGICAM_ADS1115_ADDR);
    adsReady = joystickReady;
    if (joystickReady) {
      joystickAdc.setGain(GAIN_ONE);
      joystickAdc.setDataRate(RATE_ADS1115_860SPS);
    }
  } else { joystickReady = false; adsReady = false; }

  if (i2cDevicePresent(DIGICAM_PCF8574_ADDR)) {
    pcfReady = true;
    Wire.beginTransmission(DIGICAM_PCF8574_ADDR);
    Wire.write(static_cast<uint8_t>(0xFF));
    pcfReady = Wire.endTransmission() == 0;
    previousCaptureButtonHigh = true;
  } else pcfReady = false;
#else
  joystickReady = false;
  adsReady = false;
  pcfReady = false;
#endif
  if (oledReady) lastAction = String("SH1106 ready at 0x") + String(detectedOledAddress, HEX) + String(" on GPIO") + String(DIGICAM_I2C_SDA_GPIO) + "/" + String(DIGICAM_I2C_SCL_GPIO);
  else lastAction = String("OLED not detected at 0x3C/0x3D on GPIO") + String(DIGICAM_I2C_SDA_GPIO) + "/" + String(DIGICAM_I2C_SCL_GPIO);
}

#if DIGICAM_ENABLE_JOYSTICK
static bool readMpu6050() {
  if (!i2cEnabled) return false;
  const uint32_t now = millis();
  if (now - lastMpuPollMs < 250) return mpuReady;
  lastMpuPollMs = now;
  Wire.beginTransmission(activeMpuAddress);
  Wire.write(0x3B); // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) { mpuReady = false; return false; }
  if (Wire.requestFrom(activeMpuAddress, static_cast<uint8_t>(6), static_cast<uint8_t>(true)) != 6) {
    mpuReady = false; return false;
  }
  int16_t ax = static_cast<int16_t>((Wire.read() << 8) | Wire.read());
  int16_t ay = static_cast<int16_t>((Wire.read() << 8) | Wire.read());
  int16_t az = static_cast<int16_t>((Wire.read() << 8) | Wire.read());
  // Wake MPU6050; a successful read is enough for optional tilt sensing.
  mpuReady = true;
  const float fx = static_cast<float>(ax), fy = static_cast<float>(ay), fz = static_cast<float>(az);
  mpuRollDegrees = atan2f(fy, fz) * 57.2957795f;
  mpuPitchDegrees = atan2f(-fx, sqrtf(fy * fy + fz * fz)) * 57.2957795f;
  return true;
}

static void setWirelessEnabled(bool enabled) {
  if (wifiEnabled == enabled) return;
  if (!enabled) {
    // Stop network services before powering down the radio. Routes remain registered,
    // so enabling Wi-Fi again can restart the existing server without duplicate handlers.
    if (otaReady) { ArduinoOTA.end(); otaReady = false; }
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    (void)esp_wifi_stop();
    wifiEnabled = false;
    standaloneMenuActive = true;
    lastAction = "Standalone mode: Wi-Fi radio off";
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    const bool apStarted = WiFi.softAP(DIGICAM_AP_SSID, DIGICAM_AP_PASSWORD, 6, false, 4);
    if (apStarted) {
      server.begin();
      initOta();
      wifiEnabled = true;
      standaloneMenuActive = false;
      lastAction = "Wi-Fi on; web interface restored";
    } else {
      WiFi.mode(WIFI_OFF);
      wifiEnabled = false;
      standaloneMenuActive = true;
      lastAction = "Wi-Fi restart failed; staying standalone";
    }
  }
}

static void applyLocalMenuAction() {
  switch (localMenuIndex) {
    case 0: setWirelessEnabled(!wifiEnabled); break;
    case 1: {
      const ResolutionOption *current = resolutionBySize(selectedCameraFrameSize);
      size_t idx = 0;
      if (current) idx = static_cast<size_t>(current - RESOLUTION_OPTIONS);
      // Rotate through available modes; memory guard + frame probe decide acceptance.
      const size_t next = (idx + 1) % RESOLUTION_OPTION_COUNT;
      String why;
      if (!selectCameraResolution(RESOLUTION_OPTIONS[next], why)) lastAction = why;
      else lastAction = String("Resolution ") + RESOLUTION_OPTIONS[next].label;
      break;
    }
    case 2: currentFilter = static_cast<PhotoFilter>((static_cast<uint8_t>(currentFilter) + 1U) % 5U); lastAction = String("Filter ") + filterName(currentFilter); break;
    case 3: setPhotoMode(static_cast<PhotoMode>((static_cast<uint8_t>(currentMode) + 1U) % 5U)); break;
    case 4: flashEnabled = !flashEnabled; lastAction = flashEnabled ? "Flash armed" : "Flash off"; break;
    case 5: { int ev = exposureCompensation + 1; if (ev > 2) ev = -2; (void)setExposureCompensation(ev); lastAction = String("Exposure EV ") + String(exposureCompensation); break; }
    case 6: {
      bool found = false;
      for (int i = static_cast<int>(RESOLUTION_OPTION_COUNT) - 1; i >= 0; --i) {
        String why;
        if (!resolutionMemoryFits(RESOLUTION_OPTIONS[i], why)) continue;
        if (selectCameraResolution(RESOLUTION_OPTIONS[i], why)) {
          lastAction = String("Highest verified standalone mode: ") + RESOLUTION_OPTIONS[i].label;
          found = true; break;
        }
        lastAction = why;
      }
      if (!found) lastAction = "No larger verified mode; kept current resolution";
      break;
    }
    default: break;
  }
}

static void handleJoystick() {
  if (!joystickReady || !pcfReady) { (void)readMpu6050(); return; }
  const uint32_t now = millis();
  if (!wifiEnabled) (void)readMpu6050();
  if (!wifiEnabled && now - lastJoystickPoll >= 100) {
    lastJoystickPoll = now;
    const int16_t x = joystickAdc.readADC_SingleEnded(0);
    const int16_t y = joystickAdc.readADC_SingleEnded(1);
    if (now - localLastNavMs >= 220) {
      if (y < 8200) { localMenuIndex = (localMenuIndex + 6U) % 7U; localLastNavMs = now; }
      else if (y > 18500) { localMenuIndex = (localMenuIndex + 1U) % 7U; localLastNavMs = now; }
      else if (x < 8200) { int ev = exposureCompensation - 1; if (ev < -2) ev = 2; (void)setExposureCompensation(ev); localLastNavMs = now; }
      else if (x > 18500) { int ev = exposureCompensation + 1; if (ev > 2) ev = -2; (void)setExposureCompensation(ev); localLastNavMs = now; }
    }
  }
  if (now - lastButtonPoll >= 25) {
    lastButtonPoll = now;
    uint8_t pins = 0xFF;
    if (readPcf8574(pins)) {
      const bool low = (pins & (1U << DIGICAM_PCF_CAPTURE_BUTTON_BIT)) == 0;
      if (low && !localButtonWasLow) { localButtonDownMs = now; localLongPressHandled = false; }
      if (low && !localLongPressHandled && now - localButtonDownMs >= 800) {
        localLongPressHandled = true;
        if (wifiEnabled) setWirelessEnabled(false); // long press enters low-power standalone mode
        else applyLocalMenuAction();                 // long press selects highlighted menu item
      }
      if (!low && localButtonWasLow && !localLongPressHandled) {
        physicalCapturePending = true; // short click remains the shutter
        lastAction = "Physical shutter pressed";
      }
      localButtonWasLow = low;
      previousCaptureButtonHigh = !low;
    }
  }
}

static void servicePhysicalCapture() {
  if (!physicalCapturePending) return;
  physicalCapturePending = false;
  uint8_t *jpg = nullptr;
  size_t len = 0;
  if (!captureStillToBuffer(&jpg, &len)) {
    if (jpg) free(jpg);
    lastAction = "Physical capture failed";
    return;
  }
  ++captureCount;
  lastStillJpegBytes = static_cast<uint32_t>(len);
  lastStillJpegValid = len >= 4 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[len-2] == 0xFF && jpg[len-1] == 0xD9;
  String saved;
  const bool savedOk = saveJpegToSd(jpg, len, &saved);
  free(jpg);
  lastAction = savedOk ? String("Photo saved ") + saved : (sdReady ? "Photo captured; SD write failed" : "Photo captured; no SD card");
}
#else
static void handleJoystick() {}
static void servicePhysicalCapture() {}
#endif

static void drawLocalThumbnail() {
  // SH1106 is monochrome: draw a tiny 80x40 luminance thumbnail only in standalone mode.
  // This intentionally runs at low cadence and never competes with the web preview.
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb || !fb->buf || fb->format != PIXFORMAT_YUV422 || fb->width < 2 || fb->height < 2 ||
      fb->len < static_cast<size_t>(fb->width) * fb->height * 2U) {
    if (fb) esp_camera_fb_return(fb);
    oled.drawFrame(0, 12, 82, 42);
    oled.setFont(u8g2_font_5x8_tf); oled.drawStr(8, 34, "NO FRAME");
    return;
  }
  for (uint8_t y = 0; y < 40; ++y) {
    const uint16_t sy = static_cast<uint16_t>((static_cast<uint32_t>(y) * fb->height) / 40U);
    for (uint8_t x = 0; x < 80; ++x) {
      const uint16_t sx = static_cast<uint16_t>((static_cast<uint32_t>(x) * fb->width / 80U) & ~1U);
      const uint8_t luma = fb->buf[(static_cast<size_t>(sy) * fb->width + sx) * 2U];
      if (luma > 112) oled.drawPixel(x, 13 + y);
    }
  }
  // Rule-of-thirds overlay and center mark.
  oled.drawVLine(26, 13, 40); oled.drawVLine(53, 13, 40);
  oled.drawHLine(0, 26, 80); oled.drawHLine(0, 39, 80);
  oled.drawHLine(38, 33, 5); oled.drawVLine(40, 31, 5);
  esp_camera_fb_return(fb);
}

static void updateOled() {
  if (!oledReady) return;
  const uint32_t now = millis();
  const uint32_t interval = (!wifiEnabled && !standaloneMenuActive) ? 450 : 250;
  if (now - lastOledUpdate < interval) return;
  lastOledUpdate = now;
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  if (wifiEnabled) {
    // Keep the display lightweight while web mode is active; the local camera UI is dormant.
    oled.drawStr(0, 8, "DIGICAM  CONNECTED");
    oled.drawHLine(0, 11, 128);
    oled.setCursor(0, 22); oled.print("CAM "); oled.print(cameraReady ? "READY" : "ERROR");
    oled.setCursor(0, 32); oled.print("PHOTO "); oled.print(frameWidth(selectedCameraFrameSize)); oled.print("x"); oled.print(frameHeight(selectedCameraFrameSize));
    oled.setCursor(0, 42); oled.print("SD "); oled.print(sdReady ? "READY" : "NO CARD"); oled.print(" MPU "); oled.print(mpuReady ? "OK" : "--");
    oled.setCursor(0, 54); oled.print("WEB 192.168.4.1");
    oled.setCursor(0, 63); oled.print("Hold click: Wi-Fi OFF");
  } else if (standaloneMenuActive) {
    // Thumbnail mode is intentionally exclusive to standalone mode, to preserve Wi-Fi responsiveness.
    drawLocalThumbnail();
    oled.drawVLine(84, 12, 52);
    oled.setCursor(88, 20); oled.print("D-CAM");
    oled.setCursor(88, 30); oled.print(wifiEnabled ? "WIFI ON" : "NO WIFI");
    oled.setCursor(88, 40); oled.print(mpuReady ? "LEVEL OK" : "NO MPU");
    oled.setCursor(88, 50); oled.print(sdReady ? "SD OK" : "SD --");
    oled.setCursor(88, 61); oled.print("EV"); oled.print(exposureCompensation > 0 ? "+" : ""); oled.print(exposureCompensation);
    oled.drawHLine(0, 55, 80);
    oled.setCursor(0, 63); oled.print("Hold: "); oled.print(LOCAL_MENU_ITEMS[localMenuIndex]);
  } else {
    oled.drawStr(0, 8, "DIGICAM STARTING");
    oled.drawFrame(8, 22, 112, 18);
    oled.drawBox(10, 24, static_cast<uint8_t>((now / 25U) % 108U), 14);
  }
  oled.sendBuffer();
}
#else
static void initPeripherals() { i2cEnabled = false; oledReady = false; adsReady = false; pcfReady = false; joystickReady = false; }
static void handleJoystick() {}
static void servicePhysicalCapture() {}
static void updateOled() {}
#endif

#if DIGICAM_I2C_ENABLED
static void showBootAnimation() {
  if (!oledReady) return;
  oled.setFont(u8g2_font_5x8_tf);
  for (uint8_t step = 0; step < 4; ++step) {
    oled.clearBuffer();
    oled.drawStr(28, 12, "D I G I C A M");
    oled.drawFrame(14, 25, 100, 12);
    oled.drawBox(16, 27, static_cast<uint8_t>((step + 1U) * 24U), 8);
    oled.setCursor(26, 49); oled.print("STARTING "); oled.print((step + 1U) * 25U); oled.print("%");
    oled.setCursor(16, 61); oled.print("GC2145 / SH1106");
    oled.sendBuffer();
    delay(55);
  }
}
#endif

static bool initCamera(framesize_t frameSize) {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_Y2;
  config.pin_d1 = CAM_PIN_Y3;
  config.pin_d2 = CAM_PIN_Y4;
  config.pin_d3 = CAM_PIN_Y5;
  config.pin_d4 = CAM_PIN_Y6;
  config.pin_d5 = CAM_PIN_Y7;
  config.pin_d6 = CAM_PIN_Y8;
  config.pin_d7 = CAM_PIN_Y9;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = DIGICAM_XCLK_HZ;
  config.pixel_format = PIXFORMAT_YUV422;
  config.frame_size = frameSize;
  config.jpeg_quality = 12;
  // GC2145 has no hardware JPEG output in this driver, so the app captures YUV422
  // and software-encodes it. Espressif explicitly recommends using one framebuffer
  // for YUV/RGB and reserves continuous multi-buffer mode for JPEG; this is the
  // key integrity fix for torn/missing rows while Wi-Fi is active.
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    cameraReady = false;
    return false;
  }
  sensor_t *sensor = esp_camera_sensor_get();
#if !DIGICAM_I2C_USES_UART
  if (sensor) {
    // Keep startup conservative; do not force OV2640-only sensor controls.
    Serial.printf("[CAM] PID=0x%04x\n", sensor->id.PID);
  }
#else
  (void)sensor;
#endif
  cameraReady = true;
  return true;
}

static void initSdCard() {
  // 1-bit SDMMC mode frees DAT1/DAT2/DAT3; do not switch to 4-bit mode with this pin plan.
  if (SD_MMC.begin("/sdcard", true)) {
    sdReady = SD_MMC.cardType() != CARD_NONE;
    if (sdReady) {
      if (!SD_MMC.exists("/DCIM")) SD_MMC.mkdir("/DCIM");
      lastAction = "microSD ready";
    }
  } else {
    sdReady = false;
    lastAction = "No microSD; web capture still works";
  }
}

static void initOta() {
  ArduinoOTA.setHostname(DIGICAM_OTA_HOSTNAME);
  ArduinoOTA.setPort(DIGICAM_OTA_PORT);
  ArduinoOTA.setPassword(DIGICAM_OTA_PASSWORD);
  ArduinoOTA.onStart([]() { lastAction = "OTA update started"; });
  ArduinoOTA.onEnd([]() { lastAction = "OTA update complete; restarting"; });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) lastAction = String("OTA ") + String((progress * 100U) / total) + "%";
  });
  ArduinoOTA.onError([](ota_error_t error) {
    lastAction = String("OTA error ") + String(static_cast<int>(error));
  });
  ArduinoOTA.begin();
  otaReady = true;
}

static void initRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/snapshot", HTTP_GET, handleSnapshot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/file", HTTP_GET, handleFile);
  server.on("/delete", HTTP_POST, handleDeleteFile);
  server.on("/filter", HTTP_GET, handleSetFilter);
  server.on("/flash", HTTP_GET, handleFlash);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/recover", HTTP_GET, handleCameraRecover);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.onNotFound([]() { server.send(404, "text/plain; charset=utf-8", "DigiCam route not found. Try / or /status"); });
  server.begin();
  lastAction = "Web UI ready";
}

void setup() {
#if !DIGICAM_I2C_USES_UART
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("===================================="));
  Serial.println(F(" DigiCam - Full OTA Camera"));
  Serial.println(F("===================================="));
  Serial.printf("[SYS] Chip=%s, CPU=%u MHz, flash=%u bytes, PSRAM=%s\n",
                ESP.getChipModel(), ESP.getCpuFreqMHz(), ESP.getFlashChipSize(),
                psramFound() ? "YES" : "NO");
#endif

  pinMode(DIGICAM_FLASH_GPIO, OUTPUT);
  digitalWrite(DIGICAM_FLASH_GPIO, LOW);

  nativePreviewFallback = false;
  selectedCameraFrameSize = DIGICAM_STILL_FRAME_SIZE;
  activeCameraFrameSize = selectedCameraFrameSize;
  cameraReady = initCamera(selectedCameraFrameSize);
  String bootProbeError;
  if (cameraReady && !probeCameraFrame(frameWidth(selectedCameraFrameSize), frameHeight(selectedCameraFrameSize), &bootProbeError)) {
    (void)esp_camera_deinit();
    cameraReady = false;
    lastAction = bootProbeError;
  }
  if (cameraReady) {
    activeCameraFrameSize = selectedCameraFrameSize;
    lastAction = "Camera initialized and a complete frame was verified";
  } else {
    // Recovery baseline: verify VGA too, rather than treating init success as camera-ready.
    nativePreviewFallback = true;
    activeCameraFrameSize = FRAMESIZE_VGA;
    cameraReady = initCamera(FRAMESIZE_VGA);
    if (cameraReady && !probeCameraFrame(frameWidth(FRAMESIZE_VGA), frameHeight(FRAMESIZE_VGA), &bootProbeError)) {
      (void)esp_camera_deinit();
      cameraReady = false;
    }
    if (cameraReady) {
      selectedCameraFrameSize = FRAMESIZE_VGA;
      lastAction = "Using verified VGA fallback for preview and photos";
    } else {
      lastAction = String("Camera initialization/probe failed: ") + bootProbeError;
    }
  }
#if !DIGICAM_I2C_USES_UART
  Serial.printf("[CAM] %s; active=%ux%u, web preview=160x120, still=%ux%u, shared-mode=%s\n", cameraReady ? "ready" : "FAILED", frameWidth(activeCameraFrameSize), frameHeight(activeCameraFrameSize), frameWidth(selectedCameraFrameSize), frameHeight(selectedCameraFrameSize), activeCameraFrameSize == selectedCameraFrameSize ? "yes" : "no");
#endif
  (void)setExposureCompensation(0);
  initSdCard();
#if !DIGICAM_I2C_USES_UART
  Serial.printf("[SD] %s (1-bit SDMMC)\n", sdReady ? "ready" : "not detected");
#endif
  initPeripherals();
#if DIGICAM_I2C_ENABLED
  showBootAnimation();
#endif

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  const bool apStarted = WiFi.softAP(DIGICAM_AP_SSID, DIGICAM_AP_PASSWORD, 6, false, 4);
#if !DIGICAM_I2C_USES_UART
  Serial.printf("[WIFI] AP %s: %s; IP=%s\n", apStarted ? "ready" : "FAILED", DIGICAM_AP_SSID,
                WiFi.softAPIP().toString().c_str());
#endif

  initOta();
  initRoutes();
#if !DIGICAM_I2C_USES_UART
  Serial.println(F("[OTA] Ready on port 3232"));
  Serial.println(F("[WEB] http://192.168.4.1/"));
  Serial.println(F("[SYS] Setup complete"));
#else
  lastAction = String("Display profile AP ") + (apStarted ? "ready" : "failed") +
               String("; OLED ") + (oledReady ? "ready" : "not detected") +
               String("; I2C GPIO ") + String(DIGICAM_I2C_SDA_GPIO) + "/" + String(DIGICAM_I2C_SCL_GPIO);
#endif
}

void loop() {
  if (wifiEnabled) {
    ArduinoOTA.handle();
    server.handleClient();
  }
  handleJoystick();
  servicePhysicalCapture();
  updateOled();
  delay(2);
}
