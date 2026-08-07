#pragma once

#include <Arduino.h>

#include "settings.h"

constexpr uint8_t kMaxDxSpots = 8;
// Every mode the dashboard can recognise, plus the catch-all for spots whose
// mode cannot be worked out.
constexpr uint8_t kDxModeOptionCount = 10;

struct DxModeOption {
  const char* name;   // As stored in DxSpot::mode.
  const char* label;  // As shown in the settings page.
  uint16_t bit;
};

struct DxSpot {
  String time;
  String freq;
  String call;
  String mode;
  String spotter;
  String comment;
  String band;
  String country;
  String continent;
};

struct DxSpotsData {
  bool hasData;
  String status;
  String updated;
  String source;
  String provider;
  uint8_t spotCount;
  DxSpot spots[kMaxDxSpots];
};

void dxSpotsBegin();
bool refreshDxSpotsIfNeeded(bool wifiConnected);

// Drop the persistent Telnet connection and wait out any in-flight connect
// attempt, so an OTA flash gets the socket and the heap. Safe to call when
// nothing is connected. If the flash fails the normal reconnect logic takes
// over again on the next loop.
void dxSpotsPrepareForOta();

void requestDxSpotsRefresh();
const DxSpotsData& getDxSpotsData();
String getDxSpotsUrl();

// Shared spot formatting, so the backfill renders frequencies and modes
// identically to live spots.
String dxFormatFrequency(const String& value);
String dxDeriveMode(const String& freq, const String& comment);

// Mode filtering. Every source drops a spot the filter rejects before it
// reaches the spot list or the watchlist.
const DxModeOption& dxModeOption(uint8_t index);
uint16_t dxModeBitForName(const String& name);
bool dxModeIsEnabled(const String& mode);
bool dxModeFilterIsActive();
// "FT8/CW" while one or two modes are selected, "4 modes" once the list gets
// longer than a status line can hold, and empty when nothing is filtered out.
String dxModeFilterSummary();
