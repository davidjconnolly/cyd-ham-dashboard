#pragma once

#include <Arduino.h>

#include "connectivity.h"

void displayBegin();
void displayUpdate(const ClockSnapshot& snapshot);
void applyDisplaySettings();
uint8_t getCurrentDashboardPageNumber();
uint8_t getDashboardPageCount();
void displayShowMessage(const String& title, const String& subtitle);

// Firmware update screen. displayBeginOtaScreen paints the labels and an empty
// progress track; displayUpdateOtaProgress fills the bar and rewrites the line
// under it. Call requestDisplayRedraw afterwards — these draw over the whole
// panel and leave every cached field string stale.
void displayBeginOtaScreen(const String& title, const String& subtitle);
void displayUpdateOtaProgress(uint8_t percent, const String& detail);

void requestDisplayRedraw();
