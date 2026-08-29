#include "WeatherClient.h"
#include "Platform.h"
#include <ArduinoJson.h>

static uint32_t g_lastOkMs = 0;
static bool     g_error = false;
static uint32_t g_nextPollMs = 0;

static uint16_t g_tlsRx = 0;
static const char * g_tlsProbedHost = nullptr;

static TlsSession g_tlsSession;

static WeatherStage g_stage = WEATHER_IDLE;
static int g_lastHttp = 0;
static uint16_t g_lastTryMs = 0;
static String g_lastUrl;

static String g_station;

uint32_t weatherLastOkMs(void) { return g_lastOkMs; }
bool weatherError(void) { return g_error; }

WeatherStage weatherStage(void) { return g_stage; }
int weatherLastHttp(void) { return g_lastHttp; }
uint16_t weatherTlsRx(void) { return g_tlsRx; }
uint32_t weatherLastTryMs(void) { return g_lastTryMs; }
const String& weatherLastUrl(void) { return g_lastUrl; }

const char* weatherStageName() {
  switch (g_stage) {
    case WEATHER_NO_HOME:       return "no home set";
    case WEATHER_LOW_HEAP:      return "skipped, low heap";
    case WEATHER_CONNECT_FAIL:  return "connect failed";
    case WEATHER_HTTP_ERROR:    return "http error";
    case WEATHER_PARSE_FAIL:    return "parse failed";
    case WEATHER_NO_STATION:    return "no station set or found";
    case WEATHER_NO_FEATURES:   return "no features in feed";
    case WEATHER_NO_PROPERTIES: return "no properties in feed";
    case WEATHER_OK:            return "ok";
    default:                    return "idle";
  }
}

void weatherInit(const Settings& s) {
    g_error = false;
    g_lastOkMs = 0;
    g_nextPollMs = millis();
    g_stage = WEATHER_IDLE;
    g_lastHttp = 0;
    g_lastTryMs = 0;
    g_lastUrl = "";
    g_station = s.weather.station;
}

void weatherForceRefresh(void) { g_nextPollMs = millis(); }

static String buildPointsDirectUrl(const Settings& s) {
  String u = F("https://");
  u += WEATHER_HOST;
  u += WEATHER_POINTS_PATH;
  u += String(s.weather.lat, 4);
  u += F(",");
  u += String(s.weather.lon, 4);
  return u;
}

static String buildPointsWebhookUrl(const Settings& s) {
  String u = s.weather.webhookUrl;
  char sep = (u.indexOf('?') >= 0) ? '&' : '?';
  u += sep;
  u += F("lat=") + String(s.weather.lat, 4);
  u += F("&lon=") + String(s.weather.lon, 4);
  u += F("&action=points");
  return u;
}

static String buildCurrentDirectUrl(const Settings& s) {
  String u = F("https://");
  u += WEATHER_HOST;
  u += WEATHER_CURRENT_PATH;
  u += g_station;
  u += F("/observations/latest");
  return u;
}

static String buildCurrentWebhookUrl(const Settings &s) {
  String u = s.weather.webhookUrl;
  char sep = (u.indexOf('?') >= 0) ? '&' : '?';
  u += sep;
  u += F("station=") + String(g_station, 4);
  u += F("&action=current");
  return u;
}

// ---- probe MFLN once so TLS can use the smallest safe buffer ---------------
// Re-probed when the provider changes: the two hosts answer differently
// (adsb.lol negotiates MFLN, adsb.fi behind Cloudflare does not).
static void probeTls(const Settings& s) {
#if defined(SMALLTV_ESP8266)
  const char* host = directHost(s);
  if (g_tlsRx && g_tlsProbedHost == host) return;
  g_tlsProbedHost = host;
  g_tlsSession = TlsSession();   // stored params are per-host; drop them with it
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 512))       g_tlsRx = 512;
  else if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 1024)) g_tlsRx = 1024;
  else                                                                         g_tlsRx = 4096;
#else
  (void)s;
#endif
}

static bool parsePoints(const Settings &s, Stream& stream) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, stream);
  if (err) { g_stage = WEATHER_PARSE_FAIL; return false; }
  JsonArrayConst features = doc["features"];
  if (features.isNull()) { g_stage = WEATHER_NO_FEATURES; return false; }

  for (JsonObjectConst f : features) {
    JsonObjectConst properties = features["properties"];
    if (properties.isNull()) { continue; }
    if (properties["stationIdentifier"].is<const char*>()) {
      g_station = properties["stationIdentifier"].as<const char*>();
      break;
    }
  }
  if (g_station == "") {
    g_stage = WEATHER_NO_STATION;
    return false;
  }
  g_error = false;
  g_stage = WEATHER_OK;
  return true;
}

static bool parseCurrent(const Settings &s, Stream& stream) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, stream);
  if (err) { g_stage = WEATHER_PARSE_FAIL; return false; }
  JsonObjectConst properties = doc["properties"];
  if (properties.isNull()) { g_state = WEATHER_NO_PROPERTIES; return false; }
}

// ---- one HTTP(S) GET + parse ----------------------------------------------
static bool fetchUrl(const Settings& s, const String& url, bool needStation) {
  bool https = url.startsWith("https://");

  std::unique_ptr<NetClient> client;
  if (https) {
    // The handshake needs one contiguous block, not total free heap: 16 KB is
    // the same floor the cash.ch fetch uses for this identical handshake shape.
    // (The old total-heap >= 18000 test passed on fragmented heaps that then
    // failed the allocation, and failed healthy ones that would have worked.)
    if (platformMaxFreeBlock() < 16000) { g_stage = WEATHER_LOW_HEAP; return false; }
    probeTls(s);
    client.reset(platformMakeSecureClient(g_tlsRx, &g_tlsSession)); // no cert validation (public read-only API)
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  // HTTP/1.0: a 1.0 response cannot be chunked, and parseAdsb reads the raw
  // stream. Cloudflare (fronting adsb.fi) answers 1.1 requests chunked, and the
  // chunk-size framing then reaches ArduinoJson, which reads the first hex
  // length as a bare number and reports a valid document with no "ac" in it —
  // an empty scope with HTTP 200 and no error anywhere.
  http.useHTTP10(true);
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  if (!http.begin(*client, url)) { g_stage = WEATHER_CONNECT_FAIL; return false; }
  http.addHeader("Accept", "application/geo+json");
  http.setUserAgent(F(WEATHER_USER_AGENT));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  g_lastHttp = code;
  if (code != HTTP_CODE_OK) {
    http.end();
    // A negative code is an ESP8266HTTPClient internal error (connection
    // refused, read timeout, connection lost) rather than a server reply.
    g_stage = (code < 0) ? WEATHER_CONNECT_FAIL : WEATHER_HTTP_ERROR;
    return false;
  }

  bool ok = false;
  if (needStation) {
    ok = parsePoints(s, http.getStream());
  } else {
    ok = parseCurrent(s, http.getStream());
  }
  http.end();
  return ok;
}

void weatherService(const Settings& s) {
  if ((s.weather.lat == 0.0f) && (s.weather.lon == 0.0f)) { g_stage = WEATHER_NO_HOME; return; }

  if ((int32_t)(millis - g_nextPollMs) < 0) return;
  g_nextPollMs = millis + (uint32_t)s.weather.pollSec * 1000UL;

  bool useWebhook = (s.weather.source == WEATHER_SRC_WEBHOOK) && (s.weather.webhookUrl.length() >= 8);
  String url;
  if (g_station == "") {
    // collect the station first
    String url = useWebhook ? buildPointsWebhookUrl(s) : buildPointsDirectUrl(s);
    g_lastUrl = url;
    g_lastTryMs = millis();
    if (!fetchUrl(s, url, true)) g_error = true;
  }
  if (g_station != "") {
    String url = useWebhook : buildCurrentWebhookUrl(s) : buildCurrentDirectUrl(s);
    g_lastUrl = url;
    g_lastTryMs = millis();
    if (!fetchUrl(s, url, false)) {
      g_error = true;
    }
  }
}
