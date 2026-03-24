#include "neon_letter_service.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

// -------------------- State --------------------

enum led_mode_t current_led_mode = LED_MODE_ALL_PATTERNS;

static int s_pin_s1;
static int s_pin_u;
static int s_pin_s2;
static int s_pin_h;
static int s_pin_i;

// -------------------- Patterns --------------------

static void led_heartbeat(void)
{
    for (int i = 0; i < 3; ++i) {
        // All on (long)
        gpio_set_level(s_pin_s1, 1);
        gpio_set_level(s_pin_u, 1);
        gpio_set_level(s_pin_s2, 1);
        gpio_set_level(s_pin_h, 1);
        gpio_set_level(s_pin_i, 1);
        vTaskDelay(pdMS_TO_TICKS(320));
        // All off (short)
        gpio_set_level(s_pin_s1, 0);
        gpio_set_level(s_pin_u, 0);
        gpio_set_level(s_pin_s2, 0);
        gpio_set_level(s_pin_h, 0);
        gpio_set_level(s_pin_i, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
        // All on (short)
        gpio_set_level(s_pin_s1, 1);
        gpio_set_level(s_pin_u, 1);
        gpio_set_level(s_pin_s2, 1);
        gpio_set_level(s_pin_h, 1);
        gpio_set_level(s_pin_i, 1);
        vTaskDelay(pdMS_TO_TICKS(160));
        // All off (long)
        gpio_set_level(s_pin_s1, 0);
        gpio_set_level(s_pin_u, 0);
        gpio_set_level(s_pin_s2, 0);
        gpio_set_level(s_pin_h, 0);
        gpio_set_level(s_pin_i, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

static void led_marquee(void)
{
    int pins[5] = {s_pin_s1, s_pin_u, s_pin_s2, s_pin_h, s_pin_i};
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 5; ++k) {
                gpio_set_level(pins[k], (k == j || k == j + 1) ? 1 : 0);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (int j = 3; j >= 0; --j) {
            for (int k = 0; k < 5; ++k) {
                gpio_set_level(pins[k], (k == j || k == j + 1) ? 1 : 0);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], 0);
}

static void led_random_walk(void)
{
    int pins[5] = {s_pin_s1, s_pin_u, s_pin_s2, s_pin_h, s_pin_i};
    int pos = rand() % 5;
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], k == pos ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        int dir = rand() % 2 ? 1 : -1;
        pos += dir;
        if (pos < 0) pos = 0;
        if (pos > 4) pos = 4;
    }
    for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], 0);
}

static void led_alternate(void)
{
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            gpio_set_level(s_pin_s1, j == 0 ? 1 : 0);
            gpio_set_level(s_pin_u,  j == 1 ? 1 : 0);
            gpio_set_level(s_pin_s2, j == 2 ? 1 : 0);
            gpio_set_level(s_pin_h,  j == 3 ? 1 : 0);
            gpio_set_level(s_pin_i,  j == 4 ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        for (int j = 3; j > 0; --j) {
            gpio_set_level(s_pin_s1, j == 0 ? 1 : 0);
            gpio_set_level(s_pin_u,  j == 1 ? 1 : 0);
            gpio_set_level(s_pin_s2, j == 2 ? 1 : 0);
            gpio_set_level(s_pin_h,  j == 3 ? 1 : 0);
            gpio_set_level(s_pin_i,  j == 4 ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
}

static void led_wave(void)
{
    int pattern[8][5] = {
        {1, 0, 0, 0, 0},
        {0, 1, 0, 0, 0},
        {0, 0, 1, 0, 0},
        {0, 0, 0, 1, 0},
        {0, 0, 0, 0, 1},
        {0, 0, 0, 1, 0},
        {0, 0, 1, 0, 0},
        {0, 1, 0, 0, 0}
    };
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 8; ++j) {
            gpio_set_level(s_pin_s1, pattern[j][0]);
            gpio_set_level(s_pin_u,  pattern[j][1]);
            gpio_set_level(s_pin_s2, pattern[j][2]);
            gpio_set_level(s_pin_h,  pattern[j][3]);
            gpio_set_level(s_pin_i,  pattern[j][4]);
            vTaskDelay(pdMS_TO_TICKS(240));
        }
    }
}

static void led_full_flicker(void)
{
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(s_pin_s1, 1);
        gpio_set_level(s_pin_u, 1);
        gpio_set_level(s_pin_s2, 1);
        gpio_set_level(s_pin_h, 1);
        gpio_set_level(s_pin_i, 1);
        vTaskDelay(pdMS_TO_TICKS(160 + i * 60));
        gpio_set_level(s_pin_s1, 0);
        gpio_set_level(s_pin_u, 0);
        gpio_set_level(s_pin_s2, 0);
        gpio_set_level(s_pin_h, 0);
        gpio_set_level(s_pin_i, 0);
        vTaskDelay(pdMS_TO_TICKS(160 + i * 60));
    }
}

static void led_chase(void)
{
    for (int i = 0; i < 3; ++i) {
        gpio_set_level(s_pin_s1, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(s_pin_u,  1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(s_pin_s2, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(s_pin_h,  1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(s_pin_i,  1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(s_pin_s1, 0);
        gpio_set_level(s_pin_u,  0);
        gpio_set_level(s_pin_s2, 0);
        gpio_set_level(s_pin_h,  0);
        gpio_set_level(s_pin_i,  0);
        vTaskDelay(pdMS_TO_TICKS(240));
    }
}

static void led_bounce(void)
{
    int pins[5] = {s_pin_s1, s_pin_u, s_pin_s2, s_pin_h, s_pin_i};
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 5; ++j) {
            for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], k == j ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (int j = 3; j > 0; --j) {
            for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], k == j ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void led_sparkle(void)
{
    for (int i = 0; i < 20; ++i) {
        gpio_set_level(s_pin_s1, rand() % 2);
        gpio_set_level(s_pin_u,  rand() % 2);
        gpio_set_level(s_pin_s2, rand() % 2);
        gpio_set_level(s_pin_h,  rand() % 2);
        gpio_set_level(s_pin_i,  rand() % 2);
        vTaskDelay(pdMS_TO_TICKS(160));
    }
    gpio_set_level(s_pin_s1, 0);
    gpio_set_level(s_pin_u,  0);
    gpio_set_level(s_pin_s2, 0);
    gpio_set_level(s_pin_h,  0);
    gpio_set_level(s_pin_i,  0);
}

// -------------------- Task --------------------

static void led_blink_task(void *arg)
{
    (void)arg;
    while (1) {
        switch (current_led_mode) {
            case LED_MODE_ON:
                gpio_set_level(s_pin_s1, 1);
                gpio_set_level(s_pin_u,  1);
                gpio_set_level(s_pin_s2, 1);
                gpio_set_level(s_pin_h,  1);
                gpio_set_level(s_pin_i,  1);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case LED_MODE_OFF:
                gpio_set_level(s_pin_s1, 0);
                gpio_set_level(s_pin_u,  0);
                gpio_set_level(s_pin_s2, 0);
                gpio_set_level(s_pin_h,  0);
                gpio_set_level(s_pin_i,  0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case LED_MODE_ALL_PATTERNS:
            default:
                led_alternate();
                led_wave();
                led_full_flicker();
                led_chase();
                led_bounce();
                led_sparkle();
                led_heartbeat();
                led_marquee();
                led_random_walk();
                break;
        }
    }
}

// -------------------- Public API --------------------

void neon_letter_init(int pin_s1, int pin_u, int pin_s2, int pin_h, int pin_i)
{
    s_pin_s1 = pin_s1;
    s_pin_u  = pin_u;
    s_pin_s2 = pin_s2;
    s_pin_h  = pin_h;
    s_pin_i  = pin_i;

    gpio_config_t cfg = {
        .pin_bit_mask =
            (1ULL << pin_s1) |
            (1ULL << pin_u)  |
            (1ULL << pin_s2) |
            (1ULL << pin_h)  |
            (1ULL << pin_i),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 2, NULL);
}
