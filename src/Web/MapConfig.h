// Map tile settings, persisted to /map.json.
//
// mode selects where the web app pulls tiles from:
//   off    - no map preview/modal, position shows as text only
//   sd     - the device SD card (/api/map/tile), the default
//   online - the browser fetches from online_url directly (opt-in; this
//            leaks the viewed position to the tile host, so it is never
//            the default and the user turns it on in the Map settings)
//
// maps_dir is the slippy-tile root on the card; the firmware reads tiles
// from <maps_dir>/{z}/{x}/{y}.png. default_zoom is where the modal opens.
#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"

namespace Web {
namespace MapConfig {

enum Mode : uint8_t { OFF = 0, SD = 1, ONLINE = 2 };

inline constexpr const char* CONFIG_PATH      = "/map.json";
inline constexpr const char* DEFAULT_MAPS_DIR = "/maps";
inline constexpr const char* DEFAULT_ONLINE_URL =
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
inline constexpr uint8_t DEFAULT_ZOOM = 16;
inline constexpr uint8_t MAX_ZOOM     = 19;
inline constexpr const char* DEFAULT_PMTILES = "/maps/basemap.pmtiles";

// Tile encoding. RASTER = a {z}/{x}/{y}.png pyramid (simple, lets the device
// scope an arbitrary viewport on download). VECTOR = one .pmtiles archive
// (compact, infinitely zoomable, rendered by protomaps-leaflet).
enum Format : uint8_t { RASTER = 0, VECTOR = 1 };

struct Config {
  uint8_t     mode         = SD;
  uint8_t     format       = RASTER;
  std::string maps_dir     = DEFAULT_MAPS_DIR;
  std::string pmtiles      = DEFAULT_PMTILES;
  uint8_t     default_zoom = DEFAULT_ZOOM;
  std::string online_url   = DEFAULT_ONLINE_URL;
};

inline Config& config() { static Config c; return c; }

inline const char* mode_str(uint8_t m) {
  return m == OFF ? "off" : (m == ONLINE ? "online" : "sd");
}
inline uint8_t mode_from(const char* s, uint8_t def) {
  if (!s) return def;
  if (!strcmp(s, "off"))    return OFF;
  if (!strcmp(s, "online")) return ONLINE;
  if (!strcmp(s, "sd"))     return SD;
  return def;
}
inline const char* format_str(uint8_t f) { return f == VECTOR ? "vector" : "raster"; }
inline uint8_t format_from(const char* s, uint8_t def) {
  if (!s) return def;
  if (!strcmp(s, "vector")) return VECTOR;
  if (!strcmp(s, "raster")) return RASTER;
  return def;
}

// Clamp a maps_dir to an absolute single-segment-ish path; reject empties
// and anything that doesn't start at root so a tile path can't escape.
inline std::string sanitize_dir(const std::string& d) {
  if (d.empty() || d[0] != '/' || d.find("..") != std::string::npos)
    return DEFAULT_MAPS_DIR;
  std::string out = d;
  while (out.size() > 1 && out.back() == '/') out.pop_back();   // no trailing /
  return out;
}
// Same guard for a single file path (the .pmtiles): absolute, no "..".
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
  c.format       = format_from(doc["format"] | "raster", RASTER);
  c.maps_dir     = sanitize_dir((const char*)(doc["maps_dir"] | DEFAULT_MAPS_DIR));
  c.pmtiles      = sanitize_file((const char*)(doc["pmtiles"] | DEFAULT_PMTILES));
  c.default_zoom = doc["default_zoom"] | DEFAULT_ZOOM;
  if (c.default_zoom < 1)        c.default_zoom = 1;
  if (c.default_zoom > MAX_ZOOM) c.default_zoom = MAX_ZOOM;
  std::string u = (const char*)(doc["online_url"] | "");
  if (!u.empty()) c.online_url = u;
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = config();
  Common::PsramJsonDocument doc;
  doc["mode"]         = mode_str(c.mode);
  doc["format"]       = format_str(c.format);
  doc["maps_dir"]     = c.maps_dir;
  doc["pmtiles"]      = c.pmtiles;
  doc["default_zoom"] = c.default_zoom;
  doc["online_url"]   = c.online_url;
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()), out.length());
}

}  // namespace MapConfig
}  // namespace Web
