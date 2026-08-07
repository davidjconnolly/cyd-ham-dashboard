#include "dx_spots.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

#include "app_config.h"
#include "dx_backfill.h"
#include "dx_json_scanner.h"
#include "dx_watch.h"
#include "settings.h"

#ifndef DX_SPOTS_URL
#define DX_SPOTS_URL ""
#endif

namespace {
#define DEFAULT_DX_SPOTS_URL "https://web.cluster.iz3mez.it/spots.json/"

#ifndef DEBUG_DX_TELNET
#define DEBUG_DX_TELNET 0
#endif

constexpr uint32_t kHttpTimeoutMs = 5000;
constexpr size_t kMaxDxObjectChars = 1536;
constexpr size_t kDxObjectDocBytes = 2048;
// A narrow mode filter can reject nearly all of a feed, so the scan is allowed
// to read the whole of a typical one rather than give up with a half-empty
// page. Without a filter it still stops at the eighth spot. The time budget
// above is the real guard against a pathologically large endpoint.
constexpr uint16_t kMaxDxObjectsScanned = 250;
// Read budget per loop() iteration, matching dx_backfill. The scan is spread
// across as many iterations as it takes rather than draining the feed in one.
constexpr size_t kMaxDxJsonBytesPerLoop = 2048;
// Wall-clock bound on a whole scan, now that it outlives a single call. Longer
// than the old in-call budget because it now covers the gaps between
// iterations as well as the reading.
constexpr uint32_t kDxJsonScanBudgetMs = 20000;
constexpr uint32_t kTelnetReconnectIntervalMs = 30000;
constexpr uint32_t kTelnetLoginDelayMs = 1500;
constexpr int32_t kTelnetConnectTimeoutMs = 5000;
constexpr size_t kMaxTelnetLineChars = 180;
constexpr size_t kMaxTelnetBytesPerLoop = 512;

enum TelnetConnectState : uint8_t {
  kTelnetConnectIdle,
  kTelnetConnectRunning,
  kTelnetConnectSucceeded,
  kTelnetConnectFailed,
  kTelnetConnectCancelled
};

struct TelnetConnectRequest {
  char host[65];
  uint16_t port;
};

DxSpotsData g_data;
uint32_t g_lastAttemptMs = 0;
bool g_refreshRequested = true;

// --- Incremental JSON scan ---
// The HTTP client, the stream and the scanner all outlive a single loop()
// iteration, so the feed is read a bounded slice at a time. Everything the
// scan needs to resume lives here.
HTTPClient g_jsonHttp;
WiFiClient g_jsonPlainClient;
WiFiClientSecure g_jsonSecureClient;
Stream* g_jsonStream = nullptr;
bool g_jsonStreaming = false;
uint32_t g_jsonStartedMs = 0;
DxJsonObjectScanner<kMaxDxObjectChars> g_jsonScanner;
// One document, reused for every object, and deliberately static rather than
// heap: the scan is now spread over many loop() iterations, so a document
// allocated and freed per object would interleave 2 KB churn with the display,
// the web server and the Telnet buffer for the length of a scan. Fragmentation
// is the constraint here, not peak usage, so this is kept out of the heap
// entirely rather than merely allocated less often.
StaticJsonDocument<kDxObjectDocBytes> g_jsonDoc;
DxSpotsData g_jsonParsed;
uint16_t g_jsonObjectsScanned = 0;
// Captured when the scan starts, because the decision the old synchronous
// fetch made on its return value now has to be made iterations later, when
// the reason for starting is no longer on the stack.
bool g_jsonAutoMode = false;
bool g_jsonReconnectFallback = false;
bool g_jsonTelnetWasActive = false;
WiFiClient g_telnetClient;
String g_telnetLineBuffer;
bool g_telnetLineOverflow = false;
bool g_telnetConnected = false;
bool g_telnetLoggedIn = false;
bool g_telnetHasCurrentSpots = false;
bool g_autoUsingTelnet = false;
uint32_t g_lastTelnetConnectAttemptMs = 0;
uint32_t g_telnetConnectedAtMs = 0;
uint32_t g_lastTelnetDataMs = 0;
uint8_t g_lastSourceMode = 0xFF;
TelnetConnectRequest g_telnetConnectRequest;
volatile TelnetConnectState g_telnetConnectState = kTelnetConnectIdle;
volatile bool g_telnetConnectCancelRequested = false;
portMUX_TYPE g_telnetConnectMux = portMUX_INITIALIZER_UNLOCKED;

String valueOrDash(String value) {
  value.trim();
  if (value == "null") {
    return "--";
  }
  return value.length() > 0 ? value : String("--");
}

String providerNameFromAddress(String address) {
  address.trim();
  const int schemeEnd = address.indexOf("://");
  if (schemeEnd >= 0) {
    address = address.substring(schemeEnd + 3);
  }

  const int pathStart = address.indexOf('/');
  if (pathStart >= 0) {
    address = address.substring(0, pathStart);
  }
  const int credentialsEnd = address.lastIndexOf('@');
  if (credentialsEnd >= 0) {
    address = address.substring(credentialsEnd + 1);
  }
  const int portStart = address.indexOf(':');
  if (portStart >= 0) {
    address = address.substring(0, portStart);
  }

  address.toLowerCase();
  const char* prefixes[] = {"www.", "web.", "cluster.", "telnet.", "dxcluster."};
  bool removedPrefix = true;
  while (removedPrefix) {
    removedPrefix = false;
    for (const char* prefix : prefixes) {
      if (address.startsWith(prefix)) {
        address = address.substring(strlen(prefix));
        removedPrefix = true;
        break;
      }
    }
  }

  const int domainEnd = address.indexOf('.');
  if (domainEnd > 0) {
    address = address.substring(0, domainEnd);
  }
  address.toUpperCase();
  return valueOrDash(address);
}

String jsonProviderName() {
  return providerNameFromAddress(getDxSpotsUrl());
}

String telnetProviderName() {
  return providerNameFromAddress(getSettings().dxTelnetHost);
}

String isoTimeToDisplay(String iso) {
  iso.trim();
  if (iso.length() >= 16 && iso[10] == 'T') {
    return iso.substring(11, 16) + " UTC";
  }
  if (iso.length() >= 16 && iso[10] == ' ') {
    return iso.substring(11, 16) + " UTC";
  }
  return valueOrDash(iso);
}

String formatSpotTime(String value) {
  value.trim();
  if (value.length() >= 5 && value[2] == ':') {
    return value.substring(0, 2) + value.substring(3, 5) + "Z";
  }
  if (value.length() >= 4) {
    return value.substring(0, 4) + "Z";
  }
  return valueOrDash(value);
}

String upperCopy(String value) {
  value.toUpperCase();
  return value;
}

// Order matters: the comment scan takes the first hit, so SSB is tested before
// USB and LSB. The unknown catch-all is always the last entry.
constexpr DxModeOption kDxModeOptions[kDxModeOptionCount] = {
    {"FT8", "FT8", kDxModeFt8},     {"FT4", "FT4", kDxModeFt4},
    {"CW", "CW", kDxModeCw},        {"SSB", "SSB", kDxModeSsb},
    {"USB", "USB", kDxModeUsb},     {"LSB", "LSB", kDxModeLsb},
    {"RTTY", "RTTY", kDxModeRtty},  {"SSTV", "SSTV", kDxModeSstv},
    {"PSK", "PSK", kDxModePsk},     {"--", "Unknown", kDxModeUnknown},
};

uint16_t enabledModeMask() {
  return getSettings().dxModeMask & static_cast<uint16_t>(kDxModeAll);
}

bool containsModeToken(const String& comment, const char* token) {
  return comment.indexOf(token) >= 0;
}

bool frequencyIsNear(const String& freq, double targetMhz) {
  double mhz = freq.toDouble();
  if (mhz > 1000.0) {
    mhz /= 1000.0;
  }
  return fabs(mhz - targetMhz) < 0.002;
}

String formatFrequency(String value) {
  value.trim();
  if (value.length() == 0) {
    return "--";
  }

  double mhz = value.toDouble();
  if (mhz <= 0.0) {
    return valueOrDash(value);
  }
  if (mhz > 1000.0) {
    mhz /= 1000.0;
  }

  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%.3f", mhz);
  return String(buffer);
}

String deriveMode(const String& freq, const String& comment) {
  const String upperComment = upperCopy(comment);
  for (uint8_t i = 0; i + 1 < kDxModeOptionCount; ++i) {
    if (containsModeToken(upperComment, kDxModeOptions[i].name)) {
      return String(kDxModeOptions[i].name);
    }
  }

  if (frequencyIsNear(freq, 7.074) || frequencyIsNear(freq, 14.074) ||
      frequencyIsNear(freq, 21.074) || frequencyIsNear(freq, 28.074)) {
    return "FT8";
  }
  if (frequencyIsNear(freq, 7.047) || frequencyIsNear(freq, 14.080) ||
      frequencyIsNear(freq, 21.080) || frequencyIsNear(freq, 28.080)) {
    return "FT4";
  }

  return "--";
}

bool beginHttp(const String& url, HTTPClient& http, WiFiClient& plainClient,
               WiFiClientSecure& secureClient) {
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }
  return http.begin(plainClient, url);
}

uint32_t refreshIntervalMs() {
  const AppSettings& settings = getSettings();
  const uint16_t minutes = constrain(settings.dxRefreshMinutes, static_cast<uint16_t>(1),
                                     static_cast<uint16_t>(120));
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

String currentUtcDisplay(const String& spotTime = "") {
  const time_t now = time(nullptr);
  if (now >= 1704067200) {
    tm utc;
    gmtime_r(&now, &utc);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%H:%M UTC", &utc);
    return String(buffer);
  }
  if (spotTime.length() == 5 && spotTime[4] == 'Z') {
    return spotTime.substring(0, 2) + ":" + spotTime.substring(2, 4) + " UTC";
  }
  return "--";
}

bool nextToken(const String& text, int& position, String& token, int* tokenStart = nullptr) {
  while (position < static_cast<int>(text.length()) &&
         isspace(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position >= static_cast<int>(text.length())) {
    return false;
  }

  const int start = position;
  while (position < static_cast<int>(text.length()) &&
         !isspace(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (tokenStart != nullptr) {
    *tokenStart = start;
  }
  token = text.substring(start, position);
  return true;
}

bool isUtcTimeToken(const String& token) {
  if (token.length() != 5 || (token[4] != 'Z' && token[4] != 'z')) {
    return false;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    if (!isdigit(static_cast<unsigned char>(token[i]))) {
      return false;
    }
  }
  const int hour = token.substring(0, 2).toInt();
  const int minute = token.substring(2, 4).toInt();
  return hour <= 23 && minute <= 59;
}

bool parseDxClusterSpotLine(const String& rawLine, DxSpot& out) {
  String line = rawLine;
  line.trim();
  if (!line.startsWith("DX de ")) {
    return false;
  }

  const int colon = line.indexOf(':', 6);
  if (colon < 7) {
    return false;
  }

  out = DxSpot();
  out.spotter = line.substring(6, colon);
  out.spotter.trim();
  out.spotter.toUpperCase();

  const String remainder = line.substring(colon + 1);
  int position = 0;
  String frequencyToken;
  String callToken;
  if (!nextToken(remainder, position, frequencyToken) ||
      !nextToken(remainder, position, callToken) || frequencyToken.toDouble() <= 0.0) {
    return false;
  }

  const int commentStart = position;
  int timeStart = -1;
  String timeToken;
  String token;
  int tokenStart = 0;
  while (nextToken(remainder, position, token, &tokenStart)) {
    if (isUtcTimeToken(token)) {
      timeStart = tokenStart;
      timeToken = token;
      break;
    }
  }

  out.freq = formatFrequency(frequencyToken);
  out.call = callToken;
  out.call.toUpperCase();
  out.time = timeToken.length() > 0 ? formatSpotTime(timeToken) : String("--");
  out.comment = timeStart >= 0 ? remainder.substring(commentStart, timeStart)
                               : remainder.substring(commentStart);
  out.comment.trim();
  out.comment = valueOrDash(out.comment);
  out.mode = deriveMode(out.freq, out.comment);
  out.band = "--";
  out.country = "--";
  out.continent = "--";
  return out.spotter.length() > 0 && out.freq != "--" && out.call.length() > 0;
}

bool isDuplicateDxSpot(const DxSpot& spot) {
  for (uint8_t i = 0; i < g_data.spotCount; ++i) {
    const DxSpot& existing = g_data.spots[i];
    if (existing.call.equalsIgnoreCase(spot.call) && existing.freq == spot.freq &&
        existing.time == spot.time) {
      return true;
    }
  }
  return false;
}

void setTelnetStatus(const String& status) {
  g_data.status = status;
  g_data.provider = telnetProviderName();
  if (g_telnetHasCurrentSpots) {
    g_data.source = "Telnet";
  } else if (g_data.hasData) {
    g_data.source = "Last good";
  } else {
    g_data.source = "Telnet";
  }
}

void stopDxTelnet(bool resetReconnectTimer) {
  if (g_telnetConnected || g_telnetClient.connected()) {
    Serial.println("DX Telnet disconnected");
  }

  bool stopClient = false;
  portENTER_CRITICAL(&g_telnetConnectMux);
  if (g_telnetConnectState == kTelnetConnectRunning) {
    g_telnetConnectCancelRequested = true;
  } else {
    g_telnetConnectState = kTelnetConnectIdle;
    stopClient = true;
  }
  portEXIT_CRITICAL(&g_telnetConnectMux);
  if (stopClient) {
    g_telnetClient.stop();
  }
  g_telnetConnected = false;
  g_telnetLoggedIn = false;
  g_telnetLineBuffer = "";
  g_telnetLineOverflow = false;
  if (resetReconnectTimer) {
    g_lastTelnetConnectAttemptMs = 0;
  }
}

void sendDxTelnetLoginIfNeeded() {
  if (!g_telnetConnected || g_telnetLoggedIn || !g_telnetClient.connected()) {
    return;
  }

  String callsign = getSettings().callsign;
  callsign.trim();
  callsign.toUpperCase();
  if (callsign.length() == 0) {
    callsign = "NOCALL"; // Safe placeholder when the station callsign has not been configured.
  }
  g_telnetClient.println(callsign);
  g_telnetLoggedIn = true;
  setTelnetStatus("Login sent");
  Serial.print("DX Telnet login sent: ");
  Serial.println(callsign);
}

bool addDxSpotToList(const DxSpot& spot) {
  if (isDuplicateDxSpot(spot)) {
#if DEBUG_DX_TELNET
    Serial.println("DX Telnet duplicate ignored");
#endif
    return false;
  }

  if (!g_telnetHasCurrentSpots) {
    g_data = DxSpotsData();
    g_telnetHasCurrentSpots = true;
  }

  const uint8_t last = min<uint8_t>(g_data.spotCount, kMaxDxSpots - 1);
  for (uint8_t i = last; i > 0; --i) {
    g_data.spots[i] = g_data.spots[i - 1];
  }
  g_data.spots[0] = spot;
  if (g_data.spotCount < kMaxDxSpots) {
    ++g_data.spotCount;
  }
  g_data.hasData = true;
  g_data.source = "Telnet";
  g_data.provider = telnetProviderName();
  g_data.status = "Reading";
  g_data.updated = currentUtcDisplay(spot.time);
  Serial.print("DX Telnet spot: ");
  Serial.print(spot.freq);
  Serial.print(" ");
  Serial.print(spot.call);
  Serial.print(" ");
  Serial.println(spot.mode);
  return true;
}

bool handleDxTelnetLine(const String& line) {
#if DEBUG_DX_TELNET
  Serial.print("DX Telnet line: ");
  Serial.println(line);
#endif

  String lower = line;
  lower.toLowerCase();
  if (!g_telnetLoggedIn &&
      (lower.indexOf("login:") >= 0 || lower.indexOf("callsign:") >= 0 ||
       lower.indexOf("call:") >= 0 || lower.indexOf("please enter your call") >= 0)) {
    sendDxTelnetLoginIfNeeded();
    return true;
  }

  DxSpot spot;
  const bool isSpotLine = parseDxClusterSpotLine(line, spot);
  if (isSpotLine && dxModeIsEnabled(spot.mode)) {
    // The watchlist must see every spot on the stream, including ones the
    // display list drops as duplicates, so note it before adding.
    const bool watchChanged = dxWatchNoteSpot(spot);
    const bool listChanged = addDxSpotToList(spot);
    return listChanged || watchChanged;
  }

#if DEBUG_DX_TELNET
  if (isSpotLine) {
    Serial.print("DX Telnet mode filtered: ");
    Serial.println(spot.mode);
  } else if (line.startsWith("DX de ")) {
    Serial.println("DX Telnet spot parse failed");
  }
#endif

  // A spot dropped by the mode filter still proves the stream is alive, so the
  // status settles to "Reading" either way.
  if (g_telnetLoggedIn && g_data.status != "Reading") {
    setTelnetStatus("Reading");
    return true;
  }
  return false;
}

void dxTelnetConnectTask(void*) {
  WiFiClient pendingClient;
  const bool connected = pendingClient.connect(g_telnetConnectRequest.host,
                                                g_telnetConnectRequest.port,
                                                kTelnetConnectTimeoutMs);

  bool cancelled = false;
  portENTER_CRITICAL(&g_telnetConnectMux);
  if (g_telnetConnectCancelRequested) {
    cancelled = true;
    g_telnetConnectState = kTelnetConnectCancelled;
  } else if (connected) {
    g_telnetClient = pendingClient;
    g_telnetConnectState = kTelnetConnectSucceeded;
  } else {
    g_telnetConnectState = kTelnetConnectFailed;
  }
  portEXIT_CRITICAL(&g_telnetConnectMux);
  if (cancelled) {
    pendingClient.stop();
  }
  vTaskDelete(nullptr);
}

bool connectDxTelnet() {
  const AppSettings& settings = getSettings();
  portENTER_CRITICAL(&g_telnetConnectMux);
  if (g_telnetConnectState == kTelnetConnectRunning) {
    portEXIT_CRITICAL(&g_telnetConnectMux);
    return false;
  }
  strlcpy(g_telnetConnectRequest.host, settings.dxTelnetHost.c_str(),
          sizeof(g_telnetConnectRequest.host));
  g_telnetConnectRequest.port = settings.dxTelnetPort;
  g_telnetConnectCancelRequested = false;
  g_telnetConnectState = kTelnetConnectRunning;
  portEXIT_CRITICAL(&g_telnetConnectMux);

  g_lastTelnetConnectAttemptMs = millis();
  setTelnetStatus("Connecting");
  Serial.print("DX Telnet connect attempt: ");
  Serial.print(settings.dxTelnetHost);
  Serial.print(":");
  Serial.println(settings.dxTelnetPort);

  if (xTaskCreate(dxTelnetConnectTask, "dx-telnet-connect", 4096, nullptr, 1, nullptr) != pdPASS) {
    portENTER_CRITICAL(&g_telnetConnectMux);
    g_telnetConnectState = kTelnetConnectIdle;
    portEXIT_CRITICAL(&g_telnetConnectMux);
    setTelnetStatus("Failed");
    Serial.println("DX Telnet connection task failed");
  }
  return true;
}

bool pollDxTelnetConnect() {
  TelnetConnectState state;
  portENTER_CRITICAL(&g_telnetConnectMux);
  state = g_telnetConnectState;
  if (state == kTelnetConnectSucceeded || state == kTelnetConnectFailed ||
      state == kTelnetConnectCancelled) {
    g_telnetConnectState = kTelnetConnectIdle;
  }
  portEXIT_CRITICAL(&g_telnetConnectMux);

  if (state == kTelnetConnectSucceeded) {
    g_telnetConnected = true;
    g_telnetLoggedIn = false;
    g_telnetConnectedAtMs = millis();
    g_lastTelnetDataMs = g_telnetConnectedAtMs;
    g_telnetLineBuffer = "";
    g_telnetLineOverflow = false;
    setTelnetStatus("Connected");
    Serial.println("DX Telnet connected");
    return true;
  }
  if (state == kTelnetConnectFailed) {
    g_telnetConnected = false;
    setTelnetStatus("Failed");
    Serial.println("DX Telnet connection failed");
    return true;
  }
  return false;
}

bool loopDxTelnet() {
  bool changed = pollDxTelnetConnect();
  const uint32_t nowMs = millis();

  if (g_telnetConnected && !g_telnetClient.connected()) {
    stopDxTelnet(false);
    g_telnetHasCurrentSpots = false;
    g_lastTelnetConnectAttemptMs = nowMs;
    setTelnetStatus("Disconnected");
    changed = true;
  }

  if (!g_telnetConnected) {
    if (g_lastTelnetConnectAttemptMs == 0 ||
        nowMs - g_lastTelnetConnectAttemptMs >= kTelnetReconnectIntervalMs) {
      changed |= connectDxTelnet();
    }
    return changed;
  }

  size_t bytesRead = 0;
  while (g_telnetClient.available() > 0 && bytesRead < kMaxTelnetBytesPerLoop) {
    const int incoming = g_telnetClient.read();
    if (incoming < 0) {
      break;
    }
    ++bytesRead;
    g_lastTelnetDataMs = nowMs;
    const char c = static_cast<char>(incoming);
    if (c == '\n') {
      if (!g_telnetLineOverflow && g_telnetLineBuffer.length() > 0) {
        changed |= handleDxTelnetLine(g_telnetLineBuffer);
      }
      g_telnetLineBuffer = "";
      g_telnetLineOverflow = false;
    } else if (c != '\r' && static_cast<uint8_t>(c) >= 32 && static_cast<uint8_t>(c) < 127) {
      if (g_telnetLineBuffer.length() < kMaxTelnetLineChars) {
        g_telnetLineBuffer += c;
      } else {
        g_telnetLineOverflow = true;
      }
    }
  }

  if (!g_telnetLoggedIn && nowMs - g_telnetConnectedAtMs >= kTelnetLoginDelayMs) {
    sendDxTelnetLoginIfNeeded();
    changed = true;
  }
  return changed;
}

void markDxFailure(const String& status, const String& attemptedSource,
                   const String& attemptedProvider) {
  g_data.status = status;
  g_data.source = g_data.hasData ? String("Last good") : attemptedSource;
  if (!g_data.hasData) {
    g_data.provider = attemptedProvider;
  }
}

// Returns true when the spot was kept. A rejected spot is invisible to the
// dashboard, so the mode filter is applied before the watchlist sees it.
bool appendJsonSpot(JsonObject spotObject, DxSpotsData& parsed) {
  if (parsed.spotCount >= kMaxDxSpots) {
    return false;
  }

  DxSpot spot;
  spot.time = formatSpotTime(spotObject["spot_time"].as<String>());
  spot.freq = formatFrequency(spotObject["frequency"].as<String>());
  spot.call = valueOrDash(spotObject["spotted"].as<String>());
  spot.spotter = valueOrDash(spotObject["spotter"].as<String>());
  spot.comment = valueOrDash(spotObject["spotter_comment"].as<String>());
  spot.band = valueOrDash(spotObject["band"].as<String>());
  spot.country = valueOrDash(spotObject["spotted_country"].as<String>());
  spot.continent = valueOrDash(spotObject["spotted_continent"].as<String>());
  spot.mode = deriveMode(spot.freq, spot.comment);

  if (spot.freq == "--" || spot.call == "--" || !dxModeIsEnabled(spot.mode)) {
    return false;
  }

  if (parsed.updated == "") {
    parsed.updated = isoTimeToDisplay(spotObject["spot_datetime"].as<String>());
  }
  dxWatchNoteSpot(spot);
  parsed.spots[parsed.spotCount] = spot;
  ++parsed.spotCount;
  return true;
}

enum DxJsonResult : uint8_t {
  kDxJsonOk,
  // Spots were found, but the scan stopped before the end of the feed with the
  // list still unfilled, so a wider scan might have found more. Carries data.
  kDxJsonPartial,
  // The feed read cleanly, but the mode filter kept none of it. That is a
  // filter that matched nothing, not a broken feed, and it reads differently.
  kDxJsonNoMatch,
  kDxJsonFailed
};

// Why a scan stopped. Kept apart from the outcome because the same outcome can
// be reached several ways and the log line is the only place that distinction
// survives.
enum DxJsonEnd : uint8_t {
  kDxEndArray,         // saw the closing ']' — the whole feed was read
  kDxEndPageFull,      // eight wanted spots found; the rest of the feed is surplus
  kDxEndObjectCap,     // read kMaxDxObjectsScanned objects without filling the page
  kDxEndBudget,        // ran out of wall-clock
  kDxEndDisconnected   // the feed stopped arriving before it ended
};

// One object at a time, so memory stays flat regardless of feed size.
void handleJsonObject(const char* json, size_t length) {
  g_jsonDoc.clear();
  if (!deserializeJson(g_jsonDoc, json, length) && g_jsonDoc.is<JsonObject>()) {
    appendJsonSpot(g_jsonDoc.as<JsonObject>(), g_jsonParsed);
  }
}

void stopDxJsonStream() {
  if (g_jsonStreaming) {
    g_jsonHttp.end();
  }
  g_jsonStreaming = false;
  g_jsonStream = nullptr;
}

// Everything that used to be decided from fetchDxSpots()'s return value. The
// answer now arrives several loop iterations after the request, so the reason
// the scan was started has to be replayed from the captured context rather
// than read off the stack.
bool completeDxJsonScan(bool success) {
  if (!g_jsonAutoMode) {
    return true;
  }
  if (success) {
    g_autoUsingTelnet = false;
    g_telnetHasCurrentSpots = false;
    stopDxTelnet(true);
    return true;
  }
  // Auto mode reads a failure as "JSON had nothing usable" and moves to
  // Telnet, which is the right answer for an empty filter match too: the
  // stream will find the wanted modes eventually.
  g_autoUsingTelnet = true;
  if (g_jsonReconnectFallback) {
    stopDxTelnet(true);
    g_telnetHasCurrentSpots = false;
  } else if (g_jsonTelnetWasActive) {
    setTelnetStatus("Reading");
  }
  return true;
}

// Turn a finished scan into a status, a source decision and, when it found
// anything, the spot list the dashboard renders.
bool finishDxJsonScan(DxJsonEnd end) {
  stopDxJsonStream();

  const bool reachedEnd = end == kDxEndArray;
  const bool listFull = g_jsonParsed.spotCount >= kMaxDxSpots;
  // A short scan only costs something when the list is still unfilled: stopping
  // early on a full page is the design, not a shortfall.
  const bool truncated = !reachedEnd && !listFull;

  Serial.print("DX JSON scan ended: ");
  Serial.print(end == kDxEndArray          ? "end of feed"
               : end == kDxEndPageFull     ? "page full"
               : end == kDxEndObjectCap    ? "object cap"
               : end == kDxEndDisconnected ? "connection closed"
                                           : "budget");
  Serial.print(", ");
  Serial.print(g_jsonParsed.spotCount);
  Serial.print(" spots of ");
  Serial.print(g_jsonObjectsScanned);
  Serial.print(" objects scanned in ");
  Serial.print(millis() - g_jsonStartedMs);
  Serial.println(" ms");

  g_jsonParsed.source = "JSON";
  g_jsonParsed.provider = jsonProviderName();

  DxJsonResult result;
  if (g_jsonParsed.spotCount == 0) {
    // Four ways to end up with nothing, and only one of them is the filter's
    // doing. Blaming the filter for any of the others sends someone to the mode
    // list to fix a problem that is not there — so the feed has to have carried
    // at least one spot before the filter can be held responsible for the page
    // being empty.
    if (truncated) {
      g_jsonParsed.status = "Feed cut short";       // never finished reading it
      result = kDxJsonFailed;
    } else if (g_jsonObjectsScanned == 0) {
      g_jsonParsed.status = "Empty feed";           // well-formed, and holds nothing
      result = kDxJsonFailed;
    } else if (dxModeFilterIsActive()) {
      g_jsonParsed.status = "No matching modes";    // spots arrived; none were wanted
      result = kDxJsonNoMatch;
    } else {
      g_jsonParsed.status = "Parse failed";         // spots arrived; none were usable
      result = kDxJsonFailed;
    }
  } else {
    g_jsonParsed.updated = valueOrDash(g_jsonParsed.updated);
    g_jsonParsed.hasData = true;
    // "Partial" reads amber on the DX page, so a page that is short because the
    // scan ran out of time or objects says so, rather than looking like a quiet
    // band.
    g_jsonParsed.status = truncated ? "Partial" : "OK";
    result = truncated ? kDxJsonPartial : kDxJsonOk;
  }

  // A partial read still carries spots, so it is shown rather than discarded;
  // its status says the page may be short.
  const bool success = result == kDxJsonOk || result == kDxJsonPartial;
  if (success) {
    g_data = g_jsonParsed;
    Serial.print("DX status message: ");
    Serial.println(g_data.status);
  } else {
    // The scan set the wording, since it is the only thing that knows which way
    // it ended.
    markDxFailure(g_jsonParsed.status, "JSON", jsonProviderName());
  }
  g_jsonParsed = DxSpotsData();
  return completeDxJsonScan(success);
}

// Reads at most kMaxDxJsonBytesPerLoop, so the dashboard keeps rendering while
// a feed the mode filter rejects most of is scanned. Returns true when the
// display has something new to show, which for a scan still in flight is
// never.
bool pumpDxJsonScan() {
  size_t bytesRead = 0;

  while (bytesRead < kMaxDxJsonBytesPerLoop && g_jsonStream != nullptr &&
         g_jsonStream->available() > 0) {
    const int incoming = g_jsonStream->read();
    if (incoming < 0) {
      break;
    }
    ++bytesRead;

    const auto event = g_jsonScanner.feed(static_cast<char>(incoming));
    if (event == g_jsonScanner.kArrayEnd) {
      return finishDxJsonScan(kDxEndArray);
    }
    if (event != g_jsonScanner.kObjectReady) {
      continue;
    }

    ++g_jsonObjectsScanned;
    if (!g_jsonScanner.objectOverflowed()) {
      handleJsonObject(g_jsonScanner.object(), g_jsonScanner.objectLength());
    }
    if (g_jsonParsed.spotCount >= kMaxDxSpots) {
      return finishDxJsonScan(kDxEndPageFull);
    }
    if (g_jsonObjectsScanned >= kMaxDxObjectsScanned) {
      return finishDxJsonScan(kDxEndObjectCap);
    }
  }

  // Checked after the read, so a feed that arrives complete inside one slice is
  // finished by the ']' above rather than being called disconnected.
  const bool disconnected = g_jsonStream != nullptr && g_jsonStream->available() == 0 &&
                            !g_jsonPlainClient.connected() && !g_jsonSecureClient.connected();
  if (disconnected) {
    return finishDxJsonScan(kDxEndDisconnected);
  }
  if (millis() - g_jsonStartedMs >= kDxJsonScanBudgetMs) {
    return finishDxJsonScan(kDxEndBudget);
  }
  return false;
}

// Opens the feed and hands the reading to pumpDxJsonScan. Returns false when
// no scan is running afterwards, so the caller can settle the source decision
// straight away.
bool startDxJsonScan(bool autoMode, bool reconnectFallback, bool telnetWasActive) {
  g_jsonAutoMode = autoMode;
  g_jsonReconnectFallback = reconnectFallback;
  g_jsonTelnetWasActive = telnetWasActive;

  // Two HTTP streams at once is more than the radio and the heap handle
  // comfortably, so yield to an in-flight backfill.
  if (dxBackfillIsStreaming()) {
    Serial.println("DX fetch deferred: backfill streaming");
    return false;
  }

  const String url = getDxSpotsUrl();
  if (url.length() == 0) {
    Serial.println("DX fetch skipped: DX URL not set");
    markDxFailure("DX URL not set", "JSON", jsonProviderName());
    return false;
  }

  Serial.println("DX fetch start");
  Serial.print("DX URL used: ");
  Serial.println(url);

  if (!beginHttp(url, g_jsonHttp, g_jsonPlainClient, g_jsonSecureClient)) {
    Serial.println("DX fetch failure reason: http.begin");
    markDxFailure("Fetch failed", "JSON", jsonProviderName());
    return false;
  }

  g_jsonHttp.setTimeout(kHttpTimeoutMs);
  g_jsonHttp.setConnectTimeout(kHttpTimeoutMs);
  g_jsonHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int httpCode = g_jsonHttp.GET();
  Serial.print("DX HTTP status code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    g_jsonHttp.end();
    Serial.println("DX fetch failure reason: HTTP status");
    markDxFailure("Fetch failed", "JSON", jsonProviderName());
    return false;
  }

  g_jsonScanner.reset();
  g_jsonParsed = DxSpotsData();
  g_jsonObjectsScanned = 0;
  g_jsonStream = &g_jsonHttp.getStream();
  g_jsonStreaming = true;
  g_jsonStartedMs = millis();
  return true;
}
}

void dxSpotsBegin() {
  g_data = DxSpotsData();
  g_data.status = "Waiting";
  g_data.updated = "--";
  g_data.source = "--";
  g_data.provider = "--";
  g_telnetLineBuffer.reserve(kMaxTelnetLineChars);
  g_lastSourceMode = 0xFF;
  g_autoUsingTelnet = false;
  g_telnetHasCurrentSpots = false;
  stopDxTelnet(true);
  stopDxJsonStream();
  g_refreshRequested = true;
  dxWatchBegin();
}

void dxSpotsPrepareForOta() {
  stopDxTelnet(true);
  // The JSON scan now holds a socket of its own between iterations, so it has
  // to be dropped for the flash as well.
  stopDxJsonStream();
  // stopDxTelnet only *requests* cancellation of an in-flight connect: the
  // task is sitting in connect() and owns both a socket and its own stack.
  // Wait for it to notice, rather than starting a flash alongside it. The
  // bound is its own connect timeout plus a margin, so this cannot hang.
  const uint32_t deadlineMs = millis() + kTelnetConnectTimeoutMs + 1000;
  while (g_telnetConnectState == kTelnetConnectRunning &&
         static_cast<int32_t>(millis() - deadlineMs) < 0) {
    delay(20);
  }
  // The attempt may have succeeded during that wait and handed us a live
  // client, so tear down once more now that nothing is in flight.
  stopDxTelnet(true);
  g_telnetHasCurrentSpots = false;
  Serial.println("DX Telnet released for firmware update");
}

bool dxJsonScanIsStreaming() {
  return g_jsonStreaming;
}

String dxFormatFrequency(const String& value) {
  return formatFrequency(value);
}

String dxDeriveMode(const String& freq, const String& comment) {
  return deriveMode(freq, comment);
}

const DxModeOption& dxModeOption(uint8_t index) {
  return kDxModeOptions[index < kDxModeOptionCount ? index : kDxModeOptionCount - 1];
}

uint16_t dxModeBitForName(const String& name) {
  for (uint8_t i = 0; i < kDxModeOptionCount; ++i) {
    if (name.equalsIgnoreCase(kDxModeOptions[i].name)) {
      return kDxModeOptions[i].bit;
    }
  }
  return 0;
}

bool dxModeIsEnabled(const String& mode) {
  const uint16_t mask = enabledModeMask();
  if (mask == 0 || mask == kDxModeAll) {
    return true;
  }
  // Anything the mode derivation could not name is treated as unknown rather
  // than quietly disappearing.
  const uint16_t bit = dxModeBitForName(mode);
  return (mask & (bit != 0 ? bit : static_cast<uint16_t>(kDxModeUnknown))) != 0;
}

bool dxModeFilterIsActive() {
  const uint16_t mask = enabledModeMask();
  return mask != 0 && mask != kDxModeAll;
}

String dxModeFilterSummary() {
  if (!dxModeFilterIsActive()) {
    return "";
  }

  const uint16_t mask = enabledModeMask();
  uint8_t selected = 0;
  for (uint8_t i = 0; i < kDxModeOptionCount; ++i) {
    if ((mask & kDxModeOptions[i].bit) != 0) {
      ++selected;
    }
  }
  // Both pages append this to a status line that is already close to the panel
  // width, so the named form is only used while it stays short.
  if (selected > 2) {
    return String(selected) + " modes";
  }

  String summary;
  for (uint8_t i = 0; i < kDxModeOptionCount; ++i) {
    if ((mask & kDxModeOptions[i].bit) == 0) {
      continue;
    }
    if (summary.length() > 0) {
      summary += '/';
    }
    summary += kDxModeOptions[i].label;
  }
  return summary;
}

String getDxSpotsUrl() {
  String url = getSettings().dxSpotsUrl;
  url.trim();
  if (url.length() == 0) {
    url = DEFAULT_DX_SPOTS_URL;
    url.trim();
  }
  return url;
}

bool refreshDxSpotsIfNeeded(bool wifiConnected) {
  const uint32_t nowMs = millis();
  const uint32_t intervalMs = refreshIntervalMs();
  const bool due = g_lastAttemptMs == 0 || nowMs - g_lastAttemptMs >= intervalMs;
  const DxSourceMode mode = getSettings().dxSourceMode;
  bool changed = false;

  if (g_lastSourceMode != static_cast<uint8_t>(mode)) {
    stopDxTelnet(true);
    stopDxJsonStream();
    g_telnetHasCurrentSpots = false;
    g_autoUsingTelnet = false;
    g_lastAttemptMs = 0;
    g_refreshRequested = true;
    g_lastSourceMode = static_cast<uint8_t>(mode);
    changed = true;
  }

  if (!wifiConnected) {
    stopDxTelnet(true);
    stopDxJsonStream();
    g_telnetHasCurrentSpots = false;
    g_lastAttemptMs = nowMs - intervalMs + 5000UL;
    g_refreshRequested = false;
    const bool telnetOnly = mode == kDxSourceTelnet;
    markDxFailure("WiFi offline", telnetOnly ? String("Telnet") : String("JSON"),
                  telnetOnly ? telnetProviderName() : jsonProviderName());
    return true;
  }

  if (mode == kDxSourceJson) {
    stopDxTelnet(false);
    if (g_jsonStreaming) {
      // A refresh request means the settings changed under the scan, so its
      // results would be stale on arrival. Abandon it and start again below.
      if (!g_refreshRequested) {
        return pumpDxJsonScan() || changed;
      }
      stopDxJsonStream();
    }
    if (!g_refreshRequested && !due) {
      return changed;
    }
    g_lastAttemptMs = nowMs;
    g_refreshRequested = false;
    if (!startDxJsonScan(false, false, false)) {
      changed |= completeDxJsonScan(false);
    }
    return changed;
  }

  if (mode == kDxSourceTelnet) {
    if (g_refreshRequested) {
      stopDxTelnet(true);
      g_telnetHasCurrentSpots = false;
      setTelnetStatus("Disconnected");
      g_refreshRequested = false;
      changed = true;
    }
    return loopDxTelnet() || changed;
  }

  if (g_jsonStreaming) {
    if (g_refreshRequested) {
      stopDxJsonStream();   // stale before it finishes; restarted below
    } else {
      changed |= pumpDxJsonScan();
      // Telnet keeps running underneath an in-flight scan while it is the
      // active source, exactly as it did while the old fetch blocked.
      if (g_autoUsingTelnet) {
        changed |= loopDxTelnet();
      }
      return changed;
    }
  }

  if (g_refreshRequested || due) {
    const bool reconnectFallback = g_refreshRequested;
    const bool telnetWasActive = g_autoUsingTelnet && g_telnetConnected;
    g_lastAttemptMs = nowMs;
    g_refreshRequested = false;
    // The source decision now waits for finishDxJsonScan; only a scan that
    // never started is settled here.
    if (!startDxJsonScan(true, reconnectFallback, telnetWasActive)) {
      changed |= completeDxJsonScan(false);
    }
  }

  if (g_autoUsingTelnet) {
    changed |= loopDxTelnet();
  }
  return changed;
}

void requestDxSpotsRefresh() {
  g_refreshRequested = true;

  // Saving a tighter mode filter must not leave rows on screen that the filter
  // now rejects, so drop them here rather than waiting for the next spot. The
  // watchlist gets the same treatment: its rows are just as stale, and left
  // alone one could sit there "active" for the whole hold window on a mode the
  // filter would no longer record.
  dxWatchDropFilteredModes();

  uint8_t kept = 0;
  for (uint8_t i = 0; i < g_data.spotCount; ++i) {
    if (!dxModeIsEnabled(g_data.spots[i].mode)) {
      continue;
    }
    if (kept != i) {
      g_data.spots[kept] = g_data.spots[i];
    }
    ++kept;
  }
  for (uint8_t i = kept; i < g_data.spotCount; ++i) {
    g_data.spots[i] = DxSpot();
  }
  g_data.spotCount = kept;
  g_data.hasData = kept > 0;
}

const DxSpotsData& getDxSpotsData() {
  return g_data;
}
