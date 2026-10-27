#pragma once
#include <Arduino.h>
namespace dc {
enum class Rotation : uint8_t { Deg0=0, Deg90=1, Deg180=2, Deg270=3 };
enum class Resolution : uint8_t { VGA=0, QVGA=1 };
enum class Style : uint8_t { Normal=0, Vintage=1, Warm=2, Noir=3, Cinematic=4 };
enum class CaptureMode : uint8_t { Auto=0, ExposureLock=1, Timer3=2, Timer5=3, Timer10=4, LowLight=5, Ghost=6 };
enum class NetworkMode : uint8_t { Off=0, PhotoTransfer=1, FirmwareUpdate=2, TimeSetup=3 };
enum class AppState : uint8_t { Booting=0, Ready=1, Menu=2, Preview=3, Capturing=4, Network=5, Error=6 };
struct Settings {
  uint8_t rotation = static_cast<uint8_t>(Rotation::Deg270);
  uint8_t resolution = static_cast<uint8_t>(Resolution::VGA);
  int8_t ev = 0;                       // desired EV compensation, -2..+2
  uint8_t style = static_cast<uint8_t>(Style::Normal);
  uint8_t mode = static_cast<uint8_t>(CaptureMode::Auto);
  bool flashEnabled = false;
  bool grainEnabled = false;
  bool oledPreview = false;
  bool timerBlink = true;
  bool orientationConfirmed = false;
  bool timestampEnabled = false;
};
struct ImageDimensions { uint16_t width=0; uint16_t height=0; };
}
