#pragma once

#include <Arduino.h>

// Firmware version, stamped by CI from the release tag through
// PLATFORMIO_BUILD_FLAGS (see .github/workflows/build.yml). A local build has
// no stamp and reports "dev", which never equals a release tag, so a
// hand-flashed board always reports an update as available rather than quietly
// deciding it is already current.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// The one release asset this build may install, set per environment in
// platformio.ini. Flashing the other display variant's image leaves a garbled
// panel recoverable only over USB, so a missing flag is a build failure rather
// than a default: a new environment must state which asset it owns.
#ifndef OTA_ASSET_NAME
#error "OTA_ASSET_NAME must be defined per environment in platformio.ini"
#endif

// This repository. Public, and named here deliberately: the device queries its
// releases over the unauthenticated GitHub API.
#ifndef OTA_REPO
#define OTA_REPO "davidjconnolly/cyd-ham-dashboard"
#endif

struct OtaStatus {
  bool checking;            // a check is queued or running
  bool installing;          // a download/flash is in progress
  bool updateAvailable;     // a matching asset newer than this build was found
  bool everChecked;         // false until the first check completes
  uint8_t progressPercent;  // 0-100 while installing
  int lastCheckHttpCode;    // HTTP status of the last releases API call, 0 if none
  uint32_t lastCheckedAtMs; // millis() of the last completed check
  String message;           // human-readable outcome, shown on the web page
  String availableTag;      // tag of the found release, empty when none
};

void otaBegin();

// Runs queued work: a web-requested check, a web-requested install, and the
// periodic background check. Must be called from loop().
void otaLoop(bool wifiConnected);

// Queued by the web portal; the work happens in otaLoop so the HTTP response
// goes out first.
void otaRequestCheck();
void otaRequestInstall();

const OtaStatus& getOtaStatus();
const char* otaRunningVersion();
const char* otaAssetName();
