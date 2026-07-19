#pragma once
#include "weather_icons.h"

void start_webserver(void);
void stop_webserver(void);
void ws_broadcast(const char *json);
const char *weather_emoji(const char *owm_code);
const char *xbm_emoji(xbm_icon_t icon);
