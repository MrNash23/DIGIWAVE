#include <LilyGoLib.h>
#include <LV_Helper.h>

#include "engine.h"
#include "hal/audio_if.h"
#include "hal/ptt_if.h"
#include "settings.h"
#include "timebase.h"
#include "ui.h"

void setup()
{
    Serial.begin(115200);

    // The LoRa radio, NFC and motion sensor are not used by this firmware.
    // NO_INIT_FATFS: the library otherwise waits forever for a FAT partition in flash.
    uint32_t probe = instance.begin(NO_HW_LORA | NO_HW_NFC | NO_HW_SENSOR | NO_INIT_FATFS);
    Serial.printf("[dw] board up, devices 0x%06x\n", (unsigned)probe);
    beginLvglHelper(instance);
    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    ui::splash();

    settings.load();
    timebase::begin();
    Serial.printf("[dw] clock source %s\n", timebase::sourceName());

    AudioIo &audio = es8311Audio();
    if (!audio.begin()) {
        Serial.println("[dw] audio codec did not start");
    }
    Ptt &ptt = extensionPortPtt(settings.ptt_active_high);
    ptt.begin();

    engine::begin(audio, ptt);
    ui::begin();
    Serial.println("[dw] running");
}

// Diagnostic commands over USB serial: "m 0".."m 5" selects a mode,
// "s TEXT" sends text in CW or RTTY, "r N" records N seconds of received
// audio and prints it as hexadecimal 16-bit samples, "g 1" prints the state of
// GPS reception and of the clock, "n $GP..." feeds a GPS sentence to the clock.
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
                    Serial.printf("[mem] internal free %u (lowest %u), PSRAM free %u (lowest %u), uptime %lu s\n",
                                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram(),
                                  (unsigned)ESP.getMinFreePsram(), (unsigned long)(millis() / 1000));
                    Serial.printf("[gps] locator \"%s\", from %s\n", settings.grid, settings.grid_auto ? "GPS" : "manual entry");
                } else if (line[0] == 'r') {
                    engine::capture((size_t)constrain(atoi(line + 2), 1, 10) * 12000);
                    recording = true;
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
    ui::loop();
}
