# DigiCam V12 architecture

## Module boundaries

- `DigiCamApp` — explicit UI/menu state, capture workflow, first-run orientation wizard, timer and clock-validity policy.
- `CameraController` — GC2145 camera lifecycle, YUV422 VGA/QVGA initialization fallback, frame acquire/release and sensor-control callbacks.
- `ImageProcessor` — non-destructive coordinate transform, styles, grain, Ghost blending, shared orientation mapping and optional stamp burn-in.
- `StorageManager` — 1-bit SD_MMC initialization, sequential photo naming, temporary JPEG files, JPEG markers and final rename.
- `SettingsStore` — versioned CRC-protected SD-only config with temp write, backup recovery and no user settings in NVS.
- `DisplayUI` — SH1106 drawing, Helvetica-like typography, small transitions, home/status screens, menus, preview and OTA progress.
- `ButtonInput` — debounced single/double/long press events.
- `NetworkManager` — exclusive local AP modes for time setup, photo transfer and firmware update. Does not access camera APIs.

## Startup and persistence flow

```text
Power on
  ├─ Force LED OFF; force Wi-Fi OFF
  ├─ Initialize SH1106 and button
  ├─ Mount microSD in 1-bit mode
  ├─ Load /DIGICAM.CFG (CRC/version checks, recover /DIGICAM.BAK)
  ├─ If no valid config: use defaults (initial rotation candidate = 90° CCW)
  ├─ Initialize camera at VGA, retry QVGA if frame validation fails
  ├─ Apply saved camera settings where supported
  └─ Run first-start orientation confirmation wizard if needed
```

`SettingsStore` is initialized only after `StorageManager` reports the SD card ready. If the card is unavailable, settings remain in RAM with safe defaults and are not silently written to NVS.

## Capture pipeline

```text
Button / timer expiry
  ├─ Request AE/AWB where supported
  ├─ Optional fill-light settle or exposure-lock attempt
  ├─ Acquire immutable YUV422 camera frame
  ├─ Allocate processed output buffer
  ├─ Remap output coordinates for 0/90CW/180/90CCW
  ├─ Apply style / optional grain / optional valid timestamp
  ├─ fmt2jpg_cb streams JPEG to temporary SD file
  ├─ Flush, validate size + JPEG SOI/EOI
  ├─ Rename temp to final .jpg only on success
  └─ Free working buffer and return sensor frame
```

The rotation functions map output coordinates to source coordinates. For the selected orientation, the OLED preview applies the same map so the framing composition matches the saved image. The display itself stays physically unrotated.

## Clock policy

`clockValid_` and `utcOffsetMinutes_` live only in RAM. At boot, the clock is considered unknown. In `TimeSetup` mode, the browser sends Unix epoch seconds and its local UTC offset to `/time-sync`; the app applies the epoch with `settimeofday` and holds the offset for local timestamp formatting. Clock validity is lost on reboot by policy, even if the system clock happens to retain an approximation. No stale clock is loaded from the SD config file.

The optional timestamp is applied after image rotation, so the glyphs remain upright. When time is unset and stamping is OFF, choosing Date stamp opens the local time-setup AP and marks a pending opt-in. Only a successful time sync enables and saves that setting. If stamping was already saved ON from a previous session, capture still succeeds without a fabricated timestamp until time is synchronized again.

## Exclusive network modes

Before any AP starts, the app forces GPIO4 LOW and calls `CameraController::end()` to deinitialize the camera and assert sensor PWDN. `NetworkManager` then exposes one of:

- `TimeSetup`: authenticated web page sends the host device's current time and timezone; it does not access SD photo files or firmware update APIs.
- `PhotoTransfer`: read-only download of existing `/DCIM/IMG_*.jpg` files; camera and image pipeline are inactive.
- `FirmwareUpdate`: uploads a firmware binary to the inactive application slot; camera is inactive.

Network mode exits by button hold/double press; Wi-Fi shuts down before the camera is restarted and saved settings are applied.

## Memory and failure handling

- One camera frame buffer is used; JPEG is streamed in chunks rather than accumulated into a full output-sized heap buffer.
- Rotation/styles work in a separate output buffer so the sensor framebuffer is never corrupted.
- Ghost allocates one previous-frame PSRAM copy and may fall back to a normal shot if allocation fails.
- SD JPEGs are written to `.tmp` and only renamed after conversion and marker checks.
- Settings writes are CRC checked, read back and promoted from a temp file, retaining a backup during promotion.
- Missing SD blocks normal photo saves but should not prevent the camera from booting; config persistence is unavailable until a writable card is mounted.

## OTA caveat

The partition table uses `otadata` and two app slots, each 0x1E0000 bytes. The firmware must fit in the inactive app slot. The app confirms a pending image only after required boot modules are available, but automatic rollback requires bootloader rollback support and must be tested on the toolchain/board. The current environment could not build or test this bootloader configuration.
