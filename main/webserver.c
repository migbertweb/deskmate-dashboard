/*
 * webserver.c — Panel web DeskMate con WebSocket
 * HTTP server + WebSocket nativos de ESP-IDF (sin dependencias externas)
 * HTML/CSS/JS embebido — sin SPIFFS
 */

#include "webserver.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "led_control.h"
#include "weather_icons.h"

static const char *TAG = "web";

/* ─── Mapeo de iconos OWM a emoji ─── */
const char *weather_emoji(const char *owm_code) {
    if (!owm_code) return "--";
    char c = owm_code[0];
    // Usar primer dígito del código OWM (01d → '0', 02d → '0', etc.)
    switch (c) {
        case '0': return (owm_code[1] == '1') ? ((owm_code[2] == 'd') ? "☀️" : "🌙") : "⛅";
        case '2': return "⛅";
        case '3': return "☁️";
        case '4': return "☁️";
        case '9': return "🌧️";
        case '1': return (owm_code[1] == '0') ? "🌦️" : "⛈️";
        case '5': return "🌧️";
        case '6': return "🌨️";
        case '7': return "🌫️";
        case '8': return "☀️";
        default:  return "--";
    }
}

/* ─── Mapeo de xbm_icon_t a emoji para forecast ─── */
const char *xbm_emoji(xbm_icon_t icon) {
    switch (icon) {
        case XBM_SUN:         return "☀️";
        case XBM_MOON:        return "🌙";
        case XBM_CLOUD_SUN:   return "⛅";
        case XBM_CLOUD_MOON:  return "⛅";
        case XBM_CLOUD:       return "☁️";
        case XBM_CLOUDS:      return "☁️";
        case XBM_RAIN0:       return "🌧️";
        case XBM_RAIN1_SUN:   return "🌦️";
        case XBM_RAIN1_MOON:  return "🌦️";
        case XBM_RAIN_LIGHTNING: return "⛈️";
        case XBM_SNOW:        return "🌨️";
        case XBM_WIND:        return "🌫️";
        default:              return "--";
    }
}

static httpd_handle_t server = NULL;

#define MAX_WS_CLIENTS 4
static int ws_fds[MAX_WS_CLIENTS] = {-1, -1, -1, -1};

static void ws_add_client(int fd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_fds[i] == -1) { ws_fds[i] = fd; return; }
    }
}
static void ws_remove_client(int fd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_fds[i] == fd) { ws_fds[i] = -1; return; }
    }
}
void ws_broadcast(const char *json) {
    if (!server || !json) return;
    int len = strlen(json);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_fds[i] == -1) continue;
        httpd_ws_frame_t pkt = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = len
        };
        if (httpd_ws_send_frame_async(server, ws_fds[i], &pkt) != ESP_OK) {
            ws_remove_client(ws_fds[i]);
        }
    }
}

static const char *HTML = R"RAW(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>DeskMate</title>
<style>
:root {
  --bg: #0d1117;
  --card: #161b22;
  --border: #30363d;
  --text: #c9d1d9;
  --muted: #8b949e;
  --teal: #06bfad;
  --green: #3fb950;
  --amber: #d2991d;
  --red: #da3633;
  --blue: #58a6ff;
}
* { margin:0; padding:0; box-sizing:border-box; }
body {
  background: var(--bg);
  color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  min-height: 100vh;
  display: flex;
  justify-content: center;
  padding: 16px;
}
.dashboard {
  max-width: 420px;
  width: 100%;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.card {
  background: var(--card);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 16px;
}
.card-title {
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 1px;
  color: var(--muted);
  margin-bottom: 8px;
}
.clock-time {
  font-size: 48px;
  font-weight: 700;
  letter-spacing: 2px;
  text-align: center;
  color: #fff;
}
.clock-date {
  text-align: center;
  color: var(--teal);
  font-size: 16px;
  margin-top: 2px;
}
.clock-date small { color: var(--muted); }
.weather-row {
  display: flex;
  align-items: center;
  gap: 16px;
}
.weather-icon {
  font-size: 48px;
  width: 56px;
  text-align: center;
}
.weather-temp {
  font-size: 40px;
  font-weight: 700;
}
.weather-desc {
  font-size: 14px;
  color: var(--muted);
}
.weather-pop {
  margin-top: 8px;
  font-size: 14px;
  color: var(--green);
}
.fc-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 6px 0;
  border-bottom: 1px solid var(--border);
}
.fc-row:last-child { border-bottom: none; }
.fc-day { color: var(--teal); font-weight: 600; width: 36px; }
.fc-icon { font-size: 24px; width: 32px; text-align: center; }
.fc-temps { color: var(--text); font-size: 14px; }
.fc-pop { color: var(--blue); font-size: 13px; width: 36px; text-align: right; }
.led-status {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 10px;
}
.led-dot {
  width: 12px; height: 12px;
  border-radius: 50%;
  background: var(--muted);
  transition: background .3s;
}
.led-dot.on { background: var(--teal); box-shadow: 0 0 8px var(--teal); }
.led-name { font-size: 14px; }
.led-buttons {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}
.led-btn {
  background: var(--border);
  color: var(--text);
  border: none;
  border-radius: 6px;
  padding: 6px 12px;
  font-size: 12px;
  cursor: pointer;
  transition: background .2s;
}
.led-btn:hover { background: #484f58; }
.led-btn.active { background: var(--teal); color: #000; }
.footer {
  text-align: center;
  font-size: 11px;
  color: var(--muted);
  padding: 8px;
}
.ws-dot {
  display: inline-block;
  width: 6px; height: 6px;
  border-radius: 50%;
  margin-right: 4px;
}
.ws-dot.ok { background: var(--green); }
.ws-dot.err { background: var(--red); }
</style>
</head>
<body>
<div class="dashboard">
  <div class="card" id="clock-card">
    <div class="card-title">Reloj</div>
    <div class="clock-time" id="time">--:--:--</div>
    <div class="clock-date"><span id="wday">---</span> <small id="date">--/--/----</small></div>
  </div>
  <div class="card" id="weather-card">
    <div class="card-title">Clima &middot; Joinville</div>
    <div class="weather-row">
      <div class="weather-icon" id="w-icon">--</div>
      <div>
        <div class="weather-temp" id="w-temp">--°C</div>
        <div class="weather-desc" id="w-desc">Cargando...</div>
      </div>
    </div>
    <div class="weather-pop" id="w-pop">Lluvia: --%</div>
  </div>
  <div class="card" id="forecast-card">
    <div class="card-title">Próximos días</div>
    <div id="fc-container">Cargando...</div>
  </div>
  <div class="card" id="led-card">
    <div class="card-title">Anillo LED</div>
    <div class="led-status">
      <div class="led-dot" id="led-dot"></div>
      <span class="led-name" id="led-mode">--</span>
      <button class="led-btn" id="led-power" style="margin-left:auto">ON/OFF</button>
    </div>
    <div style="display:flex;align-items:center;gap:8px;margin:8px 0">
      <input type="color" id="led-color" value="#ff8000" style="width:36px;height:36px;border:none;border-radius:6px;cursor:pointer">
      <input type="range" id="led-brightness" min="0" max="255" value="255" style="flex:1;accent-color:var(--teal)">
    </div>
    <div class="led-buttons" id="led-btns">
      <button class="led-btn" data-mode="0">Pulse</button>
      <button class="led-btn" data-mode="1">Chase</button>
      <button class="led-btn" data-mode="2">Lamp</button>
      <button class="led-btn" data-mode="3">Rainbow</button>
      <button class="led-btn" data-mode="4">Candle</button>
      <button class="led-btn" data-mode="5">Aurora</button>
      <button class="led-btn" data-mode="6">Solid</button>
      <button class="led-btn" data-mode="7">Off</button>
    </div>
  </div>
  <div class="footer">
    <span class="ws-dot" id="ws-dot"></span>
    DeskMate &middot; <span id="ws-status">conectando...</span>
  </div>
</div>
<script>
const WS_URL = 'ws://' + window.location.host + '/ws';
let ws = null, wsOk = false;
function setWsStatus(ok, txt) {
  wsOk = ok;
  document.getElementById('ws-dot').className = 'ws-dot ' + (ok ? 'ok' : 'err');
  document.getElementById('ws-status').textContent = txt;
}
function connect() {
  ws = new WebSocket(WS_URL);
  ws.onopen = () => setWsStatus(true, 'conectado');
  ws.onclose = () => { setWsStatus(false, 'desconectado'); setTimeout(connect, 3000); };
  ws.onerror = () => setWsStatus(false, 'error');
  ws.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data);
      if (data.type === 'clock') updateClock(data);
      else if (data.type === 'weather') updateWeather(data);
      else if (data.type === 'forecast') updateForecast(data);
      else if (data.type === 'led') updateLed(data);
    } catch(_) {}
  };
}
function updateClock(d) {
  document.getElementById('time').textContent = d.time || '--:--:--';
  document.getElementById('wday').textContent = d.wday || '---';
  document.getElementById('date').textContent = d.date || '--/--/----';
}
function updateWeather(d) {
  document.getElementById('w-icon').textContent = d.icon || '--';
  document.getElementById('w-temp').textContent = (d.temp != null ? d.temp + '°C' : '--°C');
  document.getElementById('w-desc').textContent = d.desc || 'Cargando...';
  document.getElementById('w-pop').textContent = d.pop || 'Lluvia: --%';
}
function updateForecast(d) {
  const days = d.days || [];
  if (!days.length) { document.getElementById('fc-container').textContent = 'Cargando...'; return; }
  let h = '';
  days.forEach(day => {
    h += '<div class=\"fc-row\"><span class=\"fc-day\">' + day.label + '</span><span class=\"fc-icon\">' + day.icon + '</span><span class=\"fc-temps\">' + day.max + '°/' + day.min + '°</span><span class=\"fc-pop\">' + (day.pop > 0 ? day.pop + '%' : '') + '</span></div>';
  });
  document.getElementById('fc-container').innerHTML = h;
}
function updateLed(d) {
  const modes = ['Pulse','Chase','Lamp','Rainbow','Candle','Aurora','Solid','Off'];
  document.getElementById('led-mode').textContent = modes[d.mode] || '--';
  const dot = document.getElementById('led-dot');
  dot.className = 'led-dot' + (d.mode !== 7 ? ' on' : '');
  document.querySelectorAll('.led-btn').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.mode) === d.mode);
  });
}
/* ── LED controls ── */
document.getElementById('led-btns').addEventListener('click', (e) => {
  if (!e.target.classList.contains('led-btn')) return;
  if (!wsOk) return;
  ws.send(JSON.stringify({ command: 'led_mode', mode: parseInt(e.target.dataset.mode) }));
});
document.getElementById('led-color').addEventListener('input', (e) => {
  if (!wsOk) return;
  const hex = e.target.value;
  ws.send(JSON.stringify({ command: 'led_color', r: parseInt(hex.substr(1,2),16), g: parseInt(hex.substr(3,2),16), b: parseInt(hex.substr(5,2),16) }));
});
document.getElementById('led-brightness').addEventListener('input', (e) => {
  if (!wsOk) return;
  ws.send(JSON.stringify({ command: 'led_brightness', value: parseInt(e.target.value) }));
});
document.getElementById('led-power').addEventListener('click', () => {
  if (!wsOk) return;
  ws.send(JSON.stringify({ command: 'led_power', on: !document.getElementById('led-dot').classList.contains('on') }));
});
connect();
</script>
</body>
</html>
)RAW";

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS conectado fd=%d", fd);
        return ESP_OK;
    }
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) return ret;
    if (pkt.len > 0) {
        uint8_t *buf = calloc(1, pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WS msg: %.*s", pkt.len, (char*)pkt.payload);

            /* Parsear comando JSON */
            cJSON *root = cJSON_Parse((char*)pkt.payload);
            if (root) {
                cJSON *cmd = cJSON_GetObjectItem(root, "command");
                if (cJSON_IsString(cmd)) {
                    const char *c = cmd->valuestring;

                    if (strcmp(c, "led_mode") == 0) {
                        cJSON *m = cJSON_GetObjectItem(root, "mode");
                        if (cJSON_IsNumber(m)) led_set_mode((led_mode_t)m->valueint);
                    }
                    else if (strcmp(c, "led_color") == 0) {
                        cJSON *r = cJSON_GetObjectItem(root, "r");
                        cJSON *g = cJSON_GetObjectItem(root, "g");
                        cJSON *b = cJSON_GetObjectItem(root, "b");
                        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b))
                            led_set_color(r->valueint, g->valueint, b->valueint);
                    }
                    else if (strcmp(c, "led_brightness") == 0) {
                        cJSON *v = cJSON_GetObjectItem(root, "value");
                        if (cJSON_IsNumber(v)) led_set_brightness(v->valueint);
                    }
                    else if (strcmp(c, "led_power") == 0) {
                        cJSON *o = cJSON_GetObjectItem(root, "on");
                        bool on_val = cJSON_IsTrue(o) ? true : (cJSON_IsFalse(o) ? false : led_is_on());
                        if (on_val != led_is_on()) led_toggle_power();
                    }
                }
                cJSON_Delete(root);
            }
        }
        free(buf);
    }
    return ret;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, HTML, strlen(HTML));
}

static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
};

static const httpd_uri_t ws_uri = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .is_websocket = true,
};

void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &ws_uri);
        ESP_LOGI(TAG, "Servidor iniciado en puerto %d", cfg.server_port);
    } else {
        ESP_LOGE(TAG, "Error al iniciar servidor");
    }
}

void stop_webserver(void)
{
    if (server) { httpd_stop(server); server = NULL; }
}
