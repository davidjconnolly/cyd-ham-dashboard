#pragma once

#include <Arduino.h>

constexpr uint8_t kMaxDxSpots = 8;

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
void requestDxSpotsRefresh();
const DxSpotsData& getDxSpotsData();
String getDxSpotsUrl();

// Shared spot formatting, so the backfill renders frequencies and modes
// identically to live spots.
String dxFormatFrequency(const String& value);
String dxDeriveMode(const String& freq, const String& comment);
