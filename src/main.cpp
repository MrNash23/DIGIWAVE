// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include <LilyGoLib.h>
#include <LV_Helper.h>

#include "engine.h"
#include "hal/audio_if.h"
#include "hal/ptt_if.h"
#include "settings.h"
#include "timebase.h"
#include "ui.h"
#include "voice.h"
#include "web.h"

// Drawing with LVGL needs more than the 8 kB the main task gets by default
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

void setup()
{
    Serial.begin(115200);

    // The LoRa radio, NFC and motion sensor are not used by this firmware.
    // NO_INIT_FATFS: the library otherwise waits forever for a FAT partition in flash.
    uint32_t probe = instance.begin(NO_HW_LORA | NO_HW_NFC | NO_HW_SENSOR | NO_INIT_FATFS);
    // The PTT line is driven low before anything else; the library leaves it alone
    // (see the variant's pins_arduino.h), so it has been low since power-on.
    settings.load();
    Ptt &ptt = extensionPortPtt(settings.ptt_active_high);
    ptt.begin();
    Serial.printf("[dw] board up, devices 0x%06x\n", (unsigned)probe);
    // The library switches everything on; the NFC chip and the LoRa radio only
    // draw current here. The speaker amplifier is switched by the audio driver.
    instance.io.digitalWrite(EXPANDS_NFC_EN, LOW);
    instance.io.digitalWrite(EXPANDS_LORA_EN, LOW);
    beginLvglHelper(instance);
    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    ui::splash();

    timebase::begin();
    Serial.printf("[dw] clock source %s\n", timebase::sourceName());

    // The interface board is used when it is plugged in and chosen in the setup;
    // otherwise, or if its codec does not start, the built-in one.
    bool board = settings.audio_board && interfaceBoardPresent();
    if (board && !interfaceAudio().begin()) {
        Serial.println("[dw] interface board codec did not start");
        board = false;
    }
    AudioIo &audio = board ? interfaceAudio() : es8311Audio();
    if (!board && !audio.begin()) {
        Serial.println("[dw] audio codec did not start");
    }
    if (board) {
        instance.io.digitalWrite(EXPANDS_AMP_EN, LOW);  // the speaker stays silent
    }
    audio_on_board = board;
    Serial.printf("[dw] audio %s\n", board ? "interface board" : "built-in");
    // Announcements use the speaker, which is the path to the radio without the board
    voice::begin(es8311Audio(), !board);
    engine::begin(audio, ptt);
    ui::begin();
    Serial.println("[dw] running");
}

// Diagnostic commands over USB serial: "m 0".."m 5" selects a mode,
// "s TEXT" sends text in CW or RTTY, "r N" records N seconds of received
// audio and prints it as hexadecimal 16-bit samples, "f 1" saves the WEFAX picture received so far, "g 1" prints the state of
// GPS reception and of the clock, "n $GP..." feeds a GPS sentence to the clock,
// "v 0".."v 3" plays an announcement.
void pollSerialCommands()
{
    static char line[96];
    static size_t len = 0;
    static bool recording = false;

    size_t count = 0;
    const int16_t *pcm = recording ? engine::captured(&count) : nullptr;
    if (pcm != nullptr) {
        recording = false;
        Serial.printf("\n[cap] begin %u\n", (unsigned)count);
        for (size_t i = 0; i < count; ++i) {
            Serial.printf("%04x%s", (uint16_t)pcm[i], (i % 32 == 31) ? "\n" : "");
        }
        Serial.println("\n[cap] end");
    }

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            if (len >= 3 && line[1] == ' ') {
                if (line[0] == 'n') {
                    // A GPS sentence as if it came from the receiver, to test the clock without reception
                    for (const char *p = line + 2; *p; ++p) {
                        instance.gps.encode(*p);
                    }
                    instance.gps.encode('\r');
                    instance.gps.encode('\n');
                } else if (line[0] == 'm') {
                    ui::selectMode(atoi(line + 2));
                } else if (line[0] == 'l') {
                    // Sets the locator, "l -" clears it
                    strncpy(settings.grid, line[2] == '-' ? "" : line + 2, 4);
                    settings.grid[4] = '\0';
                    settings.save();
                    engine::applySettings();
                } else if (line[0] == 'g') {
                    timebase::printDiagnostics();
                    ui::printBattery();
                    Serial.printf("[mem] internal free %u (lowest %u), PSRAM free %u (lowest %u), uptime %lu s\n",
                                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram(),
                                  (unsigned)ESP.getMinFreePsram(), (unsigned long)(millis() / 1000));
                    Serial.printf("[gps] locator \"%s\", from %s\n", settings.grid, settings.grid_auto ? "GPS" : "manual entry");
                    Serial.printf("[log] frequency FT8 %u, FT4 %u, CB FT8 %u, CB FT4 %u, CW %u, RTTY %u, NAVTEX %u, WEFAX %u Hz\n",
                                  (unsigned)settings.dial_ft8_hz, (unsigned)settings.dial_ft4_hz, (unsigned)settings.dial_cb_ft8_hz,
                                  (unsigned)settings.dial_cb_ft4_hz, (unsigned)settings.dial_cw_hz, (unsigned)settings.dial_rtty_hz,
                                  (unsigned)settings.dial_navtex_hz, (unsigned)settings.dial_wefax_hz);
                } else if (line[0] == 'f') {
                    // Saves the WEFAX picture received so far and tells how long that took
                    uint32_t t0 = millis();
                    unsigned lines = ui::saveFaxNowForTest();
                    Serial.printf("[fax] saved %u lines in %u ms; PSRAM free %u (lowest %u), internal free %u (lowest %u)\n", lines,
                                  (unsigned)(millis() - t0), (unsigned)ESP.getFreePsram(), (unsigned)ESP.getMinFreePsram(),
                                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
                } else if (line[0] == 'r') {
                    engine::capture((size_t)constrain(atoi(line + 2), 1, 60) * 12000);
                    recording = true;
                } else if (line[0] == 'w') {
                    web::printDiagnostics();
                } else if (line[0] == 'v') {
                    voice::say((voice::Phrase)constrain(atoi(line + 2), 0, 3));
                } else if (line[0] == 's') {
                    Serial.printf("[dw] send: %s\n", ui::submitText(line + 2) ? "queued" : "mode cannot send");
                }
            }
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = c;
        }
    }
}

void loop()
{
    timebase::poll();
    pollSerialCommands();
    voice::poll();
    ui::loop();
}
