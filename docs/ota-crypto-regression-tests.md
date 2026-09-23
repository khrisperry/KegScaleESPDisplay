# Local OTA and controller crypto regression tests

Run under Linux/WSL with a C compiler, Python 3, and `libmbedtls-dev`:

```sh
bash tests/run_host_tests.sh
```

The full runner first requires the companion Scale checkout and checks that
both shared controller source/header copies are byte-identical. By default,
Scale is the sibling `KegScaleESP` directory; set `KEGSCALE_SCALE_ROOT` for a
different location. A missing checkout/checker or mismatch stops validation.
For just this gate on Windows, run `python tools/check_controller_link.py`.

The runner includes existing power/touch calibration tests, the OTA tests, the
touchscreen text guard, the real controller protocol tests, and PSA fault
injection tests. No firmware is downloaded, flashed, or published by this runner.

## OTA coverage

- Unapproved or unreadable bundles never display update progress or install.
- Wrong hardware, invalid offers, same-version offers, and mismatched bundles
  do not install.
- Allocation failure defers OTA without changing the screen.
- An approved installation shows progress before invoking the installer.
- Installer/network failure clears the pending-screen flag and restores the
  ordinary keg display with a forced refresh.
- Successful staging clears the credential bundle and restarts, retaining the
  pending-screen flag for the next boot.
- The downloader checks HTTP response status, exact byte count, and real SHA-256
  before selecting the boot partition. Tests cover connection/header/read
  failures, HTTP 404, truncated/corrupt data, missing partitions, allocation
  failure, and begin/write/end/boot-selection errors.

`run_ota_tests.py` extracts named functions from the current production sources
into temporary include files. Tests therefore execute the actual orchestration
and download functions, with fake hardware/HTTP/flash operations, rather than a
separately maintained copy of their behavior. A missing or ambiguous function
definition fails the runner. SHA-256 uses the real host PSA implementation.

Scale-side manual/automatic approval is tested separately by
`KegScaleESP/tests/run_host_tests.sh`. Its production policy rejects incompatible
updates even when automatic or manual approval exists; manual approval is tied
to the offered version. Both Scale policy call sites use this tested helper.

## Crypto coverage

GNU linker wrappers inject PSA export, agreement, import, encryption, and
decryption failures. Tests also inject malformed export/agreement lengths and
malformed agreement input. They verify key destruction against the real PSA
key store, zero frame length on encryption failure, unchanged sequence counters
on failed encryption/decryption, and successful retry afterward.

The malformed-input test exposed an ephemeral key left allocated by `cl_agree`.
Both Scale and touchscreen copies now destroy it before returning invalid input.
The wire protocol and pairing code format are unchanged.

## Hardware checks still required

Host tests do not prove radio behavior, physical screen rendering, power-loss
recovery, or bootloader rollback. On Dev hardware, verify authorization before
the update screen, interrupted download recovery, successful reboot into the new
image, and preservation of settings and pairing. Production remains on hold.
