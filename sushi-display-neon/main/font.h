#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// 8×8 bitmap font — printable ASCII (0x20–0x7E)
// ---------------------------------------------------------------------------
// Each glyph is 8 rows × 8 columns, 1 bit per pixel.
// LSB of each byte = leftmost pixel, top row first.
//
// Colors must be in RGB565 big-endian format (byte-swapped for ST7789).
// Use font_color() to convert 8-bit R,G,B to the correct format.
//
// Usage example:
//   uint16_t fg = font_color(255, 255,   0);  // yellow
//   uint16_t bg = font_color(  0,   0,   0);  // black
//   font_draw_string(fb, 240, 320, 8, 8, "HELLO", fg, bg, 2);
// ---------------------------------------------------------------------------

#define FONT_GLYPH_W  8   ///< Base glyph width  in pixels
#define FONT_GLYPH_H  8   ///< Base glyph height in pixels

/**
 * @brief Pack 8-bit R,G,B values into RGB565 big-endian (byte-swapped for ST7789).
 */
static inline uint16_t font_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((uint16_t)(r & 0xF8u) << 8)
               | ((uint16_t)(g & 0xFCu) << 3)
               |  (uint16_t)(b          >> 3);
    return (uint16_t)((v >> 8) | (v << 8));  // byte-swap → big-endian
}

// Pre-built colours (evaluated at runtime via static inline; no ROM cost)
#define FONT_WHITE    font_color(255, 255, 255)
#define FONT_BLACK    font_color(  0,   0,   0)
#define FONT_RED      font_color(255,   0,   0)
#define FONT_GREEN    font_color(  0, 255,   0)
#define FONT_BLUE     font_color(  0,   0, 255)
#define FONT_YELLOW   font_color(255, 255,   0)
#define FONT_CYAN     font_color(  0, 255, 255)
#define FONT_MAGENTA  font_color(255,   0, 255)
#define FONT_ORANGE   font_color(255, 165,   0)

/** Pass as @p bg to skip drawing background pixels (transparent). */
#define FONT_TRANSPARENT  ((uint16_t)0xFFFF)

/**
 * @brief Draw one character into an RGB565 framebuffer.
 *
 * @param fb     Pointer to the framebuffer (must be fb_w × fb_h uint16_t values).
 * @param fb_w   Framebuffer width in pixels (also the row stride).
 * @param fb_h   Framebuffer height in pixels (used for clipping).
 * @param x      Left edge of the glyph in pixels.
 * @param y      Top  edge of the glyph in pixels.
 * @param c      ASCII character to draw.
 * @param fg     Foreground colour (RGB565 big-endian, use font_color()).
 * @param bg     Background colour (RGB565 big-endian, use font_color()).
 * @param scale  Integer upscale factor (1 = normal, 2 = 2×, etc.).
 */
void font_draw_char(uint16_t *fb, int fb_w, int fb_h,
                    int x, int y, char c,
                    uint16_t fg, uint16_t bg, int scale);

/**
 * @brief Draw a null-terminated ASCII string into an RGB565 framebuffer.
 *
 * Characters are placed left-to-right starting at (x, y).  No automatic
 * line-wrapping is performed.
 *
 * @param fb     Pointer to the framebuffer.
 * @param fb_w   Framebuffer width in pixels.
 * @param fb_h   Framebuffer height in pixels.
 * @param x      Left edge of the first character.
 * @param y      Top  edge of the first character.
 * @param str    Null-terminated ASCII string.
 * @param fg     Foreground colour (RGB565 big-endian).
 * @param bg     Background colour (RGB565 big-endian).
 * @param scale  Integer upscale factor.
 * @return       X coordinate immediately after the last character drawn.
 */
int font_draw_string(uint16_t *fb, int fb_w, int fb_h,
                     int x, int y, const char *str,
                     uint16_t fg, uint16_t bg, int scale);
