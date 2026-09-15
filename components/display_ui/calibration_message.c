#include "display_ui.h"
#include "epaper.h"

/* The window covers every drawn pixel and obeys SSD1680 Y alignment. */
enum { LEFT = 10, TOP = 8, WIDTH = 230, HEIGHT = 104 };

static void centered(int y, const char *text, const epaper_font_t *preferred)
{
    if (text == NULL || text[0] == '\0') return;
    const epaper_font_t *font = preferred;
    if (epaper_font_text_width(text, font) > WIDTH)
        font = &EPAPER_FONT_BODY_MEDIUM;
    if (epaper_font_text_width(text, font) > WIDTH)
        font = &EPAPER_FONT_BODY_SMALL;
    int width = epaper_font_text_width(text, font);
    epaper_draw_text_font((EPAPER_WIDTH - width) / 2, y, text, font, true);
}

esp_err_t display_ui_show_calibration(
    const char *heading, const char *action, const char *detail,
    const char *hint, unsigned step, bool full_refresh)
{
    esp_err_t err = epaper_init();
    if (err != ESP_OK) return err;
    if (full_refresh) epaper_clear(false);
    else epaper_fill_rect(LEFT, TOP, WIDTH, HEIGHT, false);

    centered(10, heading, &EPAPER_FONT_BODY_SMALL);
    for (unsigned i = 0; i < 4; ++i) {
        int x = 77 + (int)i * 25;
        epaper_draw_rect(x, 25, 21, 3, true);
        if (i < step) epaper_fill_rect(x, 25, 21, 3, true);
    }
    centered(39, action, &EPAPER_FONT_BODY_LARGE);
    centered(69, detail, &EPAPER_FONT_BODY_MEDIUM);
    centered(98, hint, &EPAPER_FONT_BODY_SMALL);

    err = full_refresh ? epaper_refresh() :
        epaper_refresh_partial(LEFT, TOP, WIDTH, HEIGHT);
    if (err == ESP_OK) err = epaper_sleep();
    return err;
}
