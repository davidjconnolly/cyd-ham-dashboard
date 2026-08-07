#include <Arduino.h>

#include "connectivity.h"
#include "dashboard_display.h"
#include "ota.h"
#include "reset_button.h"
#include "settings.h"
#include "setup_portal.h"

namespace {
constexpr uint32_t kLoopDelayMs = 10;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  settingsBegin();
  displayBegin();
  resetButtonBegin();
  otaBegin();
  setupPortalBegin();
  connectivityBegin();
  displayUpdate(getClockSnapshot());
}

void loop() {
  setupPortalLoop();
  connectivityLoop();
  const ClockSnapshot snapshot = getClockSnapshot();
  // After the portal, so a queued check or install runs with its HTTP response
  // already sent. An install from here never returns: it reboots.
  otaLoop(snapshot.wifiConnected);
  const bool resettingNow = resetButtonLoop();
  if (!resettingNow) {
    displayUpdate(snapshot);
  }
  delay(kLoopDelayMs);
}
