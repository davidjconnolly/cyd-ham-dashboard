#include "setup_portal.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "connectivity.h"
#include "dashboard_display.h"
#include "dx_backfill.h"
#include "dx_spots.h"
#include "dx_watch.h"
#include "greyline.h"
#include "ota.h"
#include "propagation.h"
#include "settings.h"

namespace {
constexpr byte kDnsPort = 53;
constexpr char kApSsid[] = "CYD-HamClock-Setup";
constexpr char kApPassword[] = "hamclock";

constexpr uint32_t kApAutoOffConfirmMs = 8000;

DNSServer dnsServer;
WebServer server(80);
bool portalStarted = false;
bool mdnsStarted = false;
bool pendingWifiReconnect = false;
uint32_t pendingWifiReconnectAtMs = 0;
bool pendingReboot = false;
uint32_t pendingRebootAtMs = 0;
bool hotspotActive = false;
uint32_t staConfirmedSinceMs = 0;

void startHotspot() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kApSsid, kApPassword);
  delay(100);
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  hotspotActive = true;
  staConfirmedSinceMs = 0;
  Serial.println("Setup hotspot is on");
}

void stopHotspot(const char* reason) {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  hotspotActive = false;
  Serial.print("Setup hotspot turned off (");
  Serial.print(reason);
  Serial.println(")");
}

String htmlEscape(const String& input) {
  String out;
  out.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

String boolSelected(bool selected) {
  return selected ? F(" selected") : F("");
}

String checked(bool value) {
  return value ? F(" checked") : F("");
}

String limitedArg(const char* name, size_t maxLen, bool trimWhitespace = true) {
  String value = server.arg(name);
  if (trimWhitespace) {
    value.trim();
  }
  if (value.length() > maxLen) {
    value = value.substring(0, maxLen);
  }
  return value;
}

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += F("\\n");
    } else if (c == '\r') {
      out += F("\\r");
    } else {
      out += c;
    }
  }
  return out;
}

String uptimeText(uint32_t seconds) {
  char buffer[18];
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  const uint32_t secs = seconds % 60;
  snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
  return String(buffer);
}

String statusJson() {
  const ClockSnapshot snapshot = getClockSnapshot();
  const PropagationData& propagation = getPropagationData();
  const DxSpotsData& dx = getDxSpotsData();

  String json;
  json.reserve(820 + (dxWatchCount() * 160));
  json += F("{\"project\":\"CYD HamClock\",");
  json += F("\"wifi\":");
  json += snapshot.wifiConnected ? F("true") : F("false");
  json += F(",\"ip\":\"");
  json += snapshot.wifiConnected ? WiFi.localIP().toString() : String("");
  json += F("\",\"uptime\":\"");
  json += uptimeText(snapshot.uptimeSeconds);
  json += F("\",\"free_heap\":");
  json += String(ESP.getFreeHeap());
  json += F(",\"ntp\":");
  json += snapshot.timeValid ? F("true") : F("false");
  json += F(",\"page\":");
  json += String(getCurrentDashboardPageNumber());
  json += F(",\"propagation_status\":\"");
  json += jsonEscape(propagation.status);
  json += F("\",\"dx_status\":\"");
  json += jsonEscape(dx.status);
  json += F("\",\"dx_source\":\"");
  json += jsonEscape(dx.source);
  json += F("\",\"dx_modes\":\"");
  json += jsonEscape(dxModeFilterIsActive() ? dxModeFilterSummary() : String("all"));
  json += F("\",\"dx_backfill\":\"");
  json += jsonEscape(getDxBackfillStatus());
  const OtaStatus& ota = getOtaStatus();
  json += F("\",\"version\":\"");
  json += jsonEscape(otaRunningVersion());
  json += F("\",\"ota_asset\":\"");
  json += jsonEscape(otaAssetName());
  json += F("\",\"ota_status\":\"");
  json += jsonEscape(ota.message);
  json += F("\",\"ota_available\":");
  json += ota.updateAvailable ? F("true") : F("false");
  json += F(",\"ota_available_version\":\"");
  json += jsonEscape(ota.availableTag);
  json += F("\",\"ota_checking\":");
  json += ota.checking ? F("true") : F("false");
  json += F(",\"ota_installing\":");
  json += ota.installing ? F("true") : F("false");
  json += F(",\"ota_progress\":");
  json += String(ota.progressPercent);
  json += F(",\"ota_check_code\":");
  json += String(ota.lastCheckHttpCode);
  json += F(",\"ota_auto\":");
  json += getSettings().otaAutoUpdate ? F("true") : F("false");
  json += F(",\"dx_watch\":[");
  for (uint8_t i = 0; i < dxWatchCount(); ++i) {
    const DxWatchEntry& entry = dxWatchEntry(i);
    if (i > 0) {
      json += ',';
    }
    json += F("{\"pattern\":\"");
    json += jsonEscape(entry.pattern);
    json += F("\",\"heard\":");
    json += entry.heard ? F("true") : F("false");
    json += F(",\"active\":");
    json += dxWatchEntryIsActive(entry) ? F("true") : F("false");
    json += F(",\"call\":\"");
    json += jsonEscape(entry.call);
    json += F("\",\"freq\":\"");
    json += jsonEscape(entry.freq);
    json += F("\",\"mode\":\"");
    json += jsonEscape(entry.mode);
    json += F("\",\"spotter\":\"");
    json += jsonEscape(entry.spotter);
    json += F("\",\"age\":\"");
    json += jsonEscape(dxWatchAgeText(entry));
    json += F("\",\"spots\":");
    json += String(entry.hitCount);
    json += '}';
  }
  json += F("]}");
  return json;
}

String watchStatusHtml() {
  const uint8_t count = dxWatchCount();
  if (count == 0) {
    return F("<div>DX watch: <code>no callsigns watched</code></div>");
  }

  String out;
  out.reserve(80 * count);
  out += F("<div>DX watch:</div>");
  for (uint8_t i = 0; i < count; ++i) {
    const DxWatchEntry& entry = dxWatchEntry(i);
    out += F("<div>&nbsp;&nbsp;<code>");
    out += htmlEscape(entry.pattern);
    out += F("</code> ");
    if (!entry.heard) {
      out += F("<small>not heard</small>");
    } else {
      out += F("<strong class='");
      out += dxWatchEntryIsActive(entry) ? F("ok'>") : F("warn'>");
      out += htmlEscape(entry.call);
      out += F("</strong> <small>");
      out += htmlEscape(entry.freq);
      out += F(" ");
      out += htmlEscape(entry.mode);
      out += F(" &middot; ");
      out += htmlEscape(dxWatchAgeText(entry));
      out += F(" ago &middot; ");
      out += String(entry.hitCount);
      out += F(" spot");
      out += entry.hitCount == 1 ? F("") : F("s");
      out += F("</small>");
    }
    out += F("</div>");
  }
  return out;
}

String pageHtml(const String& message = "") {
  const AppSettings& settings = getSettings();
  const ClockSnapshot snapshot = getClockSnapshot();
  const PropagationData& propagation = getPropagationData();
  const DxSpotsData& dx = getDxSpotsData();

  String html;
  html.reserve(10500);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>CYD HamClock Settings</title><style>");
  html += F("body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#10151c;color:#f3f7fb}");
  html += F("main{max-width:760px;margin:0 auto;padding:24px}label{display:block;margin:14px 0 6px;color:#aeb8c4}");
  html += F("input,select,textarea{box-sizing:border-box;width:100%;padding:11px;border-radius:6px;border:1px solid #3a4653;background:#18212b;color:#fff;font-size:16px;font-family:inherit}input[type=checkbox]{width:auto;margin-right:8px}");
  html += F("button{margin-top:18px;margin-right:8px;padding:12px 16px;border:0;border-radius:6px;background:#1aa7c8;color:#001018;font-weight:700;font-size:16px}");
  html += F(".danger{background:#ffbd66}.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}.card{border:1px solid #293440;border-radius:8px;padding:16px;margin:16px 0;background:#141b24}.ok{color:#71e58d}.warn{color:#ffbd66}");
  html += F(".modes{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:0 12px}.modes label{margin:6px 0;color:#f3f7fb}");
  html += F("small{color:#aeb8c4}code{background:#202b36;padding:2px 5px;border-radius:4px}</style></head><body><main>");
  html += F("<h1>CYD HamClock Settings</h1>");

  if (message.length() > 0) {
    html += F("<div class='card ok'>");
    html += htmlEscape(message);
    html += F("</div>");
  }

  html += F("<div class='card'>");
  html += F("<div>Wi-Fi: <strong class='");
  html += snapshot.wifiConnected ? F("ok'>connected") : F("warn'>not connected");
  html += F("</strong></div><div>NTP: <strong class='");
  html += snapshot.timeValid ? F("ok'>synced") : F("warn'>waiting");
  html += F("</strong></div><div>LAN IP: <code>");
  html += snapshot.wifiConnected ? WiFi.localIP().toString() : String("--");
  html += F("</code></div><div>mDNS: <code>");
  html += mdnsStarted ? F("cyd-ham.local") : F("--");
  html += F("</code></div><div>Uptime: <code>");
  html += uptimeText(snapshot.uptimeSeconds);
  html += F("</code></div><div>Free heap: <code>");
  html += String(ESP.getFreeHeap());
  html += F("</code></div><div>Current page: <code>");
  html += String(getCurrentDashboardPageNumber());
  html += F("/");
  html += String(getDashboardPageCount());
  html += F("</code></div><div>Propagation: <code>");
  html += htmlEscape(propagation.status);
  html += F("</code></div><div>DX: <code>");
  html += htmlEscape(dx.source);
  html += F(" / ");
  html += htmlEscape(dx.status);
  html += F("</code></div><div>DX modes: <code>");
  html += dxModeFilterIsActive() ? htmlEscape(dxModeFilterSummary()) : String("all");
  html += F("</code></div>");
  html += watchStatusHtml();
  html += F("<div>Setup hotspot: <strong class='");
  html += hotspotActive ? F("warn'>on") : F("ok'>off");
  html += F("</strong></div><div>Setup AP: <code>");
  html += kApSsid;
  html += F("</code>, password <code>");
  html += kApPassword;
  html += F("</code></div><div>Portal IP: <code>");
  html += hotspotActive ? WiFi.softAPIP().toString() : String("--");
  html += F("</code></div></div>");

  html += F("<form method='post' action='/save'><div class='card'><h2>Station</h2>");
  html += F("<label for='callsign'>Callsign</label><input id='callsign' name='callsign' maxlength='16' value='");
  html += htmlEscape(settings.callsign);
  html += F("'><small>This callsign is also used to log in to a Telnet DX Cluster. If blank, <code>NOCALL</code> is used.</small>");
  html += F("<label for='ssid'>Wi-Fi SSID</label><input id='ssid' name='ssid' value='");
  html += htmlEscape(settings.wifiSsid);
  html += F("' autocomplete='off'>");
  html += F("<label for='pass'>Wi-Fi password</label><input id='pass' name='pass' type='password' value='' maxlength='64' autocomplete='new-password' placeholder='Leave blank to keep saved password'>");
  html += F("<label><input name='clearpass' type='checkbox' value='1'>Clear the saved password (for an open network)</label>");
  html += F("<label><input name='keepap' type='checkbox' value='1'");
  html += checked(settings.keepHotspotOn);
  html += F(">Keep the <code>");
  html += kApSsid;
  html += F("</code> setup hotspot switched on</label><small>By default this hotspot switches off a few seconds after the device confirms it has joined your Wi-Fi network. Check this box to leave it running (for example, to reach the settings page again without your router).</small>");
  html += F("</div><div class='card'><h2>Time and Location</h2>");
  html += F("<label for='tzpreset'>Timezone preset</label><select id='tzpreset' onchange='applyPreset(this.value)'>");
  html += F("<option value='CUSTOM'>Custom POSIX TZ</option>");
  html += F("<option value='UTC'>UTC</option>");
  html += F("<option value='UK'>United Kingdom GMT/BST</option>");
  html += F("<option value='IE'>Ireland GMT/IST</option>");
  html += F("<option value='EU_CENTRAL'>Central Europe CET/CEST</option>");
  html += F("<option value='EU_EASTERN'>Eastern Europe EET/EEST</option>");
  html += F("<option value='US_EASTERN'>US Eastern</option>");
  html += F("<option value='US_CENTRAL'>US Central</option>");
  html += F("<option value='US_MOUNTAIN'>US Mountain</option>");
  html += F("<option value='US_PACIFIC'>US Pacific</option>");
  html += F("<option value='CA_ATLANTIC'>Canada Atlantic</option>");
  html += F("<option value='AU_EASTERN'>Australia Eastern</option>");
  html += F("<option value='AU_CENTRAL'>Australia Central</option>");
  html += F("<option value='AU_WESTERN'>Australia Western</option>");
  html += F("<option value='NZ'>New Zealand</option>");
  html += F("<option value='JP'>Japan</option>");
  html += F("<option value='CN'>China</option>");
  html += F("<option value='IN'>India</option>");
  html += F("<option value='BR_EAST'>Brazil East</option>");
  html += F("<option value='ZA'>South Africa</option></select>");
  html += F("<label for='tzlabel'>Timezone label</label><input id='tzlabel' name='tzlabel' value='");
  html += htmlEscape(settings.timezoneLabel);
  html += F("'>");
  html += F("<label for='tz'>POSIX timezone rule</label><input id='tz' name='tz' oninput='syncPreset()' value='");
  html += htmlEscape(settings.timezone);
  html += F("'><small>UK default: <code>GMT0BST-1,M3.5.0/1,M10.5.0/2</code></small>");
  html += F("<label for='locator'>Maidenhead locator</label><input id='locator' name='locator' maxlength='6' pattern='[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?' value='");
  html += htmlEscape(settings.locator);
  html += F("'><small>Use a four- or six-character Maidenhead locator. Changes are applied immediately.</small>");
  html += F("</div><div class='card'><h2>Data Sources</h2>");
  html += F("<label for='propmode'>Propagation source mode</label><select id='propmode' name='propmode'>");
  html += F("<option value='direct'");
  html += boolSelected(!settings.useJsonPropagationProxy);
  html += F(">Direct HamQSL XML</option><option value='json'");
  html += boolSelected(settings.useJsonPropagationProxy);
  html += F(">JSON proxy URL</option></select>");
  html += F("<small>Direct HamQSL XML: <code>https://www.hamqsl.com/solarxml.php</code></small>");
  html += F("<label for='propurl'>Propagation JSON URL</label><input id='propurl' name='propurl' maxlength='180' value='");
  html += htmlEscape(settings.propagationJsonUrl);
  html += F("'>");
  html += F("<label for='dxmode'>DX source mode</label><select id='dxmode' name='dxmode'>");
  html += F("<option value='auto'");
  html += boolSelected(settings.dxSourceMode == kDxSourceAuto);
  html += F(">Auto (JSON then Telnet)</option><option value='json'");
  html += boolSelected(settings.dxSourceMode == kDxSourceJson);
  html += F(">JSON only</option><option value='telnet'");
  html += boolSelected(settings.dxSourceMode == kDxSourceTelnet);
  html += F(">Telnet only</option></select>");
  html += F("<label for='dxurl'>DX JSON URL</label><input id='dxurl' name='dxurl' maxlength='180' value='");
  html += htmlEscape(settings.dxSpotsUrl);
  html += F("'>");
  html += F("<div class='grid'><div><label for='dxhost'>DX Telnet host</label><input id='dxhost' name='dxhost' maxlength='64' value='");
  html += htmlEscape(settings.dxTelnetHost);
  html += F("'></div><div><label for='dxport'>DX Telnet port</label><input id='dxport' name='dxport' type='number' min='1' max='65535' value='");
  html += String(settings.dxTelnetPort);
  html += F("'></div></div>");
  html += F("<div class='grid'><div><label for='propmins'>Propagation refresh minutes</label><input id='propmins' name='propmins' type='number' min='1' max='120' value='");
  html += String(settings.propagationRefreshMinutes);
  html += F("'></div><div><label for='dxmins'>DX refresh minutes</label><input id='dxmins' name='dxmins' type='number' min='1' max='120' value='");
  html += String(settings.dxRefreshMinutes);
  html += F("'></div></div>");
  html += F("<label>DX spot modes</label><div class='modes'>");
  // The unknown catch-all is the last entry and gets its own control below,
  // because including or excluding unlabelled spots is the decision that
  // actually changes how full the page looks.
  for (uint8_t i = 0; i + 1 < kDxModeOptionCount; ++i) {
    const DxModeOption& option = dxModeOption(i);
    html += F("<label><input name='dxm' type='checkbox' value='");
    html += option.name;
    html += F("'");
    html += checked((settings.dxModeMask & option.bit) != 0);
    html += F(">");
    html += option.label;
    html += F("</label>");
  }
  html += F("</div><small>Only the ticked modes are registered. Unticked ones are dropped as they arrive, so they appear neither in the spot list nor on the <strong>DX Watch</strong> page. Ticking every box, or none, means no filtering.</small>");
  html += F("<label>Unrecognised modes</label>");
  html += F("<label><input name='dxm' type='checkbox' value='");
  html += dxModeOption(kDxModeOptionCount - 1).name;
  html += F("'");
  html += checked((settings.dxModeMask & kDxModeUnknown) != 0);
  html += F(">Include spots whose mode cannot be worked out</label>");
  html += F("<small>The mode is read from the spotter's comment, falling back to the FT8 and FT4 calling frequencies. Many spotters label nothing, so a large share of any feed lands here as <code>Unknown</code>.<br>");
  html += F("Leave this ticked to keep those spots. Untick it to see only spots that state their mode - a much shorter list, and a genuine SSB or CW contact whose spotter said nothing is lost with it.</small></div>");

  html += F("<div class='card'><h2>DX Watchlist</h2>");
  html += F("<label for='dxwatch'>Watched callsigns</label>");
  html += F("<textarea id='dxwatch' name='dxwatch' rows='3' maxlength='240' placeholder='3Y0J, VP6D, FT8WW'>");
  html += htmlEscape(settings.dxWatchList);
  html += F("</textarea>");
  html += F("<small>Up to 8 callsigns, separated by commas, spaces, or new lines. Every spot from both DX sources is matched against this list and shown on the <strong>DX Watch</strong> page.<br>");
  html += F("<code>3Y0J</code> also matches <code>3Y0J/MM</code> and <code>FT4/3Y0J</code>. Add <code>*</code> for a prefix watch, for example <code>VP6*</code>.<br>");
  html += F("For expedition monitoring set <strong>DX source mode</strong> to <code>Auto</code> or <code>Telnet only</code>: JSON polling reads just the newest few spots each refresh and will miss most appearances.</small>");
  html += F("<div class='grid'><div><label for='dxwhold'>Active for (minutes)</label><input id='dxwhold' name='dxwhold' type='number' min='1' max='720' value='");
  html += String(settings.dxWatchHoldMinutes);
  html += F("'><small>How long after a spot a call counts as on the air. A later spot inside this window updates the row quietly instead of alerting again.</small></div><div>");
  html += F("<label>Alerts</label>");
  html += F("<label><input name='dxwalert' type='checkbox' value='1'");
  html += checked(settings.dxWatchAlertEnabled);
  html += F(">Flash the backlight on a new hit</label>");
  html += F("<label><input name='dxwauto' type='checkbox' value='1'");
  html += checked(settings.dxWatchAutoPage);
  html += F(">Jump to the DX Watch page on a new hit</label>");
  html += F("<small>Page jumps are held back for 15 seconds after you touch the screen.</small></div></div>");
  html += F("<label for='dxwbfurl'>History backfill URL</label><input id='dxwbfurl' name='dxwbfurl' maxlength='180' value='");
  html += htmlEscape(settings.dxWatchBackfillUrl);
  html += F("'><small>Seeds the watch rows from recent spot history at start-up, whenever you change the list, and on the interval below. Without it a watched call stays blank until its next live appearance.<br>");
  html += F("The default is DXSummit. Its API has no per-callsign filter, so the device pulls a wide window and matches on the fly - <code>limit=500</code> covers roughly the last 45 minutes, <code>limit=1000</code> about 95. Larger windows take longer to download. Leave blank to disable.<br>");
  html += F("Note that DXSummit's HTTPS endpoint is unreliable; the plain <code>http://</code> URL is used deliberately.</small>");
  html += F("<label for='dxwbfmin'>Backfill every (minutes)</label><input id='dxwbfmin' name='dxwbfmin' type='number' min='1' max='240' value='");
  html += String(settings.dxWatchBackfillMinutes);
  html += F("'>");
  html += F("<div>History status: <code>");
  html += htmlEscape(getDxBackfillStatus());
  html += F("</code></div>");
  html += F("</div>");

  html += F("<div class='card'><h2>Display</h2>");
  html += F("<label for='bright'>Backlight brightness percent</label><input id='bright' name='bright' type='number' min='5' max='100' value='");
  html += String(settings.brightnessPercent);
  html += F("'><label><input name='swaprb' type='checkbox' value='1'");
  if (settings.swapRedBlueChannels) {
    html += F(" checked");
  }
  html += F(">Swap red/blue display channels</label><small>Enable this only when red appears blue and yellow appears cyan. It is applied immediately and saved for this board.</small>");
  html += F("<label><input name='rot90' type='checkbox' value='1'");
  if (settings.rotate90) {
    html += F(" checked");
  }
  html += F(">Rotate display 90&deg;</label><small>Enable this if the screen shows portrait and cropped on first start.</small>");
  html += F("<label><input name='flip180' type='checkbox' value='1'");
  if (settings.flip180) {
    html += F(" checked");
  }
  html += F(">Flip display 180&deg;</label><small>Enable this if the screen is upside down.</small></div>");

  const OtaStatus& ota = getOtaStatus();
  html += F("<div class='card'><h2>Firmware</h2>");
  html += F("<div>Running version: <code>");
  html += htmlEscape(otaRunningVersion());
  html += F("</code></div><div>This board installs: <code>");
  html += htmlEscape(otaAssetName());
  html += F("</code></div><div>Update status: <code>");
  html += htmlEscape(ota.message);
  html += F("</code></div>");
  html += F("<small>Each release carries one image per display driver. This board will only ever install the asset named above, because the other variant's image leaves the panel garbled and recoverable only over USB.<br>");
  html += F("A version of <code>dev</code> means this firmware was built locally rather than from a release, so every release looks newer than it.</small>");
  html += F("<label><input name='otaauto' type='checkbox' value='1'");
  html += checked(settings.otaAutoUpdate);
  html += F(">Install new releases automatically</label>");
  html += F("<small>Off by default. The device checks every six hours; with this ticked it downloads and flashes a new release on its own, which takes the dashboard away for about a minute while it reboots.</small></div>");

  html += F("<button type='submit'>Save settings</button></form>");
  html += F("<form method='post' action='/ota/check'><button type='submit'>Check for update</button></form>");
  if (ota.updateAvailable) {
    html += F("<form method='post' action='/ota/install'><button class='danger' type='submit'>Install v");
    html += htmlEscape(ota.availableTag);
    html += F(" now</button><small>The device drops off the network while it flashes and reboots. Do not power it off. Progress is shown on the panel.</small></form>");
  }
  html += F("<form method='post' action='/reboot'><button class='danger' type='submit'>Restart device</button></form>");
  html += F("<p><small>This page is intended for trusted LAN use only. No admin password is configured in this project.</small></p>");
  html += F("<script>");
  html += F("const presets={");
  html += F("UTC:['UTC0','UTC'],");
  html += F("UK:['GMT0BST-1,M3.5.0/1,M10.5.0/2','UK local'],");
  html += F("IE:['IST-1GMT0,M10.5.0,M3.5.0/1','Ireland local'],");
  html += F("EU_CENTRAL:['CET-1CEST-2,M3.5.0/2,M10.5.0/3','Central Europe'],");
  html += F("EU_EASTERN:['EET-2EEST-3,M3.5.0/3,M10.5.0/4','Eastern Europe'],");
  html += F("US_EASTERN:['EST5EDT,M3.2.0/2,M11.1.0/2','US Eastern'],");
  html += F("US_CENTRAL:['CST6CDT,M3.2.0/2,M11.1.0/2','US Central'],");
  html += F("US_MOUNTAIN:['MST7MDT,M3.2.0/2,M11.1.0/2','US Mountain'],");
  html += F("US_PACIFIC:['PST8PDT,M3.2.0/2,M11.1.0/2','US Pacific'],");
  html += F("CA_ATLANTIC:['AST4ADT,M3.2.0/2,M11.1.0/2','Canada Atlantic'],");
  html += F("AU_EASTERN:['AEST-10AEDT-11,M10.1.0/2,M4.1.0/3','Australia East'],");
  html += F("AU_CENTRAL:['ACST-9:30ACDT-10:30,M10.1.0/2,M4.1.0/3','Australia Central'],");
  html += F("AU_WESTERN:['AWST-8','Australia West'],");
  html += F("NZ:['NZST-12NZDT-13,M9.5.0/2,M4.1.0/3','New Zealand'],");
  html += F("JP:['JST-9','Japan'],CN:['CST-8','China'],IN:['IST-5:30','India'],");
  html += F("BR_EAST:['BRT3','Brazil East'],ZA:['SAST-2','South Africa']};");
  html += F("function applyPreset(v){if(!presets[v])return;document.getElementById('tz').value=presets[v][0];document.getElementById('tzlabel').value=presets[v][1];}");
  html += F("function syncPreset(){const rule=document.getElementById('tz').value;let selected='CUSTOM';for(const key in presets){if(presets[key][0]===rule){selected=key;break;}}document.getElementById('tzpreset').value=selected;}");
  html += F("syncPreset();");
  html += F("</script>");
  html += F("</main></body></html>");
  return html;
}

void handleRoot() {
  String message = "";
  if (server.hasArg("saved")) {
    message = "Settings saved.";
  } else if (server.hasArg("rebooting")) {
    message = "Restart requested. The device will be back shortly.";
  } else if (server.hasArg("checking")) {
    message = "Checking GitHub for a new release. Reload this page in a few seconds.";
  } else if (server.hasArg("noupdate")) {
    message = "No update is available to install. Check for one first.";
  }
  server.send(200, "text/html", pageHtml(message));
}

void handleSave() {
  const AppSettings previousSettings = getSettings();
  AppSettings settings = previousSettings;
  settings.callsign = limitedArg("callsign", 16);
  settings.wifiSsid = limitedArg("ssid", 64, false);
  const String submittedPassword = limitedArg("pass", 64, false);
  if (server.hasArg("clearpass")) {
    settings.wifiPassword = "";
  } else if (submittedPassword.length() > 0) {
    settings.wifiPassword = submittedPassword;
  }
  settings.timezone = limitedArg("tz", 80);
  settings.timezoneLabel = limitedArg("tzlabel", 24);
  settings.locator = limitedArg("locator", 6);
  settings.useJsonPropagationProxy = server.arg("propmode") == "json";
  settings.propagationJsonUrl = limitedArg("propurl", 180);
  const String dxMode = server.arg("dxmode");
  settings.dxSourceMode = dxMode == "json" ? kDxSourceJson
                          : dxMode == "telnet" ? kDxSourceTelnet
                                                : kDxSourceAuto;
  // Checkboxes sharing one name arrive as repeated arguments, so the mask has
  // to be rebuilt by walking the whole argument list.
  uint16_t modeMask = 0;
  for (int i = 0; i < server.args(); ++i) {
    if (server.argName(i) == "dxm") {
      modeMask |= dxModeBitForName(server.arg(i));
    }
  }
  settings.dxModeMask = modeMask;
  settings.dxSpotsUrl = limitedArg("dxurl", 180);
  settings.dxTelnetHost = limitedArg("dxhost", 64);
  settings.dxTelnetPort = static_cast<uint16_t>(
      constrain(server.arg("dxport").toInt(), 1L, 65535L));
  settings.dxWatchList = limitedArg("dxwatch", 240);
  settings.dxWatchAlertEnabled = server.hasArg("dxwalert");
  settings.dxWatchAutoPage = server.hasArg("dxwauto");
  settings.dxWatchHoldMinutes = static_cast<uint16_t>(
      constrain(server.arg("dxwhold").toInt(), 1L, 720L));
  settings.dxWatchBackfillUrl = limitedArg("dxwbfurl", 180);
  settings.dxWatchBackfillMinutes = static_cast<uint16_t>(
      constrain(server.arg("dxwbfmin").toInt(), 1L, 240L));
  settings.propagationRefreshMinutes = static_cast<uint16_t>(
      constrain(server.arg("propmins").toInt(), 1L, 120L));
  settings.dxRefreshMinutes = static_cast<uint16_t>(
      constrain(server.arg("dxmins").toInt(), 1L, 120L));
  settings.brightnessPercent = static_cast<uint8_t>(
      constrain(server.arg("bright").toInt(), 5L, 100L));
  settings.swapRedBlueChannels = server.hasArg("swaprb");
  settings.rotate90 = server.hasArg("rot90");
  settings.flip180 = server.hasArg("flip180");
  settings.keepHotspotOn = server.hasArg("keepap");
  settings.otaAutoUpdate = server.hasArg("otaauto");
  saveSettings(settings);
  dxWatchReloadPatterns();
  // A changed list should answer immediately rather than at the next interval.
  dxWatchRequestBackfill();
  applyTimezoneSettings();
  applyDisplaySettings();
  requestPropagationRefresh();
  requestDxSpotsRefresh();
  requestGreylineRefresh();

  if (settings.wifiSsid != previousSettings.wifiSsid ||
      settings.wifiPassword != previousSettings.wifiPassword) {
    pendingWifiReconnect = true;
    pendingWifiReconnectAtMs = millis() + 1500;
  }

  server.sendHeader("Location", "/?saved=1", true);
  server.send(303, "text/plain", "Settings saved");
}

void handleStatusJson() {
  server.send(200, "application/json", statusJson());
}

// Both OTA endpoints only queue the work. The check is a blocking HTTPS round
// trip and the install tears this very server down, so neither can run inside
// a request handler — otaLoop picks them up once the response has gone out.
void handleOtaCheck() {
  otaRequestCheck();
  server.sendHeader("Location", "/?checking=1", true);
  server.send(303, "text/plain", "Checking for update");
}

void handleOtaInstall() {
  if (!getOtaStatus().updateAvailable) {
    server.sendHeader("Location", "/?noupdate=1", true);
    server.send(303, "text/plain", "No update available");
    return;
  }
  otaRequestInstall();
  // A redirect would be pointless: this server stops before the browser could
  // follow it. Say what is about to happen instead.
  String html;
  html.reserve(600);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Updating</title></head><body style='font-family:system-ui,sans-serif;background:#10151c;color:#f3f7fb;padding:24px'>");
  html += F("<h1>Updating to v");
  html += htmlEscape(getOtaStatus().availableTag);
  html += F("</h1><p>The device is downloading <code>");
  html += htmlEscape(otaAssetName());
  html += F("</code> and will reboot into it. Progress is shown on the panel.</p>");
  html += F("<p>This settings page, the setup hotspot and the DX cluster connection all stop while it flashes. Do not power the device off. It will be back in about a minute.</p>");
  html += F("<p>If the update fails the device keeps running this firmware and this page comes back.</p></body></html>");
  server.send(200, "text/html", html);
}

void handleReboot() {
  pendingReboot = true;
  pendingRebootAtMs = millis() + 1500;
  server.sendHeader("Location", "/?rebooting=1", true);
  server.send(303, "text/plain", "Restart requested");
}

void handleRebootGet() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleCaptiveRedirect() {
  if (!hotspotActive) {
    handleRoot();
    return;
  }
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
  server.send(302, "text/plain", "");
}
}

void setupPortalBegin() {
  startHotspot();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatusJson);
  server.on("/ota/check", HTTP_POST, handleOtaCheck);
  server.on("/ota/install", HTTP_POST, handleOtaInstall);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/reboot", HTTP_GET, handleRebootGet);
  server.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
  server.on("/gen_204", HTTP_GET, handleCaptiveRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, []() { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound(handleCaptiveRedirect);
  server.begin();
  portalStarted = true;

  Serial.print("Setup portal started: ");
  Serial.println(kApSsid);
  Serial.print("Portal IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupPortalPrepareForOta() {
  server.close();
  portalStarted = false;
  if (hotspotActive) {
    stopHotspot("firmware update");
  }
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  Serial.println("Setup portal stopped for firmware update");
}

void setupPortalRestoreAfterOta() {
  // Only reached when the flash failed and the device is still running this
  // firmware. The routes are still registered on the server object, so
  // listening again is enough; setupPortalLoop puts the hotspot and mDNS back
  // according to the current settings and Wi-Fi state.
  server.begin();
  portalStarted = true;
  staConfirmedSinceMs = 0;
  Serial.println("Setup portal restarted after a failed firmware update");
}

void setupPortalLoop() {
  if (!portalStarted) {
    return;
  }
  if (!mdnsStarted && WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("cyd-ham")) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.println("mDNS started: http://cyd-ham.local/");
    } else {
      Serial.println("mDNS start failed");
    }
  }

  const AppSettings& settings = getSettings();
  const bool staConnected =
      WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  const uint32_t nowMs = millis();

  if (settings.keepHotspotOn) {
    staConfirmedSinceMs = 0;
    if (!hotspotActive) {
      startHotspot();
    }
  } else if (staConnected) {
    if (staConfirmedSinceMs == 0) {
      staConfirmedSinceMs = nowMs;
    } else if (hotspotActive && deadlineReached(nowMs, staConfirmedSinceMs + kApAutoOffConfirmMs)) {
      stopHotspot("Wi-Fi connection confirmed");
    }
  } else {
    staConfirmedSinceMs = 0;
  }

  if (hotspotActive) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  if (pendingWifiReconnect && deadlineReached(nowMs, pendingWifiReconnectAtMs)) {
    pendingWifiReconnect = false;
    reconnectWifi();
  }
  if (pendingReboot && deadlineReached(nowMs, pendingRebootAtMs)) {
    pendingReboot = false;
    ESP.restart();
  }
}
