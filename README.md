# uRSupreme

A Reticulum / LXMF node firmware focused on the **LilyGo T-Beam Supreme**
(ESP32-S3, 8 MB PSRAM, OLED, GPS, IMU, environmental sensors, AXP2101 power
management). Builds on
[microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware)
— which integrates the
[microReticulum](https://github.com/attermann/microReticulum) stack into
[RNode_Firmware](https://github.com/markqvist/RNode_Firmware) — and turns
the T-Beam Supreme into a **standalone Reticulum node you can talk to from
a browser**: no host computer, no companion app, no `rnsd` running on a
laptop somewhere. The board itself is the node, the gateway, and the chat
client.

## Why this fork

The T-Beam Supreme has enough memory and peripherals to run Reticulum
locally — PSRAM for the path table, an OLED for at-a-glance status, GPS
and an RTC for time discipline, a compass and IMU for orientation, a
BME280 for environmental telemetry, and an AXP2101 with charge control
and battery telemetry. Upstream RNode firmware treats devices as KISS
radios for an attached host. Upstream microReticulum_Firmware adds the
RNS stack and transport routing on-device, but stops short of a user
interface. **uRSupreme adds the user.**

That means an LXMF inbox/outbox running on the device, a web UI for
sending messages and attachments, multi-identity support, sensor /
position telemetry, discovery announces for community-map listeners
like [rmap.world](https://rmap.world), and a settings UI for everything
the radio and transport stack can do — all served from the device over
WiFi.

## Features

### Reticulum / LXMF on-device

- **Standalone RNS transport node** via embedded microReticulum — routes
  for other nodes without a host attached.
- **Multi-identity LXMF gateway** (up to 4 identities per device by
  default). Each identity has its own inbox, sent log, retention policy,
  display name, and announce schedule.
- **Attachments**: send and receive images and audio inline in the chat
  UI; image previews resize and re-encode in the browser before send;
  large attachments stream from PSRAM with a progress bar.
- **Ratchet support** (FN-style forward secrecy) for LXMF where the peer
  advertises it.
- **Discovery announces** with optional stamp PoW, advertising the
  device's interfaces, region settings, and (if GPS-locked) approximate
  location to community listeners.
- **Path-table sized for transport**: 2000 records across 8 × 96 KB
  PSRAM segments by default — comfortable for sitting on a busy network.

### Transports

- **LoRa** via SX1262 (Supreme V1) or LR1121 (Supreme LR variant).
  Region presets for EU, US, AU, NZ, RU, IN, KR; full manual override
  for frequency / bandwidth / SF / coding rate / TX power / airtime caps.
- **TCP client** transport — outbound TCP links to other Reticulum
  nodes for joining a remote network over the internet or a wired LAN
  bridge. (LR1121 variant only at present.)
- **WiFi**: station mode with multiple saved networks, automatic
  fallback to softAP for first-time setup or when no saved network is
  reachable. WiFi credentials and the rest of device-wide config persist
  across reboots and survive identity changes.

### Web UI (Alpine.js SPA, served from the device)

- **Single-page chat client** at `http://<device>/` — login per
  identity, conversation list, threaded view, compose with emoji picker,
  attach tray (camera capture, audio record, file pick), reactive
  unread / online / path-state indicators.
- **Identity switcher** for hopping between identities on the same
  device without re-logging-in.
- **Settings**: Identity (display name, announce cadence, attachments
  on/off), Connectivity (LoRa region + manual radio params, WiFi saved
  networks, TCP clients, Bluetooth, serial / KISS diagnostics),
  Discovery (master toggle, advertised name, cadence, stamp cost), Time
  (GPS / RTC / NTP source priority), App (UI prefs, inbox retention,
  storage), Activity (telemetry), Reset (per-identity delete + factory
  reset).
- **Live status**: battery icon updates in the topbar; radio status
  pill shows online / offline / not-configured; system popover gives a
  full breakdown of LoRa channel utilisation (own vs others), RSSI vs
  noise floor, uptime, and free heap / PSRAM.
- **Auth**: per-identity password (PBKDF2-hashed on device, bcrypt-style
  cost factor); bearer tokens issued at login; identity-code gating for
  physical-presence-required actions (WiFi reconfig, factory reset,
  enabling discovery).
- **No imperative DOM and no inline styles** — every view, modal,
  popover, list, and chip renders through Alpine directives against
  reactive store state; the entire stylesheet lives in
  `Web/spa/styles.css` and is served separately with content-hash cache
  busting.

### Sensors + telemetry

- **Battery** (AXP2101): voltage, charge / discharge current,
  percentage, charger state, USB-power detect.
- **Position** (L76K GPS): lat / lon / altitude / fix quality / HDOP /
  satellite count; used to discipline the RTC and to tag discovery
  announces.
- **Clock** (PCF8563 hardware RTC): kept in sync from GPS or NTP;
  source priority configurable in Settings → Time.
- **Environment** (BME280): temperature, humidity, pressure.
- **Motion** (QMI8658 IMU): orientation + tilt; available to apps but
  not yet surfaced in the SPA beyond raw telemetry.
- **Compass** (QMC6310): heading.
- All sensor reads are coalesced into a periodic WebSocket frame so the
  SPA stays live without polling.

### Storage

- **LittleFS on internal flash** for identities, config, inbox indexes,
  small attachments.
- **Optional SD card** for inbox overflow + large attachment bodies;
  switchable per-identity. Migrate-from-flash helper in Settings →
  Storage.
- **Per-chat retention policies** (forever / N days / last-N messages),
  with eviction at the device end rather than the client.

### Build / dev workflow

- **PlatformIO** project; build environments for both Supreme variants
  (`ttgo-t-beam-supreme` for SX1262, `ttgo-t-beam-supreme-lr1121` for
  LR1121) plus inherited build envs for the other RNode-targeted
  boards.
- **SPA embedded in the firmware image** via `extra_script.py` —
  `Web/spa/index.html`, `styles.css`, and the Alpine bundle are all
  gzipped into `Web/SPAEmbedded.h` at build time. A content hash of
  `styles.css` is substituted into the HTML so browsers refresh the
  stylesheet on every firmware ship.
- **PlayWright smoke tests** in `.venv/` (managed via `uv`) drive a
  real device for end-to-end checks.

## Hardware

Primary target:

- **LilyGo T-Beam Supreme V1** (SX1262) — env
  `ttgo-t-beam-supreme`
- **LilyGo T-Beam Supreme LR** (LR1121) — env
  `ttgo-t-beam-supreme-lr1121`, with `TCP_TRANSPORT` compiled in

Both ship with ESP32-S3 + 8 MB PSRAM + 8 MB flash + OLED + AXP2101 +
BME280 + QMI8658 + QMC6310 + L76K + PCF8563. The codebase inherits
microReticulum_Firmware's build envs for other RNode-style boards
(T-Beam classic, LilyGo T3-S3, T-Deck, Heltec, RAK4631, RAK11200,
RAK11300, RAK3112, NG-20/21, Lora32 variants) and they should still
build, but the Supreme is what gets the day-to-day testing, the web
UI, and the sensor integrations.

## Quick start

1. Install [PlatformIO](https://platformio.org) (CLI or VS Code
   extension).
2. Clone this repo and its sibling dependencies:
   ```sh
   git clone git@github.com:benagricola/uRSupreme.git
   git clone https://github.com/attermann/microReticulum.git
   git clone https://github.com/attermann/microStore.git
   ```
   The `microReticulum` and `microStore` paths are symlinked in
   `platformio.ini`; adjust the symlink targets if your layout
   differs.
3. Build + flash the variant matching your device, plugged in over USB:
   ```sh
   pio run -e ttgo-t-beam-supreme-lr1121 -t upload --upload-port /dev/ttyACM0
   # or, for the SX1262 Supreme:
   pio run -e ttgo-t-beam-supreme        -t upload --upload-port /dev/ttyACM0
   ```
4. First boot brings the device up in **softAP mode** with SSID
   `uRSupreme-<id>`. Connect to it, browse to `http://192.168.4.1/`,
   walk through the first-run setup (pick a region preset, set a
   password, join your WiFi).
5. Subsequent boots auto-join your WiFi. Find the device via mDNS at
   `http://rnode<idc>.local/` or via the IP shown on the OLED.

## Status + caveats

- This is a **fork for personal use** as much as it is a published
  project. Things change quickly; the `master` branch is what runs on
  the test rig.
- Stamp PoW: the worker is implemented but the firmware currently emits
  a zero stamp; strict listeners (e.g. rmap.world with
  `required_value=14`) will drop announces until the worker is wired
  through to live announces.
- The non-Supreme build envs are inherited from upstream and are not
  routinely tested here. If something on a non-Supreme board breaks,
  please open an issue but expect a slower turnaround than for Supreme
  bugs.

## Credits

- **Mark Qvist** — [Reticulum](https://github.com/markqvist/Reticulum)
  and [RNode_Firmware](https://github.com/markqvist/RNode_Firmware).
- **Aaron Attermann** —
  [microReticulum](https://github.com/attermann/microReticulum) and
  [microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware),
  on which this fork is built.
- **LilyGo / SensorLib / Adafruit / XPowersLib** — the driver libraries
  that make the Supreme's peripherals usable.

## Licence

GPLv3, inherited from RNode_Firmware. See the upstream repositories for
their respective licences.
