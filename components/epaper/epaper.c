#include "epaper.h"

#include <ctype.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epaper";

/* LILYGO T5 V2.3.1_2.13 published pin map. */
#define PIN_EPD_MOSI GPIO_NUM_23
#define PIN_EPD_SCLK GPIO_NUM_18
#define PIN_EPD_DC   GPIO_NUM_17
#define PIN_EPD_BUSY GPIO_NUM_4
#define PIN_EPD_RST  GPIO_NUM_16
#define PIN_EPD_CS   GPIO_NUM_5

/*
 * DEPG0213BN / GDEY0213B74 are SSD1680-class 122x250 panels.
 * The board is used in landscape orientation here: 250x122 logical pixels.
 * SSD1680 RAM is byte-aligned to 128x250, hence a 16-byte native stride.
 */
#define NATIVE_VISIBLE_WIDTH 122
#define NATIVE_HEIGHT 250
#define NATIVE_STRIDE 16
#define FRAMEBUFFER_SIZE (NATIVE_STRIDE * NATIVE_HEIGHT)

static spi_device_handle_t s_spi;
static bool s_initialized;
static uint8_t s_framebuffer[FRAMEBUFFER_SIZE];

typedef enum {
    PARTIAL_DRIVER_UNKNOWN = 0,
    PARTIAL_DRIVER_GDEM0213B74 = 1,
    PARTIAL_DRIVER_DEPG0213BN = 2,
} partial_driver_t;

RTC_DATA_ATTR static partial_driver_t s_partial_driver;

/* DEPG0213BN fast partial-update waveform from LILYGO's GxEPD2 driver. */
static const uint8_t s_depg0213bn_partial_lut[] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

static esp_err_t spi_write(const void *data, size_t length)
{
    if (length == 0) {
        return ESP_OK;
    }

    spi_transaction_t transaction = {
        .length = length * 8U,
        .tx_buffer = data,
    };

    return spi_device_transmit(
        s_spi,
        &transaction);
}

static esp_err_t send_command(uint8_t command)
{
    gpio_set_level(PIN_EPD_DC, 0);
    return spi_write(&command, 1);
}

static esp_err_t send_data(
    const void *data,
    size_t length)
{
    gpio_set_level(PIN_EPD_DC, 1);
    return spi_write(data, length);
}

static bool wait_busy(uint32_t timeout_ms)
{
    /*
     * SSD1680 asserts BUSY shortly after master activation, not necessarily
     * before the SPI transaction returns. Give it one scheduler tick to
     * assert; polling immediately can mistake an update that has not started
     * for one that has already completed and then power the panel off.
     */
    vTaskDelay(1);

    const TickType_t start =
        xTaskGetTickCount();

    while (gpio_get_level(PIN_EPD_BUSY) != 0) {
        if ((xTaskGetTickCount() - start) >=
            pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(
                TAG,
                "E-paper BUSY timeout after %u ms",
                (unsigned)timeout_ms);
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

static bool wait_busy_cycle(
    uint32_t assert_timeout_ms,
    uint32_t complete_timeout_ms)
{
    const TickType_t start =
        xTaskGetTickCount();

    while (gpio_get_level(PIN_EPD_BUSY) == 0) {
        if ((xTaskGetTickCount() - start) >=
            pdMS_TO_TICKS(assert_timeout_ms)) {
            return false;
        }

        vTaskDelay(1);
    }

    return wait_busy(complete_timeout_ms);
}

static void hardware_reset(void)
{
    gpio_set_level(PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t configure_controller(void)
{
    hardware_reset();

    ESP_RETURN_ON_ERROR(
        send_command(0x12),
        TAG,
        "Software reset failed");

    if (!wait_busy(3000)) {
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t driver_output[] = {
        0xF9, 0x00, 0x00
    };

    ESP_RETURN_ON_ERROR(
        send_command(0x01),
        TAG,
        "Driver output command failed");

    ESP_RETURN_ON_ERROR(
        send_data(
            driver_output,
            sizeof(driver_output)),
        TAG,
        "Driver output data failed");

    const uint8_t data_entry = 0x03;
    ESP_RETURN_ON_ERROR(
        send_command(0x11),
        TAG,
        "Data entry command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&data_entry, 1),
        TAG,
        "Data entry mode failed");

    const uint8_t x_range[] = {
        0x00,
        NATIVE_STRIDE - 1
    };

    ESP_RETURN_ON_ERROR(
        send_command(0x44),
        TAG,
        "X range command failed");
    ESP_RETURN_ON_ERROR(
        send_data(x_range, sizeof(x_range)),
        TAG,
        "X range failed");

    const uint8_t y_range[] = {
        0x00, 0x00,
        0xF9, 0x00
    };

    ESP_RETURN_ON_ERROR(
        send_command(0x45),
        TAG,
        "Y range command failed");
    ESP_RETURN_ON_ERROR(
        send_data(y_range, sizeof(y_range)),
        TAG,
        "Y range failed");

    const uint8_t border = 0x05;
    ESP_RETURN_ON_ERROR(
        send_command(0x3C),
        TAG,
        "Border command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&border, 1),
        TAG,
        "Border config failed");

    /*
     * Match the SSD1680 full-refresh initialization for this 122x250 panel.
     * The second byte keeps unused source outputs from leaking uninitialized
     * RAM data into the visible edge row.
     */
    const uint8_t display_update_control[] = {
        0x00, 0x80
    };

    ESP_RETURN_ON_ERROR(
        send_command(0x21),
        TAG,
        "Display update control command failed");
    ESP_RETURN_ON_ERROR(
        send_data(
            display_update_control,
            sizeof(display_update_control)),
        TAG,
        "Display update control failed");

    const uint8_t temperature_sensor = 0x80;
    ESP_RETURN_ON_ERROR(
        send_command(0x18),
        TAG,
        "Temperature sensor command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&temperature_sensor, 1),
        TAG,
        "Temperature sensor config failed");

    return ESP_OK;
}

static esp_err_t set_ram_pointer(void)
{
    const uint8_t x = 0x00;
    const uint8_t y[] = {0x00, 0x00};

    ESP_RETURN_ON_ERROR(
        send_command(0x4E),
        TAG,
        "X pointer command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&x, 1),
        TAG,
        "X pointer failed");

    ESP_RETURN_ON_ERROR(
        send_command(0x4F),
        TAG,
        "Y pointer command failed");
    ESP_RETURN_ON_ERROR(
        send_data(y, sizeof(y)),
        TAG,
        "Y pointer failed");

    return ESP_OK;
}

static esp_err_t set_partial_ram_area(
    uint8_t x_start,
    uint8_t x_end,
    uint16_t y_start,
    uint16_t y_end)
{
    const uint8_t x_range[] = {
        x_start,
        x_end,
    };
    const uint8_t y_range[] = {
        (uint8_t)(y_start & 0xFFU),
        (uint8_t)(y_start >> 8),
        (uint8_t)(y_end & 0xFFU),
        (uint8_t)(y_end >> 8),
    };
    const uint8_t y_pointer[] = {
        (uint8_t)(y_start & 0xFFU),
        (uint8_t)(y_start >> 8),
    };

    ESP_RETURN_ON_ERROR(
        send_command(0x44),
        TAG,
        "Partial X range command failed");
    ESP_RETURN_ON_ERROR(
        send_data(x_range, sizeof(x_range)),
        TAG,
        "Partial X range failed");
    ESP_RETURN_ON_ERROR(
        send_command(0x45),
        TAG,
        "Partial Y range command failed");
    ESP_RETURN_ON_ERROR(
        send_data(y_range, sizeof(y_range)),
        TAG,
        "Partial Y range failed");
    ESP_RETURN_ON_ERROR(
        send_command(0x4E),
        TAG,
        "Partial X pointer command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&x_start, 1),
        TAG,
        "Partial X pointer failed");
    ESP_RETURN_ON_ERROR(
        send_command(0x4F),
        TAG,
        "Partial Y pointer command failed");
    ESP_RETURN_ON_ERROR(
        send_data(y_pointer, sizeof(y_pointer)),
        TAG,
        "Partial Y pointer failed");

    return ESP_OK;
}

static esp_err_t write_partial_plane(
    uint8_t command,
    uint8_t x_start,
    uint8_t x_end,
    uint16_t y_start,
    uint16_t y_end)
{
    ESP_RETURN_ON_ERROR(
        set_partial_ram_area(
            x_start,
            x_end,
            y_start,
            y_end),
        TAG,
        "Could not set partial RAM area");

    ESP_RETURN_ON_ERROR(
        send_command(command),
        TAG,
        "Partial RAM write command failed");

    const size_t row_bytes =
        (size_t)(x_end - x_start + 1U);

    for (uint16_t native_y = y_start;
         native_y <= y_end;
         ++native_y) {
        const uint8_t *row =
            &s_framebuffer[
                (size_t)native_y *
                    NATIVE_STRIDE +
                x_start];

        ESP_RETURN_ON_ERROR(
            send_data(row, row_bytes),
            TAG,
            "Partial RAM row transfer failed");
    }

    return ESP_OK;
}

static bool glyph_rows(
    char value,
    uint8_t rows[7])
{
    if (value >= 'a' && value <= 'z') {
        value = (char)toupper((unsigned char)value);
    }

#define GLYPH(a,b,c,d,e,f,g) do {     rows[0]=(a); rows[1]=(b); rows[2]=(c); rows[3]=(d);     rows[4]=(e); rows[5]=(f); rows[6]=(g); return true; } while (0)

    switch (value) {
        case ' ': GLYPH(0,0,0,0,0,0,0);
        case '0': GLYPH(14,17,19,21,25,17,14);
        case '1': GLYPH(4,12,4,4,4,4,14);
        case '2': GLYPH(14,17,1,2,4,8,31);
        case '3': GLYPH(30,1,1,14,1,1,30);
        case '4': GLYPH(2,6,10,18,31,2,2);
        case '5': GLYPH(31,16,16,30,1,1,30);
        case '6': GLYPH(14,16,16,30,17,17,14);
        case '7': GLYPH(31,1,2,4,8,8,8);
        case '8': GLYPH(14,17,17,14,17,17,14);
        case '9': GLYPH(14,17,17,15,1,1,14);
        case 'A': GLYPH(14,17,17,31,17,17,17);
        case 'B': GLYPH(30,17,17,30,17,17,30);
        case 'C': GLYPH(14,17,16,16,16,17,14);
        case 'D': GLYPH(30,17,17,17,17,17,30);
        case 'E': GLYPH(31,16,16,30,16,16,31);
        case 'F': GLYPH(31,16,16,30,16,16,16);
        case 'G': GLYPH(14,17,16,23,17,17,15);
        case 'H': GLYPH(17,17,17,31,17,17,17);
        case 'I': GLYPH(14,4,4,4,4,4,14);
        case 'J': GLYPH(7,2,2,2,18,18,12);
        case 'K': GLYPH(17,18,20,24,20,18,17);
        case 'L': GLYPH(16,16,16,16,16,16,31);
        case 'M': GLYPH(17,27,21,21,17,17,17);
        case 'N': GLYPH(17,25,21,19,17,17,17);
        case 'O': GLYPH(14,17,17,17,17,17,14);
        case 'P': GLYPH(30,17,17,30,16,16,16);
        case 'Q': GLYPH(14,17,17,17,21,18,13);
        case 'R': GLYPH(30,17,17,30,20,18,17);
        case 'S': GLYPH(15,16,16,14,1,1,30);
        case 'T': GLYPH(31,4,4,4,4,4,4);
        case 'U': GLYPH(17,17,17,17,17,17,14);
        case 'V': GLYPH(17,17,17,17,17,10,4);
        case 'W': GLYPH(17,17,17,17,21,27,17);
        case 'X': GLYPH(17,17,10,4,10,17,17);
        case 'Y': GLYPH(17,17,10,4,4,4,4);
        case 'Z': GLYPH(31,1,2,4,8,16,31);
        case '.': GLYPH(0,0,0,0,0,12,12);
        case ':': GLYPH(0,12,12,0,12,12,0);
        case '-': GLYPH(0,0,0,31,0,0,0);
        case '/': GLYPH(1,1,2,4,8,16,16);
        case '%': GLYPH(17,2,4,8,16,0,17);
        case '?': GLYPH(14,17,1,2,4,0,4);
        default:
            return false;
    }

#undef GLYPH
}

esp_err_t epaper_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t outputs = {
        .pin_bit_mask =
            (1ULL << PIN_EPD_DC) |
            (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&outputs),
        TAG,
        "Could not configure e-paper outputs");

    gpio_config_t busy = {
        .pin_bit_mask =
            (1ULL << PIN_EPD_BUSY),
        .mode = GPIO_MODE_INPUT,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&busy),
        TAG,
        "Could not configure e-paper BUSY");

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_EPD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz =
            FRAMEBUFFER_SIZE,
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(
            SPI2_HOST,
            &bus,
            SPI_DMA_CH_AUTO),
        TAG,
        "Could not initialize e-paper SPI bus");

    spi_device_interface_config_t device = {
        .clock_speed_hz = 4000000,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        .queue_size = 1,
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_add_device(
            SPI2_HOST,
            &device,
            &s_spi),
        TAG,
        "Could not add e-paper SPI device");

    s_initialized = true;
    epaper_clear(false);

    return configure_controller();
}

void epaper_clear(bool black)
{
    memset(
        s_framebuffer,
        black ? 0x00 : 0xFF,
        sizeof(s_framebuffer));
}

void epaper_set_pixel(
    int x,
    int y,
    bool black)
{
    if (x < 0 ||
        x >= EPAPER_WIDTH ||
        y < 0 ||
        y >= EPAPER_HEIGHT) {
        return;
    }

    /*
     * Rotate landscape coordinates into native 122x250 controller RAM.
     */
    const int native_x = y;
    const int native_y =
        (NATIVE_HEIGHT - 1) - x;

    if (native_x < 0 ||
        native_x >= NATIVE_VISIBLE_WIDTH ||
        native_y < 0 ||
        native_y >= NATIVE_HEIGHT) {
        return;
    }

    const size_t index =
        (size_t)native_y *
            NATIVE_STRIDE +
        (size_t)native_x / 8U;

    const uint8_t mask =
        (uint8_t)(
            0x80U >>
            (native_x & 7));

    if (black) {
        s_framebuffer[index] &=
            (uint8_t)~mask;
    } else {
        s_framebuffer[index] |= mask;
    }
}

void epaper_fill_rect(
    int x,
    int y,
    int width,
    int height,
    bool black)
{
    for (int py = y;
         py < y + height;
         ++py) {
        for (int px = x;
             px < x + width;
             ++px) {
            epaper_set_pixel(
                px,
                py,
                black);
        }
    }
}

void epaper_draw_rect(
    int x,
    int y,
    int width,
    int height,
    bool black)
{
    epaper_fill_rect(
        x,
        y,
        width,
        1,
        black);
    epaper_fill_rect(
        x,
        y + height - 1,
        width,
        1,
        black);
    epaper_fill_rect(
        x,
        y,
        1,
        height,
        black);
    epaper_fill_rect(
        x + width - 1,
        y,
        1,
        height,
        black);
}

void epaper_draw_text(
    int x,
    int y,
    const char *text,
    int scale,
    bool black)
{
    if (text == NULL ||
        scale <= 0) {
        return;
    }

    int cursor = x;

    for (const char *p = text;
         *p != '\0';
         ++p) {
        uint8_t rows[7] = {0};

        if (!glyph_rows(*p, rows)) {
            glyph_rows('?', rows);
        }

        for (int row = 0;
             row < 7;
             ++row) {
            for (int col = 0;
                 col < 5;
                 ++col) {
                if ((rows[row] &
                     (1U << (4 - col))) == 0) {
                    continue;
                }

                epaper_fill_rect(
                    cursor + col * scale,
                    y + row * scale,
                    scale,
                    scale,
                    black);
            }
        }

        cursor += 6 * scale;
    }
}

int epaper_text_width(
    const char *text,
    int scale)
{
    if (text == NULL ||
        scale <= 0) {
        return 0;
    }

    const size_t length = strlen(text);

    if (length == 0) {
        return 0;
    }

    return (int)(
        length * 6U * (size_t)scale -
        (size_t)scale);
}

static const epaper_glyph_t *font_glyph(
    const epaper_font_t *font,
    char value)
{
    uint8_t codepoint = (uint8_t)value;

    if (codepoint >= (uint8_t)'a' &&
        codepoint <= (uint8_t)'z') {
        codepoint =
            (uint8_t)toupper(codepoint);
    }

    if (codepoint < font->first ||
        codepoint > font->last) {
        codepoint = (uint8_t)'?';
    }

    if (codepoint < font->first ||
        codepoint > font->last) {
        return NULL;
    }

    const epaper_glyph_t *glyph =
        &font->glyphs[codepoint - font->first];

    if (glyph->x_advance == 0 &&
        glyph->width == 0) {
        return NULL;
    }

    return glyph;
}

void epaper_draw_text_font(
    int x,
    int y,
    const char *text,
    const epaper_font_t *font,
    bool black)
{
    if (text == NULL || font == NULL) {
        return;
    }

    int cursor = x;

    for (const char *p = text;
         *p != '\0';
         ++p) {
        const epaper_glyph_t *glyph =
            font_glyph(font, *p);

        if (glyph == NULL) {
            continue;
        }

        const uint8_t *bitmap =
            font->bitmap + glyph->bitmap_offset;

        for (int row = 0;
             row < glyph->height;
             ++row) {
            for (int col = 0;
                 col < glyph->width;
                 ++col) {
                const unsigned bit_index =
                    (unsigned)row * glyph->width +
                    (unsigned)col;

                if ((bitmap[bit_index / 8U] &
                     (0x80U >> (bit_index & 7U))) == 0) {
                    continue;
                }

                epaper_set_pixel(
                    cursor + glyph->x_offset + col,
                    y + glyph->y_offset + row,
                    black);
            }
        }

        cursor += glyph->x_advance;
    }
}

int epaper_font_text_width(
    const char *text,
    const epaper_font_t *font)
{
    if (text == NULL || font == NULL) {
        return 0;
    }

    int width = 0;

    for (const char *p = text;
         *p != '\0';
         ++p) {
        const epaper_glyph_t *glyph =
            font_glyph(font, *p);

        if (glyph != NULL) {
            width += glyph->x_advance;
        }
    }

    return width;
}

esp_err_t epaper_refresh(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        configure_controller(),
        TAG,
        "Controller re-init failed");

    ESP_RETURN_ON_ERROR(
        set_ram_pointer(),
        TAG,
        "Could not set RAM pointer");

    /*
     * A full SSD1680 refresh consumes both the previous (0x26) and current
     * (0x24) RAM planes. Initialize both so a cold boot cannot expose random
     * controller RAM as dots along the panel edge.
     */
    ESP_RETURN_ON_ERROR(
        send_command(0x26),
        TAG,
        "Write previous RAM command failed");

    ESP_RETURN_ON_ERROR(
        send_data(
            s_framebuffer,
            sizeof(s_framebuffer)),
        TAG,
        "Previous framebuffer transfer failed");

    ESP_RETURN_ON_ERROR(
        set_ram_pointer(),
        TAG,
        "Could not reset RAM pointer");

    ESP_RETURN_ON_ERROR(
        send_command(0x24),
        TAG,
        "Write current RAM command failed");

    ESP_RETURN_ON_ERROR(
        send_data(
            s_framebuffer,
            sizeof(s_framebuffer)),
        TAG,
        "Current framebuffer transfer failed");

    const uint8_t update = 0xF7;

    ESP_RETURN_ON_ERROR(
        send_command(0x22),
        TAG,
        "Display update command failed");

    ESP_RETURN_ON_ERROR(
        send_data(&update, 1),
        TAG,
        "Display update config failed");

    ESP_RETURN_ON_ERROR(
        send_command(0x20),
        TAG,
        "Master activation failed");

    if (!wait_busy(8000)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "E-paper refresh complete");
    return ESP_OK;
}

static esp_err_t refresh_partial_depg0213bn(
    uint8_t native_x_start,
    uint8_t native_x_end,
    uint16_t native_y_start,
    uint16_t native_y_end)
{
    /*
     * LILYGO ships the V2.3.1 board with either a GDEM0213B74 or the
     * DEPG0213BN panel. The BN is the vendor's default and needs its partial
     * LUT plus separate power-on and partial-update commands.
     */
    ESP_RETURN_ON_ERROR(
        configure_controller(),
        TAG,
        "Could not initialize DEPG0213BN partial mode");

    ESP_RETURN_ON_ERROR(
        write_partial_plane(
            0x24,
            native_x_start,
            native_x_end,
            native_y_start,
            native_y_end),
        TAG,
        "DEPG0213BN partial framebuffer transfer failed");

    ESP_RETURN_ON_ERROR(
        send_command(0x32),
        TAG,
        "DEPG0213BN LUT command failed");
    ESP_RETURN_ON_ERROR(
        send_data(
            s_depg0213bn_partial_lut,
            sizeof(s_depg0213bn_partial_lut)),
        TAG,
        "DEPG0213BN LUT transfer failed");

    const uint8_t power_on = 0xF8;

    ESP_RETURN_ON_ERROR(
        send_command(0x22),
        TAG,
        "DEPG0213BN power-on command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&power_on, 1),
        TAG,
        "DEPG0213BN power-on config failed");
    ESP_RETURN_ON_ERROR(
        send_command(0x20),
        TAG,
        "DEPG0213BN power-on activation failed");

    if (!wait_busy_cycle(100, 2000)) {
        ESP_LOGW(TAG, "DEPG0213BN power-on did not assert BUSY");
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t update = 0xCC;

    ESP_RETURN_ON_ERROR(
        send_command(0x22),
        TAG,
        "DEPG0213BN partial update command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&update, 1),
        TAG,
        "DEPG0213BN partial update config failed");
    ESP_RETURN_ON_ERROR(
        send_command(0x20),
        TAG,
        "DEPG0213BN partial activation failed");

    if (!wait_busy_cycle(100, 2500)) {
        ESP_LOGW(TAG, "DEPG0213BN partial update did not assert BUSY");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t epaper_refresh_partial(
    int x,
    int y,
    int width,
    int height)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (x < 0 ||
        y < 0 ||
        width <= 0 ||
        height <= 0 ||
        x + width > EPAPER_WIDTH ||
        y + height > EPAPER_HEIGHT ||
        (y & 7) != 0 ||
        (height & 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Landscape Y maps to the native byte-aligned X axis. Landscape X maps
     * in reverse order onto native Y.
     */
    const uint8_t native_x_start =
        (uint8_t)(y / 8);
    const uint8_t native_x_end =
        (uint8_t)((y + height - 1) / 8);
    const uint16_t native_y_start =
        (uint16_t)(
            NATIVE_HEIGHT -
            (x + width));
    const uint16_t native_y_end =
        (uint16_t)(
            NATIVE_HEIGHT -
            1 -
            x);

    if (s_partial_driver ==
        PARTIAL_DRIVER_DEPG0213BN) {
        ESP_RETURN_ON_ERROR(
            refresh_partial_depg0213bn(
                native_x_start,
                native_x_end,
                native_y_start,
                native_y_end),
            TAG,
            "DEPG0213BN partial refresh failed");
    } else {
        ESP_RETURN_ON_ERROR(
            write_partial_plane(
                0x24,
                native_x_start,
                native_x_end,
                native_y_start,
                native_y_end),
            TAG,
            "Current partial framebuffer transfer failed");

        const uint8_t update = 0xFC;

        ESP_RETURN_ON_ERROR(
            send_command(0x22),
            TAG,
            "GDEM0213B74 partial update command failed");
        ESP_RETURN_ON_ERROR(
            send_data(&update, 1),
            TAG,
            "GDEM0213B74 partial update config failed");
        ESP_RETURN_ON_ERROR(
            send_command(0x20),
            TAG,
            "GDEM0213B74 partial activation failed");

        if (wait_busy_cycle(100, 2000)) {
            s_partial_driver =
                PARTIAL_DRIVER_GDEM0213B74;
        } else {
            ESP_LOGW(
                TAG,
                "GDEM0213B74 partial command was ignored; retrying with LILYGO's default DEPG0213BN waveform");

            s_partial_driver =
                PARTIAL_DRIVER_DEPG0213BN;

            ESP_RETURN_ON_ERROR(
                refresh_partial_depg0213bn(
                    native_x_start,
                    native_x_end,
                    native_y_start,
                    native_y_end),
                TAG,
                "DEPG0213BN fallback refresh failed");
        }
    }

    /*
     * Differential refresh compares current RAM (0x24) with previous RAM
     * (0x26). Synchronize the completed region so the acknowledgement can be
     * cleanly removed by the next partial update.
     */
    ESP_RETURN_ON_ERROR(
        write_partial_plane(
            0x26,
            native_x_start,
            native_x_end,
            native_y_start,
            native_y_end),
        TAG,
        "Previous partial framebuffer transfer failed");

    ESP_RETURN_ON_ERROR(
        write_partial_plane(
            0x24,
            native_x_start,
            native_x_end,
            native_y_start,
            native_y_end),
        TAG,
        "Current partial framebuffer synchronization failed");

    const uint8_t power_off = 0x83;

    ESP_RETURN_ON_ERROR(
        send_command(0x22),
        TAG,
        "Partial power-off command failed");
    ESP_RETURN_ON_ERROR(
        send_data(&power_off, 1),
        TAG,
        "Partial power-off config failed");
    ESP_RETURN_ON_ERROR(
        send_command(0x20),
        TAG,
        "Partial power-off activation failed");

    if (!wait_busy(2000)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(
        TAG,
        "Partial refresh complete (%s): x=%d y=%d w=%d h=%d",
        s_partial_driver ==
                PARTIAL_DRIVER_DEPG0213BN ?
            "DEPG0213BN" :
            "GDEM0213B74",
        x,
        y,
        width,
        height);

    return ESP_OK;
}

esp_err_t epaper_sleep(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t deep_sleep = 0x01;

    ESP_RETURN_ON_ERROR(
        send_command(0x10),
        TAG,
        "Deep sleep command failed");

    ESP_RETURN_ON_ERROR(
        send_data(&deep_sleep, 1),
        TAG,
        "Deep sleep data failed");

    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}
