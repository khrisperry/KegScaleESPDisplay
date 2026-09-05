from pathlib import Path

main_path = Path("main/main.c")
text = main_path.read_text()

marker = '''static esp_err_t fetch_paired_state(
    pairing_config_t *pairing,
    ble_client_scale_state_t *state)
{'''
assert marker in text

helper = '''static esp_err_t fetch_paired_state_once(
    pairing_config_t *pairing,
    ble_client_scale_state_t *state)
{
    if (pairing == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        ble_client_fetch(
            &pairing->peer,
            state);

    if (err != ESP_OK) {
        return err;
    }

    if (!ble_client_state_is_compatible(
            &pairing->peer,
            state)) {
        return ESP_ERR_INVALID_VERSION;
    }

    apply_display_settings(state);
    remember_runtime_settings(&pairing->peer);
    return ESP_OK;
}

'''
text = text.replace(marker, helper + marker, 1)

old = '''#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
        if (touch_wake) {
            bool meaningful_change = false;

            err =
                wait_for_touch_pour_result(
                    &pairing,
                    &state,
                    battery_percent,
                    &meaningful_change);

            if (err == ESP_OK) {
                handle_unpair_request(
                    &pairing,
                    &state);
            }

            if (err == ESP_OK &&
                meaningful_change) {
                render_if_needed(
                    &pairing.peer,
                    &state,
                    battery_percent);
            } else if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Touch wake ended without a successful final scale read: %s",
                    esp_err_to_name(err));
            }

            if (err == ESP_OK) {
                install_display_update_if_needed(
                    &pairing,
                    &state);
            }

            go_to_sleep();
        }
#endif
'''
assert text.count(old) == 1

new = '''#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
        if (touch_wake &&
            !s_periodic_checkin_enabled) {
            ESP_LOGI(
                TAG,
                "Touch-only mode: awake for %d seconds, then one scale check before sleeping again",
                CONFIG_KEG_DISPLAY_TOUCH_INITIAL_WAIT_SECONDS);

            vTaskDelay(
                pdMS_TO_TICKS(
                    CONFIG_KEG_DISPLAY_TOUCH_INITIAL_WAIT_SECONDS *
                    1000));

            err =
                fetch_paired_state_once(
                    &pairing,
                    &state);

            if (err == ESP_OK) {
                handle_unpair_request(
                    &pairing,
                    &state);

                render_if_needed(
                    &pairing.peer,
                    &state,
                    battery_percent);

                install_display_update_if_needed(
                    &pairing,
                    &state);
            } else {
                ESP_LOGW(
                    TAG,
                    "Touch-only scale check failed: %s; returning to deep sleep without retries",
                    esp_err_to_name(err));
            }

            go_to_sleep();
        }

        if (touch_wake) {
            bool meaningful_change = false;

            err =
                wait_for_touch_pour_result(
                    &pairing,
                    &state,
                    battery_percent,
                    &meaningful_change);

            if (err == ESP_OK) {
                handle_unpair_request(
                    &pairing,
                    &state);
            }

            if (err == ESP_OK &&
                meaningful_change) {
                render_if_needed(
                    &pairing.peer,
                    &state,
                    battery_percent);
            } else if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Touch wake ended without a successful final scale read: %s",
                    esp_err_to_name(err));
            }

            if (err == ESP_OK) {
                install_display_update_if_needed(
                    &pairing,
                    &state);
            }

            go_to_sleep();
        }
#endif
'''
text = text.replace(old, new, 1)
main_path.write_text(text)

kconfig_path = Path("main/Kconfig.projbuild")
kconfig = kconfig_path.read_text()
old_help = '''    help
        After a capacitive-touch wake, keep the existing e-paper image visible
        and wait this long before the first BLE read. This gives the user time
        to begin and mostly complete a pour.
'''
assert kconfig.count(old_help) == 1
new_help = '''    help
        After a capacitive-touch wake, keep the existing e-paper image visible
        and wait this long before the first BLE read. In touch-only mode this is
        the full intentional awake delay: one BLE read follows, then the display
        returns to deep sleep with only capacitive touch armed.
'''
kconfig = kconfig.replace(old_help, new_help, 1)
kconfig_path.write_text(kconfig)

version_path = Path("version.txt")
assert version_path.read_text().strip() == "V0.0.6"
version_path.write_text("V0.0.7\n")
