/*
 * pin_config.h — Asignación de pines para DeskMate
 * ESP32-C3 Super Mini + ST7789 240x240
 *
 * Basado en la investigación de pinout:
 *   - GPIO 2: EVITAR (strapping pin)
 *   - GPIO 8: EVITAR (LED onboard + strapping)
 *   - GPIO 9: SCLK del display
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Display ST7789 (SPI)
 * ============================================================ */
#define PIN_LCD_MOSI   6   /* SDA -> display DIN  - segun pinout: MOSI/SDA */
#define PIN_LCD_SCLK   9   /* SCL -> display CLK  - segun pinout: SCL */
#define PIN_LCD_CS     21  /* CS  -> display CS  - pinout: cualquiera */
#define PIN_LCD_DC     10  /* DC  -> display DC  - pinout: cualquiera */
#define PIN_LCD_RST    4   /* RST -> display RES - segun pinout: RST */
#define PIN_LCD_MISO   -1  /* MISO, no usado */

/* Backlight: conectado a 3.3V fijo */
#define PIN_LCD_BCKL   -1


/* Resolucion ST7789 */
#define LCD_WIDTH   240
#define LCD_HEIGHT  240

/* Host SPI */
#define LCD_HOST    SPI2_HOST

/* ============================================================
 * WiFi — ¡CAMBIAR ESTOS VALORES!
 * ============================================================ */
#define WIFI_SSID     "FliaYanez"
#define WIFI_PASS     "vtF28dd6XfauJxI"
#define WIFI_MAX_RETRY  5

/* ============================================================
 * Zona horaria (São Paulo, Brasil — UTC-3, sin DST)
 * ============================================================ */
#define TIMEZONE "BRT+3"

/* ============================================================
 * OpenWeatherMap API
 * ============================================================ */
#define OWM_API_KEY    "8c5e491a43b64ecb16ffed40f5c60695"
#define OWM_CITY_ID    "3459712"
#define OWM_LANG       "es"
#define OWM_UNITS      "metric"

#ifdef __cplusplus
}
#endif
