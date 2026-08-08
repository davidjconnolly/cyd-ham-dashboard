#include "dx_watch.h"

#include <time.h>

#include "dx_call_match.h"
#include "settings.h"

namespace {
// 2024-01-01, matching the guard dx_spots uses to decide the clock is real.
constexpr time_t kMinValidEpoch = 1704067200;

DxWatchEntry g_entries[kMaxWatchEntries];
// Reloading has to carry heard state across for patterns that survived the
// edit. The scratch copy lives here rather than on the stack because reloads
// run from the web server handler on the main loop task.
DxWatchEntry g_scratch[kMaxWatchEntries];
uint8_t g_count = 0;
bool g_alertPending = false;
bool g_backfillRequested = false;
String g_alertCall;
const DxWatchEntry kEmptyEntry;

bool isSeparator(char c) {
  return c == ',' || c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ';';
}

// A watched call is spotted in several shapes: bare (3Y0J), with a suffix
// (3Y0J/MM), or with an operating prefix (FT4/3Y0J). The rules, and why the
// whole call has to be compared as well as its components, live in
// src/dx_call_match.h where a host test can reach them.
bool patternMatches(const String& pattern, const String& call) {
  return dxCallMatchesPattern(pattern.c_str(), call.c_str());
}

bool alreadyListed(const String& pattern) {
  for (uint8_t i = 0; i < g_count; ++i) {
    if (g_entries[i].pattern == pattern) {
      return true;
    }
  }
  return false;
}

void appendPattern(const String& pattern, uint8_t previousCount) {
  if (pattern.length() == 0 || g_count >= kMaxWatchEntries || alreadyListed(pattern)) {
    return;
  }

  DxWatchEntry& entry = g_entries[g_count];
  entry = DxWatchEntry();
  entry.pattern = pattern;
  for (uint8_t i = 0; i < previousCount; ++i) {
    if (g_scratch[i].pattern == pattern) {
      entry = g_scratch[i];
      break;
    }
  }
  ++g_count;
}

uint16_t holdMinutes() {
  return constrain(getSettings().dxWatchHoldMinutes, static_cast<uint16_t>(1),
                   static_cast<uint16_t>(720));
}
}

void dxWatchBegin() {
  g_count = 0;
  g_alertPending = false;
  g_alertCall = "";
  dxWatchReloadPatterns();
}

void dxWatchReloadPatterns() {
  const uint8_t previousCount = g_count;
  for (uint8_t i = 0; i < previousCount; ++i) {
    g_scratch[i] = g_entries[i];
  }

  g_count = 0;
  String list = getSettings().dxWatchList;
  list.toUpperCase();

  String token;
  for (size_t i = 0; i <= list.length(); ++i) {
    const char c = i < list.length() ? list[i] : ',';
    if (isSeparator(c)) {
      token.trim();
      appendPattern(token, previousCount);
      token = "";
    } else {
      token += c;
    }
  }

  for (uint8_t i = 0; i < previousCount; ++i) {
    g_scratch[i] = DxWatchEntry();
  }
  for (uint8_t i = g_count; i < kMaxWatchEntries; ++i) {
    g_entries[i] = DxWatchEntry();
  }

  Serial.print("DX watch patterns loaded: ");
  Serial.println(g_count);
}

bool dxWatchDropFilteredModes() {
  bool changed = false;
  for (uint8_t i = 0; i < g_count; ++i) {
    DxWatchEntry& entry = g_entries[i];
    if (!entry.heard || dxModeIsEnabled(entry.mode)) {
      continue;
    }

    // Keep the pattern and drop everything the excluded spot put there. The row
    // returns to "not heard", which is also what lets the backfill refill it —
    // history is blocked from overwriting an entry that still counts as heard.
    const String pattern = entry.pattern;
    entry = DxWatchEntry();
    entry.pattern = pattern;
    changed = true;
    Serial.print("DX watch cleared by mode filter: ");
    Serial.println(pattern);
  }
  return changed;
}

bool dxWatchNoteSpot(const DxSpot& spot, bool historical, time_t spotEpoch) {
  if (g_count == 0) {
    return false;
  }

  String call = spot.call;
  call.trim();
  call.toUpperCase();
  if (call.length() == 0 || call == "--") {
    return false;
  }

  bool changed = false;
  for (uint8_t i = 0; i < g_count; ++i) {
    DxWatchEntry& entry = g_entries[i];
    if (!patternMatches(entry.pattern, call)) {
      continue;
    }

    // A JSON poll re-reads the same head of the feed every few minutes, so an
    // identical spot must not read as a fresh appearance.
    const String signature = call + "|" + spot.freq + "|" + spot.time;
    if (entry.heard && entry.signature == signature) {
      continue;
    }

    // Backfill runs after the live stream is already flowing, so it must never
    // overwrite a newer live spot with an older historical one.
    if (historical && entry.heard && !entry.fromHistory) {
      continue;
    }
    if (historical && entry.heard && entry.spotEpoch > 0 && spotEpoch > 0 &&
        spotEpoch <= entry.spotEpoch) {
      continue;
    }

    const bool wasActive = dxWatchEntryIsActive(entry);
    entry.call = call;
    entry.freq = spot.freq;
    entry.mode = spot.mode;
    entry.spotter = spot.spotter;
    entry.spotTime = spot.time;
    entry.signature = signature;
    entry.heard = true;
    entry.spotEpoch = spotEpoch;
    entry.heardAtMs = millis();
    entry.fromHistory = historical;
    if (entry.hitCount < 0xFFFF) {
      ++entry.hitCount;
    }
    changed = true;

    // Recovered history is not news: a spot from six hours ago must not flash
    // the backlight as though the station just came on the air.
    if (!wasActive && !historical) {
      g_alertPending = true;
      g_alertCall = call;
    }
    Serial.print(historical ? "DX watch history: " : "DX watch hit: ");
    Serial.print(call);
    Serial.print(" ");
    Serial.print(entry.freq);
    Serial.print(" ");
    Serial.print(entry.mode);
    Serial.print(" age ");
    Serial.println(dxWatchAgeText(entry));
  }
  return changed;
}

void dxWatchRequestBackfill() {
  g_backfillRequested = true;
}

bool dxWatchBackfillRequested() {
  return g_backfillRequested && g_count > 0;
}

void dxWatchClearBackfillRequest() {
  g_backfillRequested = false;
}

uint8_t dxWatchCount() {
  return g_count;
}

const DxWatchEntry& dxWatchEntry(uint8_t index) {
  return index < g_count ? g_entries[index] : kEmptyEntry;
}

bool dxWatchMatchesCall(const String& call) {
  if (g_count == 0) {
    return false;
  }

  String upper = call;
  upper.trim();
  upper.toUpperCase();
  for (uint8_t i = 0; i < g_count; ++i) {
    if (patternMatches(g_entries[i].pattern, upper)) {
      return true;
    }
  }
  return false;
}

uint32_t dxWatchMinutesSinceHeard(const DxWatchEntry& entry) {
  if (!entry.heard) {
    return 0;
  }

  // Prefer the spot's own timestamp. Backfilled history is hours old by
  // definition, and even a live spot can be a few minutes stale by the time a
  // JSON poll reads it; the millis stamp only records when we saw it.
  if (entry.spotEpoch > 0) {
    const time_t now = time(nullptr);
    if (now >= kMinValidEpoch && now > entry.spotEpoch) {
      return static_cast<uint32_t>((now - entry.spotEpoch) / 60);
    }
  }
  return (millis() - entry.heardAtMs) / 60000UL;
}

bool dxWatchEntryIsActive(const DxWatchEntry& entry) {
  return entry.heard && dxWatchMinutesSinceHeard(entry) < holdMinutes();
}

String dxWatchAgeText(const DxWatchEntry& entry) {
  if (!entry.heard) {
    return "--";
  }

  const uint32_t minutes = dxWatchMinutesSinceHeard(entry);
  if (minutes < 1) {
    return "now";
  }
  if (minutes < 60) {
    return String(minutes) + "m";
  }
  const uint32_t hours = minutes / 60;
  if (hours < 24) {
    return String(hours) + "h";
  }
  return ">1d";
}

bool dxWatchTakeAlert(String& call) {
  if (!g_alertPending) {
    return false;
  }
  g_alertPending = false;
  call = g_alertCall;
  return true;
}
