#include "display_ui.h"

#include <stdbool.h>
#include <string.h>

#include "epaper.h"
#include "esp_check.h"

static const char *TAG = "display_ui_cal";

/*
 * display_ui.c is compiled with display_ui_show_message renamed to
 * display_ui_show_message_original. Keep ordinary status screens on that
 * implementation, while calibration uses a faster partial-refresh path.
 */
esp_err_t display_ui_show_message_original(
    const char *title,
    const char *line1,
    const char *line2);

enum {
    CAL_SAFE_LEFT = 10,
    CAL_SAFE_TOP = 10,
    CAL_SAFE_RIGHT = 240,
    CAL_SAFE_BOTTOM = 112,
    CAL_SAFE_WIDTH = CAL_SAFE_RIGHT - CAL_SAFE_LEFT,
    CAL_SAFE_HEIGHT = CAL_SAFE_BOTTOM - CAL_SAFE_TOP,
};

static bool s_calibration_active;

static const epaper_font_t *fit_font(
    const char *text,
    const epaper_font_t *preferred)
{
    if (text == NULL) {
        return preferred;
    }

    if (preferred == &EPAPER_FONT_BODY_LARGE &&
        epaper_font_text_width(
            text,
            &EPAPER_FONT_BODY_LARGE) <= CAL_SAFE_WIDTH) {
        return &EPAPER_FONT_BODY_LARGE;
    }

    if (preferred != &EPAPER_FONT_BODY_SMALL &&
        epaper_font_text_width(
            text,
            &EPAPER_FONT_BODY_MEDIUM) <= CAL_SAFE_WIDTH) {
        return &EPAPER_FONT_BODY_MEDIUM;
    }

    return &EPAPER_FONT_BODY_SMALL;
}

static void draw_centered_fitted(
    int y,
    const char *text,
    const epaper_font_t *preferred)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    const epaper_font_t *font =
        fit_font(text, preferred);
    const int width =
        epaper_font_text_width(text, font);
    int x = EPAPER_WIDTH / 2 - width / 2;

    if (x < CAL_SAFE_LEFT) {
        x = CAL_SAFE_LEFT;
    }

    if (width <= CAL_SAFE_WIDTH &&
        x + width > CAL_SAFE_RIGHT) {
        x = CAL_SAFE_RIGHT - width;
    }

    epaper_draw_text_font(
        x,
        y,
        text,
        font,
        true);
}

static bool calibration_complete_title(
    const char *title)
{
    return title != NULL &&
        (strcmp(title, "CALIBRATION PASSED") == 0 ||
         strcmp(title, "CALIBRATION FAILED") == 0);
}

esp_err_t display_ui_show_message(
    const char *title,
    const char *line1,
    const char *line2)
{
    const bool calibration_start =
        title != NULL &&
        strcmp(title, "TOUCH CALIBRATION") == 0;
    const bool calibration_resume =
        title != NULL &&
        strcmp(title, "STEP 1 OF 4") == 0;

    if (calibration_start ||
        calibration_resume) {
        s_calibration_active = true;
    } else if (!s_calibration_active) {
        return display_ui_show_message_original(
            title,
            line1,
            line2);
    }

    const bool partial_refresh =
        s_calibration_active &&
        !calibration_start;
    const bool calibration_complete =
        calibration_complete_title(title);

    const char *shown_line1 = line1;
    const char *shown_line2 = line2;

    /*
     * A calibration touch is a deliberate touch/release gesture. The sensor
     * still confirms the touched state before the release step is shown.
     */
    if (shown_line1 != NULL &&
        strcmp(shown_line1, "TOUCH AND HOLD") == 0) {
        shown_line1 = "TOUCH AND RELEASE";
    }

    if (shown_line1 != NULL &&
        strcmp(shown_line1, "RELEASE TOUCH") == 0) {
        shown_line1 = "TOUCH REGISTERED";
    }

    if (shown_line2 != NULL &&
        strcmp(shown_line2, "WAIT FOR NEXT STEP") == 0) {
        shown_line2 = "PLEASE RELEASE NOW";
    }

    ESP_RETURN_ON_ERROR(
        epaper_init(),
        TAG,
        "E-paper init failed");

    if (partial_refresh) {
        epaper_fill_rect(
            CAL_SAFE_LEFT,
            CAL_SAFE_TOP,
            CAL_SAFE_WIDTH,
            CAL_SAFE_HEIGHT,
            false);
    } else {
        /* One full refresh at calibration entry gives partials a clean base. */
        epaper_clear(false);
    }

    draw_centered_fitted(
        12,
        title,
        &EPAPER_FONT_BODY_LARGE);
    draw_centered_fitted(
        55,
        shown_line1,
        &EPAPER_FONT_BODY_LARGE);
    draw_centered_fitted(
        88,
        shown_line2,
        &EPAPER_FONT_BODY_MEDIUM);

    esp_err_t err;

    if (partial_refresh) {
        err = epaper_refresh_partial(
            CAL_SAFE_LEFT,
            CAL_SAFE_TOP,
            CAL_SAFE_WIDTH,
            CAL_SAFE_HEIGHT);
    } else {
        err = epaper_refresh();
    }

    if (err == ESP_OK) {
        err = epaper_sleep();
    }

    if (calibration_complete) {
        s_calibration_active = false;
    }

    return err;
}
