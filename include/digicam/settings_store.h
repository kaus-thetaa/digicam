#pragma once
#include <Arduino.h>
#include "types.h"
namespace dc {
class SettingsStore {
 public:
  bool begin(bool sdAvailable);
  bool load(Settings& s);
  bool save(const Settings& s);
  bool available() const { return ready_; }
 private:
  bool readFile(const char* path, Settings& s);
  bool ready_ = false;
};
}
