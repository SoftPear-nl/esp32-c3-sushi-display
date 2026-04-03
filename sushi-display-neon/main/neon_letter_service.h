#pragma once

enum led_mode_t {
    LED_MODE_ALL_PATTERNS,
    LED_MODE_ON,
    LED_MODE_OFF,
    LED_MODE_COUNT
};

extern enum led_mode_t current_led_mode;

/**
 * @brief Initialise the neon letter GPIOs and start the LED animation task.
 *
 * Call once from app_main, passing the GPIO pin numbers assigned to each
 * letter.  The pin defines stay in main.c; this function accepts them so the
 * service has no board-specific knowledge.
 *
 * @param pin_s1  GPIO for letter S (first)
 * @param pin_u   GPIO for letter U
 * @param pin_s2  GPIO for letter S (second)
 * @param pin_h   GPIO for letter H
 * @param pin_i   GPIO for letter I
 */
void neon_letter_init(int pin_s1, int pin_u, int pin_s2, int pin_h, int pin_i);
