#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_ops.h"

// ---------------------------------------------------------------------------
// Scene types
// ---------------------------------------------------------------------------

typedef enum {
    SCENE_STATIC,       ///< Show a static BMP on one screen for a fixed time
    SCENE_PAN,          ///< Pan a BMP (larger than the screen) across one screen
    SCENE_BOUNCE,       ///< Bounce a small BMP around on one screen
    SCENE_ANIM_DUAL,    ///< Play a .bin animation on both screens
} scene_type_t;

typedef enum {
    SCREEN_1 = 0,       ///< First  LCD panel (SPI2)
    SCREEN_2 = 1,       ///< Second LCD panel (SPI3)
} screen_id_t;

/// Direction in which the viewport moves through the image
typedef enum {
    DIR_LEFT,   ///< Viewport moves toward higher X (image slides left)
    DIR_RIGHT,  ///< Viewport moves toward lower  X (image slides right)
    DIR_UP,     ///< Viewport moves toward higher Y (image slides up)
    DIR_DOWN,   ///< Viewport moves toward lower  Y (image slides down)
} pan_dir_t;

/// How a dual-screen animation distributes pixels across screens
typedef enum {
    DUAL_SAME,      ///< Identical frame shown centered on both screens
    DUAL_SPLIT_H,   ///< Left  half of frame → screen 1,
                    ///<  right half of frame → screen 2
                    ///<  (requires anim width == 2 × LCD_H_RES)
} dual_screen_mode_t;

// ---------------------------------------------------------------------------
// Scene descriptor
// ---------------------------------------------------------------------------

typedef struct {
    scene_type_t  type;
    const char   *file_path;   ///< SPIFFS path, e.g. "/spiffs/image.bmp"

    // ---- Used by STATIC / PAN / BOUNCE ----
    screen_id_t   screen;

    // ---- SCENE_STATIC ----
    uint32_t      duration_ms;    ///< How long to hold the image (ms)

    // ---- SCENE_PAN ----
    pan_dir_t     pan_dir;        ///< Which direction the viewport scrolls
    uint32_t      pan_step_ms;    ///< Delay between pan steps (lower = faster)

    // ---- SCENE_BOUNCE ----
    int           bounce_dx;      ///< Horizontal velocity (px/step, sign sets start direction)
    int           bounce_dy;      ///< Vertical   velocity (px/step, sign sets start direction)
    uint32_t      bounce_step_ms; ///< Delay between bounce steps (ms); ~16 = 60 fps
    uint32_t      bounce_dur_ms;  ///< Total duration of the bounce scene (ms)
    uint16_t      bounce_bg_color;///< Background fill colour (big-endian RGB565); 0 = black

    // ---- SCENE_ANIM_DUAL ----
    uint32_t           first_frame_ms; ///< Hold first frame for this long before playing
    uint32_t           last_frame_ms;  ///< Hold last  frame for this long after  playing
    dual_screen_mode_t dual_mode;      ///< Pixel distribution across screens
    int                scale;          ///< Integer upscale factor (0/1 = none, 2 = 2× nearest-neighbour)

    // ---- Parallel execution ----
    bool parallel; ///< If true, this scene and the NEXT scene run simultaneously.
                   ///< Each must target a different screen. SCENE_ANIM_DUAL must
                   ///< not be used as the paired (next) scene.
} scene_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Run a sequence of display scenes, blocking until all are complete.
 *
 * @param panel1      Handle for screen 1
 * @param panel2      Handle for screen 2
 * @param scenes      Array of scene descriptors
 * @param scene_count Number of entries in @p scenes
 */
void display_sequence_run(
        esp_lcd_panel_handle_t panel1,
        esp_lcd_panel_handle_t panel2,
        const scene_t         *scenes,
        int                    scene_count);

/**
 * @brief Request the currently running scene to exit as soon as possible.
 *
 * Safe to call from any task/ISR context.  The flag is cleared automatically
 * at the start of each display_sequence_run() call.
 */
void display_sequence_request_abort(void);
