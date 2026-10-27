#include "digicam/button_input.h"
#include "digicam/config.h"
namespace dc {
void ButtonInput::begin(int pin){pin_=pin;pinMode(pin_,INPUT_PULLUP);down_=digitalRead(pin_)==LOW;longSent_=false;pendingClick_=false;lastTransition_=millis();}
ButtonEvent ButtonInput::poll(uint32_t now){
 if(pin_<0)return ButtonEvent::None;
 bool raw=(digitalRead(pin_)==LOW);
 if(raw!=down_ && now-lastTransition_>=cfg::BUTTON_DEBOUNCE_MS){
  lastTransition_=now;down_=raw;
  if(down_){pressedAt_=now;longSent_=false;}
  else if(!longSent_){if(pendingClick_&&now-pendingAt_<=cfg::DOUBLE_CLICK_MS){pendingClick_=false;return ButtonEvent::Double;}pendingClick_=true;pendingAt_=now;}
 }
 if(down_&&!longSent_&&now-pressedAt_>=cfg::LONG_PRESS_MS){longSent_=true;pendingClick_=false;return ButtonEvent::Long;}
 if(pendingClick_&&!down_&&now-pendingAt_>cfg::DOUBLE_CLICK_MS){pendingClick_=false;return ButtonEvent::Single;}
 return ButtonEvent::None;
}
}
