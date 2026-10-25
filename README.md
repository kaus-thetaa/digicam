# DigiCam R7 — Standalone + Web Camera

R7 is based on the R6 Photo Integrity firmware source. The existing dark web interface, preview, still capture, resolution selector, filters, flash, SD endpoints, diagnostics and OTA route are retained. The added local mode is deliberately dormant while Wi-Fi is on, except for a small OLED status page and physical shutter handling.

## Important limits / verification

- This package has **not** been compiled with PlatformIO or tested on the physical camera in this environment. Run the build command below before OTA flashing.
- R7 implements MPU6050 detection and accelerometer tilt readings. It does **not yet rotate the saved JPEG pixels automatically**; first confirm the physical mounting axes and validate sensor readings. The current screen can display a tiny monochrome thumbnail in standalone mode, not a full-color video feed.
- The full-res camera path remains YUV422 -> software JPEG. `Max safe res` tries modes from largest to smallest using a PSRAM budget and actual-frame probe; it is not a guarantee that every image at that size is artifact-free. QVGA is still the known conservative baseline. Review original JPEGs before trusting high modes.
- Wi-Fi is disabled at the radio/driver level when standalone mode is entered. This necessarily disconnects the web UI and OTA until Wi-Fi is turned back on locally.

## Build and flash

From the project root:

```bash
pio run -e esp32cam-ota-r7
```

Only if the build succeeds, connect to `DigiCam-AP` and upload:

```bash
pio run -e esp32cam-ota-r7 -t upload
```

Credentials remain the same as R6: AP `DigiCam-AP`, AP password `password123`, OTA port `3232`, OTA password `DigiCamOTA2026`, address `192.168.4.1`.

The `esp32cam-ota-r7` profile enables SH1106 + ADS1115 + PCF8574 on GPIO3/GPIO1. **Disconnect USB-serial TX/RX wires while using these GPIOs as I²C.** USB power can remain connected if needed, but UART TX/RX must not drive the I²C bus. Keep a known-good serial recovery path available before flashing.

## Hardware bus and controls

All modules share the same I²C bus:

| Signal | ESP32-CAM |
|---|---|
| OLED SH1106 SDA | GPIO3 |
| OLED SH1106 SCL | GPIO1 |
| MPU6050 SDA | GPIO3 (shared) |
| MPU6050 SCL | GPIO1 (shared) |
| All grounds | GND |
| MPU6050 VCC | 3V3 |

MPU6050 normally responds at `0x68`; AD0 high selects `0x69`. R7 probes both. Leave MPU INT/XDA/XCL disconnected. The OLED should be SH1106 128x64 at `0x3C`.

The current joystick implementation expects the existing ADS1115 (`0x48`) for X/Y axes and PCF8574 (`0x20`) for the active-low click on bit 4. If your physical joystick is wired directly to ESP32 ADC/GPIO pins instead of these expander boards, this pinout/code needs to be adapted before controls will work. Do not assume the click works unless `/status` reports `joystickReady:true`.

## Controls

- **Short click:** shutter. Captures the selected full-resolution still and saves it to `/DCIM` when SD is present. Without SD, the capture is reported but not retained after its buffer is freed in local mode.
- **Hold click ~0.8 seconds while Wi-Fi is ON:** turn Wi-Fi off and enter standalone mode.
- **Standalone mode:** joystick up/down moves through the local menu. Hold click to apply the selected item. Menu items: Wi-Fi radio, Resolution, Picture style, Photo mode, Flash, Exposure EV, Max safe res.
- **Wi-Fi radio item:** in standalone mode, choose it and hold click to turn Wi-Fi back on; web + OTA return at `http://192.168.4.1/`.
- **Joystick left/right in standalone mode:** adjusts exposure compensation.

While Wi-Fi is ON, the full web interface remains the primary UI and the OLED shows only small status text. The OLED thumbnail and local menu render only after Wi-Fi is off, to avoid running two full interfaces concurrently.

## MPU6050 wiring

Power the GY-521/MPU6050 breakout from 3.3 V for this initial setup, share GND, and connect SDA/SCL to the same I²C lines as the OLED. The firmware treats a missing MPU as optional and continues booting. Tilt is estimated from accelerometer gravity; it is not absolute compass heading. Saved-photo rotation is not included in R7 until mounting orientation is confirmed.

## SD and power

SD_MMC stays in 1-bit mode. Use stable regulated 5 V at the ESP32-CAM; do not power it from another board's 3.3 V pin. If the earlier chirping coincides with flash/Wi-Fi load or boot loops, stop and test a regulated supply and inspect for brownout logs rather than disabling brownout detection.

## Rollback

Keep the current working firmware and the original R6 files. If the R7 build fails, do not upload it. If OTA fails after Wi-Fi disconnects, reboot the camera: Wi-Fi defaults ON at boot, so the AP should return.


## R7.1 display detection fix — GME12864-78

The GME12864-78 1.3-inch 128x64 I2C model is listed as an SH1106-compatible display. R7.1 keeps the U8g2 SH1106 driver, probes both common I2C addresses (`0x3C` and `0x3D`), shows a display-test screen immediately after initialization, and reports `oledI2cAddress` in `/status`.

**Important:** `default_envs` intentionally remains `esp32cam-ota-safe`, which compiles OLED support out. You must explicitly choose a display profile.

If your wires are SDA=GPIO3 and SCL=GPIO1:

```bash
pio run -e esp32cam-ota-r7-oled-3-1
pio run -e esp32cam-ota-r7-oled-3-1 -t upload
```

Disconnect USB-serial TX/RX wires from GPIO3/GPIO1 when using them for I2C. This does not disable Wi-Fi OTA.

If your existing wires are SDA=GPIO13 and SCL=GPIO12:

```bash
pio run -e esp32cam-ota-r7-oled-13-12
pio run -e esp32cam-ota-r7-oled-13-12 -t upload
```

GPIO12 is an ESP32 boot-strapping pin and the OLED's I2C pull-up may affect boot configuration on some modules. Prefer GPIO3/GPIO1 if the camera is unreliable with GPIO12 connected. GPIO12/13 must not be used for I2C with 4-bit SD_MMC.
