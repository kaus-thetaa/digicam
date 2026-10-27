#pragma once
#include <Arduino.h>
namespace dc {
enum class ButtonEvent : uint8_t { None=0, Single=1, Double=2, Long=3 };
class ButtonInput {
 public:
  void begin(int pin);
  ButtonEvent poll(uint32_t now=millis());
 private:
  int pin_=-1;
  bool down_=false,longSent_=false,pendingClick_=false;
  uint32_t pressedAt_=0,pendingAt_=0,lastTransition_=0;
};
}
