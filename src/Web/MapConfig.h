// Map settings, persisted to /map.json.
//
// mode selects whether the web app shows a map:
//   off - no map preview/modal, position shows as text only
//   sd  - the device SD card serves the vector map layers, the default
//
// The layers are .pmtiles archives under /maps/layers (a world base plus
// any downloaded detail areas), listed by /api/map/layers. default_zoom is
// where the modal opens.
#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"

namespace Web {
namespace MapConfig {

enum Mode : uint8_t { OFF = 0, SD = 1 };

inline constexpr const char* CONFIG_PATH      = "/map.json";
inline constexpr uint8_t DEFAULT_ZOOM = 16;
inline constexpr uint8_t MAX_ZOOM     = 19;
inline constexpr const char* DEFAULT_PMTILES = "/maps/basemap.pmtiles";

struct Config {
  uint8_t     mode         = SD;
  std::string pmtiles      = DEFAULT_PMTILES;
  uint8_t     default_zoom = DEFAULT_ZOOM;
};

inline Config& config() { static Config c; return c; }

inline const char* mode_str(uint8_t m) {
  return m == OFF ? "off" : "sd";
}
inline uint8_t mode_from(const char* s, uint8_t def) {
  if (!s) return def;
  if (!strcmp(s, "off")) return OFF;
  if (!strcmp(s, "sd"))  return SD;
  return def;
}

// Guard a single file path (the .pmtiles): absolute, no "..".
inline std::string sanitize_file(const std::string& p) {
  if (p.empty() || p[0] != '/' || p.find("..") != std::string::npos)
    return DEFAULT_PMTILES;
  return p;
}

inline void load(microStore::FileSystem& fs) {
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  Config& c = config();
  c.mode         = mode_from(doc["mode"] | "sd", SD);
  c.pmtiles      = sanitize_file((const char*)(doc["pmtiles"] | DEFAULT_PMTILES));
  c.default_zoom = doc["default_zoom"] | DEFAULT_ZOOM;
  if (c.default_zoom < 1)        c.default_zoom = 1;
  if (c.default_zoom > MAX_ZOOM) c.default_zoom = MAX_ZOOM;
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = config();
  Common::PsramJsonDocument doc;
  doc["mode"]         = mode_str(c.mode);
  doc["pmtiles"]      = c.pmtiles;
  doc["default_zoom"] = c.default_zoom;
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()), out.length());
}

}  // namespace MapConfig
}  // namespace Web
