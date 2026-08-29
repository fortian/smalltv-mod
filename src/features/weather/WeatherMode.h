#pragma once
#include "Mode.h"
#include "config.h"

class WeatherMode : public DisplayMode {
 public:
  const char* id() const override { return "weather"; }
  uint8_t     modeConst() const override { return MODE_WEATHER; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override {  // repaint only
  }

 private:
  void render(const Settings& s, uint8_t live);
};

extern WeatherMode g_weatherMode;
