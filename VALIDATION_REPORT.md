# DigiCam V12 validation report

## Checks completed in this packaging environment

- [x] Inspected the complete source tree and project documentation.
- [x] `python3 tools/validate_project.py`: 11/11 project-level static checks passed.
- [x] `python3 tools/test_rotation_math.py`: all four output-to-source rotation mappings passed, including swapped output dimensions at 90° and 270°.
- [x] `python3 tools/check_partitions.py`: no partition overlap; OTA app slots are each 1,966,080 bytes and fit within the declared 4 MB flash layout.
- [x] All project `.cpp` translation units passed `g++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -fsyntax-only` against temporary local mock headers. These stubs are test scaffolding only and are not included in the project archive. This verifies C++ parsing and type use against the stubs, **not** compatibility with the real Arduino/ESP32/U8g2 libraries or final linking.
- [x] User settings use the SD card only (`/DIGICAM.CFG`, `/DIGICAM.TMP`, recovery files), with CRC validation; no `Preferences`/NVS user settings code remains.
- [x] Time validity and timezone offset are RAM-only. No stored timestamp is trusted after reboot.
- [x] The Date stamp menu launches time setup when needed; successful sync enables the pending stamp preference, while leaving without syncing cancels a new pending enable.
- [x] Timestamp glyphs are burned into the transformed/rotated YUV image before JPEG encoding.
- [x] The UI uses U8g2 Helvetica-like fonts, pixel-width-based row fitting, inverse-highlight menus, a short low-cost boot animation, timer feedback and OTA progress.
- [x] Network modes are separate; app deinitializes camera and turns off flash before starting an access point.
- [x] MPU6050 logic is absent from source/runtime code.

## Compile feedback fix

- Fixed the PlatformIO compiler error in `storage_manager.cpp`: this framework version declares `fmt2jpg_cb()` source as `uint8_t*`, so `StorageManager::saveJpegFromYuv()` now accepts the mutable YUV output buffer produced by `ImageProcessor` rather than `const uint8_t*`. The app passes that generated mutable buffer; the camera driver's raw frame remains read-only and is not passed to the JPEG encoder.
- The user's report showed `image_processor.cpp.o` also failed, but did not include that file's actual diagnostic text. This patch does not claim that second failure is resolved. Rebuild and inspect the first `image_processor.cpp` error if it persists.

## Not completed; required before calling the firmware release-ready

- [ ] Actual `pio run -e esp32cam_v12` build against PlatformIO's pinned ESP32 Arduino toolchain. PlatformIO is not installed in this environment and package-index access is unavailable here.
- [ ] Actual `.pio/build/esp32cam_v12/firmware.bin` size confirmation against the 1,966,080-byte OTA slot.
- [ ] Link-time API compatibility with the selected real ESP32 Arduino core, `esp32-camera` and U8g2 packages.
- [ ] Physical SH1106 font spacing, text clipping and animation readability at 128×64.
- [ ] GC2145 color/chroma quality in 90°/270° rotations, stamped image legibility and exposure behavior.
- [ ] SD config corruption/power-loss recovery on the actual card.
- [ ] Browser time sync, photo downloads, repeated transfer exit/restart and OTA success/failure tested on board.
- [ ] Bootloader rollback support and firmware recovery verified on the actual board.

The project is an integrated **build candidate**, not a hardware-certified release. Run the real PlatformIO build first, resolve any toolchain-specific compiler errors, then complete `docs/ACCEPTANCE_TESTS.md`. Keep USB/UART recovery available until OTA and rollback behavior are proven on your device.


## Build-log follow-up fix

The reported GCC 8.4 error in `src/image_processor.cpp` was addressed by replacing `ImageDimensions{width, height}` aggregate construction with explicit `dims.width` / `dims.height` assignment. This avoids the unsupported construction pattern in the Xtensa GCC toolchain. A regression check ensures the incompatible construction does not return. This environment does not have the user's installed PlatformIO build environment, so a full target build must be rerun locally.
