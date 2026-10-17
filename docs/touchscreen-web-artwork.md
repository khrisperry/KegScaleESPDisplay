# Touchscreen web artwork (V1.4.15)

The Glass and Keg Level home screens use the exact vessel artwork from
KegScaleESP commit `1e9553931d8d9dde991d32ee82f9656ff7dd2b30`.
Can (12 oz), Pint (16 oz), Cup (20 oz), Crowler (32 oz), Growler (64 oz),
and the keg are included. Custom serving sizes retain the existing pint fallback.

Each illustration has transparent base, liquid, foam, and foreground layers.
The liquid and foam are cropped within the original alpha silhouette; they are
not stretched. Empty/unready readings hide both fill layers. Reflections,
handles, rims, seams and shadows stay in place as the level changes. The Glass
view puts the serving count in the right column so it does not cover the image.

## Local build

Use the ESP-IDF 6.0.1 terminal from the repository's `touchscreen` directory:

```powershell
idf.py build
idf.py flash monitor
```

The regular build expands `main/artwork/web_vessels.json` into a generated C++
file in the build directory using Python's standard library. No Python image
packages, browser, filesystem partition, SD card, decoder, or new LVGL Kconfig
settings are needed. The checked-in data is losslessly packed RGB565+A8; the
24 cropped image layers use 2,422,512 bytes of read-only image data in firmware.
The normal ESP-IDF app-size check still applies to the existing 6 MB OTA slots.

## Re-importing changed web artwork (authoring only)

Run from the display repository root with Playwright/Chromium and Pillow
available. `PLAYWRIGHT_CHROMIUM_EXECUTABLE` may select an existing Chromium.
Use the pinned Scale revision above; update the provenance in the packer when
intentionally importing a different source revision.

```text
node tools/import_web_artwork.cjs ../KegScaleESP /path/to/scratch/layers
python tools/pack_vessel_artwork.py /path/to/scratch/layers touchscreen/main/artwork/web_vessels.json
```

Only the packed source data is checked in. PNG previews, generated C++ arrays,
and firmware binaries remain local build/authoring output.

## Validation

The complete modified home layout, artwork module and generated arrays were
compiled against LVGL 9.5.0 in a native renderer (ESP/platform calls stubbed).
Rendered all five vessels, custom fallback, and the keg. Checked empty/full fill,
clamping, invalid/unready states, foam bounds, complete-serving counts and
repeated creation/deletion across all six home views. Physical-device rendering,
ESP-IDF linking/size and flashing remain part of the local hardware test.
