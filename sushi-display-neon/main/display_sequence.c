/**
 * display_sequence.c
 *
 * Four scene types for two 240×320 ST7789 displays:
 *   SCENE_STATIC    – show a BMP centred on one screen for a fixed time
 *   SCENE_PAN       – scroll a BMP viewport across one screen
 *   SCENE_BOUNCE    – bounce a BMP around on one screen
 *   SCENE_ANIM_DUAL – play a .bin animation on both screens
 *
 * BMP files: 16-bpp RGB565, BI_BITFIELDS, 66-byte header.
 *   Pixels are stored in little-endian order; they must be byte-swapped
 *   before being sent to the ST7789 (which expects big-endian, MSB first).
 *
 * .bin animation files: 8-byte header (w, h, frames, fps — all uint16_t LE)
 *   followed by raw big-endian RGB565 frames, ready for direct SPI output.
 */

#include "display_sequence.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "disp_seq";

#define LCD_W  240
#define LCD_H  320

// ---------------------------------------------------------------------------
// BMP loader — sequential SPIFFS read (O(1) seeks, never fseek in a loop)
// ---------------------------------------------------------------------------

typedef struct {
    int32_t  width;      ///< width  in pixels (always positive)
    int32_t  height;     ///< height in pixels (always positive)
    uint32_t row_stride; ///< bytes per row on disk, padded to 4 bytes
    bool     top_down;   ///< true when BMP height field is negative
} bmp_info_t;

/**
 * Load an entire 16-bpp BMP into a freshly malloc'd PSRAM buffer.
 *
 * Layout: top-row first, left-to-right, 2 bytes/pixel, big-endian RGB565
 * (byte-swapped, ready for ST7789 over SPI).
 *
 * Uses ONE fseek to jump to pixel data, then reads ALL rows sequentially.
 * Yields to the FreeRTOS scheduler every 16 rows so the IDLE task can run.
 *
 * Returns NULL on failure.  Caller must free().
 */
static uint16_t *bmp_load(const char *path, bmp_info_t *info_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "bmp_load: cannot open '%s'", path); return NULL; }

    uint8_t hdr[66];
    if (fread(hdr, 1, sizeof(hdr), f) < sizeof(hdr) ||
            hdr[0] != 'B' || hdr[1] != 'M') {
        ESP_LOGE(TAG, "bmp_load: bad header '%s'", path);
        fclose(f); return NULL;
    }

    uint32_t po = (uint32_t)(hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24));
    int32_t  w  = (int32_t) (hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24));
    int32_t  h  = (int32_t) (hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24));

    bmp_info_t info;
    info.width      = (w < 0) ? -w : w;
    info.height     = (h < 0) ? -h : h;
    info.top_down   = (h < 0);
    info.row_stride = ((uint32_t)(info.width * 2 + 3) / 4) * 4;
    if (info_out) *info_out = info;

    // Destination buffer: top-to-bottom, no row padding
    uint16_t *buf = heap_caps_malloc((size_t)info.width * info.height * 2,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "bmp_load: OOM %u bytes for '%s'",
                 (unsigned)(info.width * info.height * 2), path);
        fclose(f); return NULL;
    }

    // Small row buffer in internal DRAM (max 256×2 = 512 bytes)
    uint16_t *row_buf = malloc(info.row_stride);
    if (!row_buf) {
        ESP_LOGE(TAG, "bmp_load: OOM row buf");
        free(buf); fclose(f); return NULL;
    }

    // ONE seek to pixel data, then pure sequential fread
    if (fseek(f, (long)po, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "bmp_load: fseek failed '%s'", path);
        free(row_buf); free(buf); fclose(f); return NULL;
    }

    // Disk rows: bottom-up unless top_down.
    // disk_row 0 → img_row (height-1) for normal BMP, img_row 0 for top-down.
    for (int disk_row = 0; disk_row < info.height; disk_row++) {
        size_t got = fread(row_buf, 1, info.row_stride, f);
        if (got < (size_t)info.row_stride)
            memset((uint8_t *)row_buf + got, 0, info.row_stride - got);

        int img_row = info.top_down ? disk_row : (info.height - 1 - disk_row);
        uint16_t *dst = buf + (size_t)img_row * info.width;
        for (int c = 0; c < info.width; c++) {
            uint16_t px = row_buf[c];
            dst[c] = (uint16_t)((px >> 8) | (px << 8)); // LE→BE
        }
        // Yield every 16 rows so IDLE task can reset the watchdog.
        // Use raw tick (1), NOT pdMS_TO_TICKS(1) which is 0 on 100 Hz systems.
        if ((disk_row & 15) == 15) vTaskDelay(1);
    }

    free(row_buf);
    fclose(f);
    return buf;
}

// ---------------------------------------------------------------------------
// Utility: fill a screen with black
// ---------------------------------------------------------------------------

static void clear_screen(esp_lcd_panel_handle_t panel)
{
    // One full-frame zero buffer → single SPI transaction instead of 320
    uint16_t *zbuf = heap_caps_calloc(LCD_W * LCD_H, 2,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (zbuf) {
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, zbuf);
        free(zbuf);
    } else {
        // Fallback: line-by-line with periodic yields
        static uint16_t zline[LCD_W]; // zero-initialised in BSS
        for (int y = 0; y < LCD_H; y++) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + 1, zline);
            if ((y & 15) == 15) vTaskDelay(1); // raw tick, not pdMS_TO_TICKS(1)
        }
    }
}

// ---------------------------------------------------------------------------
// SCENE_STATIC
// ---------------------------------------------------------------------------

static void scene_static(esp_lcd_panel_handle_t panel, const scene_t *s)
{
    bmp_info_t info;
    uint16_t *img = bmp_load(s->file_path, &info);
    if (!img) return;

    int draw_w = info.width  < LCD_W ? info.width  : LCD_W;
    int draw_h = info.height < LCD_H ? info.height : LCD_H;
    int off_x  = (LCD_W - draw_w) / 2;
    int off_y  = (LCD_H - draw_h) / 2;

    // Full-screen frame buffer: black canvas with image composited in
    uint16_t *fb = heap_caps_calloc(LCD_W * LCD_H, 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb) {
        for (int row = 0; row < draw_h; row++) {
            memcpy(fb + (size_t)(off_y + row) * LCD_W + off_x,
                   img + (size_t)row * info.width,
                   (size_t)draw_w * 2);
        }
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        free(fb);
    }

    free(img);
    vTaskDelay(pdMS_TO_TICKS(s->duration_ms));
}

// ---------------------------------------------------------------------------
// SCENE_PAN
// ---------------------------------------------------------------------------

/**
 * Pan algorithm
 * ─────────────
 * The image may be larger than the screen in the scroll direction and/or
 * smaller in the perpendicular direction (centred with black fill).
 *
 * A full-screen PSRAM frame buffer is rebuilt for each step, then sent as a
 * single draw_bitmap call – this avoids 320 individual SPI transactions and
 * gives smooth, tear-free panning.
 */
static void scene_pan(esp_lcd_panel_handle_t panel, const scene_t *s)
{
    // Load entire image into PSRAM first — avoids SPIFFS fseek-per-row in the loop
    bmp_info_t info;
    uint16_t *img = bmp_load(s->file_path, &info);
    if (!img) return;

    bool horiz = (s->pan_dir == DIR_LEFT || s->pan_dir == DIR_RIGHT);

    int pan_steps = horiz
        ? (info.width  > LCD_W ? info.width  - LCD_W : 0)
        : (info.height > LCD_H ? info.height - LCD_H : 0);

    // Centering offset in the non-scroll dimension
    int off_x = (!horiz && info.width  < LCD_W) ? (LCD_W - info.width)  / 2 : 0;
    int off_y = ( horiz && info.height < LCD_H) ? (LCD_H - info.height) / 2 : 0;

    size_t fb_sz = (size_t)LCD_W * LCD_H * 2;
    uint16_t *fb = heap_caps_malloc(fb_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE(TAG, "pan: OOM frame buffer");
        free(img);
        return;
    }

    for (int step = 0; step <= pan_steps; step++) {
        int pan_val = (s->pan_dir == DIR_RIGHT || s->pan_dir == DIR_DOWN)
                      ? (pan_steps - step) : step;
        int bmp_ox = horiz ? pan_val : 0;
        int bmp_oy = horiz ? 0       : pan_val;

        memset(fb, 0, fb_sz);

        // Compose frame from PSRAM image — pure RAM ops, no file I/O
        for (int sy = 0; sy < LCD_H; sy++) {
            int img_row = horiz ? (sy - off_y) : (sy - off_y + bmp_oy);
            if (img_row < 0 || img_row >= info.height) continue;

            int copy_w = info.width - bmp_ox;
            if (copy_w > LCD_W - off_x) copy_w = LCD_W - off_x;
            if (copy_w <= 0) continue;

            memcpy(fb + (size_t)sy * LCD_W + off_x,
                   img + (size_t)img_row * info.width + bmp_ox,
                   (size_t)copy_w * 2);
        }

        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        vTaskDelay(pdMS_TO_TICKS(s->pan_step_ms > 0 ? s->pan_step_ms : 1));
    }

    free(fb);
    free(img);
}

// Forward declarations — implementations follow display_sequence_run below
static void scene_bounce(esp_lcd_panel_handle_t panel, const scene_t *s);
static void scene_anim_dual(esp_lcd_panel_handle_t panel1,
                             esp_lcd_panel_handle_t panel2,
                             const scene_t *s);

// ---------------------------------------------------------------------------
// Single-scene dispatcher (used by both sequential and parallel paths)
// ---------------------------------------------------------------------------

static void run_single_scene(esp_lcd_panel_handle_t panel1,
                              esp_lcd_panel_handle_t panel2,
                              const scene_t *s)
{
    esp_lcd_panel_handle_t panel =
        (s->screen == SCREEN_1) ? panel1 : panel2;

    switch (s->type) {
        case SCENE_STATIC:
            scene_static(panel, s);
            break;
        case SCENE_PAN:
            scene_pan(panel, s);
            break;
        case SCENE_BOUNCE:
            scene_bounce(panel, s);
            break;
        case SCENE_ANIM_DUAL:
            scene_anim_dual(panel1, panel2, s);
            break;
        default:
            ESP_LOGW(TAG, "Unknown scene type %d – skipped", (int)s->type);
            break;
    }
}

// ---------------------------------------------------------------------------
// Parallel task support
// ---------------------------------------------------------------------------

typedef struct {
    esp_lcd_panel_handle_t panel1;
    esp_lcd_panel_handle_t panel2;
    const scene_t         *scene;
    TaskHandle_t           caller; ///< main task to notify on completion
} parallel_arg_t;

static void parallel_scene_task(void *arg)
{
    parallel_arg_t *a = (parallel_arg_t *)arg;
    run_single_scene(a->panel1, a->panel2, a->scene);
    xTaskNotifyGive(a->caller);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void display_sequence_run(
        esp_lcd_panel_handle_t panel1,
        esp_lcd_panel_handle_t panel2,
        const scene_t         *scenes,
        int                    scene_count)
{
    for (int i = 0; i < scene_count; i++) {
        const scene_t *s = &scenes[i];

        ESP_LOGI(TAG, "Scene %d/%d  type=%d  file=%s%s",
                 i + 1, scene_count, (int)s->type, s->file_path,
                 s->parallel ? "  [parallel]" : "");

        if (s->parallel && i + 1 < scene_count) {
            const scene_t *s_next = &scenes[i + 1];
            ESP_LOGI(TAG, "Scene %d/%d  type=%d  file=%s  [parallel partner]",
                     i + 2, scene_count, (int)s_next->type, s_next->file_path);

            // arg lives on our stack — valid until ulTaskNotifyTake returns
            parallel_arg_t arg = {
                .panel1 = panel1,
                .panel2 = panel2,
                .scene  = s_next,
                .caller = xTaskGetCurrentTaskHandle(),
            };

            // Spawn partner on the other core (or same core if pinned)
            xTaskCreate(parallel_scene_task, "par_scene",
                        4096, &arg,
                        uxTaskPriorityGet(NULL), NULL);

            // Run the current scene on this task
            run_single_scene(panel1, panel2, s);

            // Wait for the partner task to finish
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            i++; // skip the paired scene — already dispatched above
        } else {
            run_single_scene(panel1, panel2, s);
        }
    }
}

// ===========================================================================
// SCENE_BOUNCE
// ===========================================================================
/**
 * Flicker-free bounce using a full-screen PSRAM framebuffer.
 *
 * Each step: erase old sprite, blit new sprite (both pure RAM ops),
 * then send the entire framebuffer in ONE draw_bitmap call.
 * A single contiguous SPI transaction = no intermediate black frame.
 */
static void scene_bounce(esp_lcd_panel_handle_t panel, const scene_t *s)
{
    bmp_info_t info;
    uint16_t *img_buf = bmp_load(s->file_path, &info);
    if (!img_buf) return;

    int img_w = info.width  < LCD_W ? info.width  : LCD_W;
    int img_h = info.height < LCD_H ? info.height : LCD_H;

    // Full-screen framebuffer in PSRAM, zero-initialised (black canvas)
    uint16_t *fb = heap_caps_calloc(LCD_W * LCD_H, 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE(TAG, "bounce: OOM framebuffer");
        free(img_buf);
        return;
    }

    int x  = (LCD_W - img_w) / 2;
    int y  = (LCD_H - img_h) / 2;
    int dx = s->bounce_dx ? s->bounce_dx : 3;
    int dy = s->bounce_dy ? s->bounce_dy : 2;

    // Blit sprite into framebuffer at (bx, by)
    #define BLIT(bx, by) \
        for (int _r = 0; _r < img_h; _r++) \
            memcpy(fb + (size_t)((by) + _r) * LCD_W + (bx), \
                   img_buf + (size_t)_r * img_w, \
                   (size_t)img_w * 2)

    // Clear sprite rect in framebuffer at (bx, by)
    #define ERASE(bx, by) \
        for (int _r = 0; _r < img_h; _r++) \
            memset(fb + (size_t)((by) + _r) * LCD_W + (bx), 0, \
                   (size_t)img_w * 2)

    BLIT(x, y);
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);

    TickType_t start  = xTaskGetTickCount();
    TickType_t dur_tk = pdMS_TO_TICKS(s->bounce_dur_ms);

    while ((xTaskGetTickCount() - start) < dur_tk) {
        int px = x, py = y;

        x += dx;
        y += dy;

        if (x < 0)             { x = 0;             dx =  abs(dx); }
        if (x + img_w > LCD_W) { x = LCD_W - img_w; dx = -abs(dx); }
        if (y < 0)             { y = 0;             dy =  abs(dy); }
        if (y + img_h > LCD_H) { y = LCD_H - img_h; dy = -abs(dy); }

        if (x != px || y != py) {
            ERASE(px, py);
            BLIT(x, y);
            // Full-screen send: one contiguous buffer, no stride issues
            esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        }

        vTaskDelay(pdMS_TO_TICKS(s->bounce_step_ms > 0 ? s->bounce_step_ms : 16));
    }

    #undef BLIT
    #undef ERASE

    free(fb);
    free(img_buf);
}

// ===========================================================================
// SCENE_ANIM_DUAL
// ===========================================================================
/**
 * .bin file format (from gif_to_bin.html tool):
 *   bytes 0-1  : image width  (uint16_t little-endian)
 *   bytes 2-3  : image height (uint16_t little-endian)
 *   bytes 4-5  : frame count  (uint16_t little-endian)
 *   bytes 6-7  : fps          (uint16_t little-endian)
 *   bytes 8+   : raw RGB565 frames, big-endian, row-major, no padding
 *
 * DUAL_SAME    : every frame is sent to both panels centred (with optional
 *                nearest-neighbour upscaling via s->scale).
 * DUAL_SPLIT_H : left half of the (optionally scaled) frame → panel1,
 *                right half → panel2.  With scale=2, a 240×160 source fills
 *                both 240×320 screens edge-to-edge.
 *
 * Nearest-neighbour scaling: each destination pixel (dr, dc) is mapped to
 * source pixel (dr/scale, dc/scale) — integer division truncates.
 */

/**
 * Fill `dest` (dest_w × dest_h pixels) with the nearest-neighbour upscaled
 * version of `src_col_offset .. src_col_offset+src_w-1` columns from a row
 * in `src` (full frame width anim_w), repeated for src_h source rows.
 * dest_w == src_w * scale,  dest_h == src_h * scale.
 */
static void scale_half(const uint16_t *frame_buf, int anim_w,
                        int src_col_offset, int src_w, int src_h,
                        int scale,
                        uint16_t *dest)
{
    int dest_w = src_w * scale;

    if (scale == 2) {
        // Fast-path for 2×: write two identical pixels as one uint32_t,
        // then memcpy the first scaled row to produce the duplicate row.
        for (int src_r = 0; src_r < src_h; src_r++) {
            const uint16_t *src_row = frame_buf + (size_t)src_r * anim_w + src_col_offset;
            uint16_t *row0 = dest + (size_t)(src_r * 2)     * dest_w;
            uint16_t *row1 = dest + (size_t)(src_r * 2 + 1) * dest_w;
            uint32_t *out32 = (uint32_t *)row0;
            for (int src_c = 0; src_c < src_w; src_c++) {
                uint32_t px = src_row[src_c];
                out32[src_c] = px | (px << 16); // two identical pixels
            }
            memcpy(row1, row0, (size_t)dest_w * 2); // duplicate row
        }
        return;
    }

    // Generic path for other scale factors
    for (int src_r = 0; src_r < src_h; src_r++) {
        const uint16_t *src_row = frame_buf + (size_t)src_r * anim_w + src_col_offset;
        for (int dr = 0; dr < scale; dr++) {
            uint16_t *dst_row = dest + (size_t)(src_r * scale + dr) * dest_w;
            for (int src_c = 0; src_c < src_w; src_c++) {
                uint16_t px = src_row[src_c];
                for (int dc = 0; dc < scale; dc++) {
                    dst_row[src_c * scale + dc] = px;
                }
            }
        }
    }
}

static void scene_anim_dual(esp_lcd_panel_handle_t panel1,
                             esp_lcd_panel_handle_t panel2,
                             const scene_t *s)
{
    FILE *f = fopen(s->file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "anim: cannot open '%s'", s->file_path);
        return;
    }

    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) < sizeof(hdr)) {
        ESP_LOGE(TAG, "anim: short header in '%s'", s->file_path);
        fclose(f);
        return;
    }
    int      anim_w   = (int)(hdr[0] | (hdr[1] << 8));
    int      anim_h   = (int)(hdr[2] | (hdr[3] << 8));
    int      frames   = (int)(hdr[4] | (hdr[5] << 8));
    int      fps      = (int)(hdr[6] | (hdr[7] << 8));
    int64_t  frame_us = (fps > 0) ? (1000000LL / fps) : 100000LL;
    int      sc       = (s->scale > 1) ? s->scale : 1;

    ESP_LOGI(TAG, "anim: %dx%d  %d frames @ %d fps  scale=%d",
             anim_w, anim_h, frames, fps, sc);

    size_t frame_bytes = (size_t)anim_w * anim_h * 2;
    uint16_t *frame_buf = heap_caps_malloc(frame_bytes,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buf) {
        ESP_LOGE(TAG, "anim: OOM frame buffer"); fclose(f); return;
    }

    int half_src_w = (s->dual_mode == DUAL_SPLIT_H) ? anim_w / 2 : anim_w;
    int out_w      = half_src_w * sc;
    int out_h      = anim_h     * sc;
    int draw_w     = out_w < LCD_W ? out_w : LCD_W;
    int draw_h     = out_h < LCD_H ? out_h : LCD_H;
    int off_x      = (LCD_W - draw_w) / 2;
    int off_y      = (LCD_H - draw_h) / 2;

    size_t out_bytes = (size_t)out_w * out_h * 2;
    uint16_t *buf1 = heap_caps_malloc(out_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *buf2 = heap_caps_malloc(out_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "anim: OOM output buffers");
        free(buf1); free(buf2); free(frame_buf); fclose(f); return;
    }

    clear_screen(panel1);
    clear_screen(panel2);

    for (int fi = 0; fi < frames; fi++) {
        int64_t t0 = esp_timer_get_time();

        if (fread(frame_buf, 2, (size_t)anim_w * anim_h, f) <
                (size_t)(anim_w * anim_h)) {
            ESP_LOGW(TAG, "anim: short read at frame %d", fi);
            break;
        }

        if (s->dual_mode == DUAL_SPLIT_H) {
            scale_half(frame_buf, anim_w, 0,          half_src_w, anim_h, sc, buf1);
            scale_half(frame_buf, anim_w, half_src_w, half_src_w, anim_h, sc, buf2);
        } else {
            scale_half(frame_buf, anim_w, 0, anim_w, anim_h, sc, buf1);
            memcpy(buf2, buf1, out_bytes);
        }

        // Sequential sends — no extra task, no priority starvation of IDLE.
        // SPI DMA is still hardware-accelerated; the CPU is mostly idle during
        // the transfer anyway (driver blocks on a semaphore between chunks).
        esp_lcd_panel_draw_bitmap(panel1, off_x, off_y,
                                  off_x + draw_w, off_y + draw_h, buf1);
        esp_lcd_panel_draw_bitmap(panel2, off_x, off_y,
                                  off_x + draw_w, off_y + draw_h, buf2);

        // Determine target frame duration
        int64_t target_us = frame_us;
        if      (fi == 0          && s->first_frame_ms > 0) target_us = (int64_t)s->first_frame_ms * 1000;
        else if (fi == frames - 1 && s->last_frame_ms  > 0) target_us = (int64_t)s->last_frame_ms  * 1000;

        // Always yield at least one raw RTOS tick so IDLE can feed the WDT.
        // CRITICAL: pdMS_TO_TICKS(1) == 0 on a 100 Hz tick system, so
        // vTaskDelay(pdMS_TO_TICKS(1)) is a no-op.  Use raw ticks instead:
        // vTaskDelay(1) always suspends for exactly 1 tick regardless of Hz.
        int64_t elapsed_us   = esp_timer_get_time() - t0;
        int64_t remaining_ms = (target_us - elapsed_us) / 1000;
        TickType_t ticks = (remaining_ms > 0) ? pdMS_TO_TICKS((uint32_t)remaining_ms) : 0;
        vTaskDelay(ticks > 0 ? ticks : 1);
    }

    free(buf1);
    free(buf2);
    free(frame_buf);
    fclose(f);
}
