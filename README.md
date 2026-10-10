# DIGIWAVE

Firmware for the LilyGo T-LoRa Pager that turns it into a portable terminal for
digital radio modes. Audio goes to and from a radio; until the line-in/line-out
hardware exists, the built-in microphone and speaker are used.

| Mode | Receive | Transmit |
| --- | --- | --- |
| FT8, FT4 | yes | yes, with automatic QSO sequence and ADIF log |
| CW | yes, in a tunable 100 Hz window | yes, from the keyboard |
| RTTY (45.45 / 50 baud) | yes | yes |
| NAVTEX | yes | - |
| WEFAX (120 LPM) | yes, saved as BMP on the SD card | - |

FT8 and FT4 can be switched to CB (11 m) operation, which uses CB callsigns
(`26AT016`), the message sequence of WSJT-CB and the country list by division number.

Time comes from GPS when there is a fix, otherwise from the RTC. A Wi-Fi access
point serves a page for a phone with the main controls and the logbook download.

## Status

Work in progress. The signal processing is covered by host tests; on the device,
FT8 reception has been checked against real signals. NAVTEX and WEFAX have only
been tested against generated signals, and transmitting has not been tested on air.

## Building

PlatformIO with the pioarduino platform (Arduino-ESP32 3.3):

    pio run -e pager        # firmware
    pio test -e native      # host tests of the signal processing

The firmware is installed as one application next to others with the
[Launcher](https://github.com/bmorcelli/Launcher); `partitions.csv` describes the
layout for a merged image. Artwork in `DIGIWAVE_Firmware_Grafiken` is converted
with `python3 tools/make_assets.py`.

## Layout

- `lib/ft8core` - hardware-independent decoders, encoders and QSO logic
- `lib/ft8_lib` - vendored [kgoba/ft8_lib](https://github.com/kgoba/ft8_lib) (MIT)
- `src` - firmware: audio and PTT interfaces, engine, user interface, web interface
- `test` - host tests and reference recordings from ft8_lib

## Sources

- FT8/FT4 coding: ft8_lib by Karlis Goba (MIT)
- Board support: LilyGoLib by LilyGo (MIT)
- CB division list and message sequence: [WSJT-CB](https://github.com/vash909/WSJT-CB)
