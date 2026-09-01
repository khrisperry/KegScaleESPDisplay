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

/*
 * Clean geometric seven-segment-style numerals for the hero serving count.
 * The supporting text intentionally stays small so the serving count owns
 * the visual hierarchy on the 250x122 panel.
 */
enum {
    SEG_A = 1U << 0,
    SEG_B = 1U << 1,
    SEG_C = 1U << 2,
    SEG_D = 1U << 3,
    SEG_E = 1U << 4,
    SEG_F = 1U << 5,
    SEG_G = 1U << 6,
};

static uint8_t hero_digit_segments(char digit)
{
    switch (digit) {
        case '0': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
        case '1': return SEG_B | SEG_C;
        case '2': return SEG_A | SEG_B | SEG_G | SEG_E | SEG_D;
        case '3': return SEG_A | SEG_B | SEG_G | SEG_C | SEG_D;
        case '4': return SEG_F | SEG_G | SEG_B | SEG_C;
        case '5': return SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
        case '6': return SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D;
        case '7': return SEG_A | SEG_B | SEG_C;
        case '8': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case '9': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
        default: return 0;
    }
}

static void draw_horizontal_segment(
    int x,
    int y,
    int width,
    int thickness)
{
    /*
     * Slightly inset the first/last row to soften the square corners and make
     * the large numerals look less like scaled bitmap text.
     */
    epaper_fill_rect(
        x + 1,
        y,
        width - 2,
        thickness,
        true);

    if (thickness >= 3) {
        epaper_fill_rect(
            x,
            y + 1,
            width,
            thickness - 2,
            true);
    }
}

static void draw_vertical_segment(
    int x,
    int y,
    int height,
    int thickness)
{
    epaper_fill_rect(
        x,
        y + 1,
        thickness,
        height - 2,
        true);

    if (thickness >= 3) {
        epaper_fill_rect(
            x + 1,
            y,
            thickness - 2,
            height,
            true);
    }
}

static void draw_hero_digit(
    int x,
    int y,
    char digit)
{
    const int width = 26;
    const int height = 43;
    const int thickness = 4;
    const int half = height / 2;

    const uint8_t segments =
        hero_digit_segments(digit);

    if (segments & SEG_A) {
        draw_horizontal_segment(
            x + thickness,
            y,
            width - 2 * thickness,
            thickness);
    }

    if (segments & SEG_G) {
        draw_horizontal_segment(
            x + thickness,
            y + half - thickness / 2,
            width - 2 * thickness,
            thickness);
    }

    if (segments & SEG_D) {
        draw_horizontal_segment(
            x + thickness,
            y + height - thickness,
            width - 2 * thickness,
            thickness);
    }

    if (segments & SEG_F) {
        draw_vertical_segment(
            x,
            y + thickness,
            half - thickness,
            thickness);
    }

    if (segments & SEG_B) {
        draw_vertical_segment(
            x + width - thickness,
            y + thickness,
            half - thickness,
            thickness);
    }

    if (segments & SEG_E) {
        draw_vertical_segment(
            x,
            y + half,
            half - thickness,
            thickness);
    }

    if (segments & SEG_C) {
        draw_vertical_segment(
            x + width - thickness,
            y + half,
            half - thickness,
            thickness);
    }
}

static int hero_number_width(const char *text)
{
    if (text == NULL ||
        text[0] == '\0') {
        return 0;
    }

    const int digit_width = 26;
    const int spacing = 5;
    const size_t count = strlen(text);

    return
        (int)count * digit_width +
        ((int)count - 1) * spacing;
}

static void draw_hero_number(
    int center_x,
    int y,
    const char *text)
{
    const int digit_width = 26;
    const int spacing = 5;

    int x =
        center_x -
        hero_number_width(text) / 2;

    for (const char *p = text;
         *p != '\0';
         ++p) {
        draw_hero_digit(
            x,
            y,
            *p);

        x += digit_width + spacing;
    }
}

static void draw_text_centered_at(
    int center_x,
    int y,
    const char *text,
    int scale)
{
    const int width =
        epaper_text_width(
            text,
            scale);

    epaper_draw_text(
        center_x - width / 2,
        y,
        text,
        scale,
        true);
}

static bool serving_size_near(
    float serving_size_oz,
    float target_oz)
{
    float delta =
        serving_size_oz - target_oz;

    if (delta < 0.0f) {
        delta = -delta;
    }

    return delta <= 0.25f;
}

static const char *serving_count_label(
    float serving_size_oz)
{
    if (serving_size_near(
            serving_size_oz,
            12.0f)) {
        return "CANS LEFT";
    }

    if (serving_size_near(
            serving_size_oz,
            16.0f)) {
        return "PINTS LEFT";
    }

    if (serving_size_near(
            serving_size_oz,
            32.0f)) {
        return "CROWLERS LEFT";
    }

    if (serving_size_near(
            serving_size_oz,
            64.0f)) {
        return "GROWLERS LEFT";
    }

    return "SERVINGS LEFT";
}

static void format_serving_size(
    float serving_size_oz,
    char *buffer,
    size_t buffer_size)
{
    const int whole =
        (int)(serving_size_oz + 0.5f);

    float delta =
        serving_size_oz - (float)whole;

    if (delta < 0.0f) {
        delta = -delta;
    }

    if (delta < 0.05f) {
        snprintf(
            buffer,
            buffer_size,
            "%d OZ",
            whole);
    } else {
        snprintf(
            buffer,
            buffer_size,
            "%.1F OZ",
            (double)serving_size_oz);
    }
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

    /*
     * Serving-focused layout:
     *
     *   [future temp]       30        [future battery]
     *                   PINTS LEFT
     *                   MILLER LITE
     *
     *        16 OZ                   31.2 LB
     *       3.80 GAL                    76%
     *
     * The two-row metric grid uses the available lower panel area instead of
     * compressing four values into one tiny line. Temperature is reserved at
     * upper-left and the display's own battery reading at upper-right.
     */

    char servings[8];

    snprintf(
        servings,
        sizeof(servings),
        "%u",
        (unsigned)state->remaining_servings);

    draw_hero_number(
        EPAPER_WIDTH / 2,
        9,
        servings);

    const char *count_label =
        serving_count_label(
            state->serving_size_oz);

    int label_scale = 2;

    if (epaper_text_width(
            count_label,
            label_scale) >
        EPAPER_WIDTH - 12) {
        label_scale = 1;
    }

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        56,
        count_label,
        label_scale);

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

    /*
     * Match the count-label size whenever the beer name fits. Longer names
     * gracefully fall back to the compact face rather than clipping.
     */
    int keg_scale = label_scale;

    while (epaper_text_width(
               keg_name,
               keg_scale) >
           EPAPER_WIDTH - 12) {
        if (keg_scale > 1) {
            keg_scale = 1;
            continue;
        }

        size_t length = strlen(keg_name);

        if (length == 0) {
            break;
        }

        keg_name[length - 1] = '\0';
    }

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        75,
        keg_name,
        keg_scale);

    char serving_size[16];
    char weight[20];
    char gallons[20];
    char percent[12];

    format_serving_size(
        state->serving_size_oz,
        serving_size,
        sizeof(serving_size));

    snprintf(
        weight,
        sizeof(weight),
        "%.1F LB",
        (double)state->total_weight_lbs);

    snprintf(
        gallons,
        sizeof(gallons),
        "%.2F GAL",
        (double)state->remaining_gallons);

    snprintf(
        percent,
        sizeof(percent),
        "%.0F%%",
        (double)state->remaining_percent);

    /*
     * Two-by-two supporting metric grid. Scale 2 keeps these values readable
     * while preserving clear hierarchy below the hero serving count.
     */
    draw_text_centered_at(
        62,
        92,
        serving_size,
        2);

    draw_text_centered_at(
        188,
        92,
        weight,
        2);

    if ((state->flags &
         BLE_SCALE_FLAG_STABLE) != 0) {
        draw_text_centered_at(
            62,
            107,
            gallons,
            2);

        draw_text_centered_at(
            188,
            107,
            percent,
            2);
    } else {
        /*
         * An unsettled state is temporary and more useful than the second
         * metric row. Normal refresh logic generally avoids rendering while
         * settling, but this keeps initial/setup renders understandable.
         */
        draw_text_centered_at(
            EPAPER_WIDTH / 2,
            107,
            "SETTLING...",
            1);
    }

    return present();
}
