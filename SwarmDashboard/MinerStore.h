/*
 * MinerStore.h - persistent miner list (NVS)
 */
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

struct MinerEntry {
  char    name[16];
  char    ip[40];
  uint8_t type;
};

class MinerStore {
public:
  void begin() {
    prefs.begin("miners", false);
    load();
  }

  uint8_t           count()    const { return _count; }
  const MinerEntry& get(uint8_t i) const { return _list[i]; }

  bool add(const MinerEntry& e) {
    if (_count >= MAX_MINERS) return false;
    _list[_count++] = e;
    save();
    return true;
  }

  bool update(uint8_t i, const MinerEntry& e) {
    if (i >= _count) return false;
    _list[i] = e;
    save();
    return true;
  }

  bool remove(uint8_t i) {
    if (i >= _count) return false;
    for (uint8_t j = i; j < _count - 1; j++) _list[j] = _list[j + 1];
    _count--;
    save();
    return true;
  }

  void clear() {
    _count = 0;
    prefs.remove("data");
  }

private:
  Preferences prefs;
  MinerEntry  _list[MAX_MINERS];
  uint8_t     _count = 0;

  void load() {
    String json = prefs.getString("data", "[]");
    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, json)) { _count = 0; return; }

    JsonArray arr = doc.as<JsonArray>();
    _count = 0;
    for (JsonObject o : arr) {
      if (_count >= MAX_MINERS) break;
      strlcpy(_list[_count].name, o["n"] | "", sizeof(_list[_count].name));
      strlcpy(_list[_count].ip,   o["i"] | "", sizeof(_list[_count].ip));
      _list[_count].type = o["t"] | 0;
      _count++;
    }
  }

  void save() {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < _count; i++) {
      JsonObject o = arr.createNestedObject();
      o["n"] = _list[i].name;
      o["i"] = _list[i].ip;
      o["t"] = _list[i].type;
    }
    String json;
    serializeJson(doc, json);
    prefs.putString("data", json);
  }
};
