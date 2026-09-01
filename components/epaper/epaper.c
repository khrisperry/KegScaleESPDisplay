#include "epaper.h"

#include <ctype.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
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

    wait_busy(3000);

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

    ESP_RETURN_ON_ERROR(
        send_command(0x24),
        TAG,
        "Write RAM command failed");

    ESP_RETURN_ON_ERROR(
        send_data(
            s_framebuffer,
            sizeof(s_framebuffer)),
        TAG,
        "Framebuffer transfer failed");

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

    wait_busy(8000);

    ESP_LOGI(TAG, "E-paper refresh complete");
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
