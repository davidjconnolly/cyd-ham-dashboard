#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>

#include "dashboard_display.h"
#include "dx_spots.h"
#include "settings.h"
#include "setup_portal.h"

namespace {

constexpr uint32_t kCheckIntervalMs = 6UL * 60UL * 60UL * 1000UL;
// Also the worst case for how long a check can hold the main loop, so it is
// kept near the 5 s the DX readers use rather than a generous OTA-sized value.
// It still has to cover a TLS handshake to GitHub over a slow link.
constexpr uint32_t kHttpTimeoutMs = 8000;
// A download that produces no bytes for this long is abandoned. Separate from
// the socket timeout above: a connection can stay open and simply stop
// delivering, which no socket timeout notices.
constexpr uint32_t kStallMs = 30000;
constexpr size_t kDownloadChunkBytes = 1024;

// The releases API response is tens of kilobytes, nearly all of it release
// notes. The filter keeps three fields, so the document below only ever holds
// the tag and the assets' names and URLs, and the rest is discarded as it
// streams past rather than buffered.
//
// Measured against a real 33 KB releases/latest payload carrying this repo's
// two assets: 577 bytes used of 2048, and the same document holds ten assets
// before it runs out. An overflow reports NoMemory and installs nothing, so
// the failure mode is a refused update rather than a wrong one.
constexpr size_t kFilterDocBytes = 256;
constexpr size_t kReleaseDocBytes = 2048;

OtaStatus g_status;
bool g_checkRequested = false;
bool g_installRequested = false;
uint32_t g_lastPeriodicCheckMs = 0;
String g_pendingUrl;

void setMessage(const String& message) {
  g_status.message = message;
  Serial.print(F("OTA: "));
  Serial.print(message);
  Serial.print(F(" (free heap "));
  Serial.print(ESP.getFreeHeap());
  Serial.println(')');
}

bool beginRequest(HTTPClient& http, WiFiClientSecure& client, const String& url) {
  client.setInsecure();
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("User-Agent", "cyd-ham-dashboard");
  return true;
}

// Query the latest release and report whether it carries an installable image
// for THIS build. Returns true only when the tag differs from the running
// version and the release contains an asset named exactly OTA_ASSET_NAME.
//
// Any difference counts as an update, not only a higher version: GitHub's
// "latest" moves backwards when a bad release is deleted, and following it
// down is how a fleet gets rolled back.
bool fetchLatestRelease(String& tagOut, String& urlOut) {
  const uint32_t startedAtMs = millis();
  const String apiUrl = String(F("https://api.github.com/repos/")) + OTA_REPO + F("/releases/latest");

  WiFiClientSecure client;
  HTTPClient http;
  // HTTP/1.0, because the parse below reads the socket directly and chunked
  // transfer framing would be fed straight into the JSON parser as garbage.
  http.useHTTP10(true);
  if (!beginRequest(http, client, apiUrl)) {
    setMessage(F("check failed: cannot open connection"));
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");

  const int code = http.GET();
  g_status.lastCheckHttpCode = code;
  if (code != HTTP_CODE_OK) {
    http.end();
    setMessage(String(F("check failed: HTTP ")) + code);
    return false;
  }

  StaticJsonDocument<kFilterDocBytes> filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;

  DynamicJsonDocument doc(kReleaseDocBytes);
  const DeserializationError error =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    setMessage(String(F("check failed: JSON ")) + error.c_str());
    return false;
  }

  String tag = doc["tag_name"].as<String>();
  if (tag.startsWith("v")) {
    tag = tag.substring(1);
  }
  if (tag.length() == 0) {
    setMessage(F("no published release found"));
    return false;
  }

  Serial.print(F("OTA: latest release v"));
  Serial.print(tag);
  Serial.print(F(", running v"));
  Serial.print(FIRMWARE_VERSION);
  Serial.print(F(", check took "));
  Serial.print(millis() - startedAtMs);
  Serial.println(F(" ms"));

  if (tag == FIRMWARE_VERSION) {
    setMessage(String(F("up to date (v")) + FIRMWARE_VERSION + ")");
    return false;
  }

  // Exact, whole-name match against this build's own asset. Never a suffix or
  // "first .bin found": the other environment's image is also a .bin, also
  // attached to this release, and flashing it bricks the panel until someone
  // reaches the board with a USB cable.
  String url;
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    const char* name = asset["name"] | "";
    if (strcmp(name, OTA_ASSET_NAME) == 0) {
      url = asset["browser_download_url"].as<String>();
      break;
    }
  }
  if (url.length() == 0) {
    setMessage(String(F("v")) + tag + F(" has no ") + OTA_ASSET_NAME);
    return false;
  }
  // Second check on the same constraint, against the URL rather than the JSON
  // field, so a release whose asset name and upload path disagree is refused
  // instead of downloaded.
  if (!url.endsWith(String('/') + OTA_ASSET_NAME)) {
    setMessage(String(F("v")) + tag + F(": asset URL does not end in ") + OTA_ASSET_NAME);
    return false;
  }

  tagOut = tag;
  urlOut = url;
  setMessage(String(F("v")) + tag + F(" available"));
  return true;
}

// Record a failed install everywhere it can be seen. The message matters as
// much as the log line: the check last set it to "vX available", and the web
// page reads only that — so without this a failed install leaves the portal
// still advertising an update that just fell over.
void installFailed(const String& message, const String& panelText) {
  g_status.installing = false;
  g_status.progressPercent = 0;
  setMessage(message);
  displayShowMessage(F("UPDATE FAILED"), panelText);
  delay(2500);
  // The panel caches the last string drawn per field, and this screen has
  // scribbled over all of them.
  requestDisplayRedraw();
  setupPortalRestoreAfterOta();
}

// Download `url` and write it to the inactive OTA slot, then reboot into it.
//
// This blocks the main loop for the whole download, which is the opposite of
// the rule everywhere else in this firmware (see CLAUDE.md and the bounded
// readers in dx_spots.cpp / dx_backfill.cpp). It is deliberate here: the
// device is being flashed and is about to reboot, there is nothing left worth
// servicing, and a resumable writer would have to keep a half-written flash
// slot valid across loop iterations. Every failure path below returns with the
// current firmware still running and still bootable — the boot target only
// moves on a successful Update.end(true).
void downloadAndFlash(const String& url) {
  g_status.installing = true;
  g_status.progressPercent = 0;
  Serial.print(F("OTA: installing from "));
  Serial.println(url);

  // Free the sockets and heap the TLS download needs. Both come back on
  // failure: the portal is restarted by installFailed(), and the Telnet
  // reconnect timer was reset so the cluster is rejoined on the next loop.
  dxSpotsPrepareForOta();
  setupPortalPrepareForOta();
  Serial.print(F("OTA: free heap after releasing Telnet and hotspot: "));
  Serial.println(ESP.getFreeHeap());

  displayBeginOtaScreen(F("UPDATING"), F("do not power off"));

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, url)) {
    installFailed(F("download failed: cannot open connection"), F("connection failed"));
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    installFailed(String(F("download failed: HTTP ")) + code, String(F("HTTP ")) + code);
    return;
  }

  // Update.begin() needs the image size up front, so a chunked or
  // unknown-length response cannot be flashed at all. GitHub's asset URLs
  // always send Content-Length; if a redirect ever lands somewhere that does
  // not, this says so instead of failing silently inside Update.begin(-1).
  const int totalBytes = http.getSize();
  if (totalBytes <= 0) {
    http.end();
    installFailed(String(F("download failed: no content-length (")) + totalBytes + ")",
                  F("no content length"));
    return;
  }
  Serial.print(F("OTA: image is "));
  Serial.print(totalBytes);
  Serial.println(F(" bytes"));

  if (!Update.begin(static_cast<size_t>(totalBytes))) {
    const String reason = Update.errorString();
    Update.abort();
    http.end();
    installFailed(String(F("flash failed to start: ")) + reason, reason);
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[kDownloadChunkBytes];
  int written = 0;
  int lastPercent = -1;
  uint32_t lastProgressAtMs = millis();

  while (written < totalBytes) {
    // Checked first and on every path through the loop. A connection that has
    // been drained and closed is over now; one that is still open but silent
    // gets kStallMs. Testing at the top means no sequence of empty reads can
    // slip past into an unbounded spin.
    const int available = stream->available();
    const bool closed = available <= 0 && !client.connected();
    if (closed || millis() - lastProgressAtMs > kStallMs) {
      const int percent = (written * 100) / totalBytes;
      Update.abort();
      http.end();
      installFailed(String(F("download stalled at ")) + percent + F("% (") + written + '/' +
                        totalBytes + F(" bytes)") + (closed ? F(", connection closed") : F("")),
                    String(F("stalled at ")) + percent + '%');
      return;
    }
    if (available <= 0) {
      delay(1);
      continue;
    }

    const int read = stream->readBytes(
        buffer, min(static_cast<int>(sizeof(buffer)), available));
    if (read <= 0) {
      delay(1);  // available() promised bytes and none came; let the stall timer run
      continue;
    }
    if (Update.write(buffer, read) != static_cast<size_t>(read)) {
      const String reason = Update.errorString();
      Update.abort();
      http.end();
      installFailed(String(F("flash write failed at ")) + written + '/' + totalBytes + F(": ") +
                        reason,
                    reason);
      return;
    }
    written += read;
    lastProgressAtMs = millis();  // only real bytes reset the stall timer
    yield();

    const int percent = (written * 100) / totalBytes;
    if (percent != lastPercent) {
      lastPercent = percent;
      g_status.progressPercent = static_cast<uint8_t>(percent);
      displayUpdateOtaProgress(static_cast<uint8_t>(percent),
                               String(written / 1024) + F(" / ") + (totalBytes / 1024) + F(" KB"));
      // Every 10% rather than every 1%: the serial log is the only record of a
      // flash that happened while nobody was watching the panel.
      if (percent % 10 == 0) {
        Serial.print(F("OTA: "));
        Serial.print(percent);
        Serial.print(F("% ("));
        Serial.print(written);
        Serial.print('/');
        Serial.print(totalBytes);
        Serial.println(F(" bytes)"));
      }
    }
  }
  http.end();

  if (!Update.end(true)) {
    const String reason = Update.errorString();
    installFailed(String(F("flash failed to finalise: ")) + reason, reason);
    return;
  }

  Serial.print(F("OTA: flashed "));
  Serial.print(written);
  Serial.println(F(" bytes, rebooting"));
  displayShowMessage(F("UPDATED"), F("restarting"));
  delay(2000);
  ESP.restart();
}

void runCheck(bool fromUser) {
  g_status.checking = true;
  Serial.print(F("OTA: checking "));
  Serial.print(OTA_REPO);
  Serial.print(F(" for a newer "));
  Serial.print(OTA_ASSET_NAME);
  Serial.println(fromUser ? F(" (requested)") : F(" (scheduled)"));

  String tag;
  String url;
  const bool available = fetchLatestRelease(tag, url);
  g_status.updateAvailable = available;
  g_status.availableTag = available ? tag : String("");
  g_pendingUrl = available ? url : String("");
  g_status.everChecked = true;
  g_status.lastCheckedAtMs = millis();
  g_status.checking = false;
}

}  // namespace

void otaBegin() {
  g_status.checking = false;
  g_status.installing = false;
  g_status.updateAvailable = false;
  g_status.everChecked = false;
  g_status.progressPercent = 0;
  g_status.lastCheckHttpCode = 0;
  g_status.lastCheckedAtMs = 0;
  g_status.message = F("not checked yet");

  Serial.print(F("OTA: running v"));
  Serial.print(FIRMWARE_VERSION);
  Serial.print(F(", asset "));
  Serial.print(OTA_ASSET_NAME);
  Serial.print(F(", repo "));
  Serial.println(OTA_REPO);
  if (String(FIRMWARE_VERSION) == "dev") {
    Serial.println(F("OTA: this is an unstamped local build; any release will look newer"));
  }
}

void otaLoop(bool wifiConnected) {
  if (!wifiConnected) {
    return;
  }

  if (g_installRequested) {
    g_installRequested = false;
    if (g_pendingUrl.length() > 0) {
      downloadAndFlash(g_pendingUrl);  // reboots on success
    } else {
      setMessage(F("no update to install; check first"));
    }
    return;
  }

  if (g_checkRequested) {
    g_checkRequested = false;
    g_lastPeriodicCheckMs = millis();
    runCheck(true);
    return;
  }

  // The periodic check is the second place in this firmware that blocks the
  // loop on a network round trip (a TLS handshake plus a filtered parse, on
  // the order of a second). It runs once every six hours, against the
  // once-per-refresh cost that issue #3 is about, and it is bounded by
  // kHttpTimeoutMs. Doing it on a task instead would mean a second
  // TLS-capable stack on a ~300 KB heap, which is the worse trade.
  const uint32_t nowMs = millis();
  if (g_lastPeriodicCheckMs == 0 || nowMs - g_lastPeriodicCheckMs >= kCheckIntervalMs) {
    g_lastPeriodicCheckMs = nowMs;
    runCheck(false);
    if (g_status.updateAvailable && getSettings().otaAutoUpdate) {
      Serial.print(F("OTA: auto-update is on, installing v"));
      Serial.println(g_status.availableTag);
      downloadAndFlash(g_pendingUrl);  // reboots on success
    }
  }
}

void otaRequestCheck() {
  g_checkRequested = true;
  g_status.checking = true;  // so the page says so the moment it reloads
}

void otaRequestInstall() {
  g_installRequested = true;
}

const OtaStatus& getOtaStatus() {
  return g_status;
}

const char* otaRunningVersion() {
  return FIRMWARE_VERSION;
}

const char* otaAssetName() {
  return OTA_ASSET_NAME;
}
