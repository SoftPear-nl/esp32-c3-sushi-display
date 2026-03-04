#include "gc9a01.h"

#include <stdio.h>
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GC9A01";

static spi_device_handle_t s_spi;
static int s_dc_pin;
static int s_rst_pin;

/* ------------------------------------------------------------------ */
/*  Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static void send_cmd(uint8_t cmd)
{
    gpio_set_level(s_dc_pin, 0);
    spi_transaction_t t = {
        .length   = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(s_dc_pin, 1);
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static inline void send_byte(uint8_t b) { send_data(&b, 1); }

/* ------------------------------------------------------------------ */
/*  Init sequence (standard GC9A01 init)                               */
/* ------------------------------------------------------------------ */

static void gc9a01_reset(void)
{
    if (s_rst_pin < 0) {
        vTaskDelay(pdMS_TO_TICKS(120));
        return;
    }
    gpio_set_level(s_rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(s_rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void gc9a01_run_init_sequence(void)
{
    send_cmd(0xEF);
    send_cmd(0xEB); send_byte(0x14);

    send_cmd(0xFE);
    send_cmd(0xEF);

    send_cmd(0xEB); send_byte(0x14);
    send_cmd(0x84); send_byte(0x40);
    send_cmd(0x85); send_byte(0xFF);
    send_cmd(0x86); send_byte(0xFF);
    send_cmd(0x87); send_byte(0xFF);
    send_cmd(0x88); send_byte(0x0A);
    send_cmd(0x89); send_byte(0x21);
    send_cmd(0x8A); send_byte(0x00);
    send_cmd(0x8B); send_byte(0x80);
    send_cmd(0x8C); send_byte(0x01);
    send_cmd(0x8D); send_byte(0x01);
    send_cmd(0x8E); send_byte(0xFF);
    send_cmd(0x8F); send_byte(0xFF);

    send_cmd(0xB6);
    send_byte(0x00);
    send_byte(0x20);

    send_cmd(0x36); send_byte(0x08);   // MADCTL  – BGR bit set, no mirroring

    send_cmd(0x3A); send_byte(0x05);   // COLMOD  – RGB565

    send_cmd(0x90);
    send_byte(0x08); send_byte(0x08); send_byte(0x08); send_byte(0x08);

    send_cmd(0xBD); send_byte(0x06);
    send_cmd(0xBC); send_byte(0x00);

    send_cmd(0xFF);
    send_byte(0x60); send_byte(0x01); send_byte(0x04);

    send_cmd(0xC3); send_byte(0x13);
    send_cmd(0xC4); send_byte(0x13);
    send_cmd(0xC9); send_byte(0x22);
    send_cmd(0xBE); send_byte(0x11);

    send_cmd(0xE1);
    send_byte(0x10); send_byte(0x0E);

    send_cmd(0xDF);
    send_byte(0x21); send_byte(0x0C); send_byte(0x02);

    send_cmd(0xF0);
    send_byte(0x45); send_byte(0x09); send_byte(0x08);
    send_byte(0x08); send_byte(0x26); send_byte(0x2A);

    send_cmd(0xF1);
    send_byte(0x43); send_byte(0x70); send_byte(0x72);
    send_byte(0x36); send_byte(0x37); send_byte(0x6F);

    send_cmd(0xF2);
    send_byte(0x45); send_byte(0x09); send_byte(0x08);
    send_byte(0x08); send_byte(0x26); send_byte(0x2A);

    send_cmd(0xF3);
    send_byte(0x43); send_byte(0x70); send_byte(0x72);
    send_byte(0x36); send_byte(0x37); send_byte(0x6F);

    send_cmd(0xED);
    send_byte(0x1B); send_byte(0x0B);

    send_cmd(0xAE); send_byte(0x77);
    send_cmd(0xCD); send_byte(0x63);

    send_cmd(0x70);
    send_byte(0x07); send_byte(0x07); send_byte(0x04);
    send_byte(0x0E); send_byte(0x0F); send_byte(0x09);
    send_byte(0x07); send_byte(0x08); send_byte(0x03);

    send_cmd(0xE8); send_byte(0x34);

    send_cmd(0x62);
    send_byte(0x18); send_byte(0x0D); send_byte(0x71);
    send_byte(0xED); send_byte(0x70); send_byte(0x70);
    send_byte(0x18); send_byte(0x0F); send_byte(0x71);
    send_byte(0xEF); send_byte(0x70); send_byte(0x70);

    send_cmd(0x63);
    send_byte(0x18); send_byte(0x11); send_byte(0x71);
    send_byte(0xF1); send_byte(0x70); send_byte(0x70);
    send_byte(0x18); send_byte(0x13); send_byte(0x71);
    send_byte(0xF3); send_byte(0x70); send_byte(0x70);

    send_cmd(0x64);
    send_byte(0x28); send_byte(0x29); send_byte(0xF1);
    send_byte(0x01); send_byte(0xF1); send_byte(0x00);
    send_byte(0x07);

    send_cmd(0x66);
    send_byte(0x3C); send_byte(0x00); send_byte(0xCD);
    send_byte(0x67); send_byte(0x45); send_byte(0x45);
    send_byte(0x10); send_byte(0x00); send_byte(0x00);
    send_byte(0x00);

    send_cmd(0x67);
    send_byte(0x00); send_byte(0x3C); send_byte(0x00);
    send_byte(0x00); send_byte(0x00); send_byte(0x01);
    send_byte(0x54); send_byte(0x10); send_byte(0x32);
    send_byte(0x98);

    send_cmd(0x74);
    send_byte(0x10); send_byte(0x85); send_byte(0x80);
    send_byte(0x00); send_byte(0x00); send_byte(0x4E);
    send_byte(0x00);

    send_cmd(0x98);
    send_byte(0x3E); send_byte(0x07);

    send_cmd(0x35);             // Tearing Effect Line ON
    send_cmd(0x21);             // Display Inversion ON

    send_cmd(0x11);             // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));

    send_cmd(0x29);             // Display ON
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t gc9a01_init(int mosi, int clk, int cs, int dc, int rst)
{
    s_dc_pin  = dc;
    s_rst_pin = rst;

    ESP_LOGI(TAG, "Initialising GC9A01 (MOSI=%d CLK=%d CS=%d DC=%d RST=%d)",
             mosi, clk, cs, dc, rst);

    /* Configure DC and (optionally) RST as digital outputs */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << dc) | (rst >= 0 ? (1ULL << rst) : 0ULL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = mosi,
        .miso_io_num     = -1,
        .sclk_io_num     = clk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = GC9A01_WIDTH * GC9A01_HEIGHT * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* SPI device */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,   // 40 MHz
        .mode           = 0,
        .spics_io_num   = cs,
        .queue_size     = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    gc9a01_reset();
    gc9a01_run_init_sequence();

    ESP_LOGI(TAG, "GC9A01 ready");
    return ESP_OK;
}

void gc9a01_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];

    send_cmd(0x2A);                     // Column Address Set
    d[0] = (x0 >> 8); d[1] = x0 & 0xFF;
    d[2] = (x1 >> 8); d[3] = x1 & 0xFF;
    send_data(d, 4);

    send_cmd(0x2B);                     // Row Address Set
    d[0] = (y0 >> 8); d[1] = y0 & 0xFF;
    d[2] = (y1 >> 8); d[3] = y1 & 0xFF;
    send_data(d, 4);

    send_cmd(0x2C);                     // Memory Write
}

void gc9a01_fill_color(uint16_t color)
{
    gc9a01_set_window(0, 0, GC9A01_WIDTH - 1, GC9A01_HEIGHT - 1);

    /* Build one line's worth of pixels in a DMA-capable buffer */
    const int LINE_PIXELS = GC9A01_WIDTH;
    uint8_t *buf = heap_caps_malloc(LINE_PIXELS * 2, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        return;
    }

    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    for (int i = 0; i < LINE_PIXELS; i++) {
        buf[i * 2]     = hi;
        buf[i * 2 + 1] = lo;
    }

    gpio_set_level(s_dc_pin, 1);
    spi_transaction_t t = {
        .length    = LINE_PIXELS * 2 * 8,
        .tx_buffer = buf,
    };
    for (int row = 0; row < GC9A01_HEIGHT; row++) {
        spi_device_polling_transmit(s_spi, &t);
    }

    heap_caps_free(buf);
}

esp_err_t gc9a01_draw_bitmap_spiffs(const char *path, uint16_t x, uint16_t y)
{
    // Auto-mount SPIFFS if not already mounted
    if (!esp_spiffs_mounted(NULL)) {
        esp_vfs_spiffs_conf_t spiffs_conf = {
            .base_path              = "/spiffs",
            .partition_label        = NULL,
            .max_files              = 4,
            .format_if_mount_failed = false,
        };
        esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // --- Parse BMP headers (14-byte file header + 40-byte DIB header) ---
    uint8_t hdr[54];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr[0] != 'B' || hdr[1] != 'M') {
        ESP_LOGE(TAG, "%s: not a BMP", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pixel_offset = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                            ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t  bmp_w  = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) |
                                ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t  bmp_h  = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
                                ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    uint16_t bpp    = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);

    if (bpp != 16) {
        ESP_LOGE(TAG, "%s: only 16-bpp RGB565 BMP supported (got %d bpp)", path, bpp);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Negative height means top-down storage (no row reversal needed)
    bool    top_down  = (bmp_h < 0);
    int32_t img_w     = bmp_w;
    int32_t img_h     = top_down ? -bmp_h : bmp_h;
    int32_t row_bytes = (img_w * 2 + 3) & ~3;  // rows padded to 4-byte boundary

    // Clip to screen
    int32_t draw_w = img_w;
    int32_t draw_h = img_h;
    if (x + draw_w > GC9A01_WIDTH)  draw_w = (int32_t)GC9A01_WIDTH  - x;
    if (y + draw_h > GC9A01_HEIGHT) draw_h = (int32_t)GC9A01_HEIGHT - y;
    if (draw_w <= 0 || draw_h <= 0) {
        fclose(f);
        return ESP_OK;  // fully off-screen
    }

    uint8_t *row_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (!row_buf) {
        ESP_LOGE(TAG, "Row buffer alloc failed");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    gc9a01_set_window(x, y, x + (uint16_t)draw_w - 1, y + (uint16_t)draw_h - 1);
    gpio_set_level(s_dc_pin, 1);

    for (int32_t row = 0; row < draw_h; row++) {
        // For bottom-up BMPs seek to the correct row; top-down files are sequential
        int32_t file_row = top_down ? row : (img_h - 1 - row);
        fseek(f, (long)(pixel_offset + (uint32_t)(file_row * row_bytes)), SEEK_SET);
        fread(row_buf, 1, (size_t)row_bytes, f);

        // Byte-swap each pixel: BMP is little-endian, display expects big-endian
        for (int32_t px = 0; px < draw_w; px++) {
            uint8_t lo = row_buf[px * 2];
            uint8_t hi = row_buf[px * 2 + 1];
            row_buf[px * 2]     = hi;
            row_buf[px * 2 + 1] = lo;
        }

        spi_transaction_t t = {
            .length    = (size_t)draw_w * 2 * 8,
            .tx_buffer = row_buf,
        };
        spi_device_polling_transmit(s_spi, &t);
    }

    heap_caps_free(row_buf);
    fclose(f);
    ESP_LOGI(TAG, "Drew %s (%ldx%ld) at (%d,%d)", path, img_w, img_h, x, y);
    return ESP_OK;
}
