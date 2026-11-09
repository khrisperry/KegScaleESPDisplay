# Temporary 4.3-inch connection test

Original **Waveshare ESP32-S3-Touch-LCD-4.3**, 800x480, CH422G + GT911,
16 MB flash / 8 MB octal PSRAM. Not the square 4B or the 4.3C.

This USB-only test target shares the current Dev touchscreen connection,
pairing, Wi-Fi, discovery and reconnect code. The existing 480x480 UI is
centered with 160-pixel side margins, including keyboards and overlays.
Touch coordinates remain in the native 800x480 system; LVGL accounts for
the viewport offset. Brightness is on/off because this board uses CH422G
backlight control. The normal touchscreen OTA feed is disabled in this
image, including manual installs; it is built for different hardware.

## First USB installation on Windows

Use the **UART** USB-C port. Download and extract the `touchscreen43-usb-test`
artifact from the Temporary 4.3-inch connection test Actions run.
From the extracted directory, using your actual COM port:

```powershell
py -m pip install esptool
py -m esptool --chip esp32s3 --port COM13 write-flash 0x0 firmware-usb.bin
```

Press RESET after flashing. If connection fails, hold BOOT, press RESET,
release BOOT, and retry. The merged image contains the bootloader,
partitions, initial OTA metadata and application at their correct offsets.
Do not use the `touchscreen-usb-test` artifact on the 4.3-inch board.

On Setup, join the test Wi-Fi network. Open Add touchscreen on the scale,
select Find scale (or enter its IP), and authorize the pairing code from
the touchscreen on the scale web page. The scale supports one authorized
Wi-Fi touchscreen; use a spare scale for an independent test, or expect
to replace the existing touchscreen pairing. BLE e-paper pairing is separate.

## Build locally (ESP-IDF v6.0.1)

```powershell
cd touchscreen43
idf.py set-target esp32s3
idf.py build
idf.py -p COM13 flash monitor
```

## Connection comparison

Record both firmware versions, AP/network, IP addresses and start time.
Run for the same duration and capture scale support JSON and touchscreen
UART logs if it disconnects. Check automatic recovery after restarting the
AP and after restarting the scale. This changes display hardware and network
at once, so a successful test narrows the cause but does not isolate it.

Physical acceptance still requires verifying touch alignment at all edges,
Setup/keyboard/menu/pairing overlays, and reconnect behavior on the board.
Compilation and host tests cannot validate those hardware behaviors.

Hardware reference:
https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4.3/tree/main/examples/ESP-IDF/09_lvgl_v9_demo
