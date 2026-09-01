#include "display_ui.h"

#include <stdio.h>
#include <string.h>

#include "epaper.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "display_ui";

static void draw_centered(
    int y,
    const char *text,
    int scale,
    bool black)
{
    int width =
        epaper_text_width(text, scale);

    int x =
        (EPAPER_WIDTH - width) / 2;

    if (x < 0) {
        x = 0;
    }

    epaper_draw_text(
        x,
        y,
        text,
        scale,
        black);
}

static esp_err_t present(void)
{
    esp_err_t err = epaper_refresh();

    if (err == ESP_OK) {
        err = epaper_sleep();
    }

    return err;
}

esp_err_t display_ui_show_message(
    const char *title,
    const char *line1,
    const char *line2)
{
    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    epaper_clear(false);
    epaper_draw_rect(
        0,
        0,
        EPAPER_WIDTH,
        EPAPER_HEIGHT,
        true);

    if (title != NULL) {
        draw_centered(
            12,
            title,
            2,
            true);
    }

    if (line1 != NULL) {
        draw_centered(
            55,
            line1,
            2,
            true);
    }

    if (line2 != NULL) {
        draw_centered(
            88,
            line2,
            1,
            true);
    }

    return present();
}

esp_err_t display_ui_show_candidates(
    const ble_client_peer_t *candidates,
    size_t count)
{
    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    epaper_clear(false);
    epaper_draw_text(
        6,
        7,
        "SELECT SCALE",
        2,
        true);

    epaper_draw_text(
        6,
        27,
        "USE SERIAL: PAIR <ID>",
        1,
        true);

    size_t shown =
        count < 4 ? count : 4;

    for (size_t i = 0;
         i < shown;
         ++i) {
        char line[40];

        snprintf(
            line,
            sizeof(line),
            "%u %s %dDB",
            (unsigned)(i + 1),
            candidates[i].scale_id,
            (int)candidates[i].rssi);

        epaper_draw_text(
            6,
            46 + (int)i * 18,
            line,
            1,
            true);
    }

    if (count > shown) {
        epaper_draw_text(
            6,
            46 + (int)shown * 18,
            "MORE ON SERIAL",
            1,
            true);
    }

    return present();
}

esp_err_t display_ui_show_scale(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state)
{
    if (peer == NULL ||
        state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    epaper_clear(false);

    epaper_fill_rect(
        0,
        0,
        EPAPER_WIDTH,
        23,
        true);

    char keg_name[
        BLE_CLIENT_KEG_NAME_MAX + 1];

    if (state->keg_name[0] != '\0') {
        strlcpy(
            keg_name,
            state->keg_name,
            sizeof(keg_name));
    } else {
        strlcpy(
            keg_name,
            peer->scale_id,
            sizeof(keg_name));
    }

    while (epaper_text_width(
               keg_name,
               2) >
           EPAPER_WIDTH - 10) {
        size_t length = strlen(keg_name);

        if (length == 0) {
            break;
        }

        keg_name[length - 1] = '\0';
    }

    draw_centered(
        4,
        keg_name,
        2,
        false);

    char servings[8];
    snprintf(
        servings,
        sizeof(servings),
        "%u",
        (unsigned)state->remaining_servings);

    epaper_draw_text(
        8,
        35,
        servings,
        5,
        true);

    epaper_draw_text(
        8,
        77,
        "SERVINGS",
        2,
        true);

    char percent[12];
    snprintf(
        percent,
        sizeof(percent),
        "%.0f%%",
        (double)state->remaining_percent);

    epaper_draw_text(
        155,
        38,
        percent,
        4,
        true);

    char gallons[20];
    snprintf(
        gallons,
        sizeof(gallons),
        "%.2f GAL",
        (double)state->remaining_gallons);

    epaper_draw_text(
        155,
        78,
        gallons,
        2,
        true);

    char footer[48];

    snprintf(
        footer,
        sizeof(footer),
        "%s  %.1F LB  #%u",
        (state->flags &
         BLE_SCALE_FLAG_STABLE) ?
            "STABLE" :
            "SETTLING",
        (double)state->total_weight_lbs,
        (unsigned)state->sequence);

    epaper_draw_text(
        7,
        108,
        footer,
        1,
        true);

    return present();
}
