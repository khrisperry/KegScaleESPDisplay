# V0.0.2

Synchronize two RGB framebuffers with bounce-frame completion. Open the keyboard only after a tap, not the press that starts a scroll. Detach the keyboard before deleting its text field. Wait for Wi-Fi and poll pairing status before opening the authenticated connection; show actionable setup and address errors.

Update the scale to V0.0.33. Open Wi-Fi touchscreen setup on its web page, select Add touchscreen, and save the scale address on the touchscreen. Enter its six-character hexadecimal code on the scale page. BLE e-paper pairing is separate.

Existing local builds: set Board Support Package > Display > number of frame buffers to 2 in menuconfig. New builds use sdkconfig.defaults. A compile-time check prevents shipping the old one-buffer configuration. OTA installs the complete compiled configuration.

Hardware acceptance: scroll Setup and Keg without opening the keyboard; tap/dismiss each field; pair, save keg details, reboot either device, and verify reconnection. Check scrolling with Wi-Fi active and during OTA. Physical hardware verification is still required.
