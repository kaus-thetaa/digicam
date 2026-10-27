# DigiCam V12 wireless firmware update

## Requirements

- V12 must first be installed over USB/UART with the included `partitions.csv` because R11 did not use a dual-slot OTA table.
- `pio run -e esp32cam_v12` must succeed, and `firmware.bin` must fit within one 1,966,080-byte app slot.
- Use regulated 5 V power and do not power off while updating.
- Change the sample AP/web passwords before distributing firmware.

## Update procedure

1. Build the exact DigiCam V12 project.
2. Locate `.pio/build/esp32cam_v12/firmware.bin`.
3. On the camera: Menu → Connectivity → Update firmware.
4. Join `DigiCam-V12-Update` (password `DigiCam1234`).
5. Open `http://192.168.4.1` and authenticate with the configured browser credentials.
6. Choose `firmware.bin` and upload. Keep power connected.
7. Wait for the success page and automatic reboot. Reconnect UART if it fails to boot.

The camera sensor is deinitialized and the flash LED forced off before the AP starts. The OTA handler writes via Arduino-ESP32 `Update` to the inactive application partition and calls `esp_restart()` only after `Update.end()` succeeds and the image is finished.

## Clock sync is a separate mode

`DigiCam-Time` accepts only host date/time synchronization. It does not expose firmware upload routes. `DigiCam-Transfer` exposes read-only photo downloads. The three AP modes are separate so the camera is not running capture/processing during network operations.

## Rollback and recovery

The code checks for `ESP_OTA_IMG_PENDING_VERIFY` and only marks the application valid after the key modules are available. **Automatic rollback after a booting but unhealthy app depends on the ESP-IDF bootloader being compiled with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.** The stock PlatformIO board bootloader configuration has not been verified here; do not claim rollback works until you test it on this board/toolchain.

For a failed upload or failed boot:

1. Power down the camera.
2. Disconnect OLED SDA/SCL from GPIO3/GPIO1.
3. Connect the USB-UART adapter (TX→GPIO3, RX→GPIO1, GND→GND); pull GPIO0 to GND for upload.
4. Flash the known-good project or V12 over USB with the appropriate partition table.
5. Reconnect the OLED wires after flashing.

A failed upload should leave the currently selected application intact; a full erase/reflash is the recovery option if the boot/partition metadata is damaged. Photo files and settings on the removable SD card are separate from application flash, although removing or formatting the SD card removes the settings file.
