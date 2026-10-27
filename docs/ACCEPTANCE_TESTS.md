# DigiCam V12 hardware acceptance tests

Run these on the actual AI-Thinker ESP32-CAM + GC2145, SH1106 OLED at 0x3C, single GPIO13 button and microSD in 1-bit mode. Keep the known-working firmware available for recovery.

## A. First boot and UI

- [ ] Flash with OLED SDA/SCL disconnected from GPIO3/GPIO1; reconnect after UART flashing.
- [ ] OLED starts with clean DigiCam V12 splash animation; all text remains inside 128×64 pixels.
- [ ] Startup forces Wi-Fi OFF and GPIO4 LED OFF.
- [ ] With a fresh/empty SD card, menu defaults are used and initial rotation wizard starts at 90° anticlockwise as the candidate orientation.
- [ ] The orientation sample is recognizably rotated with 0°, 90° clockwise, 180°, 90° anticlockwise; confirm using a page/card with a marked top edge.
- [ ] Long press confirms orientation; selection survives reboot when SD settings are writable.
- [ ] One-button gestures work: tap, hold, double press. Menu footer and behavior are consistent on all pages.
- [ ] Menus are not clipped and Back always returns to the expected parent.

## B. Camera and rotation

- [ ] Camera initializes at VGA (640×480); if VGA validation is forced to fail, it falls back to QVGA (320×240).
- [ ] Capture 5+ images at each of 0°, 90° CW, 180°, 90° CCW.
- [ ] Verify saved JPEG dimensions: landscape 640×480 or 320×240; portrait 480×640 or 240×320.
- [ ] Verify preview and saved photo show the same orientation.
- [ ] Verify sensor top-edge test confirms the default/orientation wizard choice is correct for the physical mounting.
- [ ] JPEGs open on PC/phone and have correct content, no tears/garbage/blank frames.
- [ ] No stale `.tmp` is presented as a valid photo after forced power loss during a write.
- [ ] Attempt 50 consecutive captures; each successful capture creates exactly one valid final JPEG.

## C. UI choices and capture behavior

- [ ] Mode changes persist after reboot: Auto, available Exposure Lock, Timer 3s/5s/10s, Low Light, Ghost.
- [ ] Exposure EV control is shown only if GC2145 callback is present; verify changes affect brightness before relying on it.
- [ ] Exposure Lock is hidden/skipped or gracefully rejected if sensor AE/gain controls are unsupported.
- [ ] Fill LED remains OFF unless enabled or Low Light is selected; no LED stays on after any failure.
- [ ] Timer blinks briefly according to setting, turns LED OFF before capture, saves exactly one photo at expiry.
- [ ] Long/double press cancels timer; canceled timer never captures.
- [ ] Styles and grain alter the saved image; Standard/Normal capture remains stable.
- [ ] Ghost allocation failure falls back safely to a normal image.
- [ ] OLED preview is optional, monochrome, correctly rotated, and does not prevent button/menu use.

## D. SD-only settings persistence

- [ ] `/DIGICAM.CFG` is created on SD; no user preferences are written through `Preferences`/NVS.
- [ ] Change rotation, resolution, mode, style, timestamp preference, fill LED, grain and preview; reboot and confirm all supported settings persist.
- [ ] Confirm the initial orientation setting stays confirmed after reboot.
- [ ] Inspect `/DIGICAM.CFG` and confirm it is a binary config file; normal photo list should not show it.
- [ ] Corrupt `/DIGICAM.CFG`; if `/DIGICAM.BAK` exists, recover from it; otherwise use defaults without crashing.
- [ ] Remove SD: camera/UI still boot, photo capture reports SD unavailable and settings changes are not falsely reported as saved.
- [ ] Force an interrupted settings write if practical; verify a valid prior config or safe defaults remain usable.

## E. Clock and visible timestamp

- [ ] After every fresh power cycle, home status reports time not set until explicit browser sync.
- [ ] With time unset and Date stamp OFF, selecting Date stamp opens the local time-setup AP; leaving without syncing must leave the preference OFF.
- [ ] Successfully syncing through that flow enables Date stamp and persists the preference to SD.
- [ ] If a previously enabled Date stamp is loaded after reboot, captures save normally without a fabricated date until time is synchronized again.
- [ ] Choose Connectivity → Set date & time; AP `DigiCam-Time` starts with camera deinitialized and LED OFF.
- [ ] Connect phone/laptop, open `http://192.168.4.1`, authenticate, and choose Sync time to camera.
- [ ] OLED reports `TIME SET`; check timestamp against the host device local time and timezone.
- [ ] Capture with Date stamp enabled; visible `YYYY-MM-DD HH:MM` text is burned into the saved image, remains upright under every rotation, and is legible at VGA/QVGA.
- [ ] Exit AP mode; Wi-Fi shuts down and camera restarts. Clock remains valid for the current power session.
- [ ] Reboot and verify stamp cannot be emitted again until another time sync.

## F. Transfer power state

- [ ] Select Photo transfer; flash turns off and camera driver deinitializes before AP becomes available.
- [ ] AP `DigiCam-Transfer`, login, and photo download work.
- [ ] Transfer page lists only final `.jpg` images, never settings files or `.tmp` files.
- [ ] No capture, preview update, JPEG conversion, picture style or grain processing runs while transfer mode is active.
- [ ] Hold/double press exits; AP turns off first, camera restarts and saved resolution/controls are reapplied.
- [ ] Repeat enter/exit at least 10 times and confirm camera continues to capture afterwards.

## G. OTA and recovery

- [ ] `pio run -e esp32cam_v12` succeeds; binary size <= 1,966,080 bytes.
- [ ] Partition report confirms valid non-overlapping offsets and two OTA app slots.
- [ ] First V12 install uses USB/UART; do not migrate R11 partition table via OTA.
- [ ] Open Firmware update AP; camera and LED remain off; upload the firmware built from this project.
- [ ] Valid upload completes, device reboots, camera/SD/OLED work and settings remain on SD.
- [ ] Wrong file, aborted upload, Wi-Fi interruption and image too large do not activate an incomplete update.
- [ ] Test booting-but-faulty image rollback only after confirming bootloader rollback support; do not assume the application-level pending-verify call alone guarantees rollback.
- [ ] Keep USB recovery available until OTA is verified repeatedly.

## Release gate

Do not label the build final until the PlatformIO build, binary size, all rotation orientations, SD-only settings, timestamp behavior, 50-shot capture test, transfer shutdown/resume, and OTA failure cases pass on the real board. Record actual results and toolchain versions in `VALIDATION_REPORT.md`.
