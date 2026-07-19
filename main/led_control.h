#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LED_COUNT       8
#define LED_GPIO        5

typedef enum {
    LED_MODE_PULSE = 0,
    LED_MODE_CHASE_RANDOM,
    LED_MODE_LAMP,
    LED_MODE_RAINBOW,
    LED_MODE_CANDLE,
    LED_MODE_AURORA,
    LED_MODE_SOLID,
    LED_MODE_OFF,
    LED_MODE_COUNT
} led_mode_t;

typedef struct {
    uint8_t r, g, b;
} rgb_t;

void led_init(void);
void led_set_mode(led_mode_t mode);
void led_cycle_mode(void);
void led_toggle_power(void);
void led_tick(void);
led_mode_t led_get_mode(void);
bool led_is_on(void);

/* Fase 3 — Control remoto */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b);
void led_set_brightness(uint8_t b);
uint8_t led_get_brightness(void);
