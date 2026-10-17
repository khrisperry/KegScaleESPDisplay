# Touchscreen web artwork (V1.4.23)

The Glass screen and generic Keg Level fallback use the vessel artwork from
KegScaleESP commit `1e9553931d8d9dde991d32ee82f9656ff7dd2b30`.
Can (12 oz), Pint (16 oz), Cup (20 oz), Crowler (32 oz), Growler (64 oz),
and the generic keg are included. Custom serving sizes retain the existing pint fallback.

V1.4.17 adds the four rendered keg illustrations from Scale V1.4.9
(commit `74776c1002724f8640c5baf8296c013482ae4550`) to Keg
Level only: 1/6 Barrel (5.17 gal, also accepts saved 5.16), 1/4 Barrel
(7.75 gal pony), Corny Keg (5 gal), and 1/2 Barrel (15.5 gal). Selection
follows the received capacity, with a 0.02 gal tolerance. Custom capacities
keep the generic keg. Capacity changes refresh the image immediately.
The web WebP sources are rasterized into the same native transparent layers;
brushed-steel shading is preserved under the moving amber fill.

V1.4.18 follows Scale V1.4.10's curved fill treatment
(commit `e09712d31d2c52734b9b98f910c2f2ea8863e10e`). A shaded elliptical
surface with a fine rim moves with the measured level. The lower amber edge
follows the curved seam: Corny leaves its black rubber boot exposed, while
commercial barrels leave only their lowest silver chime unfilled. Near-empty
and full surfaces are clipped to the same body silhouette. LVGL reuses the
liquid layer's existing alpha plane as an A8 mask; no extra mask pixel data,
frame sequences, or runtime image conversion is needed.

V1.4.19 refines only the commercial barrels to match the approved cutaway
reference, using Scale V1.4.11 (commit `1bd1f8ded70a8d4a5ef89bc557c4cd2475acb5bf`). The original raised steel bands and narrow outer walls cross in
front of the amber fill. The complete lower chime, including hand holes,
remains silver. A soft cream surface is inset inside the barrel silhouette.
Corny, serving vessels, and the generic keg keep their previous pixels.
Web and native pixel checks cover preserved steel bands and lower chimes at
empty, near-empty, partial, and full readings.

V1.4.20 uses Scale V1.4.12 (commit `b1d34101391bbb714076bbe14b87c6e03332cefa`).
It adjusts only the 1/4 and 1/2 barrel surface ellipse depth to match
the perspective of their wider steel bands. The 1/6 barrel and Corny pixels,
all steel bands, body fill masks and lower chimes are unchanged.

V1.4.21 uses Scale V1.4.13 (commit `f088593517e72efe0eed83e7495fd3dd55b4859e`).
It restores the original lower silver band on all four rendered kegs,
including the Corny band above its rubber boot. A curved foreground mask
preserves the source steel shading instead of tinting the band amber. The
body, moving surface and upper bands retain their V1.4.20 pixels. Web and
native pixel comparisons confirm the lower bands stay silver across levels.

V1.4.22 uses Scale V1.4.14 (commit `ff35afc935421e9a10b0feb7c5a58e550e95d698`).
It matches only the half-barrel lower seam and silver band to the
quarter-barrel curve profile. Its top surface and upper bands are unchanged;
all other kegs retain their prior image layers.

Each illustration has transparent base, liquid, foam, and foreground layers.
The liquid and foam are cropped within the original alpha silhouette; they are
not stretched. Empty/unready readings hide both fill layers. Reflections,
handles, rims, seams and shadows stay in place as the level changes. The Glass
view puts the serving count in the right column so it does not cover the image.

V1.4.23 retains every artwork pixel and changes LVGL allocation to standard
system malloc so large transient masked-image layers can use the board's PSRAM.
The previous fixed 64 KiB non-expanding LVGL pool could exhaust and halt in
LVGL's default malloc assertion. CMake migrates existing builtin-pool choices
in local sdkconfig before Kconfig is loaded; fresh builds select CLIB malloc
from sdkconfig.defaults. No Wi-Fi, pairing, calibration, or other local build
settings are changed. Startup logs identify the active allocator.

The native rendering stress check held 24 KiB of additional LVGL allocations
to represent retained shell/UI state. The fixed 64 KiB pool hit its allocation
assertion when rendering a capacity-specific keg. System malloc passed the
same test across all vessels, fill bounds, capacities and view lifecycles.
This establishes the allocation failure mechanism; hardware soak testing is
still required for the reported intermittent reboot/lockup.

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
40 cropped image layers use 3,406,278 bytes of read-only image data in firmware.
The normal ESP-IDF app-size check still applies to the existing 6 MB OTA slots.

## Re-importing changed web artwork (authoring only)

Run from the display repository root with Playwright/Chromium and Pillow
available. `PLAYWRIGHT_CHROMIUM_EXECUTABLE` may select an existing Chromium.
Use the V1.4.14 Scale revision above; update the provenance in the packer when
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
Rendered all five vessels, custom fallback, generic keg, and four new keg types.
Checked capacity changes with unchanged serving size, including saved 5.16 gal. Checked empty/full fill,
clamping, invalid/unready states, foam bounds, complete-serving counts and
repeated creation/deletion across all six home views. Physical-device rendering,
ESP-IDF linking/size and flashing remain part of the local hardware test.
Curved-surface validation includes 0/1/22/50/99/100%, invalid/unready states,
capacity/view switching, and pixel comparisons confirming the protective bases
remain unchanged in both web and native renderers.
