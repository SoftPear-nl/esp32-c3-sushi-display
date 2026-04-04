#include "esp_log.h"
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"

#include "esp_spiffs.h"
#include <dirent.h>
#include <sys/stat.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "neon_letter_service.h"
#include "display_sequence.h"
#include "font.h"

static const char *TAG = "main";

static const char *s_led_mode_names[] = {
    [LED_MODE_ALL_PATTERNS] = "ALL_PATTERNS",
    [LED_MODE_ON]           = "ON",
    [LED_MODE_OFF]          = "OFF",
};

// -------------------- Display mode --------------------
typedef enum {
    DISPLAY_MODE_NORMAL = 0,
    DISPLAY_MODE_WIFI,
    DISPLAY_MODE_COUNT,
} display_mode_t;

static volatile display_mode_t s_display_mode = DISPLAY_MODE_NORMAL;

// Position of the kirb_walk overlay (top-left corner, in screen pixels).
// Initialised to centred when the animation loads; adjusted by the 5-way switch.
static volatile int s_walk_x = 0;
static volatile int s_walk_y = 0;
static volatile int s_walk_dir = 1; // 1 = right (default), -1 = left (mirrored)

static const char *s_display_mode_names[] = {
    [DISPLAY_MODE_NORMAL] = "NORMAL",
    [DISPLAY_MODE_WIFI]   = "WIFI",
};

// -------------------- Display config --------------------
// ESP32-S3 N16R8: two independent SPI buses, one per screen
#define LCD_HOST               SPI2_HOST   // Screen 1
#define LCD_HOST2              SPI3_HOST   // Screen 2

#define LCD_H_RES              240
#define LCD_V_RES              320
#define LCD_Y_OFFSET           0
#define LCD_X_OFFSET           0

// Panel color order: set to 1 for RGB, 0 for BGR
#define PANEL_COLOR_ORDER_RGB  1

// Screen 1 — SPI2
#define PIN_NUM_MOSI2           41
#define PIN_NUM_SCLK2           14
#define PIN_NUM_CS2             42
#define PIN_NUM_DC2             13
#define PIN_NUM_RST2            11

// Screen 2 — SPI3 (independent bus: runs in parallel with SPI2)
#define PIN_NUM_MOSI          9
#define PIN_NUM_SCLK          6
#define PIN_NUM_CS            7
#define PIN_NUM_DC            8
#define PIN_NUM_RST           5

#define PIN_LETTER_S1       15
#define PIN_LETTER_U        16
#define PIN_LETTER_S2       17
#define PIN_LETTER_H        18
#define PIN_LETTER_I        4

// -------------------- 5-way switch --------------------
// NOTE: GPIO 26-32 = internal flash; GPIO 33-40 = OctalSPI PSRAM (N16R8).
//       Use only GPIOs outside those ranges for user I/O.
#define PIN_5WAY_UP         1
#define PIN_5WAY_DOWN      21
#define PIN_5WAY_LEFT      2
#define PIN_5WAY_RIGHT     47
#define PIN_5WAY_CENTER    48

// -------------------- 5-way switch --------------------
static const struct { int pin; const char *name; } s_5way_buttons[] = {
    { PIN_5WAY_UP,     "UP"     },
    { PIN_5WAY_DOWN,   "DOWN"   },
    { PIN_5WAY_LEFT,   "LEFT"   },
    { PIN_5WAY_RIGHT,  "RIGHT"  },
    { PIN_5WAY_CENTER, "CENTER" },
};
#define NUM_5WAY_BUTTONS (sizeof(s_5way_buttons) / sizeof(s_5way_buttons[0]))

#define HOLD_REPEAT_MS    80  ///< Auto-repeat interval while held in DISPLAY_MODE_WIFI (ms)

static void five_way_switch_task(void *arg)
{
    bool prev[NUM_5WAY_BUTTONS];
    int  hold_ms[NUM_5WAY_BUTTONS];

    for (int i = 0; i < (int)NUM_5WAY_BUTTONS; i++) {
        prev[i]    = gpio_get_level(s_5way_buttons[i].pin);
        hold_ms[i] = 0;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        for (int i = 0; i < (int)NUM_5WAY_BUTTONS; i++) {
            bool cur = gpio_get_level(s_5way_buttons[i].pin);
            int  pin = s_5way_buttons[i].pin;

            bool first_press = (!cur &&  prev[i]);
            bool held        = (!cur && !prev[i]);

            if (first_press) {
                hold_ms[i] = 0;
                ESP_LOGI(TAG, "Button pressed: %s", s_5way_buttons[i].name);
            } else if (held) {
                hold_ms[i] += 20;
            } else {
                hold_ms[i] = 0;
            }

            // Center: single fire only.
            // Directional in WIFI mode: fire on first press, then repeat every
            // HOLD_REPEAT_MS with no initial delay.
            // Directional in other modes: first press only.
            bool fire = false;
            if (pin == PIN_5WAY_CENTER) {
                fire = first_press;
            } else if (s_display_mode == DISPLAY_MODE_WIFI) {
                if (first_press) {
                    fire = true;
                } else if (held && hold_ms[i] >= HOLD_REPEAT_MS) {
                    fire = true;
                    hold_ms[i] -= HOLD_REPEAT_MS;
                }
            } else {
                fire = first_press;
            }

            if (fire) {
                switch (pin) {
                    case PIN_5WAY_CENTER:
                        s_display_mode = (s_display_mode + 1) % DISPLAY_MODE_COUNT;
                        ESP_LOGI(TAG, "Display mode -> %s", s_display_mode_names[s_display_mode]);
                        display_sequence_request_abort();
                        break;
                    case PIN_5WAY_LEFT:
                    case PIN_5WAY_RIGHT:
                    case PIN_5WAY_UP:
                    case PIN_5WAY_DOWN:
                        if (s_display_mode == DISPLAY_MODE_NORMAL) {
                            current_led_mode = (current_led_mode + 1) % LED_MODE_COUNT;
                            ESP_LOGI(TAG, "LED mode -> %s", s_led_mode_names[current_led_mode]);
                        } else if (s_display_mode == DISPLAY_MODE_WIFI) {
                            if      (pin == PIN_5WAY_LEFT)  { s_walk_x -= 10; s_walk_dir = -1; }
                            else if (pin == PIN_5WAY_RIGHT) { s_walk_x += 10; s_walk_dir =  1; }
                            else if (pin == PIN_5WAY_UP)    s_walk_y -= 10;
                            else if (pin == PIN_5WAY_DOWN)  s_walk_y += 10;
                            // Clamp to the virtual two-screen canvas
                            if (s_walk_x < 0)                  s_walk_x = 0;
                            if (s_walk_x > 2 * LCD_H_RES - 1) s_walk_x = 2 * LCD_H_RES - 1;
                            if (s_walk_y < 0)                  s_walk_y = 0;
                            if (s_walk_y > LCD_V_RES  - 1)     s_walk_y = LCD_V_RES  - 1;
                            ESP_LOGI(TAG, "walk pos -> x=%d y=%d", s_walk_x, s_walk_y);
                        }
                        break;
                    default:
                        break;
                }
            }

            prev[i] = cur;
        }
    }
}

static void five_way_switch_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_5WAY_UP)   |
                        (1ULL << PIN_5WAY_DOWN)  |
                        (1ULL << PIN_5WAY_LEFT)  |
                        (1ULL << PIN_5WAY_RIGHT) |
                        (1ULL << PIN_5WAY_CENTER),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    xTaskCreate(five_way_switch_task, "5way_sw", 4096, NULL, 5, NULL);
}

// -------------------- LCD handles --------------------
static esp_lcd_panel_handle_t s_panel1 = NULL;
static esp_lcd_panel_handle_t s_panel2 = NULL;

// -------------------- LCD init --------------------
static void lcd_init(void)
{
    // ── SPI bus 1 (SPI2) ─────────────────────────────────────────────────────
    spi_bus_config_t bus1 = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_NUM_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // Split into 16-row chunks so each DMA bounce buffer is ~7.5 KB instead
        // of 150 KB.  WiFi's static BSS consumes DMA-capable SRAM and a 150 KB
        // contiguous allocation fails once WiFi is linked in; 7.5 KB never does.
        .max_transfer_sz = LCD_H_RES * 16 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus1, SPI_DMA_CH_AUTO));

    // ── SPI bus 2 (SPI3) ─────────────────────────────────────────────────────
    spi_bus_config_t bus2 = {
        .mosi_io_num     = PIN_NUM_MOSI2,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_NUM_SCLK2,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * 16 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST2, &bus2, SPI_DMA_CH_AUTO));

    // ── Panel IO 1 ────────────────────────────────────────────────────────────
    esp_lcd_panel_io_handle_t io1 = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg1 = {
        .dc_gpio_num       = PIN_NUM_DC,
        .cs_gpio_num       = PIN_NUM_CS,
        .pclk_hz           = 80 * 1000 * 1000,  // 40 MHz; bump to 80 MHz if stable
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg1, &io1));

    // ── Panel IO 2 ────────────────────────────────────────────────────────────────
    esp_lcd_panel_io_handle_t io2 = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg2 = {
        .dc_gpio_num       = PIN_NUM_DC2,
        .cs_gpio_num       = PIN_NUM_CS2,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST2, &io_cfg2, &io2));

    // ── ST7789 panel 1 ────────────────────────────────────────────────────────
    esp_lcd_panel_dev_config_t pcfg1 = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order  = PANEL_COLOR_ORDER_RGB
                              ? LCD_RGB_ELEMENT_ORDER_RGB
                              : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io1, &pcfg1, &s_panel1));

    // ── ST7789 panel 2 ────────────────────────────────────────────────────────
    esp_lcd_panel_dev_config_t pcfg2 = {
        .reset_gpio_num = PIN_NUM_RST2,
        .rgb_ele_order  = PANEL_COLOR_ORDER_RGB
                              ? LCD_RGB_ELEMENT_ORDER_RGB
                              : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io2, &pcfg2, &s_panel2));

    // ── Reset, init, turn on ──────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel1));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel2));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel1));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel2));

    // Uncomment if colours are inverted on your specific module:
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel1, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel2, true));

    // Pixel-memory offset (usually 0,0 for 240×320 ST7789)
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel1, LCD_X_OFFSET, LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel2, LCD_X_OFFSET, LCD_Y_OFFSET));

    // Rotate 180°: mirror both axes (no XY-swap needed)
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel1, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel2, true, true));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel1, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel2, true));
}

// -------------------- WiFi AP + HTTP server --------------------
#define WIFI_AP_SSID     "SushiDisplay"
#define WIFI_AP_PASSWORD "SushiDisplay1!"  // min 8 chars; change before deployment
#define WIFI_AP_CHANNEL  1
#define WIFI_AP_MAX_CONN 4

static esp_netif_t     *s_ap_netif = NULL;
static httpd_handle_t   s_httpd    = NULL;
static bool             s_wifi_prereqs_done = false;

static esp_err_t hello_get_handler(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        ESP_LOGE(TAG, "index.html not found on SPIFFS");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "index.html not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // signal end of chunked response
    return ESP_OK;
}

// ── File-manager helpers ───────────────────────────────────────────────────────

static bool get_query_param(httpd_req_t *req, const char *key, char *dst, size_t dst_len)
{
    char qbuf[256];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return false;
    return httpd_query_key_value(qbuf, key, dst, dst_len) == ESP_OK;
}

// Non-empty, ≤31 chars (SPIFFS default name limit), no path separators
static bool valid_filename(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len > 31) return false;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] == '\\') return false;
    return !(len == 1 && name[0] == '.') &&
           !(len == 2 && name[0] == '.' && name[1] == '.');
}

// ── GET /api/files — JSON list of all SPIFFS files with sizes ─────────────────
static esp_err_t api_files_handler(httpd_req_t *req)
{
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);

    DIR *dir = opendir("/spiffs");
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "opendir failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");

    char buf[192];
    int n = snprintf(buf, sizeof(buf), "{\"total\":%zu,\"used\":%zu,\"files\":[", total, used);
    httpd_resp_send_chunk(req, buf, n);

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;   // skip . and ..

        char path[272];  // "/spiffs/" (8) + NAME_MAX (255) + NUL
        snprintf(path, sizeof(path), "/spiffs/%s", entry->d_name);
        struct stat st;
        long fsize = (stat(path, &st) == 0) ? (long)st.st_size : -1;

        // JSON-escape " and \ in the filename
        char esc[520];  // worst case: every char escaped + NUL
        size_t ei = 0;
        for (const char *p = entry->d_name; *p && ei < sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') esc[ei++] = '\\';
            esc[ei++] = *p;
        }
        esc[ei] = '\0';

        n = snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"size\":%ld}",
                     first ? "" : ",", esc, fsize);
        httpd_resp_send_chunk(req, buf, n);
        first = false;
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ── GET /download?name=<file> — stream file to client ─────────────────────────
static esp_err_t download_handler(httpd_req_t *req)
{
    char name[64];
    if (!get_query_param(req, "name", name, sizeof(name)) || !valid_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or invalid 'name'");
        return ESP_FAIL;
    }

    char path[72];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char buf[512];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)rd) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ── POST /upload?name=<file>  (request body = raw file bytes) ─────────────────
#define UPLOAD_BUF_SZ 1024

static esp_err_t upload_handler(httpd_req_t *req)
{
    char name[64];
    if (!get_query_param(req, "name", name, sizeof(name)) || !valid_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or invalid 'name'");
        return ESP_FAIL;
    }
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty upload");
        return ESP_FAIL;
    }

    char path[72];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
        return ESP_FAIL;
    }

    char *buf = malloc(UPLOAD_BUF_SZ);
    if (!buf) {
        fclose(f); remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        size_t want = remaining < UPLOAD_BUF_SZ ? remaining : UPLOAD_BUF_SZ;
        int got = httpd_req_recv(req, buf, want);
        if (got <= 0) { ok = false; break; }
        if (fwrite(buf, 1, (size_t)got, f) != (size_t)got) { ok = false; break; }
        remaining -= (size_t)got;
    }
    free(buf);
    fclose(f);

    if (!ok) {
        remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── POST /delete?name=<file> — remove a file from SPIFFS ──────────────────────
static esp_err_t delete_handler(httpd_req_t *req)
{
    char name[64];
    if (!get_query_param(req, "name", name, sizeof(name)) || !valid_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or invalid 'name'");
        return ESP_FAIL;
    }

    char path[72];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    if (remove(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static void wifi_http_start(void)
{
    // Initialise the one-time WiFi prerequisites lazily so they don't consume
    // DMA-capable internal SRAM during DISPLAY_MODE_NORMAL.
    if (!s_wifi_prereqs_done) {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(nvs_err);
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_wifi_prereqs_done = true;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {};
    memcpy(ap_cfg.ap.ssid,     WIFI_AP_SSID,     strlen(WIFI_AP_SSID));
    memcpy(ap_cfg.ap.password, WIFI_AP_PASSWORD, strlen(WIFI_AP_PASSWORD));
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(WIFI_AP_SSID);
    ap_cfg.ap.channel        = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_WPA3_PSK;  // transition: WPA2 + WPA3 SAE
    ap_cfg.ap.sae_pwe_h2e    = WPA3_SAE_PWE_BOTH;        // support both H2E and hunt-and-peck

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP started: SSID=%s  ->  http://192.168.4.1/", WIFI_AP_SSID);

    httpd_config_t hcfg    = HTTPD_DEFAULT_CONFIG();
    hcfg.stack_size        = 8192;  // file I/O needs more than the 4 KB default
    hcfg.recv_wait_timeout = 30;    // seconds; needed for large file uploads
    hcfg.max_uri_handlers  = 8;
    if (httpd_start(&s_httpd, &hcfg) == ESP_OK) {
        static const httpd_uri_t uri_root     = { .uri="/",          .method=HTTP_GET,  .handler=hello_get_handler  };
        static const httpd_uri_t uri_files    = { .uri="/api/files", .method=HTTP_GET,  .handler=api_files_handler  };
        static const httpd_uri_t uri_download = { .uri="/download",  .method=HTTP_GET,  .handler=download_handler   };
        static const httpd_uri_t uri_upload   = { .uri="/upload",    .method=HTTP_POST, .handler=upload_handler     };
        static const httpd_uri_t uri_delete   = { .uri="/delete",    .method=HTTP_POST, .handler=delete_handler     };
        httpd_register_uri_handler(s_httpd, &uri_root);
        httpd_register_uri_handler(s_httpd, &uri_files);
        httpd_register_uri_handler(s_httpd, &uri_download);
        httpd_register_uri_handler(s_httpd, &uri_upload);
        httpd_register_uri_handler(s_httpd, &uri_delete);
        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }
}

static void wifi_http_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    ESP_LOGI(TAG, "WiFi AP stopped");
}

// -------------------- Display mode: kirb_back.bmp + kirb_walk.bin overlay --------------------
static void display_white_screens(void)
{
    uint16_t *bg1 = NULL;  // 240×LCD_V_RES, left  half → panel 1
    uint16_t *bg2 = NULL;  // 240×LCD_V_RES, right half → panel 2

    // ── Load and split background ─────────────────────────────────────────────
    do {
        FILE *f = fopen("/spiffs/kirb_back.bmp", "rb");
        if (!f) { ESP_LOGE(TAG, "Cannot open /spiffs/kirb_back.bmp"); break; }

        uint8_t hdr[66];
        if (fread(hdr, 1, sizeof(hdr), f) < sizeof(hdr) ||
                hdr[0] != 'B' || hdr[1] != 'M') {
            ESP_LOGE(TAG, "kirb_back.bmp: bad header");
            fclose(f); break;
        }

        uint32_t po     = (uint32_t)(hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24));
        int32_t  w      = (int32_t) (hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24));
        int32_t  h      = (int32_t) (hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24));
        int      width  = w < 0 ? -w : w;
        int      height = h < 0 ? -h : h;
        bool     top_down   = h < 0;
        uint32_t row_stride = ((uint32_t)(width * 2 + 3) / 4) * 4;

        uint16_t *img     = heap_caps_malloc((size_t)width * height * 2,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *row_buf = malloc(row_stride);
        if (!img || !row_buf) {
            ESP_LOGE(TAG, "kirb_back.bmp: OOM");
            free(img); free(row_buf); fclose(f); break;
        }

        fseek(f, (long)po, SEEK_SET);
        for (int disk_row = 0; disk_row < height; disk_row++) {
            size_t got = fread(row_buf, 1, row_stride, f);
            if (got < (size_t)row_stride)
                memset((uint8_t *)row_buf + got, 0, row_stride - got);
            int img_row = top_down ? disk_row : (height - 1 - disk_row);
            uint16_t *dst = img + (size_t)img_row * width;
            for (int c = 0; c < width; c++) {
                uint16_t px = row_buf[c];
                dst[c] = (uint16_t)((px >> 8) | (px << 8)); // LE→BE
            }
            if ((disk_row & 15) == 15) vTaskDelay(1);
        }
        free(row_buf);
        fclose(f);

        int bg_rows = height < LCD_V_RES ? height : LCD_V_RES;
        bg1 = heap_caps_calloc((size_t)LCD_H_RES * LCD_V_RES, 2,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bg2 = heap_caps_calloc((size_t)LCD_H_RES * LCD_V_RES, 2,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bg1 && bg2) {
            for (int y = 0; y < bg_rows; y++) {
                uint16_t *row = img + (size_t)y * width;
                memcpy(bg1 + (size_t)y * LCD_H_RES, row,             (size_t)LCD_H_RES * 2);
                memcpy(bg2 + (size_t)y * LCD_H_RES, row + LCD_H_RES, (size_t)LCD_H_RES * 2);
            }
        }
        free(img);
    } while (0);

    if (!bg1 || !bg2) {
        ESP_LOGE(TAG, "display_white_screens: failed to load background");
        free(bg1); free(bg2);
        while (s_display_mode == DISPLAY_MODE_WIFI)
            vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }

    // ── Overlay WiFi info into the background buffers before first draw ────────
    {
        uint16_t fg     = font_color(  0,  40, 120);  // dark navy — readable on light blue
        uint16_t fg_val = font_color(180,   0,   0);  // deep red for values
        uint16_t bk     = FONT_TRANSPARENT;
        // Screen 1: SSID and password
        font_draw_string(bg1, LCD_H_RES, LCD_V_RES,  4,  4, "WIFI:",           fg,     bk, 2);
        font_draw_string(bg1, LCD_H_RES, LCD_V_RES,  4, 24, WIFI_AP_SSID,      fg_val, bk, 2);
        font_draw_string(bg1, LCD_H_RES, LCD_V_RES,  4, 52, "PASS:",           fg,     bk, 2);
        font_draw_string(bg1, LCD_H_RES, LCD_V_RES,  4, 72, WIFI_AP_PASSWORD,  fg_val, bk, 2);
        // Screen 2: URL
        font_draw_string(bg2, LCD_H_RES, LCD_V_RES,  4,  4, "URL:",            fg,     bk, 2);
        font_draw_string(bg2, LCD_H_RES, LCD_V_RES,  4, 24, "192.168.4.1",     fg_val, bk, 2);
    }

    // Draw background once before WiFi starts (while DMA-capable heap is intact)
    esp_lcd_panel_draw_bitmap(s_panel1, 0, 0, LCD_H_RES, LCD_V_RES, bg1);
    esp_lcd_panel_draw_bitmap(s_panel2, 0, 0, LCD_H_RES, LCD_V_RES, bg2);

    // Start WiFi AP + HTTP server AFTER the initial background draw so the one-time
    // PSRAM→DMA bounce buffer allocation happens while DMA heap is still plentiful.
    wifi_http_start();

    // ── Animate kirb_walk.bin / kirby_idle.bin / kirb_fly.bin on top of background ──
    // kirb_fly.bin  : y <= 210
    // kirb_walk.bin : y >  210 and moving (last button press < 300 ms ago)
    // kirby_idle.bin: y >  210 and stationary
    do {
        // Open all three animation files upfront
        FILE *walk_f = fopen("/spiffs/kirb_walk.bin",  "rb");
        if (!walk_f) { ESP_LOGE(TAG, "Cannot open /spiffs/kirb_walk.bin"); break; }
        FILE *fly_f  = fopen("/spiffs/kirb_fly.bin",   "rb");
        if (!fly_f)  { ESP_LOGE(TAG, "Cannot open /spiffs/kirb_fly.bin");  fclose(walk_f); break; }
        FILE *idle_f = fopen("/spiffs/kirby_idle.bin", "rb");
        if (!idle_f) { ESP_LOGE(TAG, "Cannot open /spiffs/kirby_idle.bin"); fclose(walk_f); fclose(fly_f); break; }

        // Helper macro: read 8-byte header, return false on failure
        #define READ_ANIM_HDR(f, w, h, fr, fps_, us_, ofs_) do { \
            uint8_t _h[8]; \
            if (fread(_h, 1, 8, (f)) < 8) { ESP_LOGE(TAG, "short header"); goto anim_done; } \
            (w)   = (int)(_h[0] | (_h[1] << 8)); \
            (h)   = (int)(_h[2] | (_h[3] << 8)); \
            (fr)  = (int)(_h[4] | (_h[5] << 8)); \
            (fps_)= (int)(_h[6] | (_h[7] << 8)); \
            (us_) = ((fps_) > 0) ? (1000000LL / (fps_)) : 100000LL; \
            (ofs_)= ftell(f); \
        } while (0)

        int     walk_w, walk_h, walk_frames, walk_fps;  int64_t walk_us;  long walk_ofs;
        int     fly_w,  fly_h,  fly_frames,  fly_fps;   int64_t fly_us;   long fly_ofs;
        int     idle_w, idle_h, idle_frames, idle_fps;  int64_t idle_us;  long idle_ofs;
        READ_ANIM_HDR(walk_f, walk_w, walk_h, walk_frames, walk_fps, walk_us, walk_ofs);
        READ_ANIM_HDR(fly_f,  fly_w,  fly_h,  fly_frames,  fly_fps,  fly_us,  fly_ofs);
        READ_ANIM_HDR(idle_f, idle_w, idle_h, idle_frames, idle_fps, idle_us, idle_ofs);
        (void)idle_us;  // frame timing for idle is driven by IDLE_FRAME_US[], not fps
        #undef READ_ANIM_HDR

        // Allocate frame buffer large enough for whichever is biggest
        size_t walk_px = (size_t)walk_w * walk_h;
        size_t fly_px  = (size_t)fly_w  * fly_h;
        size_t idle_px = (size_t)idle_w * idle_h;
        size_t max_px  = walk_px > fly_px ? walk_px : fly_px;
        if (idle_px > max_px) max_px = idle_px;
        uint16_t *frame_buf = heap_caps_malloc(max_px * 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        // comp1/comp2 in PSRAM: max_transfer_sz=16-row chunks means bounce buffers
        // are only ~7.5 KB each, well within DMA SRAM even with WiFi running.
        size_t screen_px = (size_t)LCD_H_RES * LCD_V_RES;
        uint16_t *comp1 = heap_caps_malloc(screen_px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *comp2 = heap_caps_malloc(screen_px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame_buf || !comp1 || !comp2) {
            ESP_LOGE(TAG, "anim: OOM (frame_buf=%p comp1=%p comp2=%p)", frame_buf, comp1, comp2);
            free(frame_buf); free(comp1); free(comp2);
            goto anim_done;
        }

        int sc = 3;
        // Centred start position (based on walk animation — both should be same size)
        int draw_w = (walk_w * sc) < LCD_H_RES ? (walk_w * sc) : LCD_H_RES;
        int draw_h = (walk_h * sc) < LCD_V_RES ? (walk_h * sc) : LCD_V_RES;
        (void)draw_h;
        s_walk_x = (LCD_H_RES - draw_w) / 2;
        s_walk_y = 211 ;

        ESP_LOGI(TAG, "walk: %dx%d %d fr @%d fps  fly: %dx%d %d fr @%d fps  idle: %dx%d %d fr @%d fps  scale=3",
                 walk_w, walk_h, walk_frames, walk_fps,
                 fly_w,  fly_h,  fly_frames,  fly_fps,
                 idle_w, idle_h, idle_frames, idle_fps);

        int  walk_fi = 0, fly_fi = 0, idle_fi = 0;
        bool io_ok   = true;
        // Timestamp of the last position change (us); initialised to now so we start in walk.
        int64_t last_move_us  = esp_timer_get_time();
        int     last_walk_x   = s_walk_x, last_walk_y = s_walk_y;
        // Idle blink timing: frame 0 = eyes open (3 s), frame 1 = blink (150 ms)
        static const int64_t IDLE_FRAME_US[] = { 3000000LL, 150000LL };
        int64_t idle_frame_start_us = esp_timer_get_time();

        while (io_ok && s_display_mode == DISPLAY_MODE_WIFI) {
            int64_t t0    = esp_timer_get_time();
            int cur_x     = s_walk_x;
            int cur_y     = s_walk_y;
            int cur_dir   = s_walk_dir;

            // Detect movement
            if (cur_x != last_walk_x || cur_y != last_walk_y) {
                last_move_us = esp_timer_get_time();
                last_walk_x  = cur_x;
                last_walk_y  = cur_y;
            }
            bool moving  = (esp_timer_get_time() - last_move_us) < 300000LL; // 300 ms
            bool use_fly  = (cur_y <= 210);
            bool use_idle = !use_fly && !moving;

            // For the idle animation, advance the frame based on custom per-frame
            // durations rather than the file's fps field.
            if (use_idle && (esp_timer_get_time() - idle_frame_start_us) >= IDLE_FRAME_US[idle_fi]) {
                idle_fi = (idle_fi + 1) % idle_frames;
                idle_frame_start_us = esp_timer_get_time();
            }
            // When switching into idle, reset to frame 0 and restart the timer.
            static bool prev_use_idle = false;
            if (use_idle && !prev_use_idle) {
                idle_fi = 0;
                idle_frame_start_us = esp_timer_get_time();
            }
            prev_use_idle = use_idle;

            FILE   *af       = use_fly  ? fly_f       : (use_idle ? idle_f : walk_f);
            int     anim_w   = use_fly  ? fly_w       : (use_idle ? idle_w : walk_w);
            int     anim_h   = use_fly  ? fly_h       : (use_idle ? idle_h : walk_h);
            int     frames   = use_fly  ? fly_frames  : (use_idle ? idle_frames : walk_frames);
            // Idle uses a fixed short render tick; frame advancement is timer-driven above.
            int64_t fus      = use_fly  ? fly_us      : (use_idle ? 50000LL : walk_us);
            long    data_ofs = use_fly  ? fly_ofs     : (use_idle ? idle_ofs : walk_ofs);
            // Idle: read current idle_fi but do NOT auto-advance here.
            int     read_fi;
            int    *fi;
            if (use_idle) { read_fi = idle_fi; fi = &read_fi; }
            else          { fi = use_fly ? &fly_fi : &walk_fi; }

            // Seek to current frame and read it
            long frame_byte_ofs = data_ofs + (long)(*fi) * anim_w * anim_h * 2;
            if (fseek(af, frame_byte_ofs, SEEK_SET) != 0 ||
                fread(frame_buf, 2, (size_t)anim_w * anim_h, af) < (size_t)(anim_w * anim_h)) {
                io_ok = false; break;
            }
            if (!use_idle) *fi = (*fi + 1) % frames; // idle frame is advanced by timer above

            for (int panel_idx = 0; panel_idx < 2; panel_idx++) {
                uint16_t *bg   = (panel_idx == 0) ? bg1   : bg2;
                uint16_t *comp = (panel_idx == 0) ? comp1 : comp2;
                esp_lcd_panel_handle_t panel = (panel_idx == 0) ? s_panel1 : s_panel2;
                int panel_origin = panel_idx * LCD_H_RES;

                memcpy(comp, bg, (size_t)LCD_H_RES * LCD_V_RES * 2);

                for (int src_r = 0; src_r < anim_h; src_r++) {
                    const uint16_t *src_row = frame_buf + (size_t)src_r * anim_w;
                    for (int dr = 0; dr < sc; dr++) {
                        int dst_y = cur_y + src_r * sc + dr;
                        if ((unsigned)dst_y >= (unsigned)LCD_V_RES) continue;
                        uint16_t *dst_row = comp + (size_t)dst_y * LCD_H_RES;
                        for (int src_c = 0; src_c < anim_w; src_c++) {
                            int read_c = (cur_dir < 0) ? (anim_w - 1 - src_c) : src_c;
                            uint16_t px = src_row[read_c];
                            if (px == 0xFF07) continue; // #00FFFF (cyan) = transparent
                            for (int dc = 0; dc < sc; dc++) {
                                int dst_x = cur_x + src_c * sc + dc - panel_origin;
                                if ((unsigned)dst_x < (unsigned)LCD_H_RES)
                                    dst_row[dst_x] = px;
                            }
                        }
                    }
                }

                esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, comp);
            }

            int64_t elapsed_us   = esp_timer_get_time() - t0;
            int64_t remaining_ms = (fus - elapsed_us) / 1000;
            TickType_t ticks = (remaining_ms > 0) ? pdMS_TO_TICKS((uint32_t)remaining_ms) : 0;
            vTaskDelay(ticks > 0 ? ticks : 1);
        }

        free(frame_buf);
        free(comp1);
        free(comp2);
anim_done:
        fclose(walk_f);
        fclose(fly_f);
        fclose(idle_f);
    } while (0);

    free(bg1);
    free(bg2);
    // Guard: idle if we exited the animation early (e.g. file error)
    while (s_display_mode == DISPLAY_MODE_WIFI)
        vTaskDelay(pdMS_TO_TICKS(100));
    wifi_http_stop();
}

// -------------------- Display sequence --------------------
// Edit this table to configure what plays on which screen and for how long.
static const scene_t s_display_sequence[] = {

    // 1. Static zanmai.bmp on screen 1  ──┐ run simultaneously
    {                                      //  │
        .type        = SCENE_STATIC,       //  │
        .file_path   = "/spiffs/zanmai.bmp",
        .screen      = SCREEN_1,
        .duration_ms = 3000,
        .parallel    = true,  // <-- paired with the scene below
    },
    // 2. Pan okinomi.bmp on screen 2    ──┘
    {
        .type        = SCENE_PAN,
        .file_path   = "/spiffs/okinomi.bmp",
        .screen      = SCREEN_2,
        .pan_dir     = DIR_LEFT,
        .pan_step_ms = 80,
    },

    // 3. Bounce sushiro.bmp on screen 1  ──┐ run simultaneously
    {                                       //  │
        .type            = SCENE_BOUNCE,    //  │
        .file_path       = "/spiffs/sushiro.bmp",
        .screen          = SCREEN_1,
        .bounce_dx       = 3,
        .bounce_dy       = 2,
        .bounce_step_ms  = 16,
        .bounce_dur_ms   = 6000,
        .parallel        = true,  // <-- paired with the scene below
    },
    // 4. Static zanmai.bmp on screen 2  ──┘
    {
        .type        = SCENE_STATIC,
        .file_path   = "/spiffs/zanmai.bmp",
        .screen      = SCREEN_2,
        .duration_ms = 6000,
    },

    // 5. Kirby fly animation spanning both screens
    {
        .type           = SCENE_ANIM_DUAL,
        .file_path      = "/spiffs/kirbyfly.bin",
        .first_frame_ms = 10000,
        .last_frame_ms  = 10000,
        .dual_mode      = DUAL_SPLIT_H,
        .scale          = 2,
    },
};

// -------------------- app_main --------------------
void app_main(void)
{
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    neon_letter_init(PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I);
    five_way_switch_init(); // also starts the switch polling task

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path            = "/spiffs",
        .partition_label      = NULL,
        .max_files            = 8,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_conf));

    // Initialise LCD panels
    lcd_init();

    // Run the display loop (loops forever)
    while (1) {
        switch (s_display_mode) {
            case DISPLAY_MODE_WIFI:
                display_white_screens();
                break;
            case DISPLAY_MODE_NORMAL:
            default:
                display_sequence_run(s_panel1, s_panel2,
                                     s_display_sequence,
                                     sizeof(s_display_sequence) / sizeof(s_display_sequence[0]));
                break;
        }
    }
}
