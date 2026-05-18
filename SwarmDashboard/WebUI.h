/*
 * WebUI.h - HTTP admin server for Swarm Dashboard
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include "MinerStore.h"
#include "config.h"

extern void requestPollNow();
extern uint32_t getBootMillis();
extern uint32_t getFreeHeap();
extern int8_t   getRssi();
extern float    getEspTemp();
extern float    getPoolHashrateTHs();
extern String   getPoolUrl();
extern String   getPoolWorker();
extern uint32_t getPoolAccepted();
extern uint32_t getPoolRejected();

class WebUI {
public:
  WebUI(MinerStore& s) : _store(s), _server(80) {}

  uint32_t getPollInterval() const { return _pollMs; }

  void begin() {
    Preferences p;
    p.begin("settings", true);
    _pollMs = p.getULong("poll", DEFAULT_POLL_MS);
    p.end();

    _server.on("/",              HTTP_GET,  [this]() { handleList(); });
    _server.on("/add",           HTTP_GET,  [this]() { handleForm(-1); });
    _server.on("/edit",          HTTP_GET,  [this]() {
      int i = _server.hasArg("i") ? _server.arg("i").toInt() : -1;
      handleForm(i);
    });
    _server.on("/save",          HTTP_POST, [this]() { handleSave(); });
    _server.on("/delete",        HTTP_POST, [this]() { handleDelete(); });
    _server.on("/pool",          HTTP_GET,  [this]() { handlePool(); });
    _server.on("/settings",      HTTP_GET,  [this]() { handleSettings(); });
    _server.on("/settings/save", HTTP_POST, [this]() { handleSettingsSave(); });
    _server.on("/factory",       HTTP_POST, [this]() { handleFactory(); });
    _server.on("/wipewifi",      HTTP_POST, [this]() { handleWipeWifi(); });
    _server.on("/about",         HTTP_GET,  [this]() { handleAbout(); });
    _server.on("/refresh",       HTTP_GET,  [this]() { handleRefresh(); });
    _server.on("/reboot",        HTTP_GET,  [this]() {
      sendHtml(pageHead("Reboot") + "<h1>Rebooting...</h1>" + pageFoot(), 200);
      delay(500);
      ESP.restart();
    });
    _server.onNotFound([this]() { _server.send(404, "text/plain", "Not found"); });
    _server.begin();
  }

  void loop() { _server.handleClient(); }

private:
  MinerStore& _store;
  WebServer   _server;
  uint32_t    _pollMs = DEFAULT_POLL_MS;

  void handleList() {
    String html = pageHead("Miners");
    html += navBar("miners");
    html += "<h1>Swarm Dashboard</h1>";
    html += "<p class='muted'>Dashboard IP: " + WiFi.localIP().toString() +
            "  &middot;  " + String(_store.count()) + " of " +
            String(MAX_MINERS) + " miners</p>";

    if (_store.count() == 0) {
      html += "<p class='empty'>No miners configured yet.<br>"
              "Click <b>+ Add Miner</b> below to get started.</p>";
    } else {
      html += "<table><tr><th>#</th><th>Name</th><th>IP</th><th>Type</th><th></th></tr>";
      for (uint8_t i = 0; i < _store.count(); i++) {
        const MinerEntry& m = _store.get(i);
        html += "<tr><td>" + String(i + 1) + "</td>";
        html += "<td>" + esc(m.name) + "</td>";
        html += "<td>" + esc(m.ip) + "</td>";
        html += "<td><span class='tag t" + String(m.type) + "'>"
              + String(minerTypeName(m.type)) + "</span></td>";
        html += "<td class='actions'>";
        html += "<a class='btn' href='/edit?i=" + String(i) + "'>Edit</a> ";
        html += "<form method='POST' action='/delete?i=" + String(i) + "' "
                "style='display:inline' onsubmit=\"return confirm('Delete " + esc(m.name) + "?')\">"
                "<button class='btn danger' type='submit'>Delete</button></form>";
        html += "</td></tr>";
      }
      html += "</table>";
    }

    html += "<div class='row'>";
    html += "<a class='btn primary' href='/add'>+ Add Miner</a> ";
    if (_store.count() > 0) {
      html += "<a class='btn' href='/refresh'>Refresh Now</a> ";
      html += "<a class='btn' href='/reboot' "
              "onclick=\"return confirm('Reboot dashboard?')\">Reboot</a>";
    }
    html += "</div>";

    html += "<p class='muted'>Add/edit/delete take effect immediately.</p>";
    html += pageFoot();
    sendHtml(html, 200);
  }

  void handleForm(int idx) {
    bool editing = (idx >= 0 && idx < (int) _store.count());
    MinerEntry m = {};
    if (editing) m = _store.get(idx);

    String html = pageHead(editing ? "Edit Miner" : "Add Miner");
    html += navBar("miners");
    html += "<h1>" + String(editing ? "Edit Miner" : "Add Miner") + "</h1>";
    html += "<form method='POST' action='/save'>";
    if (editing) html += "<input type='hidden' name='i' value='" + String(idx) + "'>";

    html += "<label>Name<br><input name='n' maxlength='15' required value='"
          + esc(m.name) + "' placeholder='e.g. Bitaxe1'></label>";
    html += "<label>IP address or hostname<br><input name='ip' maxlength='39' required value='"
          + esc(m.ip) + "' placeholder='192.168.1.110'></label>";

    html += "<label>Type<br><select name='t' required>";
    for (uint8_t t = 0; t <= 3; t++) {
      html += "<option value='" + String(t) + "'";
      if (m.type == t) html += " selected";
      html += ">" + String(minerTypeName(t)) + "</option>";
    }
    html += "</select></label>";

    html += "<p class='muted'>Bitaxe / NerdAxe use AxeOS HTTP. "
            "Avalon Nano 3s uses CGMiner TCP on port 4028. "
            "Plebsource DC-series uses HTTP on port 80.</p>";

    html += "<div class='row'>";
    html += "<button class='btn primary' type='submit'>Save</button> ";
    html += "<a class='btn' href='/'>Cancel</a>";
    html += "</div></form>";
    html += pageFoot();
    sendHtml(html, 200);
  }

  void handleSave() {
    if (!_server.hasArg("n") || !_server.hasArg("ip") || !_server.hasArg("t")) {
      _server.send(400, "text/plain", "Missing fields");
      return;
    }
    MinerEntry m = {};
    strlcpy(m.name, _server.arg("n").c_str(),  sizeof(m.name));
    strlcpy(m.ip,   _server.arg("ip").c_str(), sizeof(m.ip));
    m.type = (uint8_t) _server.arg("t").toInt();

    if (_server.hasArg("i")) {
      _store.update(_server.arg("i").toInt(), m);
    } else {
      if (!_store.add(m)) {
        _server.send(400, "text/plain", "Miner list full");
        return;
      }
    }
    redirect("/");
  }

  void handleDelete() {
    if (_server.hasArg("i")) _store.remove(_server.arg("i").toInt());
    redirect("/");
  }

  void handlePool() {
    String html = pageHead("Pool");
    html += navBar("pool");
    html += "<h1>Pool Info</h1>";
    html += "<table>";
    html += poolRow("Pool URL", getPoolUrl().length() ? getPoolUrl() : "—");

    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f TH/s", getPoolHashrateTHs());
    html += poolRow("Pool Hashrate", buf);

    uint32_t a = getPoolAccepted(), r = getPoolRejected(), tot = a + r;
    snprintf(buf, sizeof(buf), "%lu (%.2f%%)", (unsigned long) a,
             tot ? 100.0 * a / tot : 0);
    html += poolRow("Accepted", buf);
    snprintf(buf, sizeof(buf), "%lu (%.2f%%)", (unsigned long) r,
             tot ? 100.0 * r / tot : 0);
    html += poolRow("Rejected", buf);
    html += "</table>";

    html += "<p class='muted'>Pool info comes from the first responding miner.</p>";
    html += pageFoot();
    sendHtml(html, 200);
  }

  String poolRow(const String& k, const String& v) {
    return "<tr><td class='k'>" + esc(k) + "</td><td>" + esc(v) + "</td></tr>";
  }

  void handleSettings() {
    String html = pageHead("Settings");
    html += navBar("settings");
    html += "<h1>Settings</h1>";

    html += "<form method='POST' action='/settings/save'>";
    html += "<label>Poll interval (seconds)<br>"
            "<input type='number' name='poll' min='5' max='300' required value='"
          + String(_pollMs / 1000) + "'></label>";
    html += "<p class='muted'>How often the dashboard fetches data from each miner.</p>";
    html += "<button class='btn primary' type='submit'>Save</button>";
    html += "</form>";

    html += "<hr><h2>Danger Zone</h2>";

    html += "<form method='POST' action='/factory' "
            "onsubmit=\"return confirm('Delete ALL configured miners? Cannot be undone.')\">"
            "<button class='btn danger' type='submit'>Delete All Miners</button>"
            "</form>";

    html += "<form method='POST' action='/wipewifi' style='margin-top:10px' "
            "onsubmit=\"return confirm('Wipe WiFi credentials and reboot?')\">"
            "<button class='btn danger' type='submit'>Wipe WiFi &amp; Reboot</button>"
            "</form>";

    html += pageFoot();
    sendHtml(html, 200);
  }

  void handleSettingsSave() {
    if (_server.hasArg("poll")) {
      uint32_t s = _server.arg("poll").toInt();
      if (s < 5)   s = 5;
      if (s > 300) s = 300;
      _pollMs = s * 1000UL;
      Preferences p;
      p.begin("settings", false);
      p.putULong("poll", _pollMs);
      p.end();
    }
    redirect("/settings");
  }

  void handleFactory() {
    _store.clear();
    redirect("/");
  }

  void handleWipeWifi() {
    sendHtml(pageHead("Reboot") + navBar("") +
             "<h1>WiFi wiped</h1>"
             "<p>Rebooting into setup mode. Connect your phone to "
             "<b>SwarmDashboard-Setup</b> after the splash appears.</p>"
             + pageFoot(), 200);
    delay(800);
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();
  }

  void handleAbout() {
    uint32_t up = (millis() - getBootMillis()) / 1000;
    uint32_t d  = up / 86400;
    uint32_t h  = (up % 86400) / 3600;
    uint32_t m  = (up % 3600) / 60;

    String html = pageHead("About");
    html += navBar("about");
    html += "<h1>About</h1>";
    html += "<table>";
    html += poolRow("Firmware",     "Swarm Dashboard v1.3");
    html += poolRow("Hardware",     "HOSYOND 4.0\" ESP32 (ST7796S)");
    html += poolRow("Dashboard IP", WiFi.localIP().toString());
    html += poolRow("WiFi SSID",    WiFi.SSID());

    char buf[32];
    snprintf(buf, sizeof(buf), "%d dBm", getRssi());
    html += poolRow("WiFi Signal", buf);
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(getFreeHeap() / 1024));
    html += poolRow("Free Heap", buf);
    snprintf(buf, sizeof(buf), "%.1f °C", getEspTemp());
    html += poolRow("ESP32 Temp", buf);
    snprintf(buf, sizeof(buf), "%lud %luh %lum",
             (unsigned long) d, (unsigned long) h, (unsigned long) m);
    html += poolRow("Uptime", buf);
    html += "</table>";
    html += pageFoot();
    sendHtml(html, 200);
  }

  void handleRefresh() {
    requestPollNow();
    String html = pageHead("Refresh");
    html += navBar("");
    html += "<h1>Refreshing...</h1>";
    html += "<p>Polling all miners now. Returning to dashboard in 2 seconds.</p>";
    html += "<meta http-equiv='refresh' content='2;url=/'>";
    html += pageFoot();
    sendHtml(html, 200);
  }

  void redirect(const String& to) {
    _server.sendHeader("Location", to);
    _server.send(303);
  }

  static String esc(const String& s) {
    String r;
    r.reserve(s.length());
    for (char c : s) {
      if (c == '<')      r += "&lt;";
      else if (c == '>') r += "&gt;";
      else if (c == '&') r += "&amp;";
      else if (c == '"') r += "&quot;";
      else r += c;
    }
    return r;
  }

  String navBar(const String& active) {
    String n = "<nav>";
    n += navLink("/",         "Miners",   active == "miners");
    n += navLink("/pool",     "Pool",     active == "pool");
    n += navLink("/settings", "Settings", active == "settings");
    n += navLink("/about",    "About",    active == "about");
    n += "</nav>";
    return n;
  }

  String navLink(const String& href, const String& label, bool active) {
    return "<a href='" + href + "'" +
           (active ? " class='active'" : "") + ">" + label + "</a>";
  }

  String pageHead(const String& title) {
    String h = F(
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>");
    h += title;
    h += F(" - Swarm Dashboard</title>"
      "<style>"
      "body{font-family:system-ui,sans-serif;background:#0a0a0a;color:#e6e6e6;margin:0;padding:0}"
      ".wrap{max-width:680px;margin:0 auto;padding:20px}"
      "h1{color:#04f3ff;margin-top:0}"
      "h2{color:#ff6b6b;margin-top:30px}"
      "hr{border:none;border-top:1px solid #222;margin:30px 0}"
      ".muted{color:#888;font-size:.9em}"
      ".empty{padding:40px;text-align:center;border:1px dashed #333;border-radius:8px;color:#888}"
      "table{width:100%;border-collapse:collapse;margin:16px 0}"
      "th,td{padding:10px;text-align:left;border-bottom:1px solid #222;vertical-align:top}"
      "th{color:#04f3ff;font-size:.85em;text-transform:uppercase;letter-spacing:.05em}"
      "td.k{color:#888;width:35%}"
      ".actions{text-align:right;white-space:nowrap}"
      ".tag{padding:2px 8px;border-radius:4px;font-size:.85em;background:#222}"
      ".tag.t0{color:#ff9800}.tag.t1{color:#ff3df8}.tag.t2{color:#7fffd4}.tag.t3{color:#07e000}"
      ".btn{display:inline-block;padding:8px 14px;background:#222;color:#e6e6e6;text-decoration:none;border-radius:6px;border:1px solid #333;cursor:pointer;font-size:.95em;font-family:inherit}"
      ".btn:hover{background:#2a2a2a}"
      ".btn.primary{background:#04f3ff;color:#000;border-color:#04f3ff}"
      ".btn.danger{background:#3a0d0d;color:#ff6b6b;border-color:#5a1a1a}"
      "label{display:block;margin:14px 0;color:#aaa;font-size:.9em}"
      "input,select{width:100%;padding:10px;background:#111;color:#fff;border:1px solid #333;border-radius:6px;font-size:1em;font-family:inherit;box-sizing:border-box;margin-top:4px}"
      "input:focus,select:focus{outline:none;border-color:#04f3ff}"
      ".row{margin:20px 0;display:flex;gap:8px;flex-wrap:wrap}"
      "form{margin:0}"
      "nav{background:#111;border-bottom:1px solid #222;padding:0 20px;display:flex;gap:0;overflow-x:auto}"
      "nav a{padding:14px 18px;color:#888;text-decoration:none;border-bottom:2px solid transparent;font-size:.95em;white-space:nowrap}"
      "nav a:hover{color:#e6e6e6}"
      "nav a.active{color:#04f3ff;border-bottom-color:#04f3ff}"
      "</style></head><body><div class='wrap'>");
    return h;
  }

  String pageFoot() { return F("</div></body></html>"); }

  void sendHtml(const String& body, int code) {
    _server.send(code, "text/html; charset=utf-8", body);
  }
};
