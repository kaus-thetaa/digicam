# DigiCam V12 UI guide

## Visual language

- Use the U8g2 `helv` family (Helvetica-like, compact sans-serif) for titles and labels.
- Use thin horizontal rules, generous spacing and monochrome inverse highlight for selection.
- Keep animations limited to a short boot loader, button/state feedback, timer countdown and OTA progress. Do not animate by repeatedly redrawing large bitmaps while a photo is being processed.
- No color icons are assumed; the shutter mark is line art, and text remains readable at 128×64.

## Screen map

```text
HOME
├── Capture
│   ├── Capture mode
│   ├── Fill LED
│   ├── Timer LED blink
│   ├── Take photo now
│   └── Back
├── Image
│   ├── Resolution (VGA / QVGA)
│   ├── Rotation (0 / 90 CW / 180 / 90 CCW)
│   ├── Exposure compensation (only if driver exposes it)
│   ├── Picture style
│   ├── Film grain
│   ├── Date stamp (opens time setup if clock is unset)
│   └── Back
├── Display
│   ├── OLED live preview
│   └── Back
├── Connectivity
│   ├── Set date & time
│   ├── Transfer photos
│   ├── Update firmware
│   └── Back
├── Settings
│   ├── Orientation test
│   ├── Reset settings
│   └── Back
├── Orientation test
└── Close menu
```

## Button contract

| Gesture | Home | Menu | Orientation setup | Network mode | Timer |
|---|---|---|---|---|---|
| Tap | Capture / begin chosen timer | Next item | Next orientation | No action | No action |
| Hold | Open menu | Activate/change | Confirm orientation | Exit network mode | Cancel |
| Double press | Open menu | Back one level | Previous orientation | Exit network mode | Cancel |

A single press is delayed briefly by `ButtonInput` so it can distinguish a double press. Keep this delay intentional; do not change it without re-testing all states.

## Home status

- Large centre heading: ready state or the most recent concise action/result.
- Second line: selected capture mode, resolution and rotation.
- Bottom status: SD availability, whether time has been initialized this power session, and timestamp/EV state.
- A fresh power-on never inherits clock validity from the SD configuration file.

## Date stamp interaction

- On a fresh power session, the clock is invalid until synced from the phone/laptop browser.
- If Date stamp is OFF and the user selects it while time is unset, V12 opens the `DigiCam-Time` access point and marks a pending enable.
- A successful browser sync enables and saves the stamp preference to SD. Exiting without syncing cancels that pending enable.
- A stamp that was already enabled and persisted stays a preference across reboot, but no overlay is burned until the clock is synchronized for that session.
