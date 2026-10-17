// Simulator stand-in for the ESP32 Preferences (NVS) library.
#pragma once
#include <map>
#include <string>
#include <stdint.h>
extern std::map<std::string, uint8_t> sim_nvs; // key -> value, namespace ignored
struct Preferences
{
  bool begin(const char *, bool) { return true; }
  void end() {}
  uint8_t getUChar(const char *k, uint8_t d)
  {
    auto it = sim_nvs.find(k);
    return it == sim_nvs.end() ? d : it->second;
  }
  size_t putUChar(const char *k, uint8_t v)
  {
    sim_nvs[k] = v;
    return 1;
  }
};
