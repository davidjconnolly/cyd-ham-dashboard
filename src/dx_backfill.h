#pragma once

#include <Arduino.h>

// Seeds the watchlist from recent spot history.
//
// DXSummit's API has no per-callsign filter, only a row limit, so the only way
// to learn that a watched station was spotted an hour ago is to pull a wide
// recent window and filter it here. The response is far too large to buffer on
// an ESP32, so it is streamed and matched object by object, and read in bounded
// chunks across loop iterations to keep touch and rendering responsive.
void dxBackfillBegin();

// Returns true when watchlist state changed and the display should redraw.
bool dxBackfillIfNeeded(bool wifiConnected);

// True while a window is still downloading. The JSON spot parser needs a large
// contiguous allocation, so it stands down until the stream is finished.
bool dxBackfillIsStreaming();

String getDxBackfillUrl();
String getDxBackfillStatus();
