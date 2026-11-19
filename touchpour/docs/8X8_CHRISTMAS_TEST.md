# TouchPour 8×8 one-off Christmas test

Firmware identity: **0.2.0-grid-test.1**. This is an experimental TouchPour-only branch, not a normal release or OTA-channel update. No other scale/display firmware is changed. Do not merge into main or publish to the production/dev OTA feeds as part of this experiment. No device has been flashed or remotely updated.

## Hardware

Use the existing Waveshare ESP32-S3 Touch LCD 2.8 TouchPour display. Replace its external VL53L0X with a **VL53L7CX breakout** on **SDA GPIO11 / SCL GPIO10**, common ground. Servo remains **GPIO18**; optional physical button remains **GPIO15 to ground**. The external I²C bus now runs at 400 kHz. Do not attach both sensors at address 0x29. Touchscreen touch remains on its separate bus.

Breakout power, LPn/enable and reset wiring depend on the purchased module. Use its documented supply and 3.3 V I²C levels, with LPn/enable asserted as its manufacturer requires. This build polls data-ready; INT is not required. The bare sensor's pin voltages are not interchangeable with breakout VIN. Confirm your actual board before connecting power.

Mount the sensor looking down at the cup opening. The whole container outline must fit inside the 8×8 view, with several valid interior zones. Clear/thin rims, dark material, hands, foam and the beer stream may prevent recognition. The classifier is an experimental translated depth/outline template matcher, not general object recognition or a volume estimator. Different containers with similar outlines intentionally remain ambiguous.

## Flash the test

The GitHub Actions test artifact includes the application, bootloader, partition table, OTA metadata, flash arguments, full-flash image, and this guide. Test artifacts are retained for 90 days, covering Christmas 2026; download a local copy before then.

For an existing TouchPour device with the current partition table, use its website → Setup → Updates and upload **touchpour_display.bin**. Verify that the installed version reads **0.2.0-grid-test.1**. This is a manual upload only; the regular OTA feed is unchanged. Keep the normal firmware binary available to return to it.

For USB on an ESP-IDF/esptool terminal, replace COM18 with the display's actual port:

```powershell
python -m esptool --chip esp32s3 --port COM18 --baud 460800 write_flash 0x0 touchpour-grid-test-full.bin
```

Do not erase flash. Existing Wi-Fi, servo calibration, original single-zone fill settings, PIN and pour history remain in their original keys/partitions; grid training uses a separate versioned NVS blob. The full-flash file installs the same TouchPour partition layout. An application-only update is preferable when already installed.

Start with the tap linkage disconnected; verify closed/open commands before reconnecting. Test manual release/STOP with water before beer. No physical hardware validation has been performed during development.

## Train the three profiles

1. Complete normal servo calibration. Open the device website → Setup → **8×8 training**. The live view displays all 64 distances and invalid zones.
2. With the tray completely empty and the sensor fixed in its final position, select **Capture empty tray / reset training**. This intentionally clears all three profiles and disables automatic grid pours.
3. Select **Small cup**. Place the empty cup under the sensor and select **Add empty-container position**. Repeat in several representative positions/heights/angles, up to eight examples. Captures require the outline away from the outer grid border and at least three interior zones. A capture is a single fresh frame; keep the cup steady when pressing it.
4. Manually fill that cup to the desired full level. Return it to a trained pose, wait for a fresh grid, then select **Capture full level**. This stores the interior-to-boundary distance gap.
5. Put a cup filled to the desired half *volume* in a trained pose and select **Capture half level**. Half volume is separately measured, never inferred from half the height. Recapturing full clears half; capture full first.
6. Repeat for **Large cup** and **Pitcher**. Capture multiple representative empty poses for each. Do not train a hand or the beer stream as part of a vessel.
7. Select **Enable trained 8×8 pours**. Training and enable state survive reboot. Moving the sensor requires a new empty-tray reference and retraining. Clearing a profile leaves it unavailable until retrained.

The device searches translations up to three grid cells and learned height variation (up to 80 mm against a captured boundary); these are software limits, not guaranteed usable tolerances. Edge-clipped and ambiguous matches cannot authorize pours. Five consecutive recognized frames plus normal distance stability are required. Short-tap FULL and 1/2 POUR use the recognized profile's captured targets. A fresh explicit tap is still required; simply placing a cup does not start pouring.

The selected identity and cutoff are latched at pour start. Losing recognition, changing identity, invalid/stale interior measurements, cup removal, raw cutoff, maximum pour time or STOP closes the tap. Targets do not follow vertical hand motion during a pour: keep the vessel steady while filling. There are no automatic repours. A low reading in a trained interior zone can close early; that is preferable to filtering away a fill cutoff. Calibration targets use the interior median while pouring uses the nearest interior return.

## Hold the screen for manual pour

On the unlocked home screen, hold the **main FULL POUR area** for two seconds. The tap opens and pours only while that same contact remains down. Release, slide off the button, touch failure, an expired 250 ms touch-input lease, STOP, screen lock, servo failure or maximum pour time closes it. A new press while a normal pour is active still stops it.

Manual hold ignores **all LiDAR measurements**, classification and distance calibration; servo calibration remains required. It works with an unknown container or disconnected sensor. The button remains touchable without a ready cup for this reason; an ordinary short tap still requires readiness. The lower 1/2 POUR area retains its short-tap behavior. Physical GPIO15 two-second hold behavior is retained. No browser hold-to-pour endpoint is added.

## Christmas acceptance checks

- Verify closed/open travel and screen release/slide-off/STOP before automatic tests.
- Verify each trained size in several shifted positions, and check that similar/unknown containers remain unknown rather than confidently mislabeled.
- Verify that a tray, hand, clipped cup and missing sensor do not authorize an automatic pour.
- Verify full and half targets independently for all three sizes using water, then test actual glass, beer and foam.
- During a pour, remove the cup, block the sensor, disconnect the sensor, and confirm the tap closes. Never disconnect servo power as a closure test: power is needed to move it.
- Reboot and verify all training survives. Confirm the original single-zone settings remain available when reverting to normal firmware.
- Download the last-five pour CSVs and capture `/api/status` JSON for grid diagnostics. Existing pour CSVs retain the selected distance/cutoff and original format; full 64-zone frames are live API data, not persistent CSV recordings.

Software tests cannot establish optical performance or mechanical closure. If the sensor cannot resolve a usable boundary/interior for a vessel, retain manual pouring and use its live grid to adjust mounting/training before trying automatic pouring.
