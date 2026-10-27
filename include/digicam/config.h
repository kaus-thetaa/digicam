#pragma once
#include <Arduino.h>

namespace dc::cfg {
constexpr char BUILD_ID[] = "DigiCam V12.0.0";
constexpr uint32_t XCLK_HZ = 20000000;
constexpr int OLED_SDA = 3;          // Existing known-working wiring: SDA -> GPIO3/U0R
constexpr int OLED_SCL = 1;          // Existing known-working wiring: SCL -> GPIO1/U0T
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr bool OLED_SH1106 = true;
constexpr int BUTTON_GPIO = 13;      // Button from GPIO13 to GND, internal pull-up
constexpr int FLASH_GPIO = 4;        // Active HIGH onboard white LED
constexpr int CAMERA_PWDN = 32;
constexpr int CAMERA_RESET = -1;
constexpr int CAMERA_XCLK = 0;
constexpr int CAMERA_SIOD = 26;
constexpr int CAMERA_SIOC = 27;
constexpr int CAMERA_Y9 = 35;
constexpr int CAMERA_Y8 = 34;
constexpr int CAMERA_Y7 = 39;
constexpr int CAMERA_Y6 = 36;
constexpr int CAMERA_Y5 = 21;
constexpr int CAMERA_Y4 = 19;
constexpr int CAMERA_Y3 = 18;
constexpr int CAMERA_Y2 = 5;
constexpr int CAMERA_VSYNC = 25;
constexpr int CAMERA_HREF = 23;
constexpr int CAMERA_PCLK = 22;
constexpr char TRANSFER_SSID[] = "DigiCam-Transfer";
constexpr char UPDATE_SSID[] = "DigiCam-V12-Update";
constexpr char TIME_SSID[] = "DigiCam-Time";
// Change these before sharing or distributing the firmware.
constexpr char AP_PASSWORD[] = "DigiCam1234"; // WPA2: at least 8 characters
constexpr char WEB_USER[] = "admin";
constexpr char WEB_PASSWORD[] = "DigiCamLocalV12";
constexpr uint32_t BUTTON_DEBOUNCE_MS = 28;
constexpr uint32_t DOUBLE_CLICK_MS = 330;
constexpr uint32_t LONG_PRESS_MS = 850;
constexpr uint8_t MAX_PHOTOS_PER_LIST = 150;
}
