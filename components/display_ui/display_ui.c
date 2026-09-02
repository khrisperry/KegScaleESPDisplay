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
esp_err_t display_ui_show_pairing_code(
    const char *scale_id,
    uint32_t passkey)
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

    draw_centered(
        7,
        "PAIR DISPLAY",
        2,
        true);

    if (scale_id != NULL) {
        draw_centered(
            29,
            scale_id,
            1,
            true);
    }

    draw_centered(
        48,
        "ENTER THIS CODE ON",
        1,
        true);
    draw_centered(
        60,
        "THE SCALE WEBPAGE",
        1,
        true);

    char code[16];
    snprintf(
        code,
        sizeof(code),
        "%03lu %03lu",
        (unsigned long)(passkey / 1000U),
        (unsigned long)(passkey % 1000U));

    draw_centered(
        82,
        code,
        3,
        true);

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

static uint8_t effective_display_flags(
    const ble_client_scale_state_t *state)
{
    if ((state->display_flags &
         BLE_DISPLAY_FLAG_CONFIG_PRESENT) == 0) {
        return BLE_DISPLAY_FLAGS_DEFAULT;
    }

    return state->display_flags;
}

static int fitting_text_scale(
    const char *text,
    int preferred,
    int max_width)
{
    int scale = preferred;

    while (scale > 1 &&
           epaper_text_width(text, scale) >
               max_width) {
        --scale;
    }

    return scale;
}

static void draw_metric_row(
    char metrics[][24],
    size_t start,
    size_t count,
    int y,
    int preferred_scale)
{
    if (count == 0) {
        return;
    }

    if (count == 1) {
        draw_text_centered_at(
            EPAPER_WIDTH / 2,
            y,
            metrics[start],
            fitting_text_scale(
                metrics[start],
                preferred_scale,
                EPAPER_WIDTH - 12));
        return;
    }

    draw_text_centered_at(
        62,
        y,
        metrics[start],
        fitting_text_scale(
            metrics[start],
            preferred_scale,
            116));

    draw_text_centered_at(
        188,
        y,
        metrics[start + 1],
        fitting_text_scale(
            metrics[start + 1],
            preferred_scale,
            116));
}

static void draw_supporting_metrics(
    char metrics[][24],
    size_t count)
{
    const size_t first_row =
        count > 1 ? 2 : count;

    draw_metric_row(
        metrics,
        0,
        first_row,
        87,
        2);

    if (count > first_row) {
        const size_t second_row =
            count - first_row > 2 ?
                2 :
                count - first_row;

        /*
         * Keep the final row above y=112. The physical panel bezel masks a
         * few pixels at the nominal 122-pixel edge on this board revision.
         */
        draw_metric_row(
            metrics,
            first_row,
            second_row,
            103,
            1);
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

    const uint8_t display_flags =
        effective_display_flags(state);

    char hero[8];
    const bool percent_layout =
        state->layout_id == 2;

    if (percent_layout) {
        float percent =
            state->remaining_percent;

        if (percent < 0.0f) {
            percent = 0.0f;
        } else if (percent > 100.0f) {
            percent = 100.0f;
        }

        snprintf(
            hero,
            sizeof(hero),
            "%.0F",
            (double)percent);
    } else {
        snprintf(
            hero,
            sizeof(hero),
            "%u",
            (unsigned)state->remaining_servings);
    }

    draw_hero_number(
        EPAPER_WIDTH / 2,
        6,
        hero);

    const char *hero_label =
        percent_layout ?
            "PERCENT FULL" :
            serving_count_label(
                state->serving_size_oz);

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        52,
        hero_label,
        fitting_text_scale(
            hero_label,
            2,
            EPAPER_WIDTH - 12));

    if ((display_flags &
         BLE_DISPLAY_FLAG_SHOW_BEER_NAME) != 0) {
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

        int keg_scale =
            fitting_text_scale(
                keg_name,
                2,
                EPAPER_WIDTH - 12);

        while (epaper_text_width(
                   keg_name,
                   keg_scale) >
               EPAPER_WIDTH - 12) {
            size_t length =
                strlen(keg_name);

            if (length == 0) {
                break;
            }

            keg_name[length - 1] = '\0';
        }

        draw_text_centered_at(
            EPAPER_WIDTH / 2,
            70,
            keg_name,
            keg_scale);
    }

    char metrics[4][24] = {{0}};
    size_t metric_count = 0;
    const bool stable =
        (state->flags &
         BLE_SCALE_FLAG_STABLE) != 0;

    if (percent_layout) {
        snprintf(
            metrics[metric_count++],
            sizeof(metrics[0]),
            "%u %s",
            (unsigned)state->remaining_servings,
            serving_count_label(
                state->serving_size_oz));
    }

    if (!percent_layout &&
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_SERVING_SIZE) != 0 &&
        metric_count < 4) {
        format_serving_size(
            state->serving_size_oz,
            metrics[metric_count++],
            sizeof(metrics[0]));
    }

    if ((display_flags &
         BLE_DISPLAY_FLAG_SHOW_TOTAL_WEIGHT) != 0 &&
        metric_count < 4) {
        snprintf(
            metrics[metric_count++],
            sizeof(metrics[0]),
            "%.1F LB",
            (double)state->total_weight_lbs);
    }

    if (stable &&
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_GALLONS) != 0 &&
        metric_count < 4) {
        snprintf(
            metrics[metric_count++],
            sizeof(metrics[0]),
            "%.2F GAL",
            (double)state->remaining_gallons);
    }

    if (!percent_layout &&
        stable &&
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_PERCENT) != 0 &&
        metric_count < 4) {
        snprintf(
            metrics[metric_count++],
            sizeof(metrics[0]),
            "%.0F%%",
            (double)state->remaining_percent);
    }

    if (percent_layout &&
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_SERVING_SIZE) != 0 &&
        metric_count < 4) {
        format_serving_size(
            state->serving_size_oz,
            metrics[metric_count++],
            sizeof(metrics[0]));
    }

    if (!stable &&
        metric_count < 4) {
        strlcpy(
            metrics[metric_count++],
            "SETTLING...",
            sizeof(metrics[0]));
    }

    draw_supporting_metrics(
        metrics,
        metric_count);

    return present();
}
