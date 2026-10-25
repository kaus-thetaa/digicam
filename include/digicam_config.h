#pragma once
#include <Arduino.h>

#ifndef DIGICAM_I2C_ON_UART_PINS
#define DIGICAM_I2C_ON_UART_PINS 1
#endif
#ifndef DIGICAM_I2C_ENABLED
#define DIGICAM_I2C_ENABLED DIGICAM_I2C_ON_UART_PINS
#endif
#ifndef DIGICAM_I2C_USES_UART
#define DIGICAM_I2C_USES_UART DIGICAM_I2C_ON_UART_PINS
#endif
#ifndef DIGICAM_ENABLE_JOYSTICK
#define DIGICAM_ENABLE_JOYSTICK DIGICAM_I2C_ENABLED
#endif
#ifndef DIGICAM_I2C_SDA_GPIO
#define DIGICAM_I2C_SDA_GPIO 3
#endif
#ifndef DIGICAM_I2C_SCL_GPIO
#define DIGICAM_I2C_SCL_GPIO 1
#endif
#ifndef DIGICAM_BUILD_ID
#define DIGICAM_BUILD_ID "DigiCam-R7.1-display-diagnostic"
#endif

static constexpr char DIGICAM_AP_SSID[] = "DigiCam-AP";
static constexpr char DIGICAM_AP_PASSWORD[] = "password123";
static constexpr char DIGICAM_OTA_HOSTNAME[] = "digicam";
static constexpr char DIGICAM_OTA_PASSWORD[] = "DigiCamOTA2026";
static constexpr uint16_t DIGICAM_OTA_PORT = 3232;

static constexpr framesize_t DIGICAM_FRAME_SIZE = FRAMESIZE_QVGA;
static constexpr framesize_t DIGICAM_STILL_FRAME_SIZE = FRAMESIZE_QVGA;
static constexpr uint8_t DIGICAM_JPEG_QUALITY = 94;
static constexpr uint8_t DIGICAM_VIEWFINDER_JPEG_QUALITY = 28;
static constexpr uint32_t DIGICAM_XCLK_HZ = 20000000;

static constexpr int CAM_PIN_PWDN = 32;
static constexpr int CAM_PIN_RESET = -1;
static constexpr int CAM_PIN_XCLK = 0;
static constexpr int CAM_PIN_SIOD = 26;
static constexpr int CAM_PIN_SIOC = 27;
static constexpr int CAM_PIN_Y9 = 35;
static constexpr int CAM_PIN_Y8 = 34;
static constexpr int CAM_PIN_Y7 = 39;
static constexpr int CAM_PIN_Y6 = 36;
static constexpr int CAM_PIN_Y5 = 21;
static constexpr int CAM_PIN_Y4 = 19;
static constexpr int CAM_PIN_Y3 = 18;
static constexpr int CAM_PIN_Y2 = 5;
static constexpr int CAM_PIN_VSYNC = 25;
static constexpr int CAM_PIN_HREF = 23;
static constexpr int CAM_PIN_PCLK = 22;
static constexpr int DIGICAM_FLASH_GPIO = 4;

static constexpr uint8_t DIGICAM_OLED_ADDR = 0x3C; // Runtime detects 0x3C or 0x3D.
static constexpr uint8_t DIGICAM_ADS1115_ADDR = 0x48;
static constexpr uint8_t DIGICAM_PCF8574_ADDR = 0x20;
static constexpr uint8_t DIGICAM_PCF_CAPTURE_BUTTON_BIT = 4;
static constexpr uint8_t DIGICAM_MPU6050_ADDR = 0x68;
#define DIGICAM_BATTERY_MONITOR_ENABLED 0
static constexpr float DIGICAM_BATTERY_DIVIDER_RATIO = 2.0f;
