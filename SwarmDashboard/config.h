/*
 * config.h - compile-time constants for Swarm Dashboard
 */
#pragma once
#include <Arduino.h>

#define DEFAULT_POLL_MS    10000
#define MAX_MINERS         16
#define DASHBOARD_ROWS     12

// Uncomment ONCE to wipe stored WiFi credentials, then re-comment.
// #define CLEAR_WIFI_ON_BOOT

enum MinerType : uint8_t {
  TYPE_BITAXE   = 0,
  TYPE_NERDAXE  = 1,
  TYPE_AVALON   = 2,
};

inline const char* minerTypeName(uint8_t t) {
  switch (t) {
    case TYPE_BITAXE:  return "Bitaxe";
    case TYPE_NERDAXE: return "NerdAxe";
    case TYPE_AVALON:  return "Avalon";
    default:           return "?";
  }
}
