#include "display_ui.h"

#include <stdio.h>
#include <string.h>

#include "epaper.h"
#include "esp_check.h"
#include "esp_log.h"
#include "qrcode.h"

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
    BATTERY_BODY_WIDTH = 20,
    BATTERY_BODY_HEIGHT = 10,
    BATTERY_TERMINAL_WIDTH = 3,
    BATTERY_RESERVED_WIDTH = 30,
    TOUCH_ACK_X = 13,
    TOUCH_ACK_Y = 16,
    TOUCH_ACK_WIDTH = 18,
    TOUCH_ACK_HEIGHT = 16,
};

static void draw_font_centered_at(
    int center_x,
    int y,
    const char *text,
    const epaper_font_t *font)
{
    const int width =
        epaper_font_text_width(
            text,
            font);

    int x = center_x - width / 2;

    if (x < DISPLAY_SAFE_LEFT) {
        x = DISPLAY_SAFE_LEFT;
    }

    if (x + width > DISPLAY_SAFE_RIGHT) {
        x = DISPLAY_SAFE_RIGHT - width;
    }

    epaper_draw_text_font(
        x,
        y,
        text,
        font,
        true);
}

static void clear_touch_ack_area(void)
{
    epaper_fill_rect(
        TOUCH_ACK_X,
        TOUCH_ACK_Y,
        TOUCH_ACK_WIDTH,
        TOUCH_ACK_HEIGHT,
        false);
}

static void draw_touch_acknowledged(void)
{
    clear_touch_ack_area();

    epaper_draw_rect(
        TOUCH_ACK_X,
        TOUCH_ACK_Y,
        TOUCH_ACK_WIDTH,
        TOUCH_ACK_HEIGHT,
        true);

    /* Compact check mark: immediately recognizable without another font. */
    for (int i = 0; i < 4; ++i) {
        epaper_fill_rect(
            TOUCH_ACK_X + 3 + i,
            TOUCH_ACK_Y + 7 + i,
            2,
            2,
            true);
    }

    for (int i = 0; i < 7; ++i) {
        epaper_fill_rect(
            TOUCH_ACK_X + 6 + i,
            TOUCH_ACK_Y + 10 - i,
            2,
            2,
            true);
    }
}

static void draw_battery_indicator(uint8_t battery_percent)
{
    if (battery_percent > 100) {
        battery_percent = 100;
    }

    const int x =
        DISPLAY_SAFE_RIGHT -
        BATTERY_BODY_WIDTH -
        BATTERY_TERMINAL_WIDTH;
    const int y = DISPLAY_SAFE_TOP + 1;

    /* Clear the reserved corner before drawing the icon over any layout. */
    epaper_fill_rect(
        DISPLAY_SAFE_RIGHT - BATTERY_RESERVED_WIDTH,
        DISPLAY_SAFE_TOP,
        BATTERY_RESERVED_WIDTH,
        BATTERY_BODY_HEIGHT + 3,
        false);

    epaper_draw_rect(
        x,
        y,
        BATTERY_BODY_WIDTH,
        BATTERY_BODY_HEIGHT,
        true);
    epaper_fill_rect(
        x + BATTERY_BODY_WIDTH,
        y + 3,
        BATTERY_TERMINAL_WIDTH,
        4,
        true);

    const unsigned bars =
        (unsigned)(battery_percent / 20U);

    for (unsigned i = 0;
         i < bars && i < 5U;
         ++i) {
        epaper_fill_rect(
            x + 2 + (int)i * 3,
            y + 2,
            2,
            BATTERY_BODY_HEIGHT - 4,
            true);
    }
}

static esp_err_t present(void)
{
    /* Every full screen restores the transient touch area's blank baseline. */
    clear_touch_ack_area();

    esp_err_t err = epaper_refresh();

    if (err == ESP_OK) {
        err = epaper_sleep();
    }

    return err;
}

static void draw_setup_qrcode(
    esp_qrcode_handle_t qrcode)
{
    const int modules =
        esp_qrcode_get_size(qrcode);
    const int module_pixels = 3;
    const int quiet_modules = 4;
    const int quiet_pixels =
        quiet_modules * module_pixels;
    const int qr_pixels =
        modules * module_pixels;
    const int origin_x =
        DISPLAY_SAFE_LEFT + quiet_pixels;
    const int origin_y =
        (EPAPER_HEIGHT - qr_pixels) / 2;

    epaper_fill_rect(
        DISPLAY_SAFE_LEFT,
        origin_y - quiet_pixels,
        qr_pixels + quiet_pixels * 2,
        qr_pixels + quiet_pixels * 2,
        false);

    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (esp_qrcode_get_module(
                    qrcode,
                    x,
                    y)) {
                epaper_fill_rect(
                    origin_x + x * module_pixels,
                    origin_y + y * module_pixels,
                    module_pixels,
                    module_pixels,
                    true);
            }
        }
    }
}

esp_err_t display_ui_show_touch_acknowledged(void)
{
    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    draw_touch_acknowledged();

    return epaper_refresh_partial(
        TOUCH_ACK_X,
        TOUCH_ACK_Y,
        TOUCH_ACK_WIDTH,
        TOUCH_ACK_HEIGHT);
}

esp_err_t display_ui_clear_touch_acknowledged(void)
{
    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    clear_touch_ack_area();

    ESP_RETURN_ON_ERROR(
        epaper_refresh_partial(
            TOUCH_ACK_X,
            TOUCH_ACK_Y,
            TOUCH_ACK_WIDTH,
            TOUCH_ACK_HEIGHT),
        TAG,
        "Could not clear touch acknowledgement");

    return epaper_sleep();
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
        draw_font_centered_at(
            EPAPER_WIDTH / 2,
            12,
            title,
            &EPAPER_FONT_BODY_LARGE);
    }

    if (line1 != NULL) {
        draw_font_centered_at(
            EPAPER_WIDTH / 2,
            55,
            line1,
            &EPAPER_FONT_BODY_LARGE);
    }

    if (line2 != NULL) {
        draw_font_centered_at(
            EPAPER_WIDTH / 2,
            88,
            line2,
            &EPAPER_FONT_BODY_MEDIUM);
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

    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        12,
        "PAIR DISPLAY",
        &EPAPER_FONT_BODY_LARGE);

    if (scale_id != NULL) {
        draw_font_centered_at(
            EPAPER_WIDTH / 2,
            29,
            scale_id,
            &EPAPER_FONT_BODY_SMALL);
    }

    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        48,
        "ENTER THIS CODE ON",
        &EPAPER_FONT_BODY_MEDIUM);
    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        60,
        "THE SCALE WEBPAGE",
        &EPAPER_FONT_BODY_MEDIUM);

    char code[16];
    snprintf(
        code,
        sizeof(code),
        "%03lu %03lu",
        (unsigned long)(passkey / 1000U),
        (unsigned long)(passkey % 1000U));

    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        82,
        code,
        &EPAPER_FONT_BODY_LARGE);

    return present();
}

esp_err_t display_ui_show_setup_qr(
    const char *scale_id,
    const char *ip_address)
{
    if (ip_address == NULL ||
        ip_address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    epaper_clear(false);

    char setup_url[32];
    snprintf(
        setup_url,
        sizeof(setup_url),
        "http://%s/",
        ip_address);

    esp_qrcode_config_t config =
        ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func =
        draw_setup_qrcode;
    config.max_qrcode_version = 2;
    config.qrcode_ecc_level =
        ESP_QRCODE_ECC_LOW;

    ESP_RETURN_ON_ERROR(
        esp_qrcode_generate(
            &config,
            setup_url),
        TAG,
        "Could not generate setup QR code");

    epaper_draw_text_font(
        126,
        14,
        "SCALE READY",
        &EPAPER_FONT_BODY_LARGE,
        true);
    epaper_draw_text_font(
        126,
        43,
        "SCAN TO OPEN",
        &EPAPER_FONT_BODY_SMALL,
        true);
    epaper_draw_text_font(
        126,
        56,
        "SETUP WIZARD",
        &EPAPER_FONT_BODY_SMALL,
        true);

    epaper_draw_text_font(
        126,
        78,
        ip_address,
        &EPAPER_FONT_BODY_SMALL,
        true);

    if (scale_id != NULL &&
        scale_id[0] != '\0') {
        epaper_draw_text_font(
            126,
            96,
            scale_id,
            &EPAPER_FONT_BODY_SMALL,
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
    epaper_draw_text_font(
        DISPLAY_SAFE_LEFT,
        DISPLAY_SAFE_TOP,
        "SELECT SCALE",
        &EPAPER_FONT_BODY_LARGE,
        true);

    epaper_draw_text_font(
        DISPLAY_SAFE_LEFT,
        27,
        "USE SERIAL: PAIR <ID>",
        &EPAPER_FONT_BODY_SMALL,
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

        epaper_draw_text_font(
            DISPLAY_SAFE_LEFT,
            42 + (int)i * 15,
            line,
            &EPAPER_FONT_BODY_MEDIUM,
            true);
    }

    if (count > shown) {
        epaper_draw_text_font(
            DISPLAY_SAFE_LEFT,
            42 + (int)shown * 15,
            "MORE ON SERIAL",
            &EPAPER_FONT_BODY_SMALL,
            true);
    }

    return present();
}

static void draw_hero_number(
    int center_x,
    int y,
    const char *text)
{
    draw_font_centered_at(
        center_x,
        y,
        text,
        &EPAPER_FONT_HERO);
}

static void draw_hero_percent(
    int center_x,
    int y,
    const char *digits)
{
    char text[12];
    snprintf(
        text,
        sizeof(text),
        "%s%%",
        digits);

    draw_font_centered_at(
        center_x,
        y,
        text,
        &EPAPER_FONT_HERO);
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

static const epaper_font_t *fitting_text_font(
    const char *text,
    const epaper_font_t *preferred,
    int max_width)
{
    if (preferred == &EPAPER_FONT_BODY_LARGE &&
        epaper_font_text_width(
            text,
            &EPAPER_FONT_BODY_LARGE) <= max_width) {
        return &EPAPER_FONT_BODY_LARGE;
    }

    if (preferred != &EPAPER_FONT_BODY_SMALL &&
        epaper_font_text_width(
            text,
            &EPAPER_FONT_BODY_MEDIUM) <= max_width) {
        return &EPAPER_FONT_BODY_MEDIUM;
    }

    return &EPAPER_FONT_BODY_SMALL;
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
    const epaper_font_t *preferred_font)
{
    char keg_name[
        BLE_CLIENT_KEG_NAME_MAX + 1];

    copy_keg_name(
        peer,
        state,
        keg_name,
        sizeof(keg_name));

    const bool reserve_corners =
        y <= 12;
    const int left_reserve =
        reserve_corners ?
            TOUCH_ACK_WIDTH + 6 :
            0;
    const int right_reserve =
        reserve_corners ?
            BATTERY_RESERVED_WIDTH :
            0;
    const int max_width =
        DISPLAY_SAFE_WIDTH -
        left_reserve -
        right_reserve;
    const int center_x =
        reserve_corners ?
            DISPLAY_SAFE_LEFT +
                left_reserve +
                max_width / 2 :
            EPAPER_WIDTH / 2;

    const epaper_font_t *keg_font =
        fitting_text_font(
            keg_name,
            preferred_font,
            max_width);

    while (epaper_font_text_width(
               keg_name,
               keg_font) >
           max_width) {
        const size_t length =
            strlen(keg_name);

        if (length == 0) {
            break;
        }

        keg_name[length - 1] = '\0';
    }

    draw_font_centered_at(
        center_x,
        y,
        keg_name,
        keg_font);
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
    const epaper_font_t *preferred_font)
{
    if (line == NULL ||
        line[0] == '\0') {
        return;
    }

    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        y,
        line,
        fitting_text_font(
            line,
            preferred_font,
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
            &EPAPER_FONT_BODY_LARGE);
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

    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        hero_y + 46,
        hero_label,
        fitting_text_font(
            hero_label,
            &EPAPER_FONT_BODY_LARGE,
            DISPLAY_SAFE_WIDTH));

    if (show_name &&
        !name_at_top) {
        draw_keg_name(
            peer,
            state,
            78,
            &EPAPER_FONT_BODY_LARGE);
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
        show_name ? 99 : 94,
        metrics,
        &EPAPER_FONT_BODY_MEDIUM);
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
            &EPAPER_FONT_BODY_LARGE);
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
            &EPAPER_FONT_BODY_LARGE);
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
        &EPAPER_FONT_BODY_LARGE);
}

static void draw_diagnostic_metric(
    int center_x,
    int value_y,
    const char *value,
    const char *label,
    int max_width)
{
    draw_font_centered_at(
        center_x,
        value_y,
        value,
        fitting_text_font(
            value,
            &EPAPER_FONT_BODY_LARGE,
            max_width));
    draw_font_centered_at(
        center_x,
        value_y + 15,
        label,
        &EPAPER_FONT_BODY_SMALL);
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
            &EPAPER_FONT_BODY_LARGE);
    } else {
        draw_font_centered_at(
            EPAPER_WIDTH / 2,
            11,
            "DIAGNOSTICS",
            &EPAPER_FONT_BODY_LARGE);
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

    draw_font_centered_at(
        EPAPER_WIDTH / 2,
        104,
        status,
        fitting_text_font(
            status,
            &EPAPER_FONT_BODY_SMALL,
            DISPLAY_SAFE_WIDTH));
}

esp_err_t display_ui_show_scale(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t battery_percent)
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

    draw_battery_indicator(battery_percent);
    return present();
}
