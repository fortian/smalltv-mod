#include "WeatherMode.h"
#include <Arduino_GFX_Library.h>
#include "WeatherClient.h"

WeatherMode g_weatherMode;

// ---- DisplayMode ----------------------------------------------------------
void WeatherMode::begin(const Settings& s) {
  weatherInit(s);
}

void WeatherMode::invalidate(const Settings& s) {
  weatherInit(s);
  weatherForceRefresh();
}

void WeatherMode::render(const Settings& s) {
}

void WeatherMode::service(const Settings& s) {
    weatherService(s);
}
