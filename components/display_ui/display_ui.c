#include "display_ui.h"

#include <stdio.h>
#include <string.h>

#include "epaper.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "display_ui";

/*
 * The controller exposes 250x122 pixels, but the V2.3.1 board's physical
 * window masks a few pixels on every edge. Keep all meaningful content
 * inside this shared safe area so no layout can regress into the bezel.
 */
enum {
    DISPLAY_SAFE_LEFT = 10,
    DISPLAY_SAFE_TOP = 10,
    DISPLAY_SAFE_RIGHT = 240,
    DISPLAY_SAFE_BOTTOM = 112,
    DISPLAY_SAFE_WIDTH =
        DISPLAY_SAFE_RIGHT - DISPLAY_SAFE_LEFT,
    DISPLAY_SAFE_HEIGHT =
        DISPLAY_SAFE_BOTTOM - DISPLAY_SAFE_TOP,
};

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

    if (x < DISPLAY_SAFE_LEFT) {
        x = DISPLAY_SAFE_LEFT;
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
        DISPLAY_SAFE_LEFT,
        DISPLAY_SAFE_TOP,
        DISPLAY_SAFE_WIDTH,
        DISPLAY_SAFE_HEIGHT,
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
        DISPLAY_SAFE_LEFT,
        DISPLAY_SAFE_TOP,
        DISPLAY_SAFE_WIDTH,
        DISPLAY_SAFE_HEIGHT,
        true);

    draw_centered(
        12,
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
        DISPLAY_SAFE_LEFT,
        DISPLAY_SAFE_TOP,
        "SELECT SCALE",
        2,
        true);

    epaper_draw_text(
        DISPLAY_SAFE_LEFT,
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
            DISPLAY_SAFE_LEFT,
            42 + (int)i * 15,
            line,
            1,
            true);
    }

    if (count > shown) {
        epaper_draw_text(
            DISPLAY_SAFE_LEFT,
            42 + (int)shown * 15,
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

static void draw_hero_percent_symbol(
    int x,
    int y)
{
    const int width = 21;
    const int height = 43;

    epaper_fill_rect(
        x + 1,
        y + 4,
        7,
        7,
        true);
    epaper_fill_rect(
        x + width - 8,
        y + height - 11,
        7,
        7,
        true);

    /* Pixel-stepped diagonal keeps the symbol consistent with the digits. */
    for (int row = 0;
         row < height - 8;
         ++row) {
        const int diagonal_x =
            x + width - 5 -
            (row * (width - 9)) /
                (height - 9);

        epaper_fill_rect(
            diagonal_x,
            y + 4 + row,
            2,
            2,
            true);
    }
}

static void draw_hero_percent(
    int center_x,
    int y,
    const char *digits)
{
    const int spacing = 6;
    const int symbol_width = 21;
    const int digits_width =
        hero_number_width(digits);
    const int total_width =
        digits_width + spacing + symbol_width;
    const int digits_center =
        center_x - total_width / 2 +
        digits_width / 2;

    draw_hero_number(
        digits_center,
        y,
        digits);
    draw_hero_percent_symbol(
        center_x - total_width / 2 +
            digits_width + spacing,
        y);
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

    int x = center_x - width / 2;

    if (x < DISPLAY_SAFE_LEFT) {
        x = DISPLAY_SAFE_LEFT;
    }

    epaper_draw_text(
        x,
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

static float clamped_percent(
    const ble_client_scale_state_t *state)
{
    float percent = state->remaining_percent;

    if (percent < 0.0f) {
        percent = 0.0f;
    } else if (percent > 100.0f) {
        percent = 100.0f;
    }

    return percent;
}

static const char *serving_unit_label(
    float serving_size_oz)
{
    if (serving_size_near(
            serving_size_oz,
            12.0f)) {
        return "CANS";
    }

    if (serving_size_near(
            serving_size_oz,
            16.0f)) {
        return "PINTS";
    }

    if (serving_size_near(
            serving_size_oz,
            32.0f)) {
        return "CROWLERS";
    }

    if (serving_size_near(
            serving_size_oz,
            64.0f)) {
        return "GROWLERS";
    }

    return "SERVINGS";
}

static void copy_keg_name(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    char *buffer,
    size_t buffer_size)
{
    if (state->keg_name[0] != '\0') {
        strlcpy(
            buffer,
            state->keg_name,
            buffer_size);
    } else {
        strlcpy(
            buffer,
            peer->scale_id,
            buffer_size);
    }
}

static void draw_keg_name(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    int y,
    int preferred_scale)
{
    char keg_name[
        BLE_CLIENT_KEG_NAME_MAX + 1];

    copy_keg_name(
        peer,
        state,
        keg_name,
        sizeof(keg_name));

    const int keg_scale =
        fitting_text_scale(
            keg_name,
            preferred_scale,
            DISPLAY_SAFE_WIDTH);

    while (epaper_text_width(
               keg_name,
               keg_scale) >
           DISPLAY_SAFE_WIDTH) {
        const size_t length =
            strlen(keg_name);

        if (length == 0) {
            break;
        }

        keg_name[length - 1] = '\0';
    }

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        y,
        keg_name,
        keg_scale);
}

static void append_metric(
    char *line,
    size_t line_size,
    const char *metric)
{
    if (metric == NULL ||
        metric[0] == '\0') {
        return;
    }

    if (line[0] != '\0') {
        strlcat(
            line,
            " / ",
            line_size);
    }

    strlcat(
        line,
        metric,
        line_size);
}

static void draw_inline_metrics(
    int y,
    const char *line,
    int preferred_scale)
{
    if (line == NULL ||
        line[0] == '\0') {
        return;
    }

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        y,
        line,
        fitting_text_scale(
            line,
            preferred_scale,
            DISPLAY_SAFE_WIDTH));
}

static void draw_servings_layout(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t display_flags)
{
    const bool show_name =
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_BEER_NAME) != 0;
    const bool name_at_top =
        show_name &&
        (display_flags &
         BLE_DISPLAY_FLAG_BEER_NAME_TOP) != 0;
    const bool stable =
        (state->flags &
         BLE_SCALE_FLAG_STABLE) != 0;
    const int hero_y =
        name_at_top ? 31 : 14;

    if (name_at_top) {
        draw_keg_name(
            peer,
            state,
            12,
            2);
    }

    char hero[8];
    snprintf(
        hero,
        sizeof(hero),
        "%u",
        (unsigned)state->remaining_servings);

    draw_hero_number(
        EPAPER_WIDTH / 2,
        hero_y,
        hero);

    const char *hero_label =
        serving_count_label(
            state->serving_size_oz);

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        hero_y + 46,
        hero_label,
        fitting_text_scale(
            hero_label,
            2,
            DISPLAY_SAFE_WIDTH));

    if (show_name &&
        !name_at_top) {
        draw_keg_name(
            peer,
            state,
            78,
            2);
    }

    char metrics[64] = {0};

    if ((display_flags &
         BLE_DISPLAY_FLAG_SHOW_SERVING_SIZE) != 0) {
        char serving[24];
        format_serving_size(
            state->serving_size_oz,
            serving,
            sizeof(serving));
        strlcat(
            serving,
            " SERVING",
            sizeof(serving));
        append_metric(
            metrics,
            sizeof(metrics),
            serving);
    }

    if (stable &&
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_GALLONS) != 0) {
        char gallons[24];
        snprintf(
            gallons,
            sizeof(gallons),
            "%.2F GAL",
            (double)state->remaining_gallons);
        append_metric(
            metrics,
            sizeof(metrics),
            gallons);
    }

    if (!stable) {
        append_metric(
            metrics,
            sizeof(metrics),
            "SETTLING...");
    }

    draw_inline_metrics(
        show_name ? 102 : 94,
        metrics,
        1);
}

static void draw_percent_layout(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t display_flags)
{
    const bool show_name =
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_BEER_NAME) != 0;
    const bool name_at_top =
        show_name &&
        (display_flags &
         BLE_DISPLAY_FLAG_BEER_NAME_TOP) != 0;
    const bool stable =
        (state->flags &
         BLE_SCALE_FLAG_STABLE) != 0;
    const int hero_y =
        name_at_top ? 31 : 14;

    if (name_at_top) {
        draw_keg_name(
            peer,
            state,
            12,
            2);
    }

    char percent[8];
    snprintf(
        percent,
        sizeof(percent),
        "%.0F",
        (double)clamped_percent(state));

    draw_hero_percent(
        EPAPER_WIDTH / 2,
        hero_y,
        percent);

    if (show_name &&
        !name_at_top) {
        draw_keg_name(
            peer,
            state,
            72,
            2);
    }

    char metrics[64] = {0};

    if (stable &&
        (display_flags &
         BLE_DISPLAY_FLAG_SHOW_GALLONS) != 0) {
        char gallons[24];
        snprintf(
            gallons,
            sizeof(gallons),
            "%.2F GAL",
            (double)state->remaining_gallons);
        append_metric(
            metrics,
            sizeof(metrics),
            gallons);
    }

    if ((display_flags &
         BLE_DISPLAY_FLAG_SHOW_TOTAL_WEIGHT) != 0) {
        char weight[24];
        snprintf(
            weight,
            sizeof(weight),
            "%.1F LB",
            (double)state->total_weight_lbs);
        append_metric(
            metrics,
            sizeof(metrics),
            weight);
    }

    if (!stable) {
        append_metric(
            metrics,
            sizeof(metrics),
            "SETTLING...");
    }

    draw_inline_metrics(
        95,
        metrics,
        2);
}

static void draw_diagnostic_metric(
    int center_x,
    int value_y,
    const char *value,
    const char *label,
    int max_width)
{
    draw_text_centered_at(
        center_x,
        value_y,
        value,
        fitting_text_scale(
            value,
            2,
            max_width));
    draw_text_centered_at(
        center_x,
        value_y + 15,
        label,
        1);
}

static void draw_diagnostics_layout(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t display_flags)
{
    if ((display_flags &
         BLE_DISPLAY_FLAG_SHOW_BEER_NAME) != 0) {
        draw_keg_name(
            peer,
            state,
            11,
            2);
    } else {
        draw_text_centered_at(
            EPAPER_WIDTH / 2,
            11,
            "DIAGNOSTICS",
            2);
    }

    char percent[12];
    char servings[24];
    char serving_size[16];
    char gallons[20];
    char scale_weight[20];
    char beer_weight[20];
    char wifi_signal[16];

    snprintf(
        percent,
        sizeof(percent),
        "%.0F%%",
        (double)clamped_percent(state));
    snprintf(
        servings,
        sizeof(servings),
        "%u %s",
        (unsigned)state->remaining_servings,
        serving_unit_label(
            state->serving_size_oz));
    format_serving_size(
        state->serving_size_oz,
        serving_size,
        sizeof(serving_size));
    snprintf(
        gallons,
        sizeof(gallons),
        "%.2F GAL",
        (double)state->remaining_gallons);
    snprintf(
        scale_weight,
        sizeof(scale_weight),
        "%.1F LB",
        (double)state->total_weight_lbs);
    snprintf(
        beer_weight,
        sizeof(beer_weight),
        "%.1F LB",
        (double)state->beverage_weight_lbs);

    if ((state->flags &
         BLE_SCALE_FLAG_WIFI_CONNECTED) != 0) {
        snprintf(
            wifi_signal,
            sizeof(wifi_signal),
            "%d DBM",
            (int)state->wifi_rssi_dbm);
    } else {
        strlcpy(
            wifi_signal,
            "OFFLINE",
            sizeof(wifi_signal));
    }

    draw_diagnostic_metric(
        62,
        28,
        percent,
        "LEFT",
        104);
    draw_diagnostic_metric(
        188,
        28,
        servings,
        "SERVINGS LEFT",
        104);
    draw_diagnostic_metric(
        62,
        53,
        serving_size,
        "SERVING SIZE",
        104);
    draw_diagnostic_metric(
        188,
        53,
        gallons,
        "REMAINING",
        104);
    draw_diagnostic_metric(
        43,
        78,
        scale_weight,
        "SCALE WEIGHT",
        68);
    draw_diagnostic_metric(
        125,
        78,
        beer_weight,
        "BEER WEIGHT",
        68);
    draw_diagnostic_metric(
        207,
        78,
        wifi_signal,
        "WIFI SIGNAL",
        68);

    char status[40];

    if ((state->flags &
         BLE_SCALE_FLAG_CALIBRATED) == 0) {
        strlcpy(
            status,
            "NOT CALIBRATED",
            sizeof(status));
    } else if ((state->flags &
                BLE_SCALE_FLAG_PROFILE_CONFIGURED) == 0) {
        strlcpy(
            status,
            "NO KEG PROFILE",
            sizeof(status));
    } else if ((state->flags &
                BLE_SCALE_FLAG_KEG_READY) == 0) {
        strlcpy(
            status,
            "KEG NOT READY",
            sizeof(status));
    } else {
        strlcpy(
            status,
            (state->flags &
             BLE_SCALE_FLAG_STABLE) != 0 ?
                "STABLE" :
                "SETTLING",
            sizeof(status));
    }

    draw_text_centered_at(
        EPAPER_WIDTH / 2,
        105,
        status,
        fitting_text_scale(
            status,
            1,
            DISPLAY_SAFE_WIDTH));
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

    switch (state->layout_id) {
        case 2:
            draw_percent_layout(
                peer,
                state,
                display_flags);
            break;

        case 3:
            draw_diagnostics_layout(
                peer,
                state,
                display_flags);
            break;

        case 1:
        default:
            draw_servings_layout(
                peer,
                state,
                display_flags);
            break;
    }

    return present();
}
