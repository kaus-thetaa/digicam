#include <Arduino.h>
#include "digicam/app.h"
static dc::DigiCamApp app;
void setup(){app.begin();}
void loop(){app.loop();}
