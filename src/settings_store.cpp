#include "digicam/settings_store.h"
#include <SD_MMC.h>
#include <string.h>

namespace dc {
namespace {
constexpr char kConfigPath[] = "/DIGICAM.CFG";
constexpr char kTempPath[]   = "/DIGICAM.TMP";
constexpr char kBackupPath[] = "/DIGICAM.BAK";
constexpr char kBadPath[] = "/DIGICAM.BAD";
constexpr uint16_t kVersion = 1;

struct __attribute__((packed)) SettingsPayload {
  uint8_t rotation;
  uint8_t resolution;
  int8_t ev;
  uint8_t style;
  uint8_t mode;
  uint8_t flags;
};
struct __attribute__((packed)) SettingsRecord {
  char magic[4];
  uint16_t version;
  uint16_t payloadSize;
  SettingsPayload payload;
  uint32_t crc32;
};

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
  }
  return ~crc;
}

SettingsRecord encode(const Settings& s) {
  SettingsRecord r{};
  memcpy(r.magic, "DCFG", 4);
  r.version = kVersion;
  r.payloadSize = sizeof(SettingsPayload);
  r.payload.rotation = s.rotation;
  r.payload.resolution = s.resolution;
  r.payload.ev = s.ev;
  r.payload.style = s.style;
  r.payload.mode = s.mode;
  r.payload.flags = (s.flashEnabled ? 1U : 0U) |
                    (s.grainEnabled ? 2U : 0U) |
                    (s.oledPreview ? 4U : 0U) |
                    (s.timerBlink ? 8U : 0U) |
                    (s.orientationConfirmed ? 16U : 0U) |
                    (s.timestampEnabled ? 32U : 0U);
  r.crc32 = crc32(reinterpret_cast<const uint8_t*>(&r.payload), sizeof(r.payload));
  return r;
}

bool validPayload(const SettingsPayload& p) {
  return p.rotation <= 3 && p.resolution <= 1 && p.ev >= -2 && p.ev <= 2 &&
         p.style <= 4 && p.mode <= 6 && (p.flags & 0xC0U) == 0;
}
void decode(const SettingsPayload& p, Settings& s) {
  s.rotation = p.rotation;
  s.resolution = p.resolution;
  s.ev = p.ev;
  s.style = p.style;
  s.mode = p.mode;
  s.flashEnabled = (p.flags & 1U) != 0;
  s.grainEnabled = (p.flags & 2U) != 0;
  s.oledPreview = (p.flags & 4U) != 0;
  s.timerBlink = (p.flags & 8U) != 0;
  s.orientationConfirmed = (p.flags & 16U) != 0;
  s.timestampEnabled = (p.flags & 32U) != 0;
}
}  // namespace

bool SettingsStore::begin(bool sdAvailable) {
  ready_ = sdAvailable && SD_MMC.cardType() != CARD_NONE;
  return ready_;
}

bool SettingsStore::readFile(const char* path, Settings& s) {
  if (!ready_ || !SD_MMC.exists(path)) return false;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  SettingsRecord r{};
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
  const bool noTrailingData = f.available() == 0;
  f.close();
  if (got != sizeof(r) || !noTrailingData || memcmp(r.magic, "DCFG", 4) != 0 ||
      r.version != kVersion || r.payloadSize != sizeof(SettingsPayload) ||
      !validPayload(r.payload) ||
      r.crc32 != crc32(reinterpret_cast<const uint8_t*>(&r.payload), sizeof(r.payload))) {
    return false;
  }
  decode(r.payload, s);
  return true;
}

bool SettingsStore::load(Settings& s) {
  if (!ready_) return false;
  if (readFile(kConfigPath, s)) return true;
  // Recover the last known-good version if power was lost during an SD write.
  if (readFile(kBackupPath, s)) return true;
  return false;
}

bool SettingsStore::save(const Settings& s) {
  if (!ready_) return false;
  SettingsRecord record = encode(s);
  if (SD_MMC.exists(kTempPath)) SD_MMC.remove(kTempPath);
  File f = SD_MMC.open(kTempPath, FILE_WRITE);
  if (!f) return false;
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
  f.flush();
  const bool sizeOk = f.size() == sizeof(record);
  f.close();
  if (written != sizeof(record) || !sizeOk) {
    SD_MMC.remove(kTempPath);
    return false;
  }
  Settings verified{};
  if (!readFile(kTempPath, verified)) {
    SD_MMC.remove(kTempPath);
    return false;
  }

  Settings current{};
  const bool hadCurrent = SD_MMC.exists(kConfigPath);
  const bool currentValid = hadCurrent && readFile(kConfigPath, current);
  if (hadCurrent && currentValid) {
    if (SD_MMC.exists(kBackupPath)) SD_MMC.remove(kBackupPath);
    if (!SD_MMC.rename(kConfigPath, kBackupPath)) {
      SD_MMC.remove(kTempPath);
      return false;
    }
  } else if (hadCurrent) {
    // Never overwrite an existing last-known-good backup with a corrupt current file.
    if (SD_MMC.exists(kBadPath)) SD_MMC.remove(kBadPath);
    if (!SD_MMC.rename(kConfigPath, kBadPath)) {
      SD_MMC.remove(kTempPath);
      return false;
    }
  }
  if (!SD_MMC.rename(kTempPath, kConfigPath)) {
    if (currentValid && SD_MMC.exists(kBackupPath)) SD_MMC.rename(kBackupPath, kConfigPath);
    else if (hadCurrent && SD_MMC.exists(kBadPath)) SD_MMC.rename(kBadPath, kConfigPath);
    SD_MMC.remove(kTempPath);
    return false;
  }
  // The promoted config is now verified; stale recovery files can be removed.
  if (SD_MMC.exists(kBackupPath)) SD_MMC.remove(kBackupPath);
  if (SD_MMC.exists(kBadPath)) SD_MMC.remove(kBadPath);
  return true;
}
}  // namespace dc
