# Third-party software and data

DIGIWAVE itself, Copyright (C) 2026 Andres Leroi, is licensed under the GNU General Public License, version 3 or
later (see `LICENSE`). It builds on the work listed here. The first table is
what this repository contains; the second is what the build fetches and links
into the firmware. Whoever passes on a built firmware passes on these parts
too and has to keep their notices.

## Contained in this repository

| Part | Where | Author | Licence |
| --- | --- | --- | --- |
| ft8_lib, FT8/FT4 coding and decoding | `lib/ft8_lib` | Kārlis Goba | MIT, text in `lib/ft8_lib/LICENSE` |
| Board definition and pin assignment of the T-LoRa Pager | `boards`, `variants` | Shenzhen Xinyuan Electronic Technology (LilyGo) | MIT |
| 11 m DX division list | `lib/ft8core/src/cb_mode.cpp` | taken from WSJT-CB, a derivative of WSJT-X by Joe Taylor K1JT and the WSJT Development Group | GPL-3.0 |
| Message sequence for CB operation | `lib/ft8core/src/ft8_qso.cpp` | follows the convention of WSJT-CB; own implementation | - |
| Recordings of FT8 signals with their expected decodes, for the tests | `test/wav` | from the test set of ft8_lib | MIT |
| Pulse shaping of the FT8/FT4 signal | `lib/ft8core/src/ft8_encoder.cpp` | follows `demo/gen_ft8.c` of ft8_lib | MIT |

FT8 and FT4 are protocols by Joe Taylor K1JT and Steve Franke K9AN.

## Fetched by the build and linked into the firmware

| Part | Version | Author | Licence |
| --- | --- | --- | --- |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) | 3.3 | Espressif Systems and contributors | LGPL-2.1 |
| [ESP-IDF](https://github.com/espressif/esp-idf), with FreeRTOS, lwIP, mbedTLS, NimBLE and others | 5.5 | Espressif Systems and the authors of the components | Apache-2.0; components under MIT, BSD and Apache-2.0; radio libraries in binary form under the Espressif licence |
| [LilyGoLib](https://github.com/Xinyuan-LilyGO/LilyGoLib) | 0.2.1 | Shenzhen Xinyuan Electronic Technology | MIT |
| [LVGL](https://github.com/lvgl/lvgl) | 9.2.2 | LVGL Kft | MIT |
| Montserrat typeface, as part of LVGL | | The Montserrat Project Authors | SIL Open Font License 1.1 |
| [RadioLib](https://github.com/jgromes/RadioLib) | 7.1.2 | Jan Gromeš | MIT |
| [SensorLib](https://github.com/lewisxhe/SensorLib), with the sensor interface of Bosch Sensortec | 0.3.1 | Lewis He; Bosch Sensortec | MIT; BSD-3-Clause |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | 0.2.9 | Lewis He | MIT |
| [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus) | 1.1.0 | Mikal Hart | LGPL-2.1 |
| [Adafruit TCA8418](https://github.com/adafruit/Adafruit_TCA8418), [Adafruit BusIO](https://github.com/adafruit/Adafruit_BusIO) | 1.0.2, 1.17.4 | Adafruit Industries | BSD; MIT |

The NFC reader library of STMicroelectronics, which the board library
normally pulls in, is deliberately kept out of the build (see
`src/hal/nfc_absent.h`): the firmware does not use NFC, and that library comes
under a licence of its own.

The libraries under the LGPL can be replaced by anyone: the whole firmware is
built from source with `pio run -e pager`.

