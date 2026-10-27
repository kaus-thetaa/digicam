#!/usr/bin/env python3
"""Fast static acceptance checks for the DigiCam V12 source bundle."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
checks = []
def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))

all_cpp = "\n".join(p.read_text(errors="replace") for p in (ROOT / "src").glob("*.cpp"))
all_hdr = "\n".join(p.read_text(errors="replace") for p in (ROOT / "include").rglob("*.h"))
config = (ROOT / "include/digicam/config.h").read_text()
settings = (ROOT / "src/settings_store.cpp").read_text()
net = (ROOT / "src/network_manager.cpp").read_text()
app = (ROOT / "src/app.cpp").read_text()
image = (ROOT / "src/image_processor.cpp").read_text()
ui = (ROOT / "src/display_ui.cpp").read_text()
platformio = (ROOT / "platformio.ini").read_text()

check("MPU6050 logic removed", "MPU6050" not in all_cpp + all_hdr)
check("No Preferences settings store", "Preferences" not in all_cpp + all_hdr and "prefs_" not in all_cpp + all_hdr)
check("Settings stored on SD", all(x in settings for x in ["/DIGICAM.CFG", "/DIGICAM.TMP", "/DIGICAM.BAK", "crc32"]) and "SD_MMC" in settings)
check("Time setup AP exists", 'TIME_SSID[] = "DigiCam-Time"' in config and "NetworkMode::TimeSetup" in net)
check("Browser time route exists", '"/time-sync"' in net and "getTimezoneOffset" in net and "settimeofday" in app)
check("Timestamp optional, burned into output, and gated by time sync", "timestampEnabled" in all_hdr and "burnTimestamp(out" in image and "stampApplied ? stamp : nullptr" in app and "timestampEnablePending_ = true" in app and "clockValid_" in app)
check("Four rotations and anticlockwise startup candidate", "Deg270" in all_hdr and "90 CCW" in ui and "Deg270);" in (ROOT / "include/digicam/types.h").read_text())
check("Helvetica-like fonts and light UI animation", "u8g2_font_helv" in ui and "drawBox" in ui and "delay(38)" in ui)
check("Network camera is stopped first", "camera_.end()" in app and "CameraController::end" in (ROOT / "src/camera_controller.cpp").read_text())
check("C++17 build mode", "-std=gnu++17" in platformio)
check("ESP32 GCC-compatible image dimension assignment", "ImageDimensions{" not in image and "dims.width=sh" in image and "dims.height=sw" in image)
check("Dual OTA partition table", "otadata" in (ROOT / "partitions.csv").read_text() and "ota_0" in (ROOT / "partitions.csv").read_text() and "ota_1" in (ROOT / "partitions.csv").read_text())

failed = False
for name, ok, detail in checks:
    print(("PASS" if ok else "FAIL") + "  " + name + (" — " + detail if detail else ""))
    failed |= not ok
print(f"\n{sum(ok for _, ok, _ in checks)}/{len(checks)} static checks passed")
sys.exit(1 if failed else 0)
