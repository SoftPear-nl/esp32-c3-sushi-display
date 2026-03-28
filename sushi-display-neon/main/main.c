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
#include "neon_letter_service.h"

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
#define PIN_NUM_MOSI           11
#define PIN_NUM_SCLK           12
#define PIN_NUM_CS             10
#define PIN_NUM_DC             13
#define PIN_NUM_RST            14

// Screen 2 — SPI3 (independent bus: runs in parallel with SPI2)
#define PIN_NUM_MOSI2          5
#define PIN_NUM_SCLK2          6
#define PIN_NUM_CS2            7
#define PIN_NUM_DC2            8
#define PIN_NUM_RST2           9

#define PIN_LETTER_S1       15
#define PIN_LETTER_U        16
#define PIN_LETTER_S2       17
#define PIN_LETTER_H        18
#define PIN_LETTER_I        21

esp_lcd_panel_handle_t s_panel_handle;
esp_lcd_panel_handle_t s_panel_handle2;

// -------------------- .bin split-screen player state --------------------
/* 8-byte header written by gif_to_bin.html (all fields little-endian) */
typedef struct { uint16_t w, h, frames, fps; } bin_hdr_t;

static uint16_t          *s_split_left        = NULL;  /* left  240×320 draw buffer, SPIRAM */
static uint16_t          *s_split_right       = NULL;  /* right 240×320 draw buffer, SPIRAM */
static SemaphoreHandle_t  s_split_right_ready = NULL;  /* task1 → task2: right half is ready */
static SemaphoreHandle_t  s_split_right_done  = NULL;  /* task2 → task1: draw finished        */
static volatile bool      s_split_active      = false; /* true while split .bin is playing    */
static QueueHandle_t      s_task2_queue       = NULL;  /* task1 → task2: phase commands       */
static SemaphoreHandle_t  s_task2_done        = NULL;  /* task2 → task1: phase complete       */

/* Commands sent from task1 to task2 */
typedef enum { T2_PAN_BOUNCE, T2_SPLIT_SLAVE } t2_cmd_type_t;
typedef struct {
    t2_cmd_type_t type;
    char          path[64];
    uint32_t      duration_ms;
} t2_cmd_t;

#define PHASE_PAN_MS    10000u  /* phase 1: panning duration (ms) */
#define PHASE_STATIC_MS 10000u  /* phase 3: static+bounce duration (ms) */

// -------------------- BMP display functions --------------------

/* Parsed information from a BMP file header */
typedef struct {
    int32_t  width;
    int32_t  height;
    bool     top_down;      /* true when original height was negative */
    uint32_t row_stride;    /* bytes per scanline (padded to 4-byte boundary) */
    uint32_t pixel_offset;  /* byte offset where pixel data begins */
} bmp_info_t;

/* Read and validate BMP header from an already-open FILE*.
   Supports 16-bpp BI_BITFIELDS bitmaps (RGB565) as used in this project.
   NOTE: pixel data bytes are in native little-endian order as stored by BMP.
         If the display shows incorrect colours, byte-swap each uint16_t after
         reading (swapped = (v << 8) | (v >> 8)). */
static esp_err_t bmp_read_info(FILE *f, bmp_info_t *out)
{
    uint8_t hdr[66];
    rewind(f);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return ESP_ERR_INVALID_SIZE;
    if (hdr[0] != 'B' || hdr[1] != 'M')               return ESP_ERR_INVALID_ARG;

    uint16_t bpp = (uint16_t)(hdr[28] | (hdr[29] << 8));
    if (bpp != 16) {
        ESP_LOGE("BMP", "Only 16-bpp BMPs are supported (got %d bpp)", bpp);
        return ESP_ERR_INVALID_ARG;
    }

    out->pixel_offset = (uint32_t)(hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24));

    int32_t raw_w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8)
                             | ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t raw_h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8)
                             | ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));

    out->width     = raw_w < 0 ? -raw_w : raw_w;
    out->height    = raw_h < 0 ? -raw_h : raw_h;
    out->top_down  = (raw_h < 0);
    /* Row padded to 4-byte boundary */
    out->row_stride = (((uint32_t)out->width * 2u) + 3u) & ~3u;
    return ESP_OK;
}

/* Draw a BMP image at (x, y) on the given panel.
   Loads all pixel data into one heap buffer, byte-swaps, then sends in a
   single draw_bitmap call. */
static esp_err_t bmp_draw_direct(esp_lcd_panel_handle_t panel, FILE *f,
                                  const bmp_info_t *bmp, int x, int y)
{
    int vis_x1 = x < 0 ? 0 : x;
    int vis_y1 = y < 0 ? 0 : y;
    int vis_x2 = (x + bmp->width)  > LCD_H_RES ? LCD_H_RES : (x + bmp->width);
    int vis_y2 = (y + bmp->height) > LCD_V_RES ? LCD_V_RES : (y + bmp->height);
    if (vis_x1 >= vis_x2 || vis_y1 >= vis_y2) return ESP_OK;

    int vis_w   = vis_x2 - vis_x1;
    int vis_h   = vis_y2 - vis_y1;
    int src_col = vis_x1 - x;

    size_t buf_bytes = (size_t)vis_w * (size_t)vis_h * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(buf_bytes);  // fallback to internal RAM
    if (!buf) {
        ESP_LOGE("BMP", "alloc failed (%u bytes)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }

    for (int r = 0; r < vis_h; r++) {
        int bmp_row = (vis_y1 - y) + r;
        if (!bmp->top_down) bmp_row = bmp->height - 1 - bmp_row;
        fseek(f, (long)bmp->pixel_offset + (long)bmp_row * (long)bmp->row_stride
                 + (long)src_col * 2, SEEK_SET);
        uint16_t *dst = buf + (size_t)r * vis_w;
        fread(dst, sizeof(uint16_t), (size_t)vis_w, f);
        for (int c = 0; c < vis_w; c++) {
            uint16_t v = dst[c];
            dst[c] = (uint16_t)((v << 8) | (v >> 8));
        }
    }

    esp_lcd_panel_draw_bitmap(panel, vis_x1, vis_y1, vis_x2, vis_y2, buf);
    free(buf);
    return ESP_OK;
}

esp_err_t display_bitmap(esp_lcd_panel_handle_t panel, const char *path, int x, int y)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE("BMP", "Cannot open: %s", path); return ESP_ERR_NOT_FOUND; }
    bmp_info_t bmp;
    esp_err_t err = bmp_read_info(f, &bmp);
    if (err == ESP_OK) err = bmp_draw_direct(panel, f, &bmp, x, y);
    fclose(f);
    return err;
}

/* In-memory BMP: pixels stored top-down, width*height uint16_t, already byte-swapped.
   Loaded entirely from SPIFFS into SPIRAM once so animation frames never touch
   the filesystem — safe for concurrent access from multiple cores. */
typedef struct {
    bmp_info_t  info;
    uint16_t   *pixels;   /* full image in SPIRAM, top-down, no row padding */
    uint16_t   *clip_buf; /* screen-sized scratch buffer for clipped draws, SPIRAM */
} bmp_buf_t;

static esp_err_t bmp_buf_load(const char *path, bmp_buf_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE("BMP", "Cannot open: %s", path); return ESP_ERR_NOT_FOUND; }
    esp_err_t err = bmp_read_info(f, &out->info);
    if (err != ESP_OK) { fclose(f); return err; }

    size_t total = (size_t)out->info.width * (size_t)out->info.height * sizeof(uint16_t);
    out->pixels = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out->pixels) out->pixels = malloc(total);
    if (!out->pixels) { fclose(f); return ESP_ERR_NO_MEM; }

    /* Pre-allocate a screen-sized scratch buffer used by bmp_draw_buf so that
       no heap allocation is needed per frame during animation. */
    size_t clip_sz = (size_t)LCD_H_RES * (size_t)LCD_V_RES * sizeof(uint16_t);
    out->clip_buf = heap_caps_malloc(clip_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out->clip_buf) out->clip_buf = malloc(clip_sz);
    if (!out->clip_buf) { free(out->pixels); fclose(f); return ESP_ERR_NO_MEM; }

    for (int row = 0; row < out->info.height; row++) {
        int bmp_row = out->info.top_down ? row : (out->info.height - 1 - row);
        fseek(f, (long)out->info.pixel_offset + (long)bmp_row * (long)out->info.row_stride, SEEK_SET);
        uint16_t *dst = out->pixels + (size_t)row * (size_t)out->info.width;
        fread(dst, sizeof(uint16_t), (size_t)out->info.width, f);
        for (int c = 0; c < out->info.width; c++) {
            uint16_t v = dst[c];
            dst[c] = (uint16_t)((v << 8) | (v >> 8));
        }
    }
    fclose(f);
    return ESP_OK;
}

static void bmp_buf_free(bmp_buf_t *bmp)
{
    free(bmp->pixels);
    free(bmp->clip_buf);
    bmp->pixels   = NULL;
    bmp->clip_buf = NULL;
}

/* Draw from in-memory buffer at (x, y), clipped to screen.
   If rows are contiguous in the buffer the data is sent directly;
   otherwise a temporary strip buffer is used for the clipped case. */
static void bmp_draw_buf(esp_lcd_panel_handle_t panel, const bmp_buf_t *bmp, int x, int y)
{
    int vis_x1 = x < 0 ? 0 : x;
    int vis_y1 = y < 0 ? 0 : y;
    int vis_x2 = (x + bmp->info.width)  > LCD_H_RES ? LCD_H_RES : (x + bmp->info.width);
    int vis_y2 = (y + bmp->info.height) > LCD_V_RES ? LCD_V_RES : (y + bmp->info.height);
    if (vis_x1 >= vis_x2 || vis_y1 >= vis_y2) return;

    int vis_w = vis_x2 - vis_x1;
    int vis_h = vis_y2 - vis_y1;
    int src_x = vis_x1 - x;
    int src_y = vis_y1 - y;

    if (src_x == 0 && vis_w == bmp->info.width && vis_h == bmp->info.height) {
        /* Entire image fits on screen with no horizontal clipping — copy to
           clip_buf first so the DMA source is guaranteed to be a contiguous
           internal/SPIRAM block rather than an arbitrary SPIRAM offset. */
        size_t total = (size_t)vis_w * (size_t)vis_h * sizeof(uint16_t);
        uint16_t *row0 = bmp->pixels + (size_t)src_y * (size_t)bmp->info.width;
        memcpy(bmp->clip_buf, row0, total);
        esp_lcd_panel_draw_bitmap(panel, vis_x1, vis_y1, vis_x2, vis_y2, bmp->clip_buf);
        return;
    }

    /* Clipped / panned case: pack visible columns into the pre-allocated
       clip_buf — no heap allocation per frame. */
    size_t row_bytes = (size_t)vis_w * sizeof(uint16_t);
    for (int r = 0; r < vis_h; r++) {
        uint16_t *src = bmp->pixels + (size_t)(src_y + r) * (size_t)bmp->info.width + src_x;
        memcpy(bmp->clip_buf + (size_t)r * vis_w, src, row_bytes);
    }
    esp_lcd_panel_draw_bitmap(panel, vis_x1, vis_y1, vis_x2, vis_y2, bmp->clip_buf);
}

/* Fill a rectangular screen region with black (0x0000). Uses a 16-row chunk
   buffer so no large allocation is needed even for full-screen fills. */
static void fill_rect_black(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > LCD_H_RES) x2 = LCD_H_RES;
    if (y2 > LCD_V_RES) y2 = LCD_V_RES;
    if (x1 >= x2 || y1 >= y2) return;
    int w = x2 - x1;
    const int CHUNK = 16;
    uint16_t *buf = malloc((size_t)w * CHUNK * sizeof(uint16_t));
    if (!buf) return;
    memset(buf, 0, (size_t)w * CHUNK * sizeof(uint16_t));
    for (int y = y1; y < y2; y += CHUNK) {
        int rows = (y + CHUNK <= y2) ? CHUNK : (y2 - y);
        esp_lcd_panel_draw_bitmap(panel, x1, y, x2, y + rows, buf);
    }
    free(buf);
}

/* Display a BMP with animation for duration_ms milliseconds.
 *
 * If the image is larger than the screen in any dimension:
 *   Pan — scrolls the viewport from (0,0) to the far corner and back,
 *   clamped to the image boundaries.
 *
 * If the image fits entirely on screen:
 *   Bounce — the image floats on a black background, bouncing off the
 *   screen edges. */
void pan_or_bounce_bitmap(esp_lcd_panel_handle_t panel, const char *path, uint32_t duration_ms)
{
    /* Load the entire image into SPIRAM once. After this point no SPIFFS
       access occurs, so both tasks can run concurrently on separate cores
       without any filesystem contention. */
    bmp_buf_t bmp = {0};
    if (bmp_buf_load(path, &bmp) != ESP_OK) return;

    const uint32_t FRAME_MS = 50;   /* ~20 fps */
    int total_frames = (int)(duration_ms / FRAME_MS);
    if (total_frames < 1) total_frames = 1;

    int max_pan_x = (bmp.info.width  > LCD_H_RES) ? (bmp.info.width  - LCD_H_RES) : 0;
    int max_pan_y = (bmp.info.height > LCD_V_RES) ? (bmp.info.height - LCD_V_RES) : 0;

    if (max_pan_x > 0 || max_pan_y > 0) {
        /* --- Pan mode: triangle-wave sweep across the image --- */
        for (int frame = 0; frame < total_frames; frame++) {
            float t    = (total_frames > 1) ? (float)frame / (float)(total_frames - 1) : 0.0f;
            float ping = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
            bmp_draw_buf(panel, &bmp,
                         -(int)(ping * (float)max_pan_x),
                         -(int)(ping * (float)max_pan_y));
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        }
    } else {
        /* --- Bounce mode: image floats on black, bounces off screen edges --- */
        float pos_x = (float)(LCD_H_RES - bmp.info.width)  / 2.0f;
        float pos_y = (float)(LCD_V_RES - bmp.info.height) / 2.0f;
        float vel_x = 2.0f;
        float vel_y = 1.5f;
        int   prev_x = -1, prev_y = -1;

        fill_rect_black(panel, 0, 0, LCD_H_RES, LCD_V_RES);

        for (int frame = 0; frame < total_frames; frame++) {
            int cur_x = (int)pos_x;
            int cur_y = (int)pos_y;

            bmp_draw_buf(panel, &bmp, cur_x, cur_y);

            if (prev_x >= 0) {
                int dx = cur_x - prev_x;
                int dy = cur_y - prev_y;
                if (dx > 0)
                    fill_rect_black(panel, prev_x, prev_y,
                                    prev_x + dx, prev_y + bmp.info.height);
                else if (dx < 0)
                    fill_rect_black(panel, cur_x + bmp.info.width, prev_y,
                                    prev_x + bmp.info.width, prev_y + bmp.info.height);
                if (dy > 0)
                    fill_rect_black(panel, prev_x, prev_y,
                                    prev_x + bmp.info.width, prev_y + dy);
                else if (dy < 0)
                    fill_rect_black(panel, prev_x, cur_y + bmp.info.height,
                                    prev_x + bmp.info.width, prev_y + bmp.info.height);
            }

            prev_x = cur_x;
            prev_y = cur_y;

            pos_x += vel_x;
            pos_y += vel_y;

            if (pos_x < 0.0f)                                                      { pos_x = 0.0f;                                  if (vel_x < 0.0f) vel_x = -vel_x; }
            if (pos_y < 0.0f)                                                      { pos_y = 0.0f;                                  if (vel_y < 0.0f) vel_y = -vel_y; }
            if (pos_x + (float)bmp.info.width  > (float)LCD_H_RES) { pos_x = (float)(LCD_H_RES - bmp.info.width);  if (vel_x > 0.0f) vel_x = -vel_x; }
            if (pos_y + (float)bmp.info.height > (float)LCD_V_RES) { pos_y = (float)(LCD_V_RES - bmp.info.height); if (vel_y > 0.0f) vel_y = -vel_y; }

            vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        }
    }

    bmp_buf_free(&bmp);
}

// -------------------- .bin split-screen player --------------------
/*
 * Play a wide .bin animation spread across two 240-wide screens.
 * The file must be exactly (LCD_H_RES*2) × LCD_V_RES pixels.
 * Pixels are big-endian RGB565 as written by gif_to_bin.html — no byte-swap needed.
 *
 * Task 1 (core 0): reads each frame, splits rows, draws left half to panel 1.
 * Task 2 (core 1): receives s_split_right_ready, draws right half to panel 2.
 * Both SPI buses run simultaneously so frame time ≈ one screen transfer.
 * Loops forever; only returns if the file cannot be opened.
 */
static void play_split_bin(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE("BIN", "Cannot open: %s", path); return; }

    bin_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        ESP_LOGE("BIN", "Header read failed"); fclose(f); return;
    }

    /* Accept either native 480×320 or half-size 240×160 (2× upscale) */
    const bool upscale2x = (hdr.w == (uint16_t)LCD_H_RES && hdr.h == (uint16_t)(LCD_V_RES / 2));
    const bool native    = (hdr.w == (uint16_t)(LCD_H_RES * 2) && hdr.h == (uint16_t)LCD_V_RES);
    if (!upscale2x && !native) {
        ESP_LOGE("BIN", "Unsupported size %ux%u (need %dx%d or %dx%d)",
                 (unsigned)hdr.w, (unsigned)hdr.h,
                 LCD_H_RES, LCD_V_RES / 2,
                 LCD_H_RES * 2, LCD_V_RES);
        fclose(f); return;
    }

    ESP_LOGI("BIN", "Playing %s: %u frames @ %u fps (%s)",
             path, (unsigned)hdr.frames, (unsigned)hdr.fps,
             upscale2x ? "2x upscale" : "native");

    const size_t half_bytes   = (size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    const size_t frame_pixels = (size_t)hdr.w * hdr.h;

    s_split_left  = heap_caps_malloc(half_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_split_right = heap_caps_malloc(half_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *frame_buf = heap_caps_malloc(frame_pixels * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!s_split_left || !s_split_right || !frame_buf) {
        ESP_LOGE("BIN", "Buffer alloc failed (%u + %u + %u bytes)",
                 (unsigned)half_bytes, (unsigned)half_bytes,
                 (unsigned)(frame_pixels * sizeof(uint16_t)));
        goto cleanup;
    }

    const uint32_t frame_ms = (hdr.fps > 0u) ? (1000u / hdr.fps) : 80u;

    fseek(f, (long)sizeof(bin_hdr_t), SEEK_SET);

    for (int fi = 0; fi < (int)hdr.frames; fi++) {
            TickType_t t_start = xTaskGetTickCount();

            /* Read the source frame in one SPIFFS call */
            fread(frame_buf, sizeof(uint16_t), frame_pixels, f);

            if (upscale2x) {
                /* 2× nearest-neighbour upscale: 240×160 → 480×320.
                 * Each source row (sy) produces 2 identical destination rows.
                 * Left screen  uses source cols  0..119 → dest cols 0..239 (each pixel doubled).
                 * Right screen uses source cols 120..239 → dest cols 0..239 (each pixel doubled). */
                for (int sy = 0; sy < (int)hdr.h; sy++) {
                    const uint16_t *src = frame_buf + (size_t)sy * hdr.w;
                    for (int rep = 0; rep < 2; rep++) {
                        const int dy = sy * 2 + rep;
                        uint16_t *dl = s_split_left  + (size_t)dy * LCD_H_RES;
                        uint16_t *dr = s_split_right + (size_t)dy * LCD_H_RES;
                        for (int dx = 0; dx < LCD_H_RES; dx++) {
                            dl[dx] = src[dx >> 1];               /* cols 0..119 doubled   */
                            dr[dx] = src[(LCD_H_RES / 2) + (dx >> 1)]; /* cols 120..239 doubled */
                        }
                    }
                }
            } else {
                /* Native 480×320: split each row at the midpoint */
                for (int row = 0; row < LCD_V_RES; row++) {
                    const uint16_t *src = frame_buf + (size_t)row * hdr.w;
                    memcpy(s_split_left  + (size_t)row * LCD_H_RES,
                           src,
                           (size_t)LCD_H_RES * sizeof(uint16_t));
                    memcpy(s_split_right + (size_t)row * LCD_H_RES,
                           src + LCD_H_RES,
                           (size_t)LCD_H_RES * sizeof(uint16_t));
                }
            }

            /* Signal task 2 to draw the right half, then draw the left half in parallel */
            xSemaphoreGive(s_split_right_ready);
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, s_split_left);

            /* Wait for task 2 to finish before we overwrite the buffers on the next frame */
            xSemaphoreTake(s_split_right_done, portMAX_DELAY);

            /* Pace to target fps */
            TickType_t elapsed = xTaskGetTickCount() - t_start;
            TickType_t target  = pdMS_TO_TICKS(frame_ms);
            if (elapsed < target) vTaskDelay(target - elapsed);

            /* Hold first and last frames for 15 seconds */
            if (fi == 0 || fi == (int)hdr.frames - 1)
                vTaskDelay(pdMS_TO_TICKS(15000));
        }

cleanup:
    free(s_split_left);  s_split_left  = NULL;
    free(s_split_right); s_split_right = NULL;
    free(frame_buf);
    fclose(f);
}

// -------------------- Bitmap draw tasks (one per screen, one per core) --------------------
static void draw_bitmap_task1(void *arg)
{
    (void)arg;
    t2_cmd_t cmd;

    while (1) {
        /* ---- Phase 1: both screens pan/bounce their images simultaneously ---- */
        cmd.type = T2_PAN_BOUNCE;
        strncpy(cmd.path, "/spiffs/okinomi.bmp", sizeof(cmd.path));
        cmd.duration_ms = PHASE_PAN_MS;
        xQueueSend(s_task2_queue, &cmd, portMAX_DELAY);
        pan_or_bounce_bitmap(s_panel_handle, "/spiffs/sushiro.bmp", PHASE_PAN_MS);
        xSemaphoreTake(s_task2_done, portMAX_DELAY);

        /* ---- Phase 2: shared split animation across both screens ---- */
        s_split_active = true;
        cmd.type = T2_SPLIT_SLAVE;
        cmd.path[0] = '\0';
        cmd.duration_ms = 0;
        xQueueSend(s_task2_queue, &cmd, portMAX_DELAY);
        play_split_bin("/spiffs/kirbyfly.bin");
        s_split_active = false;
        xSemaphoreTake(s_task2_done, portMAX_DELAY);

        /* ---- Phase 3: screen 1 static image, screen 2 bouncing ---- */
        cmd.type = T2_PAN_BOUNCE;
        strncpy(cmd.path, "/spiffs/okinomi.bmp", sizeof(cmd.path));
        cmd.duration_ms = PHASE_STATIC_MS;
        xQueueSend(s_task2_queue, &cmd, portMAX_DELAY);
        fill_rect_black(s_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES);
        display_bitmap(s_panel_handle, "/spiffs/sushiro.bmp", 0, 0);
        vTaskDelay(pdMS_TO_TICKS(PHASE_STATIC_MS));
        xSemaphoreTake(s_task2_done, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}

static void draw_bitmap_task2(void *arg)
{
    // Initialise SPI3 and panel 2 HERE, running on core 1, so the SPI ISR is
    // bound to core 1 and draw_bitmap calls never block waiting for a
    // core-0 interrupt.
    spi_bus_config_t buscfg2 = {
        .sclk_io_num     = PIN_NUM_SCLK2,
        .mosi_io_num     = PIN_NUM_MOSI2,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST2, &buscfg2, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle2 = NULL;
    esp_lcd_panel_io_spi_config_t io_config2 = {
        .dc_gpio_num       = PIN_NUM_DC2,
        .cs_gpio_num       = PIN_NUM_CS2,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST2, &io_config2, &io_handle2));

    esp_lcd_panel_dev_config_t panel_config2 = {
        .reset_gpio_num = PIN_NUM_RST2,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .rgb_endian     = LCD_RGB_DATA_ENDIAN_BIG,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle2, &panel_config2, &s_panel_handle2));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle2));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle2));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle2, LCD_X_OFFSET, LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle2, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle2, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle2, true));

    // Signal app_main that panel 2 is ready
    SemaphoreHandle_t ready = (SemaphoreHandle_t)arg;
    xSemaphoreGive(ready);

    /* Command dispatch loop: task1 sends phases via s_task2_queue */
    while (1) {
        t2_cmd_t cmd;
        xQueueReceive(s_task2_queue, &cmd, portMAX_DELAY);

        switch (cmd.type) {
            case T2_PAN_BOUNCE:
                pan_or_bounce_bitmap(s_panel_handle2, cmd.path, cmd.duration_ms);
                break;

            case T2_SPLIT_SLAVE:
                /* Draw right-half frames as signalled by task1 until s_split_active
                   goes false (checked every 200 ms so we don't block forever). */
                while (s_split_active) {
                    if (xSemaphoreTake(s_split_right_ready, pdMS_TO_TICKS(200)) == pdTRUE) {
                        esp_lcd_panel_draw_bitmap(s_panel_handle2, 0, 0, LCD_H_RES, LCD_V_RES, s_split_right);
                        xSemaphoreGive(s_split_right_done);
                    }
                }
                break;
        }

        xSemaphoreGive(s_task2_done);
    }
    vTaskDelete(NULL);
}

// -------------------- ST7789 init (screen 1 only — screen 2 is init'd from core 1) --------------------
static void init_st7789(void)
{
    // ---- SPI2: Screen 1 ----
    spi_bus_config_t buscfg1 = {
        .sclk_io_num     = PIN_NUM_SCLK,
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg1, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = PIN_NUM_DC,
        .cs_gpio_num       = PIN_NUM_CS,
        .pclk_hz           = 40 * 1000 * 1000, // 40 MHz
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .rgb_endian     = LCD_RGB_DATA_ENDIAN_BIG,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, LCD_X_OFFSET, LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));
}

// -------------------- app_main --------------------
void app_main(void)
{
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    neon_letter_init(PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I);

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_conf));

    init_st7789();

    // Create split-screen synchronisation semaphores (must exist before tasks start)
    s_split_right_ready = xSemaphoreCreateBinary();
    s_split_right_done  = xSemaphoreCreateBinary();
    s_task2_queue       = xQueueCreate(1, sizeof(t2_cmd_t));
    s_task2_done        = xSemaphoreCreateBinary();

    // Start draw task 2 on core 1 first — it initialises SPI3/panel2 itself
    // (binding the SPI ISR to core 1), then signals the semaphore when ready.
    SemaphoreHandle_t panel2_ready = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(draw_bitmap_task2, "draw_bmp2", 8192, panel2_ready, 2, NULL, 1);
    xSemaphoreTake(panel2_ready, portMAX_DELAY);
    vSemaphoreDelete(panel2_ready);

    // Now start draw task 1 on core 0 (SPI2 ISR already on core 0 from init_st7789)
    xTaskCreatePinnedToCore(draw_bitmap_task1, "draw_bmp1", 8192, NULL, 2, NULL, 0);
}
