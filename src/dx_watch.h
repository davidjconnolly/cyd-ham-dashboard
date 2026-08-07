#pragma once

#include <Arduino.h>

#include "dx_spots.h"

// Watched callsigns are matched against every spot that arrives from either DX
// source, so the dashboard can announce a DXpedition the moment it is spotted
// rather than waiting for the operator to read the spot list.
constexpr uint8_t kMaxWatchEntries = 8;

struct DxWatchEntry {
  String pattern;
  String call;
  String freq;
  String mode;
  String spotter;
  String spotTime;
  String signature;
  bool heard = false;
  // When the spot's own timestamp is known, age is measured from it. Live
  // Telnet spots without a usable clock fall back to the millis stamp, which
  // is the same moment in practice.
  time_t spotEpoch = 0;
  uint32_t heardAtMs = 0;
  bool fromHistory = false;
  uint16_t hitCount = 0;
};

void dxWatchBegin();
void dxWatchReloadPatterns();

// Forgets any row whose heard spot is on a mode the filter now excludes, so a
// tightened filter clears the rows it would never have recorded. Returns true
// when something was dropped. The pattern survives, unheard, and the next
// backfill can refill it from a mode that is still wanted.
bool dxWatchDropFilteredModes();

// Returns true when the spot changed watchlist state and the display should
// redraw. Historical spots recovered from a cluster backfill seed the rows
// without raising an alert, and carry the spot's own timestamp so a six-hour-old
// spot does not read as "now".
bool dxWatchNoteSpot(const DxSpot& spot, bool historical = false, time_t spotEpoch = 0);

// Backfill is requested at start-up and whenever the list changes, so adding a
// callsign answers immediately instead of staying blank until its next
// appearance on the live stream.
void dxWatchRequestBackfill();
bool dxWatchBackfillRequested();
void dxWatchClearBackfillRequest();

uint8_t dxWatchCount();
const DxWatchEntry& dxWatchEntry(uint8_t index);

// True when the call matches any watched pattern, so the DX Spots page can
// pick a watched station out of the list.
bool dxWatchMatchesCall(const String& call);
bool dxWatchEntryIsActive(const DxWatchEntry& entry);
uint32_t dxWatchMinutesSinceHeard(const DxWatchEntry& entry);
String dxWatchAgeText(const DxWatchEntry& entry);

// Hands over a pending alert exactly once, so a hit cannot be announced twice.
bool dxWatchTakeAlert(String& call);
