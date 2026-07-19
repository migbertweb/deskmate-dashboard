/*
 * DeskMate — Panel de escritorio inteligente
 * ESP32-C3 Super Mini + ST7789 240x240 (SPI)
 *
 * Fase 5: Reloj + Clima actual + Pronóstico 4 días
 *         Navegación entre pantallas con botón o auto-rotación
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "pin_config.h"
#include "font_5x7.h"
#include "font_8x13.h"
#include "weather_icons.h"
#include "led_control.h"
#include "webserver.h"

/* ============================================================
 * Constantes
 * ============================================================ */
static const char *TAG = "deskmate";

/* Colores RGB565 */
#define COLOR_BLACK        0x0000
#define COLOR_WHITE        0xFFFF
#define COLOR_RED          0xF800
#define COLOR_GREEN        0x07E0
#define COLOR_BLUE         0x001F
#define COLOR_CYAN         0x07FF
#define COLOR_MAGENTA      0xF81F
#define COLOR_YELLOW       0xFFE0
#define COLOR_ORANGE    0xFD20  // R=31, G=18, B=0 — naranja real
#define COLOR_GRAY         0x8410
#define COLOR_DARK_GRAY    0x4208

/* Paleta profesional */
#define COLOR_AMBER     0xFD60  // R=31, G=22, B=0 — ámbar cálido
#define COLOR_TEAL      0x06BF  // R=0,  G=13, B=31 — teal suave
#define COLOR_SOFT_WHITE 0xBDD7 // R=23, G=26, B=31 — blanco ligeramente azulado
#define COLOR_MUTED     0xAD55  // R=21, G=21, B=21 — gris claro elegante
#define COLOR_SEPARATOR 0x2945  // R=5,  G=9,  B=10 — línea divisoria sutil

/* Eventos WiFi/SNTP */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define TIME_SYNCED_BIT     BIT2

/* Intervalo de actualización del clima (segundos) */
#define WEATHER_INTERVAL    600

/* Pantallas */
typedef enum {
    SCREEN_CLOCK = 0,
    SCREEN_FORECAST,
    SCREEN_COUNT
} screen_t;

#define SCREEN_AUTO_ROTATE_SEC 15  /* auto-rotación cada 15s */

/* Forecast */
#define MAX_FORECAST_DAYS 3
#define FORECAST_BUFFER_MAX 20000  /* 20KB para respuesta JSON */

/* ============================================================
 * Estructuras
 * ============================================================ */
typedef struct {
    float temp;
    float feels_like;
    char description[64];
    char icon[8];
    bool valid;
} weather_data_t;

typedef struct {
    int8_t temp_min;
    int8_t temp_max;
    uint8_t pop;        /* 0-100% probabilidad precipitación */
    xbm_icon_t icon;
    char day_label[4];  /* "LUN", "MAR", etc. */
    bool valid;
} forecast_day_t;

/* ============================================================
 * Variables globales
 * ============================================================ */
static esp_lcd_panel_handle_t panel_handle = NULL;
static EventGroupHandle_t wifi_event_group;
static int wifi_retries = 0;
static bool time_synced = false;
static weather_data_t weather_data = {0};
static bool wifi_connected = false;

/* IP asignada por DHCP para mostrar en pantalla de boot */
static char current_ip[16] = "0.0.0.0";
static bool boot_done = false;
static int boot_counter = 0;

/* Estado de pantalla */
static screen_t current_screen = SCREEN_CLOCK;
static int screen_timer = 0;         /* contador para auto-rotación */
static bool clock_first_run = true;
static bool forecast_first_run = true;

/* Forecast */
static forecast_day_t forecast_days[MAX_FORECAST_DAYS];
static bool forecast_valid = false;
static uint8_t today_pop = 0;       /* 0-100% probabilidad de lluvia para hoy */
static bool today_pop_valid = false; /* true despues de primer fetch de forecast */

/* ============================================================
 * Prototipos
 * ============================================================ */
static void lcd_init(void);
static void lcd_fill_screen(uint16_t color);
static void lcd_draw_rect(int x, int y, int w, int h, uint16_t color);
static void lcd_draw_pixel(int x, int y, uint16_t color);

static void lcd_draw_char(int x, int y, char c, uint16_t color, uint8_t scale);
static void lcd_draw_text(int x, int y, const char *str, uint16_t color, uint8_t scale);
static int  lcd_text_width(const char *str, uint8_t scale);
static void lcd_draw_text_centered(int y, const char *str, uint16_t color, uint8_t scale);

/* 8x13 font functions */
static void lcd_draw_char_8x13(int x, int y, char c, uint16_t color, uint8_t scale);
static void lcd_draw_text_8x13(int x, int y, const char *str, uint16_t color, uint8_t scale);
static int  lcd_text_width_8x13(const char *str, uint8_t scale);
static void lcd_draw_text_centered_8x13(int y, const char *str, uint16_t color, uint8_t scale);

static void lcd_draw_xbm(int x, int y, const unsigned char *bits, int w, int h, uint16_t color, uint8_t scale);

static void wifi_init(void);
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static void sntp_init_task(void);

static void display_clock(void);
static void display_boot(void);
static void display_status_bar(void);
static void display_weather_section(void);
static weather_data_t weather_fetch(void);

/* Nuevas funciones */
static bool forecast_fetch(void);
static void display_forecast(void);

/* ============================================================
 * app_main
 * ============================================================ */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "DeskMate Dashboard iniciando...");

    wifi_event_group = xEventGroupCreate();
    lcd_init();
    led_init();
    wifi_init();
    sntp_init_task();

    /* Esperar sincronizacion NTP para saber la hora */
    while (!time_synced) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Iniciar servidor web — WiFi ya conectado */
    start_webserver();

    /* Primer fetch de clima y pronostico (tres segundos despues del sync) */
    vTaskDelay(pdMS_TO_TICKS(3000));

    weather_data_t initial_weather = weather_fetch();
    if (initial_weather.valid) {
        weather_data = initial_weather;
    }
    forecast_fetch();

    /* Bucle principal */
    uint32_t last_weather_ts = 0;
    uint32_t last_forecast_ts = 0;

    while (1) {
        /* ---- Manejo de pantallas ---- */
        if (!boot_done && time_synced) {
            display_boot();
            boot_counter++;
            if (boot_counter >= 5) { /* ~5 segundos */
                boot_done = true;
                clock_first_run = true;
                lcd_fill_screen(COLOR_BLACK);
            }
        } else if (current_screen == SCREEN_CLOCK) {
            display_clock();
            display_status_bar();
        } else {
            display_forecast();
        }

        /* ---- Auto-rotacion de pantalla cada 10s ---- */
        screen_timer++;
        if (screen_timer >= SCREEN_AUTO_ROTATE_SEC) {
            screen_timer = 0;
            current_screen = (current_screen + 1) % SCREEN_COUNT;
            ESP_LOGI(TAG, "Cambiando a pantalla %d", current_screen);
            lcd_fill_screen(COLOR_BLACK);
            clock_first_run = true;
            forecast_first_run = true;
        }

        /* ---- Refresh de datos meteorologicos ---- */
        time_t now;
        time(&now);
        uint32_t now_sec = (uint32_t)now;

        if (now_sec - last_weather_ts >= WEATHER_INTERVAL) {
            last_weather_ts = now_sec;
            weather_data_t new_data = weather_fetch();
            if (new_data.valid) {
                weather_data = new_data;
                if (current_screen == SCREEN_CLOCK) {
                    clock_first_run = true;
                }
            }
        }

        if (now_sec - last_forecast_ts >= WEATHER_INTERVAL * 2) {
            last_forecast_ts = now_sec;
            if (forecast_fetch()) {
                if (current_screen == SCREEN_FORECAST) {
                    forecast_first_run = true;
                }
            }
        }

        /* ---- WebSocket: enviar datos al panel web ---- */
        {
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            char buf[256];
            static const char *wdays[] = {"Domingo","Lunes","Martes","Miercoles","Jueves","Viernes","Sabado"};

            /* Clock */
            snprintf(buf, sizeof(buf),
                "{\"type\":\"clock\",\"time\":\"%02d:%02d:%02d\",\"wday\":\"%s\",\"date\":\"%02d/%02d/%04d\"}",
                tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                wdays[tm_info.tm_wday],
                tm_info.tm_mday, tm_info.tm_mon + 1, tm_info.tm_year + 1900);
            ws_broadcast(buf);

            /* Weather */
            if (weather_data.valid) {
                char pop_str[24];
                if (!today_pop_valid) snprintf(pop_str, sizeof(pop_str), "Lluvia: --%%");
                else if (today_pop > 0) snprintf(pop_str, sizeof(pop_str), "Lluvia: %d%%", today_pop);
                else snprintf(pop_str, sizeof(pop_str), "Sin lluvia");
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"weather\",\"icon\":\"%s\",\"temp\":%.0f,\"desc\":\"%s\",\"pop\":\"%s\"}",
                    weather_emoji(weather_data.icon),
                    weather_data.temp,
                    weather_data.description,
                    pop_str);
                ws_broadcast(buf);
            }

            /* Forecast */
            if (forecast_valid) {
                char fc[384] = "{\"type\":\"forecast\",\"days\":[";
                for (int i = 0; i < MAX_FORECAST_DAYS && forecast_days[i].valid; i++) {
                    char day[128];
                    snprintf(day, sizeof(day),
                        "%s{\"label\":\"%s\",\"icon\":\"%s\",\"max\":%d,\"min\":%d,\"pop\":%d}",
                        i > 0 ? "," : "",
                        forecast_days[i].day_label,
                        xbm_emoji(forecast_days[i].icon),
                        forecast_days[i].temp_max,
                        forecast_days[i].temp_min,
                        forecast_days[i].pop);
                    strncat(fc, day, sizeof(fc) - strlen(fc) - 1);
                }
                strncat(fc, "]}", sizeof(fc) - strlen(fc) - 1);
                ws_broadcast(fc);
            }

            /* LED */
            snprintf(buf, sizeof(buf), "{\"type\":\"led\",\"mode\":%d}", led_get_mode());
            ws_broadcast(buf);
        }

        led_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ============================================================
 * Inicialización del display ST7789 via esp_lcd
 * ============================================================ */
static void lcd_init(void)
{
    ESP_LOGI(TAG, "Inicializando SPI bus...");

    /* 1. Configurar bus SPI */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = GPIO_NUM_NC,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 2. Crear interfaz SPI para el panel */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num     = PIN_LCD_CS,
        .dc_gpio_num     = PIN_LCD_DC,
        .spi_mode        = 0,
        .pclk_hz         = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST,
        &io_config,
        &io_handle
    ));

    /* 3. Crear panel ST7789 */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num  = PIN_LCD_RST,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    /* 4. Inicializar y encender el panel */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Display ST7789 inicializado correctamente");
}

/* ============================================================
 * Evento de WiFi
 * ============================================================ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (wifi_retries < WIFI_MAX_RETRY) {
            wifi_retries++;
            ESP_LOGW(TAG, "WiFi desconectado, reintento %d/%d", wifi_retries, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retries = 0;
        wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ============================================================
 * Inicializar WiFi (Station mode)
 * ============================================================ */
static void wifi_init(void)
{
    ESP_LOGI(TAG, "Inicializando WiFi...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* La IP se asigna por DHCP
     * Para IP fija: hacer reserva DHCP en el router (192.168.1.99 por MAC 8c:d0:b2:a9:f8:67) */

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi inicializado");
}

/* ============================================================
 * Callback de SNTP (tiempo sincronizado)
 * ============================================================ */
static void sntp_time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Tiempo SNTP sincronizado!");
    xEventGroupSetBits(wifi_event_group, TIME_SYNCED_BIT);
    time_synced = true;
}

/* ============================================================
 * Inicializar SNTP
 * ============================================================ */
static void sntp_init_task(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP...");

    setenv("TZ", TIMEZONE, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_time_sync_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP inicializado - esperando sincronizacion");
}

/* ============================================================
 * Fetch del clima via OpenWeatherMap API (tiempo actual)
 * ============================================================ */
static weather_data_t weather_fetch(void)
{
    weather_data_t wd = {0};
    char url[256];

    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/weather"
             "?id=%s&appid=%s&units=%s&lang=%s",
             OWM_CITY_ID, OWM_API_KEY, OWM_UNITS, OWM_LANG);

    ESP_LOGI(TAG, "Fetching weather...");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return wd;
    }

    /* Método manual: open + fetch_headers + read */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return wd;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d, content_length: %d",
             http_status, content_length);
    if (http_status != 200 || content_length <= 0) {
        ESP_LOGE(TAG, "Bad response: status=%d, len=%d", http_status, content_length);
        esp_http_client_cleanup(client);
        return wd;
    }

    /* Leer respuesta — heap allocation */
    int buf_size = content_length + 1;
    if (buf_size > 4096) buf_size = 4096;
    char *buffer = malloc(buf_size);
    if (!buffer) {
        ESP_LOGE(TAG, "OOM for HTTP buffer (%d)", buf_size);
        esp_http_client_cleanup(client);
        return wd;
    }

    int total_read = 0;
    int read = 0;
    do {
        read = esp_http_client_read(client, buffer + total_read,
                                     buf_size - 1 - total_read);
        if (read > 0) total_read += read;
    } while (read > 0 && total_read < buf_size - 1);
    buffer[total_read] = '\0';

    ESP_LOGI(TAG, "Weather raw (%dB): %.120s", total_read, buffer);

    /* Parsear JSON */
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        free(buffer);
        esp_http_client_cleanup(client);
        return wd;
    }

    cJSON *main_obj = cJSON_GetObjectItem(root, "main");
    if (main_obj) {
        cJSON *temp_item = cJSON_GetObjectItem(main_obj, "temp");
        cJSON *feels_item = cJSON_GetObjectItem(main_obj, "feels_like");
        if (cJSON_IsNumber(temp_item)) wd.temp = temp_item->valuedouble;
        if (cJSON_IsNumber(feels_item)) wd.feels_like = feels_item->valuedouble;
    }

    cJSON *weather_arr = cJSON_GetObjectItem(root, "weather");
    if (cJSON_IsArray(weather_arr) && cJSON_GetArraySize(weather_arr) > 0) {
        cJSON *weather_item = cJSON_GetArrayItem(weather_arr, 0);
        cJSON *desc = cJSON_GetObjectItem(weather_item, "description");
        if (cJSON_IsString(desc) && desc->valuestring) {
            strncpy(wd.description, desc->valuestring, sizeof(wd.description) - 1);
        }
        cJSON *icon_item = cJSON_GetObjectItem(weather_item, "icon");
        if (cJSON_IsString(icon_item) && icon_item->valuestring) {
            strncpy(wd.icon, icon_item->valuestring, sizeof(wd.icon) - 1);
        }
    }

    cJSON_Delete(root);
    free(buffer);
    esp_http_client_cleanup(client);

    wd.valid = true;
    ESP_LOGI(TAG, "Weather: %.1fC (feels %.1fC) - %s",
             wd.temp, wd.feels_like, wd.description);
    return wd;
}

/* ============================================================
 * Fetch de pronóstico 5 días (OWM /forecast)
 * Agrega los datos en el arreglo forecast_days[]
 * ============================================================ */
static bool forecast_fetch(void)
{
    char url[256];

    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/forecast"
             "?id=%s&appid=%s&units=%s&lang=%s",
             OWM_CITY_ID, OWM_API_KEY, OWM_UNITS, OWM_LANG);

    ESP_LOGI(TAG, "Fetching forecast...");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for forecast");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Forecast HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Forecast HTTP status: %d, content_length: %d",
             http_status, content_length);
    if (http_status != 200 || content_length <= 0) {
        ESP_LOGE(TAG, "Forecast bad response: status=%d, len=%d",
                 http_status, content_length);
        esp_http_client_cleanup(client);
        return false;
    }

    /* Buffer más grande para forecast (hasta 16KB) */
    int buf_size = content_length + 1;
    if (buf_size > FORECAST_BUFFER_MAX) buf_size = FORECAST_BUFFER_MAX;
    char *buffer = malloc(buf_size);
    if (!buffer) {
        ESP_LOGE(TAG, "OOM for forecast buffer (%d)", buf_size);
        esp_http_client_cleanup(client);
        return false;
    }

    int total_read = 0;
    int read = 0;
    do {
        read = esp_http_client_read(client, buffer + total_read,
                                     buf_size - 1 - total_read);
        if (read > 0) total_read += read;
    } while (read > 0 && total_read < buf_size - 1);
    buffer[total_read] = '\0';

    ESP_LOGI(TAG, "Forecast raw (%dB): %.200s", total_read, buffer);

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        ESP_LOGE(TAG, "Forecast JSON parse error");
        free(buffer);
        esp_http_client_cleanup(client);
        return false;
    }

    /* Limpiar forecast anterior */
    for (int i = 0; i < MAX_FORECAST_DAYS; i++) {
        forecast_days[i].valid = false;
    }
    forecast_valid = false;
    today_pop = 0;
    today_pop_valid = false;

    cJSON *list = cJSON_GetObjectItem(root, "list");
    if (!cJSON_IsArray(list)) {
        ESP_LOGE(TAG, "Forecast: no 'list' array in response");
        cJSON_Delete(root);
        free(buffer);
        esp_http_client_cleanup(client);
        return false;
    }

    int size = cJSON_GetArraySize(list);
    ESP_LOGI(TAG, "Forecast entries: %d", size);

    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);

    /* Días de la semana (3 letras) */
    static const char *day_labels[] = {"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"};

    int day_idx = -1;           /* índice en forecast_days[] */
    int current_date_key = 0;   /* YYYYMMDD del día que procesamos */

    for (int i = 0; i < size; i++) {
        cJSON *entry = cJSON_GetArrayItem(list, i);
        if (!entry) continue;

        cJSON *dt_item = cJSON_GetObjectItem(entry, "dt");
        if (!cJSON_IsNumber(dt_item)) continue;

        time_t entry_time = (time_t)dt_item->valuedouble;
        struct tm entry_tm;
        localtime_r(&entry_time, &entry_tm);

        /* Saltar entradas del día de hoy (capturar probabilidad de lluvia) */
        if (entry_tm.tm_mday == now_tm.tm_mday &&
            entry_tm.tm_mon  == now_tm.tm_mon  &&
            entry_tm.tm_year == now_tm.tm_year) {
            cJSON *pop_item = cJSON_GetObjectItem(entry, "pop");
            if (cJSON_IsNumber(pop_item)) {
                int pop_val = (int)(pop_item->valuedouble * 100.0f + 0.5f);
                if (pop_val > today_pop) {
                    today_pop = (uint8_t)(pop_val > 100 ? 100 : pop_val);
                }
                today_pop_valid = true;
            }
            continue;
        }

        /* Fecha como entero YYYYMMDD para agrupar por día */
        int date_key = (entry_tm.tm_year + 1900) * 10000
                     + (entry_tm.tm_mon + 1) * 100
                     + entry_tm.tm_mday;

        /* Si cambió de día, avanzar al siguiente slot */
        if (date_key != current_date_key) {
            day_idx++;
            current_date_key = date_key;
            if (day_idx >= MAX_FORECAST_DAYS) break;

            forecast_days[day_idx].temp_min = 100;
            forecast_days[day_idx].temp_max = -100;
            forecast_days[day_idx].pop = 0;
            forecast_days[day_idx].icon = XBM_SUN;  /* default */
            forecast_days[day_idx].valid = true;

            /* Etiqueta del día (3 letras) */
            int wday = entry_tm.tm_wday;
            if (wday >= 0 && wday <= 6) {
                strncpy(forecast_days[day_idx].day_label,
                        day_labels[wday], sizeof(forecast_days[day_idx].day_label) - 1);
                forecast_days[day_idx].day_label[3] = '\0';
            }
        }

        if (day_idx < 0) continue; /* no debería pasar */

        /* === Parsear main (temp, temp_min, temp_max) === */
        cJSON *main_obj = cJSON_GetObjectItem(entry, "main");
        if (main_obj) {
            cJSON *temp      = cJSON_GetObjectItem(main_obj, "temp");
            cJSON *temp_min  = cJSON_GetObjectItem(main_obj, "temp_min");
            cJSON *temp_max  = cJSON_GetObjectItem(main_obj, "temp_max");

            if (cJSON_IsNumber(temp)) {
                int ti = (int)(temp->valuedouble + 0.5f);
                if (ti < forecast_days[day_idx].temp_min)
                    forecast_days[day_idx].temp_min = ti;
                if (ti > forecast_days[day_idx].temp_max)
                    forecast_days[day_idx].temp_max = ti;
            }
            if (cJSON_IsNumber(temp_min)) {
                int ti = (int)(temp_min->valuedouble + 0.5f);
                if (ti < forecast_days[day_idx].temp_min)
                    forecast_days[day_idx].temp_min = ti;
            }
            if (cJSON_IsNumber(temp_max)) {
                int ti = (int)(temp_max->valuedouble + 0.5f);
                if (ti > forecast_days[day_idx].temp_max)
                    forecast_days[day_idx].temp_max = ti;
            }
        }

        /* === Parsear pop (probabilidad de precipitación) === */
        cJSON *pop_item = cJSON_GetObjectItem(entry, "pop");
        if (cJSON_IsNumber(pop_item)) {
            uint8_t pop_val = (uint8_t)(pop_item->valuedouble * 100.0f + 0.5f);
            if (pop_val > forecast_days[day_idx].pop) {
                forecast_days[day_idx].pop = pop_val;
            }
        }

        /* === Icono: preferir el del mediodía (hora 9-15) === */
        cJSON *weather_arr = cJSON_GetObjectItem(entry, "weather");
        if (cJSON_IsArray(weather_arr) && cJSON_GetArraySize(weather_arr) > 0) {
            cJSON *weather_item = cJSON_GetArrayItem(weather_arr, 0);
            cJSON *icon_item = cJSON_GetObjectItem(weather_item, "icon");
            if (cJSON_IsString(icon_item) && icon_item->valuestring) {
                xbm_icon_t icon_idx = xbm_icon_from_code(icon_item->valuestring);
                int hour = entry_tm.tm_hour;
                /* Si es horario diurno (9-15h), usar este icono */
                if (hour >= 9 && hour <= 15) {
                    forecast_days[day_idx].icon = icon_idx;
                } else if (forecast_days[day_idx].icon == XBM_SUN) {
                    /* Primer icono disponible si no hay diurno todavía */
                    forecast_days[day_idx].icon = icon_idx;
                }
            }
        }
    }

    forecast_valid = (day_idx >= 0);

    /* Log del forecast */
    for (int i = 0; i < MAX_FORECAST_DAYS && forecast_days[i].valid; i++) {
        ESP_LOGI(TAG, "FCST %s: %d/%dC POP:%d%%",
                 forecast_days[i].day_label,
                 forecast_days[i].temp_min, forecast_days[i].temp_max,
                 forecast_days[i].pop);
    }

    cJSON_Delete(root);
    free(buffer);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Forecast fetch %s", forecast_valid ? "OK" : "sin datos");

    /* Si estamos en pantalla forecast, forzar redibujo */
    if (current_screen == SCREEN_FORECAST) {
        forecast_first_run = true;
    }

    return forecast_valid;
}

/* ============================================================
 * Pantalla de boot — muestra IP y estado WiFi
 * ============================================================ */
static void display_boot(void)
{
    char bar[24];
    static bool first = true;
    if (first) {
        lcd_fill_screen(COLOR_BLACK);
        first = false;
    }

    /* Título DeskMate */
    lcd_draw_text_centered(30, "DeskMate", COLOR_TEAL, 4);

    /* IP */
    snprintf(bar, sizeof(bar), "IP: %s", current_ip);
    lcd_draw_text_centered_8x13(90, bar, wifi_connected ? COLOR_GREEN : COLOR_MUTED, 2);

    /* WiFi SSID */
    lcd_draw_text_centered_8x13(120, "Sukuna-78-2.4g", COLOR_MUTED, 1);

    /* Indicador de conectando/main loop */
    lcd_draw_text_centered_8x13(160, "Conectando...", COLOR_MUTED, 1);

    /* Barra de progreso simple (cada tick un segmento) */
    uint8_t dots = boot_counter % 4 + 1;
    for (int i = 0; i < dots; i++)
        lcd_draw_rect(100 + i * 16, 200, 8, 8, COLOR_SEPARATOR);
}

/* ============================================================
 * Mostrar reloj + clima en pantalla (dibujo diferencial)
 * ============================================================ */
static void display_clock(void)
{
    time_t now;
    struct tm tm_info;
    char buf[32];
    static int last_mday = -1, last_mon = -1, last_year = -1, last_wday = -1;
    static bool wifi_was = false;
    static int last_temp_i = -999;
    static int last_feels_i = -999;
    static xbm_icon_t last_icon_idx = 0;
    static bool weather_was_valid = false;
    static uint8_t last_today_pop = 255;
    static bool last_today_pop_valid = false;

    static const char *days[] = {
        "Domingo", "Lunes", "Martes", "Miercoles",
        "Jueves", "Viernes", "Sabado"
    };

    time(&now);
    localtime_r(&now, &tm_info);

    /* ==========================================================
     * Primera ejecución o cambio de pantalla: dibujar todo
     * ========================================================== */
    if (clock_first_run) {
        lcd_fill_screen(COLOR_BLACK);
        display_status_bar();
        wifi_was = wifi_connected;

        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        lcd_draw_text_centered(20, buf, COLOR_WHITE, 4);

        if (tm_info.tm_wday >= 0 && tm_info.tm_wday <= 6)
            lcd_draw_text_centered_8x13(54, days[tm_info.tm_wday], COLOR_TEAL, 2);

        snprintf(buf, sizeof(buf), "%02d/%02d/%04d",
                 tm_info.tm_mday, tm_info.tm_mon + 1, tm_info.tm_year + 1900);
        lcd_draw_text_centered_8x13(84, buf, COLOR_MUTED, 2);

        /* Separador sutil entre fecha y clima */
        lcd_draw_rect(24, 116, LCD_WIDTH - 48, 1, COLOR_SEPARATOR);

        display_weather_section();

        if (time_synced)
            lcd_draw_pixel(LCD_WIDTH - 6, LCD_HEIGHT - 6, COLOR_GREEN);

        last_mday = tm_info.tm_mday;
        last_mon = tm_info.tm_mon;
        last_year = tm_info.tm_year;
        last_wday = tm_info.tm_wday;
        if (weather_data.valid) {
            weather_was_valid = true;
            last_temp_i = (int)(weather_data.temp + 0.5f);
            last_feels_i = (int)(weather_data.feels_like + 0.5f);
            last_icon_idx = xbm_icon_from_code(weather_data.icon);
                last_today_pop = today_pop;
                last_today_pop_valid = today_pop_valid;
        }
        clock_first_run = false;
        ESP_LOGI(TAG, "%02d:%02d:%02d - %s",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                 tm_info.tm_wday >= 0 && tm_info.tm_wday <= 6 ? days[tm_info.tm_wday] : "?");
        return;
    }

    /* ---- Actualizar icono WiFi si cambió estado ---- */
    if (wifi_connected != wifi_was) {
        wifi_was = wifi_connected;
        lcd_draw_rect(0, 0, 24, 22, COLOR_BLACK);
        display_status_bar();
    }

    /* ---- Actualizar HH:MM:SS cada segundo ---- */
    {
        int hw = lcd_text_width("00:00:00", 4);
        int hx = (LCD_WIDTH - hw) / 2;
        lcd_draw_rect(hx, 20, hw, 28, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        lcd_draw_text_centered(20, buf, COLOR_WHITE, 4);
    }

    /* ---- Actualizar día de la semana ---- */
    if (tm_info.tm_wday != last_wday) {
        int dw = lcd_text_width_8x13("Miercoles", 2);
        int dx = (LCD_WIDTH - dw) / 2;
        lcd_draw_rect(dx, 54, dw, 26, COLOR_BLACK);
        if (tm_info.tm_wday >= 0 && tm_info.tm_wday <= 6)
            lcd_draw_text_centered_8x13(54, days[tm_info.tm_wday], COLOR_TEAL, 2);
        last_wday = tm_info.tm_wday;
    }

    /* ---- Actualizar fecha ---- */
    if (tm_info.tm_mday != last_mday || tm_info.tm_mon != last_mon || tm_info.tm_year != last_year) {
        int dw = lcd_text_width_8x13("00/00/0000", 2);
        int dx = (LCD_WIDTH - dw) / 2;
        lcd_draw_rect(dx, 84, dw, 26, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%02d/%02d/%04d",
                 tm_info.tm_mday, tm_info.tm_mon + 1, tm_info.tm_year + 1900);
        lcd_draw_text_centered_8x13(84, buf, COLOR_MUTED, 2);
        last_mday = tm_info.tm_mday;
        last_mon = tm_info.tm_mon;
        last_year = tm_info.tm_year;
    }

    /* ---- Actualizar clima cuando cambian los datos ---- */
    {
        bool weather_valid_now = weather_data.valid;
        bool weather_changed = (weather_valid_now != weather_was_valid) || (today_pop_valid != last_today_pop_valid) || (today_pop_valid && today_pop != last_today_pop);
        if (!weather_changed && weather_valid_now) {
            int temp_i = (int)(weather_data.temp + 0.5f);
            int feels_i = (int)(weather_data.feels_like + 0.5f);
            xbm_icon_t icon_idx = xbm_icon_from_code(weather_data.icon);
            weather_changed = (temp_i != last_temp_i
                              || feels_i != last_feels_i
                              || icon_idx != last_icon_idx);
        }
        if (weather_changed) {
            lcd_draw_rect(0, 114, LCD_WIDTH, 126, COLOR_BLACK);
            display_weather_section();
            weather_was_valid = weather_valid_now;
            if (weather_valid_now) {
                last_temp_i = (int)(weather_data.temp + 0.5f);
                last_feels_i = (int)(weather_data.feels_like + 0.5f);
                last_icon_idx = xbm_icon_from_code(weather_data.icon);
            }
        }
    }

    /* ---- Indicador de sincronización ---- */
    if (time_synced)
        lcd_draw_pixel(LCD_WIDTH - 6, LCD_HEIGHT - 6, COLOR_GREEN);

    ESP_LOGI(TAG, "%02d:%02d:%02d - %s",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             tm_info.tm_wday >= 0 && tm_info.tm_wday <= 6 ? days[tm_info.tm_wday] : "?");
}

/* ============================================================
 * Barra de estado superior — icono WiFi
 * ============================================================ */

/* Icono WiFi 20x16 en formato XBM (LSB = leftmost) */
#define WIFI_ICON_WIDTH  20
#define WIFI_ICON_HEIGHT 16

static const unsigned char wifi_conn_bits[] = {
    0x00, 0x00, 0x00, 0xFC, 0xFF, 0x07, 0x02, 0x00,
    0x08, 0x02, 0x00, 0x08, 0x04, 0x00, 0x04, 0xF0,
    0xFF, 0x01, 0x08, 0x00, 0x02, 0x08, 0x00, 0x02,
    0x10, 0x00, 0x01, 0xC0, 0x7F, 0x00, 0x20, 0x80,
    0x00, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x3F, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char wifi_disconn_bits[] = {
    0x00, 0x0F, 0x00, 0xFC, 0xFF, 0x07, 0x02, 0x0F,
    0x08, 0x82, 0x19, 0x08, 0xC4, 0x30, 0x04, 0xF0,
    0xFF, 0x01, 0x38, 0xC0, 0x02, 0x08, 0x00, 0x02,
    0x10, 0x00, 0x01, 0xC0, 0x7F, 0x00, 0x20, 0x80,
    0x00, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x3F, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00,
};

static void display_status_bar(void)
{
    if (wifi_connected) {
        lcd_draw_xbm(2, 2, wifi_conn_bits,
                     WIFI_ICON_WIDTH, WIFI_ICON_HEIGHT, COLOR_GREEN, 1);
    } else {
        lcd_draw_xbm(2, 2, wifi_disconn_bits,
                     WIFI_ICON_WIDTH, WIFI_ICON_HEIGHT, COLOR_RED, 1);
    }
}

/* ============================================================
 * Mostrar sección de clima
 * Layout: icono + temperatura misma franja, descripción abajo
 * ============================================================ */
static void display_weather_section(void)
{
    if (weather_data.valid) {
        char buf[64];
        int temp_i = (int)(weather_data.temp + 0.5f);

        /* === Icono del clima (64×64) === */
        xbm_icon_t icon_idx = xbm_icon_from_code(weather_data.icon);
        int icon_w = XBM_ICON_WIDTH * 2;   // 64
        int icon_h = XBM_ICON_HEIGHT * 2;  // 64
        int icon_y = 118;

        /* === Temperatura con símbolo de grado: "20°C" (scale 3) === */
        snprintf(buf, sizeof(buf), "%d%cC", temp_i, 127);
        int temp_w = lcd_text_width_8x13(buf, 3);
        int temp_h = FONT8_HEIGHT * 3;  // 39

        /* Centrar el grupo [icono + gap + temperatura] */
        int gap = 10;
        int group_w = icon_w + gap + temp_w;
        int group_x = (LCD_WIDTH - group_w) / 2;
        int icon_x = group_x;
        int temp_x = group_x + icon_w + gap;
        int temp_y = icon_y + (icon_h - temp_h) / 2;

        lcd_draw_xbm(icon_x, icon_y, xbm_icons[icon_idx],
                     XBM_ICON_WIDTH, XBM_ICON_HEIGHT, COLOR_WHITE, 2);
        lcd_draw_text_8x13(temp_x, temp_y, buf, COLOR_AMBER, 3);

        /* === Descripción centrada debajo del icono (scale 2) === */
        int desc_y = icon_y + icon_h + 2;  // y=184
        int max_desc_chars = (LCD_WIDTH - 4) / ((FONT8_WIDTH + 1) * 2);
        snprintf(buf, sizeof(buf), "%.*s", max_desc_chars, weather_data.description);
        for (char *p = buf; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        lcd_draw_text_centered_8x13(desc_y, buf, COLOR_WHITE, 2);

        /* === Probabilidad de lluvia (desde forecast) — siempre visible, scale 2 === */
        int pop_y = desc_y + FONT8_HEIGHT * 2 + 2;  // y=212
        if (today_pop_valid && today_pop > 0) {
            snprintf(buf, sizeof(buf), "Lluvia: %d%%", today_pop);
            lcd_draw_text_centered_8x13(pop_y, buf, COLOR_GREEN, 2);
        } else if (today_pop_valid) {
            lcd_draw_text_centered_8x13(pop_y, "Sin lluvia", COLOR_MUTED, 2);
        } else {
            lcd_draw_text_centered_8x13(pop_y, "Lluvia: --%", COLOR_MUTED, 2);
        }
    } else {
        lcd_draw_text_centered_8x13(170, "Clima: --", COLOR_GRAY, 2);
    }
}

/* ============================================================
 * Mostrar pantalla de pronóstico (4 días)
 *
 * Layout:
 *   ── PRONOSTICO ──     (y=12, TEAL)
 *   ☀ LUN  28°/22°  30%  (y=40, icono 32x32 + texto lado)
 *   ⛅ MAR  26°/20°  60%  (y=80)
 *   ☁ MIE  24°/18°  80%  (y=120)
 *   🌧 JUE  25°/21°  20%  (y=160)
 * ============================================================ */
static void display_forecast(void)
{
    if (!forecast_first_run) return;  /* ya esta dibujada */

    lcd_fill_screen(COLOR_BLACK);

    /* Titulo */
    lcd_draw_text_centered_8x13(10, "--- PRONOSTICO ---", COLOR_TEAL, 1);

    if (!forecast_valid) {
        lcd_draw_text_centered_8x13(115, "Sin datos", COLOR_GRAY, 2);
    } else {
        /* Separador debajo del titulo */
        lcd_draw_rect(20, 26, LCD_WIDTH - 40, 1, COLOR_SEPARATOR);

        int line_h = 56;       /* espacio entre lineas */
        int start_y = 42;      /* primera linea */
        int icon_x = 10;       /* posicion X del icono */

        for (int i = 0; i < MAX_FORECAST_DAYS && forecast_days[i].valid; i++) {
            int y = start_y + i * line_h;
            int text_y = y + 10;  /* centrado vertical respecto al icono */

            /* Icono 32x32 */
            lcd_draw_xbm(icon_x, y, xbm_icons[forecast_days[i].icon],
                         XBM_ICON_WIDTH, XBM_ICON_HEIGHT, COLOR_WHITE, 1);

            /* Etiqueta del dia (scale 2 - mas grande) */
            int text_x = icon_x + XBM_ICON_WIDTH + 6;
            lcd_draw_text_8x13(text_x, text_y,
                               forecast_days[i].day_label, COLOR_TEAL, 2);

            /* Temperaturas: "28°/22°" (scale 2) */
            char buf[24];
            snprintf(buf, sizeof(buf), "%d%c/%d%c",
                     forecast_days[i].temp_max, 127,
                     forecast_days[i].temp_min, 127);
            int label_w = lcd_text_width_8x13(forecast_days[i].day_label, 2);
            int temp_x = text_x + label_w + 8;
            lcd_draw_text_8x13(temp_x, text_y, buf, COLOR_WHITE, 2);

            /* POP (probabilidad de lluvia) alineado a la derecha (scale 2) */
            if (forecast_days[i].pop > 0) {
                snprintf(buf, sizeof(buf), "%d%%", forecast_days[i].pop);
                int pop_w = lcd_text_width_8x13(buf, 2);
                int pop_x = LCD_WIDTH - pop_w - 8;
                int pop_y2 = text_y + 28;  /* segunda línea */
                lcd_draw_text_8x13(pop_x, pop_y2, buf, COLOR_CYAN, 2);
            }
        }
    }

    /* Indicador de pantalla + hint */
    lcd_draw_text_centered_8x13(215, "[ Pronostico ]", COLOR_SEPARATOR, 1);
    lcd_draw_text_centered_8x13(228, "Presione boton o espere", COLOR_MUTED, 1);

    forecast_first_run = false;
}

/* ============================================================
 * Primitivas de dibujo
 * ============================================================ */
static void lcd_fill_screen(uint16_t color)
{
    uint16_t *line = malloc(LCD_WIDTH * sizeof(uint16_t));
    if (!line) {
        ESP_LOGE(TAG, "Error: no hay heap para buffer");
        return;
    }
    for (int x = 0; x < LCD_WIDTH; x++) line[x] = color;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_WIDTH, y + 1, line);
    }
    free(line);
}

static void lcd_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t *line = malloc(w * sizeof(uint16_t));
    if (!line) {
        ESP_LOGE(TAG, "Error: no hay heap para buffer");
        return;
    }
    for (int ix = 0; ix < w; ix++) line[ix] = color;
    for (int iy = y; iy < y + h; iy++) {
        if (iy >= 0 && iy < LCD_HEIGHT) {
            int x0 = (x < 0) ? 0 : x;
            int x1 = (x + w > LCD_WIDTH) ? LCD_WIDTH : x + w;
            if (x1 > x0) {
                esp_lcd_panel_draw_bitmap(panel_handle, x0, iy, x1, iy + 1, line + (x0 - x));
            }
        }
    }
    free(line);
}

static void lcd_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1, &color);
}

/* ============================================================
 * Dibujo de iconos XBM (1 bpp, formato bytes, 32×32)
 * =========================================================== */

/* Dibujar un bitmap en formato XBM: cada fila = (w+7)/8 bytes,
 * bit 0 (LSB) de cada byte = píxel más a la izquierda de ese grupo de 8.
 * bit = 1 → píxel encendido (color), bit = 0 → transparente */
static void lcd_draw_xbm(int x, int y, const unsigned char *bits,
                          int w, int h, uint16_t color, uint8_t scale)
{
    int bytes_per_row = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int byte_idx = row * bytes_per_row + col / 8;
            int bit = col % 8;  // XBM: LSB = leftmost pixel within byte
            if (bits[byte_idx] & (1 << bit)) {
                if (scale == 1) {
                    lcd_draw_pixel(x + col, y + row, color);
                } else {
                    lcd_draw_rect(x + col * scale, y + row * scale,
                                  scale, scale, color);
                }
            }
        }
    }
}

/* ============================================================
 * Dibujo de texto con fuente bitmap 8x13
 * =========================================================== */

/* Dibujar un carácter 8x13 en (x,y) con escala */
static void lcd_draw_char_8x13(int x, int y, char c, uint16_t color, uint8_t scale)
{
    if (c < FONT8_ASCII_START || c > FONT8_ASCII_END) {
        return;
    }

    int idx = c - FONT8_ASCII_START;
    const uint8_t *glyph = font8x13[idx];

    for (int row = 0; row < FONT8_HEIGHT; row++) {
        for (int col = 0; col < FONT8_WIDTH; col++) {
            if (glyph[row] & (1 << (FONT8_WIDTH - 1 - col))) {
                if (scale == 1) {
                    lcd_draw_pixel(x + col, y + row, color);
                } else {
                    lcd_draw_rect(x + col * scale, y + row * scale,
                                  scale, scale, color);
                }
            }
        }
    }
}

/* Dibujar texto con fuente 8x13 */
static void lcd_draw_text_8x13(int x, int y, const char *str, uint16_t color, uint8_t scale)
{
    int spacing = scale;
    int pos_x = x;

    while (*str) {
        lcd_draw_char_8x13(pos_x, y, *str, color, scale);
        pos_x += (FONT8_WIDTH * scale + spacing);
        str++;
    }
}

/* Calcular ancho de texto 8x13 en píxeles */
static int lcd_text_width_8x13(const char *str, uint8_t scale)
{
    int len = strlen(str);
    return len * (FONT8_WIDTH * scale + scale) - scale;
}

/* Dibujar texto 8x13 centrado horizontalmente */
static void lcd_draw_text_centered_8x13(int y, const char *str, uint16_t color, uint8_t scale)
{
    int w = lcd_text_width_8x13(str, scale);
    int x = (LCD_WIDTH - w) / 2;
    if (x < 0) x = 0;
    lcd_draw_text_8x13(x, y, str, color, scale);
}

/* ============================================================
 * Dibujo de texto con fuente bitmap 5x7
 * ============================================================ */

/* Dibujar un carácter en (x,y) con escala (1=5x7, 2=10x14, 3=15x21) */
static void lcd_draw_char(int x, int y, char c, uint16_t color, uint8_t scale)
{
    if (c < FONT_ASCII_START || c > FONT_ASCII_END) return;

    int idx = c - FONT_ASCII_START;
    const uint8_t *glyph = font5x7[idx];

    for (int row = 0; row < FONT_CHAR_HEIGHT; row++) {
        for (int col = 0; col < FONT_CHAR_WIDTH; col++) {
            if (glyph[row] & (1 << (FONT_CHAR_WIDTH - 1 - col))) {
                if (scale == 1) {
                    lcd_draw_pixel(x + col, y + row, color);
                } else {
                    lcd_draw_rect(x + col * scale, y + row * scale,
                                  scale, scale, color);
                }
            }
        }
    }
}

/* Dibujar texto en posición (x,y) */
static void lcd_draw_text(int x, int y, const char *str, uint16_t color, uint8_t scale)
{
    int spacing = scale;
    int pos_x = x;

    while (*str) {
        lcd_draw_char(pos_x, y, *str, color, scale);
        pos_x += (FONT_CHAR_WIDTH * scale + spacing);
        str++;
    }
}

/* Calcular ancho de un texto en píxeles */
static int lcd_text_width(const char *str, uint8_t scale)
{
    int len = strlen(str);
    return len * (FONT_CHAR_WIDTH * scale + scale) - scale;
}

/* Dibujar texto centrado horizontalmente en la línea 'y' */
static void lcd_draw_text_centered(int y, const char *str, uint16_t color, uint8_t scale)
{
    int w = lcd_text_width(str, scale);
    int x = (LCD_WIDTH - w) / 2;
    if (x < 0) x = 0;
    lcd_draw_text(x, y, str, color, scale);
}
