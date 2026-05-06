/*
 * AvalonAPI.h - CGMiner TCP client for Avalon Nano 3s
 *
 * Field reference (from Nano 3s firmware Nano3s-25021401):
 *
 * summary:
 *   MHS av        in MH/s -> divide by 1,000,000 for TH/s, by 1000 for GH/s
 *   Elapsed       seconds
 *   Accepted      count
 *   Rejected      count
 *   Best Share    difficulty value
 *
 * pools:
 *   URL=stratum+tcp://...
 *   User=walletaddress.workername
 *
 * stats (the funky one - uses KEY[value] brackets, not KEY=value):
 *   STATS=0,ID=AVALON0,Elapsed=NNNNN,...,MM ID0=Ver[...] OTemp[36] TMax[60]
 *     TAvg[55] PS[0 0 27575 3 0 3782 94] GHSavg[4953.55] ...
 *   PS array: PS[unused unused mV unused unused mA WATTS]
 *     -> position 6 is wattage in W (read directly)
 */
#pragma once
#include <Arduino.h>
#include <WiFi.h>

struct AvalonReading {
  bool     ok       = false;
  float    hashrate = 0;     // GH/s (so dashboard /1000 -> TH/s)
  float    temp     = 0;     // C (TMax - hottest chip)
  uint32_t accepted = 0;
  uint32_t rejected = 0;
  uint32_t uptime   = 0;
  uint16_t power    = 0;     // W (calculated from PS array)
  float    bestDiff = 0;
  char     poolUrl[64] = {0};
  char     worker[40]  = {0};
};

class AvalonAPI {
public:
  static bool fetch(const char* host, AvalonReading& out, uint16_t timeoutMs = 1500) {
    // Hard cap total time across all 3 queries at 5 seconds. Each call gets a
    // 1.5s budget; if the deadline is hit, remaining queries are skipped so
    // the dashboard never hangs waiting on a slow/unreachable Avalon.
    const uint32_t totalDeadline = millis() + 5000;
    String s, p, st;

    if (!queryRaw(host, "summary", s, timeoutMs)) return false;

    // Pools/stats are best-effort - skip them if we've already burned our
    // budget. Without these we lose pool URL and per-ASIC stats but the
    // dashboard still gets hashrate/temp/power from "summary".
    if (millis() < totalDeadline) queryRaw(host, "pools", p,  timeoutMs);
    if (millis() < totalDeadline) queryRaw(host, "stats", st, timeoutMs);

    out.ok = true;

    // ---- Hashrate: prefer MHS 1m (already a 60-second moving average inside
    // the miner) over MHS 5s (jumpy "instant" reading that produces visible
    // 2x spikes on small miners). The dashboard further smooths with a
    // rolling-mean buffer per miner.
    // MHS values are in MH/s. Convert MH/s -> GH/s by dividing 1000.
    float mhs = parseEq(s, "MHS 1m");
    if (mhs == 0) mhs = parseEq(s, "MHS av");
    if (mhs == 0) mhs = parseEq(s, "MHS 5s");
    out.hashrate = mhs / 1000.0f;   // GH/s

    // ---- Counters
    out.accepted = (uint32_t) parseEq(s, "Accepted");
    out.rejected = (uint32_t) parseEq(s, "Rejected");
    out.uptime   = (uint32_t) parseEq(s, "Elapsed");
    out.bestDiff = parseEq(s, "Best Share");

    // ---- Temperature - TAvg = average chip temperature (matches HashWatcher's
    // main "Chip Temperature" reading). Falls back to TMax then OTemp.
    float t = parseBracket(st, "TAvg");
    if (t == 0) t = parseBracket(st, "TMax");
    if (t == 0) t = parseBracket(st, "OTemp");
    out.temp = t;

    // ---- Power - PS array, position 6 holds wattage directly in W
    // Array layout: PS[unused unused mV unused unused mA WATTS]
    // Example from Nano 3s: PS[0 0 27575 3 0 3782 94]  -> 94W
    out.power = (uint16_t) parsePsIndex(st, 6);

    // ---- Pool / worker (only from pools section, only the active pool)
    parseActivePoolUrl(p, out.poolUrl, sizeof(out.poolUrl));
    parseActivePoolUser(p, out.worker, sizeof(out.worker));

    return true;
  }

private:
  // ----- TCP query -----
  // Sends a single CGMiner command (e.g. "summary") and reads the reply.
  // Returns false on connect failure or if the reply is empty within timeoutMs.
  // Uses a hard deadline that bounds connect+read combined, so a slow ARP
  // lookup or unreachable host can't make this call hang longer than promised.
  static bool queryRaw(const char* host, const char* cmd,
                       String& reply, uint16_t timeoutMs) {
    WiFiClient client;
    client.setTimeout(timeoutMs);

    // ESP32's client.connect() can take longer than timeoutMs on a fresh
    // ARP miss. Use a non-blocking-ish approach: try to connect, but cap
    // the total budget for both connect+read at timeoutMs.
    uint32_t deadline = millis() + timeoutMs;
    if (!client.connect(host, 4028, timeoutMs)) return false;
    if (millis() >= deadline) { client.stop(); return false; }

    client.print(cmd);
    client.print('\n');

    reply = "";
    while (millis() < deadline && (client.connected() || client.available())) {
      while (client.available()) {
        reply += (char) client.read();
        if (reply.length() > 8192) {
          client.stop();
          return reply.length() > 0;
        }
      }
      delay(5);
    }
    client.stop();
    return reply.length() > 0;
  }

  // ----- KEY=value parser (handles ',' and '|' delimiters) -----
  static float parseEq(const String& src, const char* key) {
    String needle = String(key) + "=";
    int p = src.indexOf(needle);
    if (p < 0) return 0;
    p += needle.length();
    int e = p;
    while (e < (int) src.length() &&
           src[e] != ',' && src[e] != '|' && src[e] != '\0') e++;
    return src.substring(p, e).toFloat();
  }

  // ----- KEY[value] parser (square brackets, used in MM ID0 stats blob) -----
  static float parseBracket(const String& src, const char* key) {
    String needle = String(key) + "[";
    int p = src.indexOf(needle);
    if (p < 0) return 0;
    p += needle.length();
    int e = src.indexOf(']', p);
    if (e < 0) return 0;
    return src.substring(p, e).toFloat();
  }

  // ----- PS[a b c d e f g] - extract value at given index -----
  static float parsePsIndex(const String& src, int wantIdx) {
    int p = src.indexOf("PS[");
    if (p < 0) return 0;
    p += 3;
    int e = src.indexOf(']', p);
    if (e < 0) return 0;
    String inner = src.substring(p, e);

    int idx = 0;
    int start = 0;
    for (int i = 0; i <= (int) inner.length(); i++) {
      if (i == (int) inner.length() || inner[i] == ' ') {
        if (i > start) {
          if (idx == wantIdx) {
            return inner.substring(start, i).toFloat();
          }
          idx++;
        }
        start = i + 1;
      }
    }
    return 0;
  }

  // ----- Find URL of first Alive pool (Status=Alive) -----
  static void parseActivePoolUrl(const String& src, char* out, size_t sz) {
    out[0] = 0;
    int searchFrom = 0;
    while (searchFrom < (int) src.length()) {
      int poolStart = src.indexOf("POOL=", searchFrom);
      if (poolStart < 0) break;
      int poolEnd = src.indexOf('|', poolStart);
      if (poolEnd < 0) poolEnd = src.length();
      String chunk = src.substring(poolStart, poolEnd);

      if (chunk.indexOf("Status=Alive") >= 0) {
        int u = chunk.indexOf("URL=");
        if (u >= 0) {
          u += 4;
          int e = chunk.indexOf(',', u);
          if (e < 0) e = chunk.length();
          String v = chunk.substring(u, e);
          strlcpy(out, v.c_str(), sz);
          return;
        }
      }
      searchFrom = poolEnd + 1;
    }
  }

  // ----- Find User of first Alive pool -----
  static void parseActivePoolUser(const String& src, char* out, size_t sz) {
    out[0] = 0;
    int searchFrom = 0;
    while (searchFrom < (int) src.length()) {
      int poolStart = src.indexOf("POOL=", searchFrom);
      if (poolStart < 0) break;
      int poolEnd = src.indexOf('|', poolStart);
      if (poolEnd < 0) poolEnd = src.length();
      String chunk = src.substring(poolStart, poolEnd);

      if (chunk.indexOf("Status=Alive") >= 0) {
        int u = chunk.indexOf("User=");
        if (u >= 0) {
          u += 5;
          int e = chunk.indexOf(',', u);
          if (e < 0) e = chunk.length();
          String v = chunk.substring(u, e);
          strlcpy(out, v.c_str(), sz);
          return;
        }
      }
      searchFrom = poolEnd + 1;
    }
  }
};
