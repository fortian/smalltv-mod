#pragma once
#include <Arduino.h>
#include "Settings.h"

void weatherInit(const Settings& s);
void weatherService(const Settings& s);
void weatherForceRefresh(void);

enum WeatherStage : uint8_t {
  WEATHER_IDLE = 0,
  WEATHER_NO_HOME,
  WEATHER_LOW_HEAP,
  WEATHER_CONNECT_FAIL,
  WEATHER_HTTP_ERROR,
  WEATHER_PARSE_FAIL,
  WEATHER_NO_STATION,
  WEATHER_NO_FEATURES,
  WEATHER_NO_PROPERTIES,
  WEATHER_OK,
};

struct Current {
  time_t timestamp;
};

WeatherStage weatherStage(void);    // outcome of the most recent poll
const char* weatherStageName(void); // that outcome as a short string
uint32_t weatherLastOkMs(void);
bool weatherError(void);
int weatherLastHttp(void);
uint16_t weatherTlsRx(void);
uint32_t weatherLastTryMs(void);
const String& weatherLastUrl(void);
