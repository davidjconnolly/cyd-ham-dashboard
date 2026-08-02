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
  uint32_t heardAtMs = 0;
  uint16_t hitCount = 0;
};

void dxWatchBegin();
void dxWatchReloadPatterns();

// Returns true when the spot changed watchlist state and the display should redraw.
bool dxWatchNoteSpot(const DxSpot& spot);

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
