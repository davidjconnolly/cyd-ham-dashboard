#include "dx_backfill.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "dx_spots.h"
#include "dx_watch.h"
#include "settings.h"

namespace {
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kStreamBudgetMs = 30000;
constexpr size_t kMaxBytesPerLoop = 2048;
constexpr size_t kMaxObjectChars = 1024;
constexpr uint16_t kMaxObjects = 1500;
constexpr time_t kMinValidEpoch = 1704067200;

HTTPClient g_http;
WiFiClient g_plainClient;
WiFiClientSecure g_secureClient;
Stream* g_stream = nullptr;
bool g_streaming = false;

uint32_t g_lastAttemptMs = 0;
uint32_t g_streamStartedMs = 0;
uint16_t g_objectsScanned = 0;
uint16_t g_matched = 0;

String g_objectBuffer;
bool g_foundArray = false;
bool g_inObject = false;
bool g_inString = false;
bool g_escaped = false;
bool g_objectOverflow = false;
int g_depth = 0;

String g_status = "Idle";

String valueOrDash(String value) {
  value.trim();
  if (value == "null" || value.length() == 0) {
    return "--";
  }
  return value;
}

uint32_t backfillIntervalMs() {
  const uint16_t minutes = constrain(getSettings().dxWatchBackfillMinutes,
                                     static_cast<uint16_t>(1), static_cast<uint16_t>(240));
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

// Howard Hinnant's days-from-civil. localtime/mktime would apply the station
// timezone, and spot timestamps are UTC.
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
}

// "2026-08-02T02:26:38" -> epoch seconds plus the HHMMZ form the UI shows.
bool parseIsoUtc(const String& iso, time_t& epoch, String& hhmmZ) {
  if (iso.length() < 16 || iso[4] != '-' || iso[7] != '-') {
    return false;
  }
  const int year = iso.substring(0, 4).toInt();
  const int month = iso.substring(5, 7).toInt();
  const int day = iso.substring(8, 10).toInt();
  const int hour = iso.substring(11, 13).toInt();
  const int minute = iso.substring(14, 16).toInt();
  const int second = iso.length() >= 19 ? iso.substring(17, 19).toInt() : 0;
  if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59) {
    return false;
  }

  epoch = static_cast<time_t>(daysFromCivil(year, month, day)) * 86400L +
          (hour * 3600L) + (minute * 60L) + second;
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%02d%02dZ", hour, minute);
  hhmmZ = String(buffer);
  return true;
}

void resetScanner() {
  g_objectBuffer = "";
  g_foundArray = false;
  g_inObject = false;
  g_inString = false;
  g_escaped = false;
  g_objectOverflow = false;
  g_depth = 0;
  g_objectsScanned = 0;
  g_matched = 0;
}

void stopStream() {
  if (g_streaming) {
    g_http.end();
  }
  g_streaming = false;
  g_stream = nullptr;
  g_objectBuffer = "";
}

// Each object is parsed on its own, so memory stays flat no matter how wide the
// requested window is.
bool handleObject(const String& json) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) {
    return false;
  }

  String call = doc["dx_call"].as<String>();
  call.trim();
  call.toUpperCase();
  if (call.length() == 0 || !dxWatchMatchesCall(call)) {
    return false;
  }

  time_t epoch = 0;
  String hhmmZ;
  const bool timeOk = parseIsoUtc(doc["time"].as<String>(), epoch, hhmmZ);

  DxSpot spot;
  spot.call = call;
  spot.freq = dxFormatFrequency(doc["frequency"].as<String>());
  spot.spotter = valueOrDash(doc["de_call"].as<String>());
  spot.comment = valueOrDash(doc["info"].as<String>());
  spot.time = timeOk ? hhmmZ : String("--");
  spot.mode = dxDeriveMode(spot.freq, spot.comment);
  spot.band = "--";
  spot.country = valueOrDash(doc["dx_country"].as<String>());
  spot.continent = "--";

  // History has to obey the same mode filter as the live sources, otherwise a
  // backfill would refill a watch row with a mode the user excluded.
  if (spot.freq == "--" || !dxModeIsEnabled(spot.mode)) {
    return false;
  }

  ++g_matched;
  return dxWatchNoteSpot(spot, true, timeOk ? epoch : 0);
}

bool startStream() {
  const String url = getDxBackfillUrl();
  if (url.length() == 0) {
    return false;
  }

  Serial.print("DX backfill start: ");
  Serial.println(url);

  const bool secure = url.startsWith("https://");
  if (secure) {
    g_secureClient.setInsecure();
  }
  if (!(secure ? g_http.begin(g_secureClient, url) : g_http.begin(g_plainClient, url))) {
    g_status = "Begin failed";
    Serial.println("DX backfill failure: http.begin");
    return false;
  }

  g_http.setTimeout(kConnectTimeoutMs);
  g_http.setConnectTimeout(kConnectTimeoutMs);
  g_http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int code = g_http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("DX backfill HTTP status: ");
    Serial.println(code);
    g_http.end();
    g_status = String("HTTP ") + code;
    return false;
  }

  resetScanner();
  g_stream = &g_http.getStream();
  g_streaming = true;
  g_streamStartedMs = millis();
  g_status = "Reading";
  return true;
}

// Returns true when watch state changed. Reads at most kMaxBytesPerLoop so the
// dashboard keeps rendering while a wide window downloads.
bool pumpStream() {
  bool changed = false;
  size_t bytesRead = 0;

  while (bytesRead < kMaxBytesPerLoop && g_stream != nullptr && g_stream->available() > 0) {
    const int incoming = g_stream->read();
    if (incoming < 0) {
      break;
    }
    ++bytesRead;
    const char c = static_cast<char>(incoming);

    if (!g_foundArray) {
      if (c == '[') {
        g_foundArray = true;
      }
      continue;
    }

    if (!g_inObject) {
      if (c == '{') {
        g_inObject = true;
        g_objectOverflow = false;
        g_depth = 1;
        g_objectBuffer = "{";
      } else if (c == ']') {
        g_status = String("OK, ") + g_matched + " of " + g_objectsScanned;
        Serial.print("DX backfill complete: matched ");
        Serial.print(g_matched);
        Serial.print(" of ");
        Serial.println(g_objectsScanned);
        stopStream();
        return changed;
      }
      continue;
    }

    if (g_objectBuffer.length() < kMaxObjectChars) {
      g_objectBuffer += c;
    } else {
      g_objectOverflow = true;
    }

    if (g_inString) {
      if (g_escaped) {
        g_escaped = false;
      } else if (c == '\\') {
        g_escaped = true;
      } else if (c == '"') {
        g_inString = false;
      }
      continue;
    }

    if (c == '"') {
      g_inString = true;
    } else if (c == '{') {
      ++g_depth;
    } else if (c == '}') {
      --g_depth;
      if (g_depth == 0) {
        g_inObject = false;
        ++g_objectsScanned;
        if (!g_objectOverflow) {
          changed |= handleObject(g_objectBuffer);
        }
        g_objectBuffer = "";
        if (g_objectsScanned >= kMaxObjects) {
          g_status = String("Capped, ") + g_matched + " of " + g_objectsScanned;
          Serial.println("DX backfill stopped at object cap");
          stopStream();
          return changed;
        }
      }
    }
  }

  const uint32_t nowMs = millis();
  const bool disconnected = g_stream != nullptr && g_stream->available() == 0 &&
                            !g_plainClient.connected() && !g_secureClient.connected();
  if (nowMs - g_streamStartedMs >= kStreamBudgetMs || disconnected) {
    g_status = String("Ended, ") + g_matched + " of " + g_objectsScanned;
    Serial.print("DX backfill ended early: matched ");
    Serial.print(g_matched);
    Serial.print(" of ");
    Serial.println(g_objectsScanned);
    stopStream();
  }
  return changed;
}
}

void dxBackfillBegin() {
  stopStream();
  g_lastAttemptMs = 0;
  g_status = "Idle";
  g_objectBuffer.reserve(kMaxObjectChars);
  dxWatchRequestBackfill();
}

bool dxBackfillIfNeeded(bool wifiConnected) {
  if (!wifiConnected) {
    if (g_streaming) {
      stopStream();
      g_status = "WiFi offline";
    }
    return false;
  }

  if (g_streaming) {
    return pumpStream();
  }

  if (dxWatchCount() == 0 || getDxBackfillUrl().length() == 0) {
    return false;
  }

  const uint32_t nowMs = millis();
  const bool requested = dxWatchBackfillRequested();
  const bool due = g_lastAttemptMs != 0 && nowMs - g_lastAttemptMs >= backfillIntervalMs();
  if (!requested && !due) {
    return false;
  }

  // The clock has to be right before historical ages mean anything.
  if (time(nullptr) < kMinValidEpoch) {
    g_status = "Waiting for NTP";
    return false;
  }

  dxWatchClearBackfillRequest();
  g_lastAttemptMs = nowMs;
  startStream();
  return false;
}

bool dxBackfillIsStreaming() {
  return g_streaming;
}

String getDxBackfillUrl() {
  String url = getSettings().dxWatchBackfillUrl;
  url.trim();
  return url;
}

String getDxBackfillStatus() {
  return g_status;
}
