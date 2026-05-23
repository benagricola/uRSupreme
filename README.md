# μRSupreme

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
interface.

**μRSupreme adds the user interface.**

That means an LXMF inbox / outbox running on the device, a web UI for
sending messages and attachments, multi-identity support, sensor /
position telemetry, discovery announces for community-map listeners like
[rmap.world](https://rmap.world), and a settings UI for everything the
radio and transport stack can do — all served from the device over WiFi.

## Features

### Reticulum / LXMF on-device

- **Standalone RNS transport node** — routes for other nodes without a
  host attached.
- **Multi-identity LXMF**: up to 4 identities per device, each with
  their own inbox, sent log, retention policy, display name, and
  announce schedule.
- **Attachments**: send and receive images and audio inline in the chat
  UI; images resize before send; sends and receives show a progress
  bar.
- **Forward secrecy** for LXMF — in-flight messages still decrypt
  across key rotations.
- **Discovery announces** with PoW stamps (default cost 14 leading-zero
  bits, configurable in Settings → Discovery; set cost to 0 to disable).
  Announces advertise the device's interfaces, region settings, and (if
  GPS-locked) approximate location to community listeners.

### Transports

- **LoRa** via SX1262 (Supreme V1) or LR1121 (Supreme LR variant).
  Region presets for EU, US, AU, NZ, RU, IN, KR; full manual override
  for frequency / bandwidth / SF / coding rate / TX power / airtime
  caps.
- **TCP client transport** — outbound TCP links to other Reticulum
  nodes, for joining a remote network over the internet or a wired LAN
  bridge. Available on both Supreme variants.
- **WiFi**: station mode with one saved network, automatic fallback to
  softAP for first-time setup or when the saved network is unreachable.
- **BLE** for serial / KISS over Bluetooth Low Energy.

### Web UI

- **Single-page chat client** at `http://<device>/` — login per
  identity, conversation list, threaded view, compose with emoji picker,
  attach tray (camera capture, audio record, file pick), reactive
  unread / online / path-state indicators.
- **Identity switcher** for hopping between identities on the same
  device without re-logging-in.
- **Settings**:
  - *Identity*: display name, announce cadence, attachments on/off
  - *Connectivity*: LoRa region + manual radio params, WiFi credentials,
    TCP clients, BLE, serial / KISS diagnostics
  - *Discovery*: master toggle, advertised name, cadence, stamp cost
  - *Time*: GPS / RTC / NTP source priority
  - *App*: UI prefs, per-chat retention defaults
  - *Activity*: telemetry + max send / receive size sliders
  - *Reset*: per-identity delete + factory reset
- **Live status** in the top bar — battery icon, radio status pill
  (online / offline / not-configured), and a system popover with LoRa
  channel utilisation (own vs others), RSSI vs noise floor, uptime,
  free heap / PSRAM, and the SD-migrate helper.
- **Auth**: per-identity password (hashed on device); identity-code
  gating for physical-presence-required actions (WiFi reconfig, factory
  reset, enabling discovery).

### Sensors + telemetry

- **Battery** (AXP2101): voltage, charge / discharge current, percentage,
  charger state, USB-power detect.
- **Position** (L76K GPS): lat / lon / altitude / fix quality / HDOP /
  satellite count. Used to discipline the RTC and tag discovery
  announces.
- **Clock** (PCF8563 hardware RTC): synced from GPS or NTP; source
  priority configurable.
- **Environment** (BME280): temperature, humidity, pressure.
- **Motion** (QMI8658 IMU): orientation + tilt.
- **Compass** (QMC6310): heading.

### Storage

- **Optional SD card**: auto-detected; when present, attachments overflow
  to it. A migrate-from-flash helper lives in the system popover (the
  CPU icon in the top bar).
- **Per-chat retention policies**: forever / N days / last-N messages.

## Hardware

Primary targets — both ESP32-S3, both 8 MB PSRAM, both shipped with the
full sensor + power-management complement (OLED, AXP2101, BME280,
QMI8658, QMC6310, L76K, PCF8563):

- **LilyGo T-Beam Supreme V1** (SX1262) — PIO env `ttgo-t-beam-supreme`
- **LilyGo T-Beam Supreme LR** (LR1121) — PIO env
  `ttgo-t-beam-supreme-lr1121`

The codebase inherits microReticulum_Firmware's PIO envs for other
RNode-style boards (T-Beam classic, LilyGo T3-S3, T-Deck, Heltec V2/V3/V4,
RAK4631, NG-20/21, Lora32 variants, etc.), and they should still build,
but the Supreme is what gets the day-to-day testing, the web UI, and the
sensor integrations.

## Quick start

For a device you just want to use — no toolchain, no compiler.

1. Plug your T-Beam Supreme into your computer over USB-C.
2. Install [esptool](https://docs.espressif.com/projects/esptool/):
   ```sh
   pipx install esptool       # or: pip install --user esptool
   ```
3. Grab the latest release from the
   [Releases page](https://github.com/benagricola/uRSupreme/releases).
   You need the `.factory.bin` for your variant:
   - **SX1262** (T-Beam Supreme V1) → `urSupreme-sx1262-<version>.factory.bin`
   - **LR1121** (T-Beam Supreme LR) → `urSupreme-lr1121-<version>.factory.bin`
4. Find the USB port:
   - Linux: `/dev/ttyACM0` (or `ACM1`, …)
   - macOS: `/dev/cu.usbmodemXXXXXX`
   - Windows: `COM3` (or higher; check Device Manager)
5. Flash:
   ```sh
   esptool.py --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 urSupreme-sx1262-<version>.factory.bin
   ```
   `--erase-all` is *not* needed — the factory image already covers the
   full flash layout.
6. First boot brings the device up in **softAP mode** with SSID
   `RNode XXXX` (four hex chars from the BT MAC, also shown on the
   OLED). Join that network, open `http://10.0.0.1/`, and walk through
   the first-run setup: pick a region preset, set a password, join
   your home WiFi.
7. Subsequent boots auto-join WiFi. Reach the device via mDNS at
   `http://rnodexxxx.local/` (same four hex chars, lowercase) or by
   the IP shown on the OLED.

For an OTA-style upgrade where the device already runs μRSupreme,
download the `.bin` (without `.factory`) and flash it at offset
`0x10000` instead.

## Developer setup

For building from source.

1. Install [PlatformIO](https://platformio.org) (CLI or VS Code
   extension).
2. Clone this repo and its sibling dependencies into the **same parent
   directory** — the build expects `microReticulum` and `microStore`
   on the matching `ur-patches` branches to sit next to `uRSupreme`:
   ```sh
   mkdir uRSupreme-build && cd uRSupreme-build
   git clone git@github.com:benagricola/uRSupreme.git
   git clone -b ur-patches git@github.com:benagricola/microReticulum.git
   git clone -b ur-patches git@github.com:benagricola/microStore.git
   cd uRSupreme
   ```
3. Build + flash the variant matching your device, plugged in over USB:
   ```sh
   pio run -e ttgo-t-beam-supreme-lr1121 -t upload --upload-port /dev/ttyACM0
   # or, for the SX1262 Supreme:
   pio run -e ttgo-t-beam-supreme        -t upload --upload-port /dev/ttyACM0
   ```

`master` carries the most recent work; tagged releases (`v*`) trigger
the CI that publishes the `.factory.bin` artefacts the Quick start
section uses.

## Status + caveats

- This is a **fork for personal use** as much as it is a published
  project. Things change quickly; the `master` branch is what runs on
  the test rig.
- Only one WiFi network can be saved at a time — re-running first-run
  setup replaces it.
- Stamp PoW is on by default (cost 14). Disabling it (cost 0 in
  Settings → Discovery) will cause strict listeners like rmap.world to
  drop the announce.
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
- **LilyGo / Lewis He** — hardware and the
  [SensorLib](https://github.com/lewisxhe/SensorLib) +
  [XPowersLib](https://github.com/lewisxhe/XPowersLib) drivers for the
  T-Beam Supreme peripherals.
- **Adafruit** — BME280 / SH110X / Unified Sensor libraries.
- **ESP32Async** — the maintained
  [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) and
  [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer)
  forks that the web stack relies on.
- **Benoit Blanchon** —
  [ArduinoJson](https://github.com/bblanchon/ArduinoJson).
- **Hideaki Tai** — [MsgPack for Arduino](https://github.com/hideakitai/MsgPack).

## Licence

GPLv3, inherited from RNode_Firmware. See the upstream repositories for
their respective licences.
