# BLE e-paper pairing reproduction — Dev V1.4.6-dev

This is a **diagnostic-only** build (apart from retaining the already-tested
GPIO39 recovery and hardware-reset counter fix). It does not change the
pairing timeout, generate different passkeys, retry behavior, security policy,
or the Scale web wizard.

## Reproduce

1. Flash **both** the Scale (KegScaleESP `dev`) and the e-paper display
   (KegScaleESPDisplay `dev`) with V1.4.6-dev. Flash normally; do **not**
   erase NVS.
2. Capture Scale serial on its COM port and e-paper serial on its COM port
   at 115200 baud, ideally from before entering the wizard. Using two terminals
   is fine; the timestamps in each trace are monotonic **per device**, not
   synchronized wall-clock times.
3. Unpair both sides intentionally. On e-paper: hold board **GPIO39 Button 1**,
   press/release RESET and continue holding for five seconds. On Scale: use
   the supported Remove / Reset pairing workflow as appropriate.
4. In the Scale web page, start **Add e-paper display**. Keep the wizard open.
   Record whether it reaches the code-entry step when the e-paper code appears.
5. Allow attempts to fail/retry; do not disconnect serial logging. When an
   attempt succeeds or the wizard expires, save **both logs** and the
   approximate time you saw the behavior.

## What to look for

Filter both logs by `PAIR_DIAG`.

- **Display**: `scan_result` -> `code_render_start` ->
  `code_render_complete` -> `connect_start` ->
  `gap_connect` / `gap_disconnect` -> `security_start` ->
  `passkey_action` -> `security_result` -> `pair_result`.
- **Scale**: `window_open` -> `connected` ->
  `passkey_action` -> `code_prompt_state available=1` ->
  `code_submit_requested` -> `security_change` ->
  pairing success; `disconnected` / `window_expired` explain failures.
- If the display shows a code but the Scale **never logs**
  `code_prompt_state available=1`, the web wizard correctly stays at
  "Waiting". Investigate why the secure handshake never reached the input
  stage before looking for a web UI bug.
- Pairing attempt IDs are **display-local** and session IDs are
  **Scale-local**; correlate attempts using order, connection handle,
  scale identity, and manually recorded time, not by matching ID numbers.

Codes, Wi-Fi credentials and pairing/bond keys are **not logged**.
If available, a downloaded Scale support log can supplement the serial log,
but for first-connection failures capture **both** UART outputs.

## Flash

Use ESP-IDF v6.0.1, and from the appropriate `dev` checkout:

```powershell
git switch dev
git pull
idf.py -p COM9 flash monitor # e-paper display; replace COM9 if different
```

Repeat from the KegScaleESP checkout with the Scale's actual COM port.
Do not run `erase-flash`; that would remove the state under test.
