#pragma once

#include <Arduino.h>

enum DxSourceMode : uint8_t {
  kDxSourceJson = 0,
  kDxSourceTelnet = 1,
  kDxSourceAuto = 2
};

// Which spot modes the dashboard registers, stored as a bit mask in
// AppSettings::dxModeMask. The names behind these bits live in dx_spots.
enum DxModeBit : uint16_t {
  kDxModeFt8 = 1 << 0,
  kDxModeFt4 = 1 << 1,
  kDxModeCw = 1 << 2,
  kDxModeSsb = 1 << 3,
  kDxModeUsb = 1 << 4,
  kDxModeLsb = 1 << 5,
  kDxModeRtty = 1 << 6,
  kDxModeSstv = 1 << 7,
  kDxModePsk = 1 << 8,
  kDxModeUnknown = 1 << 9,
  kDxModeAll = 0x03FF
};

struct AppSettings {
  String wifiSsid;
  String wifiPassword;
  String callsign;
  String timezone;
  String timezoneLabel;
  String locator;
  bool useJsonPropagationProxy;
  String propagationJsonUrl;
  DxSourceMode dxSourceMode;
  uint16_t dxModeMask;
  String dxSpotsUrl;
  String dxTelnetHost;
  uint16_t dxTelnetPort;
  String dxWatchList;
  bool dxWatchAlertEnabled;
  bool dxWatchAutoPage;
  uint16_t dxWatchHoldMinutes;
  String dxWatchBackfillUrl;
  uint16_t dxWatchBackfillMinutes;
  uint16_t propagationRefreshMinutes;
  uint16_t dxRefreshMinutes;
  uint8_t brightnessPercent;
  bool swapRedBlueChannels;
  bool rotate90;
  bool flip180;
  bool keepHotspotOn;
  // Install a newer release without being asked. Opt-in and off by default:
  // an update reboots the device and takes the dashboard away for a minute.
  bool otaAutoUpdate;
};

void settingsBegin();
const AppSettings& getSettings();
void saveSettings(const AppSettings& settings);
bool hasWifiCredentials();
void factoryResetSettings();
