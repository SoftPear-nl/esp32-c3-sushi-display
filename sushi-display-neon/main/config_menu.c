#include "config_menu.h"

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "neon_letter_service.h"

// ---------------------------------------------------------------------------
// Screen geometry
// ---------------------------------------------------------------------------
#define LCD_W  240
#define LCD_H  320

// ---------------------------------------------------------------------------
// Colour palette (RGB565 big-endian for ST7789)
// ---------------------------------------------------------------------------
#define COL_BG          0x0000u  // black
#define COL_TITLE_FG    0xFFE0u  // yellow
#define COL_ITEM_FG     0xFFFFu  // white
#define COL_SEL_BG      0x3166u  // dark blue highlight
#define COL_SEL_FG      0xFFFFu  // white
#define COL_HINT_FG     0x8410u  // grey

// Swap bytes for big-endian SPI
#define BE16(c) (uint16_t)(((c) >> 8) | ((c) << 8))

// ---------------------------------------------------------------------------
// 5×7 font (ASCII 32–127)
// Each character is 5 columns of 7-bit mask, LSB = top row.
// ---------------------------------------------------------------------------
static const uint8_t s_font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x30,0x30,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x00,0x08,0x14,0x22,0x41}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x41,0x22,0x14,0x08,0x00}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x09,0x01}, // 70 F
    {0x3E,0x41,0x49,0x49,0x7A}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x3F,0x40,0x38,0x40,0x3F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x07,0x08,0x70,0x08,0x07}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
    {0x00,0x7F,0x41,0x41,0x00}, // 91 [
    {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
    {0x00,0x41,0x41,0x7F,0x00}, // 93 ]
    {0x04,0x02,0x01,0x02,0x04}, // 94 ^
    {0x40,0x40,0x40,0x40,0x40}, // 95 _
    {0x00,0x01,0x02,0x04,0x00}, // 96 `
    {0x20,0x54,0x54,0x54,0x78}, // 97 a
    {0x7F,0x48,0x44,0x44,0x38}, // 98 b
    {0x38,0x44,0x44,0x44,0x20}, // 99 c
    {0x38,0x44,0x44,0x48,0x7F}, // 100 d
    {0x38,0x54,0x54,0x54,0x18}, // 101 e
    {0x08,0x7E,0x09,0x01,0x02}, // 102 f
    {0x08,0x54,0x54,0x54,0x3C}, // 103 g
    {0x7F,0x08,0x04,0x04,0x78}, // 104 h
    {0x00,0x44,0x7D,0x40,0x00}, // 105 i
    {0x20,0x40,0x44,0x3D,0x00}, // 106 j
    {0x7F,0x10,0x28,0x44,0x00}, // 107 k
    {0x00,0x41,0x7F,0x40,0x00}, // 108 l
    {0x7C,0x04,0x18,0x04,0x78}, // 109 m
    {0x7C,0x08,0x04,0x04,0x78}, // 110 n
    {0x38,0x44,0x44,0x44,0x38}, // 111 o
    {0x7C,0x14,0x14,0x14,0x08}, // 112 p
    {0x08,0x14,0x14,0x18,0x7C}, // 113 q
    {0x7C,0x08,0x04,0x04,0x08}, // 114 r
    {0x48,0x54,0x54,0x54,0x20}, // 115 s
    {0x04,0x3F,0x44,0x40,0x20}, // 116 t
    {0x3C,0x40,0x40,0x40,0x3C}, // 117 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
    {0x44,0x28,0x10,0x28,0x44}, // 120 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
    {0x44,0x64,0x54,0x4C,0x44}, // 122 z
    {0x00,0x08,0x36,0x41,0x00}, // 123 {
    {0x00,0x00,0x7F,0x00,0x00}, // 124 |
    {0x00,0x41,0x36,0x08,0x00}, // 125 }
    {0x08,0x04,0x08,0x10,0x08}, // 126 ~
    {0x00,0x00,0x00,0x00,0x00}, // 127 DEL (blank)
};

// ---------------------------------------------------------------------------
// Menu data
// ---------------------------------------------------------------------------

typedef struct {
    const char *label;
    enum led_mode_t value;
} led_mode_item_t;

static const led_mode_item_t s_led_items[] = {
    { "All patterns", LED_MODE_ALL_PATTERNS },
    { "Always on",    LED_MODE_ON           },
    { "Always off",   LED_MODE_OFF          },
};
#define NUM_LED_ITEMS  (int)(sizeof(s_led_items) / sizeof(s_led_items[0]))

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static esp_lcd_panel_handle_t s_panel   = NULL;
static QueueHandle_t           s_queue  = NULL;
static bool                    s_open   = false;
static int                     s_cursor = 0;   // currently highlighted item

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// Write one character glyph into a row-buffer starting at pixel offset x.
// Scale 2 = 10 px wide × 14 px tall.
#define GLYPH_W  5
#define GLYPH_H  7
#define SCALE    2
#define CHAR_W   (GLYPH_W * SCALE + SCALE)   // 12 px per char incl. spacing
#define CHAR_H   (GLYPH_H * SCALE)            // 14 px tall

static void draw_char(uint16_t *row_bufs, int char_row, char c, int x,
                      uint16_t fg, uint16_t bg)
{
    if (c < 32 || c > 127) c = '?';
    const uint8_t *glyph = s_font5x7[(uint8_t)c - 32];
    int glyph_row = char_row / SCALE;  // which of the 7 glyph rows
    if (glyph_row >= GLYPH_H) return;

    for (int col = 0; col < GLYPH_W; col++) {
        uint8_t bit = (glyph[col] >> glyph_row) & 1;
        uint16_t colour = bit ? fg : bg;
        for (int s = 0; s < SCALE; s++) {
            if (x + col * SCALE + s < LCD_W)
                row_bufs[x + col * SCALE + s] = colour;
        }
    }
    // Inter-character spacing column(s)
    for (int s = 0; s < SCALE; s++) {
        int px = x + GLYPH_W * SCALE + s;
        if (px < LCD_W) row_bufs[px] = bg;
    }
}

static void draw_string(uint16_t *row_buf, int char_row, const char *str,
                        int x, uint16_t fg, uint16_t bg)
{
    for (; *str; str++, x += CHAR_W)
        draw_char(row_buf, char_row, *str, x, fg, bg);
}

// Fill an entire row buffer with a solid colour
static void fill_row(uint16_t *row_buf, uint16_t colour)
{
    for (int i = 0; i < LCD_W; i++) row_buf[i] = colour;
}

// Draw one text row (CHAR_H screen rows) to the panel
static void draw_text_row(uint16_t *row_buf, int screen_y,
                          const char *str, uint16_t fg, uint16_t bg)
{
    for (int r = 0; r < CHAR_H; r++) {
        fill_row(row_buf, bg);
        draw_string(row_buf, r, str, 4, fg, bg);
        esp_lcd_panel_draw_bitmap(s_panel,
                                  0, screen_y + r,
                                  LCD_W, screen_y + r + 1,
                                  row_buf);
    }
}

// Draw a solid colour bar of 'height' screen rows
static void draw_bar(uint16_t *row_buf, int screen_y, int height, uint16_t colour)
{
    fill_row(row_buf, colour);
    for (int r = 0; r < height; r++) {
        esp_lcd_panel_draw_bitmap(s_panel,
                                  0, screen_y + r,
                                  LCD_W, screen_y + r + 1,
                                  row_buf);
    }
}

// ---------------------------------------------------------------------------
// Full menu render
// ---------------------------------------------------------------------------

#define TITLE_Y      8
#define ITEMS_START_Y 40
#define ITEM_SPACING  (CHAR_H + 8)
#define HINT_Y       (LCD_H - CHAR_H - 8)

static void render_menu(void)
{
    // Allocate a single row-buffer to avoid large stack usage
    uint16_t *row_buf = malloc(LCD_W * sizeof(uint16_t));
    if (!row_buf) return;

    // Clear screen
    draw_bar(row_buf, 0, LCD_H, BE16(COL_BG));

    // Title
    draw_text_row(row_buf, TITLE_Y, "=== CONFIG ===",
                  BE16(COL_TITLE_FG), BE16(COL_BG));

    // Divider
    draw_bar(row_buf, TITLE_Y + CHAR_H + 4, 1, BE16(0x4208u));  // dark grey line

    // LED mode label
    draw_text_row(row_buf, ITEMS_START_Y - CHAR_H - 2, "LED mode:",
                  BE16(COL_HINT_FG), BE16(COL_BG));

    // Items
    for (int i = 0; i < NUM_LED_ITEMS; i++) {
        int y = ITEMS_START_Y + i * ITEM_SPACING;
        bool sel = (i == s_cursor);

        uint16_t fg = sel ? BE16(COL_SEL_FG)  : BE16(COL_ITEM_FG);
        uint16_t bg = sel ? BE16(COL_SEL_BG)  : BE16(COL_BG);

        // Highlight bar includes a little padding
        if (sel) draw_bar(row_buf, y - 2, CHAR_H + 4, BE16(COL_SEL_BG));

        // Cursor arrow for selected item
        char line[32];
        snprintf(line, sizeof(line), "%s%s",
                 sel ? "> " : "  ",
                 s_led_items[i].label);
        draw_text_row(row_buf, y, line, fg, bg);
    }

    // Bottom hint
    draw_text_row(row_buf, HINT_Y, "CTR=confirm/exit  U/D=nav",
                  BE16(COL_HINT_FG), BE16(COL_BG));

    free(row_buf);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void config_menu_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    s_queue = xQueueCreate(16, sizeof(btn_event_t));
}

QueueHandle_t config_menu_get_queue(void)
{
    return s_queue;
}

bool config_menu_is_open(void)
{
    return s_open;
}

void config_menu_tick(void)
{
    btn_event_t evt;
    while (xQueueReceive(s_queue, &evt, 0) == pdTRUE) {
        if (!s_open) {
            if (evt == BTN_CENTER) {
                // Find the cursor position that matches the current mode
                for (int i = 0; i < NUM_LED_ITEMS; i++) {
                    if (s_led_items[i].value == current_led_mode) {
                        s_cursor = i;
                        break;
                    }
                }
                s_open = true;
                render_menu();
            }
        } else {
            switch (evt) {
                case BTN_UP:
                    if (s_cursor > 0) s_cursor--;
                    render_menu();
                    break;
                case BTN_DOWN:
                    if (s_cursor < NUM_LED_ITEMS - 1) s_cursor++;
                    render_menu();
                    break;
                case BTN_CENTER:
                    // Apply selection and close
                    current_led_mode = s_led_items[s_cursor].value;
                    s_open = false;
                    break;
                default:
                    break;
            }
        }
    }
}
