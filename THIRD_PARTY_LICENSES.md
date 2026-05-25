# Third-party software notices

μRSupreme is published as a whole under GPL-3.0 (see [LICENSE](LICENSE)).
The `.factory.bin` images released on GitHub bundle compiled object code
from several upstream libraries. Each of those libraries keeps its
original license; this file enumerates them so users who pull the binary
or extract specific portions know what they're entitled to do with each
piece.

If you only care about the project as a whole, the GPL-3.0 terms in
[LICENSE](LICENSE) cover it.

## What we directly fork

| Project | License | Notes |
|---|---|---|
| [microReticulum_Firmware](https://github.com/attermann/microReticulum_Firmware) (Aaron Attermann) | GPL-3.0 | Our direct parent. μRSupreme inherits its license. |
| [RNode_Firmware](https://github.com/markqvist/RNode_Firmware) (Mark Qvist) | GPL-3.0 | microReticulum_Firmware's parent; the same GPL-3.0 chain. |

## What the build links in

These come from `lib_deps` in `platformio.ini` and are compiled into the
final binary.

| Library | License | Copyright |
|---|---|---|
| [microReticulum](https://github.com/attermann/microReticulum) | Apache-2.0 | Aaron Attermann |
| [microStore](https://github.com/attermann/microStore) | Apache-2.0 | Aaron Attermann |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) (ESP32Async fork) | LGPL-3.0 | Hristo Gochkov + contributors |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | LGPL-3.0 | Hristo Gochkov + contributors |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT | Benoit Blanchon |
| [MsgPack](https://github.com/hideakitai/MsgPack) | MIT | Hideaki Tai |
| [SensorLib](https://github.com/lewisxhe/SensorLib) | MIT | Lewis He |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | MIT | Lewis He |
| [Adafruit_BME280_Library](https://github.com/adafruit/Adafruit_BME280_Library) | BSD-2-Clause | Adafruit Industries (Limor Fried, Kevin Townsend) |
| [Adafruit_SH110x](https://github.com/adafruit/Adafruit_SH110x) | BSD-2-Clause | Adafruit Industries |
| [Adafruit_Sensor](https://github.com/adafruit/Adafruit_Sensor) | Apache-2.0 | Adafruit Industries |
| [Crypto](https://github.com/attermann/Crypto) (Attermann fork) | MIT | Rhys Weatherley + fork contributions |

## Framework

| Framework | License | Notes |
|---|---|---|
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | LGPL-2.1-or-later + per-file headers | Bundled by PlatformIO when `framework = arduino`. |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache-2.0 + per-component licenses | Pulled in transitively by arduino-esp32. |

## Compatibility notes

- **MIT and BSD-2-Clause** code is GPL-3.0-compatible; it can be combined
  into a GPL-3.0 work without changing the original portion's license.
- **Apache-2.0** is GPL-3.0-compatible (one-way: Apache-2.0 → GPL-3.0).
- **LGPL-3.0** is GPL-3.0-compatible by design. The static linking the
  embedded build does is permitted because we also publish the full
  source the binary is built from (this repository); rebuilding from
  source with a different version of the LGPL library is the relinking
  freedom LGPL guarantees.

## Related but not bundled

[Reticulum](https://github.com/markqvist/Reticulum) (Mark Qvist) is the
reference Python implementation of the protocol this firmware speaks. It
is **not** linked into our binary. We use Aaron Attermann's clean-room
C++ port ([microReticulum](https://github.com/attermann/microReticulum),
Apache-2.0) instead.

Reticulum itself ships under a custom license (MIT-style with two
additional restrictions: a "no use to harm humans" clause and a "no use
in AI / ML / language-model training datasets" clause). Those
restrictions don't pass through to this firmware because we don't
redistribute Reticulum code, but anyone planning to ingest data
collected by Reticulum nodes into an ML dataset should read the
[Reticulum LICENSE](https://github.com/markqvist/Reticulum/blob/master/LICENSE)
first.

## Adding a new dependency

Before adding a library to `lib_deps`:

1. Note its license here with a row in the appropriate table.
2. Confirm it's GPL-3.0-compatible (Apache-2.0, MIT, BSD-2/3-Clause,
   ISC, LGPL-3.0, or GPL-3.0 itself).
3. AGPL, GPL-2.0-only (no "or later"), proprietary, or no-license-stated
   code can't be combined with our GPL-3.0 distribution. If you need
   something in that bucket, talk about it in an issue before adding it.
