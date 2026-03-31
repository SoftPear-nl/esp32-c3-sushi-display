#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_lcd_panel_ops.h"

// ---------------------------------------------------------------------------
// Button events — posted into the queue by the 5-way switch task
// ---------------------------------------------------------------------------
typedef enum {
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_CENTER,
} btn_event_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Initialise the config menu subsystem.
 *
 * Creates the button event queue.  Call once before starting the 5-way task.
 *
 * @param panel  The LCD panel to draw the menu on (screen 2).
 */
void config_menu_init(esp_lcd_panel_handle_t panel);

/**
 * @brief Returns the button event queue.
 *
 * The 5-way switch task should post btn_event_t values here on each press.
 */
QueueHandle_t config_menu_get_queue(void);

/**
 * @brief Returns true while the config menu is open.
 *
 * The main display loop should pause while this returns true.
 */
bool config_menu_is_open(void);

/**
 * @brief Process pending button events.
 *
 * Call this regularly from the main loop.  Opens the menu on CENTER press,
 * navigates / confirms while open, and closes on a second CENTER press.
 */
void config_menu_tick(void);
