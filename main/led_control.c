/*
 * led_control.c — Control de anillo WS2812B (8 LEDs) vía RMT
 * DeskMate — ESP32-C3 + ESP-IDF v5.5+
 *
 * Conexión: GPIO 5 ──[330Ω]── DI del anillo
 * Alimentación: 5V directo del USB (LEDs + ESP32 mismo bus)
 */

#include "led_control.h"
#include "freertos/FreeRTOS.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "desklamp";

#define RMT_RESOLUTION_HZ  10000000

// Timing WS2812B @ 10 MHz (datasheet: T0H=0.4µs, T1H=0.8µs, RES>50µs)
#define T0H  4   // 0.4µs high
#define T0L  8   // 0.8µs low  → bit 0 = 1.2µs
#define T1H  8   // 0.8µs high
#define T1L  4   // 0.4µs low  → bit 1 = 1.2µs
#define T_RESET 600  // 60µs reset (>50µs requerido)

static void encode_rgb(rgb_t c, rmt_symbol_word_t *sym)
{
    uint8_t buf[3] = { c.g, c.r, c.b };  // WS2812B orden GRB
    int idx = 0;
    for (int b = 0; b < 3; b++) {
        for (int i = 7; i >= 0; i--) {
            bool bit = (buf[b] >> i) & 1;
            sym[idx].level0    = 1;
            sym[idx].duration0 = bit ? T1H : T0H;
            sym[idx].level1    = 0;
            sym[idx].duration1 = bit ? T1L : T0L;
            idx++;
        }
    }
}

static rmt_channel_handle_t   tx_chan   = NULL;
static rmt_encoder_handle_t   copy_enc  = NULL;
static rgb_t                  leds[LED_COUNT];
static led_mode_t             cur_mode  = LED_MODE_PULSE;
static bool                   on        = true;
static uint32_t               tick      = 0;
static rgb_t                  solid_color = {255, 128, 0};  /* color sólido default: ámbar */
static uint8_t                brightness  = 255;            /* 0-255 */

static const int8_t sin8[64] = {
    0,  12,  25,  37,  49,  60,  71,  81,
   90,  98, 106, 112, 117, 122, 125, 126,
  127, 126, 125, 122, 117, 112, 106,  98,
   90,  81,  71,  60,  49,  37,  25,  12,
    0, -12, -25, -37, -49, -60, -71, -81,
  -90, -98,-106,-112,-117,-122,-125,-126,
 -127,-126,-125,-122,-117,-112,-106, -98,
  -90, -81, -71, -60, -49, -37, -25, -12
};

static inline int sin8_lookup(int idx) { return sin8[idx & 63]; }

static void send_leds(void)
{
    /* Aplicar brillo global */
    rgb_t scaled[LED_COUNT];
    for (int i = 0; i < LED_COUNT; i++) {
        scaled[i].r = ((uint16_t)leds[i].r * brightness) >> 8;
        scaled[i].g = ((uint16_t)leds[i].g * brightness) >> 8;
        scaled[i].b = ((uint16_t)leds[i].b * brightness) >> 8;
    }

    rmt_symbol_word_t frame[LED_COUNT * 24 + 1];
    for (int i = 0; i < LED_COUNT; i++)
        encode_rgb(scaled[i], &frame[i * 24]);
    frame[LED_COUNT * 24].level0    = 0;
    frame[LED_COUNT * 24].duration0 = T_RESET;
    frame[LED_COUNT * 24].level1    = 0;
    frame[LED_COUNT * 24].duration1 = 0;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = { .eot_level = 0 },
    };
    ESP_ERROR_CHECK(rmt_transmit(tx_chan, copy_enc, frame,
                                 sizeof(frame[0]) * (LED_COUNT * 24 + 1),
                                 &tx_cfg));
}

static void effect_lamp(void)
{
    rgb_t c = { .r = 102, .g = 64, .b = 32 };
    for (int i = 0; i < LED_COUNT; i++) leds[i] = c;
}

static void effect_rainbow(void)
{
    int rot = (tick >> 2) & 63;
    for (int i = 0; i < LED_COUNT; i++) {
        int hue = ((i * 64) / LED_COUNT + rot) & 63;
        leds[i].r = (sin8_lookup(hue + 21) + 128) >> 1;
        leds[i].g = (sin8_lookup(hue) + 128) >> 1;
        leds[i].b = (sin8_lookup(hue + 43) + 128) >> 1;
    }
}

static void effect_pulse(void)
{
    int v = (sin8_lookup(tick / 3) + 128) >> 1;
    rgb_t c = { .r = v, .g = v * 3 / 5, .b = v * 2 / 7 };
    for (int i = 0; i < LED_COUNT; i++) leds[i] = c;
}

static void effect_candle(void)
{
    for (int i = 0; i < LED_COUNT; i++) {
        int noise = (tick * 7 + i * 13) & 31;
        int v = 80 + (noise < 15 ? noise * 3 : (31 - noise) * 2);
        if (v > 130) v = 130;
        leds[i].r = v;
        leds[i].g = v * 3 / 5;
        leds[i].b = v / 4;
    }
}

static void effect_aurora(void)
{
    int phase = (tick / 8) & 127;
    for (int i = 0; i < LED_COUNT; i++) {
        int p = (phase + i * 8) & 127;
        leds[i].r = (sin8_lookup(p * 2) + 128) / 6;
        leds[i].g = (sin8_lookup(p * 2 + 21) + 128) / 4;
        leds[i].b = (sin8_lookup(p * 2 + 43) + 128) / 3;
    }
}

static void effect_off(void)
{
    rgb_t c = { 0 };
    for (int i = 0; i < LED_COUNT; i++) leds[i] = c;
}

static void effect_solid(void)
{
    for (int i = 0; i < LED_COUNT; i++) leds[i] = solid_color;
}

static void effect_chase_random(void)
{
    /* Apagar todos */
    rgb_t off = { 0 };
    for (int i = 0; i < LED_COUNT; i++) leds[i] = off;

    /* Cada ~2s mover a siguiente LED con color aleatorio */
    int pos = (tick / 1) % LED_COUNT;
    leds[pos].r = esp_random() & 0xFF;
    leds[pos].g = esp_random() & 0xFF;
    leds[pos].b = esp_random() & 0xFF;
}

void led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .gpio_num        = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz   = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &tx_chan));

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &copy_enc));
    ESP_ERROR_CHECK(rmt_enable(tx_chan));

    led_set_mode(LED_MODE_PULSE);
    send_leds();
    ESP_LOGI(TAG, "init: GPIO %d, %d LEDs", LED_GPIO, LED_COUNT);
}

void led_set_mode(led_mode_t mode)
{
    if (mode >= LED_MODE_COUNT) return;
    cur_mode = mode;
    on = (mode != LED_MODE_OFF);
    tick = 0;
    ESP_LOGI(TAG, "modo: %d", mode);
}

void led_cycle_mode(void)
{
    led_mode_t next = (cur_mode + 1) % LED_MODE_COUNT;
    led_set_mode(next);
}

void led_toggle_power(void)
{
    if (on) { on = false; effect_off(); send_leds(); }
    else   { on = true;  tick = 0; }
}

void led_tick(void)
{
    tick++;
    if (!on) return;

    switch (cur_mode) {
        case LED_MODE_PULSE:        effect_pulse();        break;
        case LED_MODE_CHASE_RANDOM: effect_chase_random(); break;
        case LED_MODE_LAMP:         effect_lamp();         break;
        case LED_MODE_RAINBOW:      effect_rainbow();      break;
        case LED_MODE_CANDLE:       effect_candle();       break;
        case LED_MODE_AURORA:       effect_aurora();       break;
        case LED_MODE_SOLID:        effect_solid();        break;
        case LED_MODE_OFF:          effect_off();          break;
        default:                    effect_pulse();        break;
    }
    send_leds();
}

led_mode_t led_get_mode(void) { return cur_mode; }
bool led_is_on(void)          { return on; }

/* Fase 3 — Control remoto */
void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    solid_color.r = r; solid_color.g = g; solid_color.b = b;
}
void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = solid_color.r; *g = solid_color.g; *b = solid_color.b;
}
void led_set_brightness(uint8_t b) { brightness = b; }
uint8_t led_get_brightness(void)   { return brightness; }
