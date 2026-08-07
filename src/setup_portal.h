#pragma once

void setupPortalBegin();
void setupPortalLoop();

// Shut the web server, captive DNS, mDNS responder and setup hotspot down so a
// firmware flash has the sockets and the heap, then bring them back if the
// flash did not happen. Called only from src/ota.cpp.
void setupPortalPrepareForOta();
void setupPortalRestoreAfterOta();
