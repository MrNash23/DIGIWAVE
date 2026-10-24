// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "voice.h"

#include <Arduino.h>

#include <ft8_adif.h>

#include "assets/voice.h"
#include "engine.h"
#include "logbook.h"
#include "settings.h"

namespace voice {

namespace {

constexpr int kChunk = 240;            // 20 ms at 12 kHz
constexpr int kMaxWaitMs = 1000;       // an announcement waiting longer for the speaker is dropped
// On a busy band nearly every station is new; one announcement now and then is enough
constexpr uint32_t kNewStationGapMs = 2UL * 60UL * 1000UL;
constexpr uint32_t kSameStationGapMs = 60UL * 60UL * 1000UL;

static_assert(VOICE_SAMPLE_RATE == 12000, "the audio path runs at 12 kHz");

AudioIo *speaker = nullptr;
bool shared = false;
bool opened = false;          // the codec has been started; only the task changes it
bool radio_holds = false;     // only the engine task changes it
SemaphoreHandle_t speaker_lock = nullptr;
QueueHandle_t phrases = nullptr;

struct Announced {
    char call[14];
    uint32_t at;  // millis()
};
Announced announced[24];
int announced_next = 0;
uint32_t last_new_station = 0;

void play(const int16_t *pcm, size_t len)
{
    static const int16_t silence[kChunk] = {0};
    speaker->setOutputEnabled(true);
    for (size_t i = 0; i < len; i += kChunk) {
        speaker->write(pcm + i, min((size_t)kChunk, len - i));
    }
    // The tail leaves the output buffer before the amplifier goes off
    for (int i = 0; i < 4; ++i) {
        speaker->write(silence, kChunk);
    }
    speaker->setOutputEnabled(false);
}

void task(void *)
{
    for (;;) {
        Phrase p;
        if (xQueueReceive(phrases, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!opened) {
            // With the interface board the built-in codec is only started for announcements
            opened = speaker->begin();
            if (!opened) {
                Serial.println("[voice] speaker codec did not start");
                continue;
            }
        }
        if (shared && xSemaphoreTake(speaker_lock, pdMS_TO_TICKS(kMaxWaitMs)) != pdTRUE) {
            continue;  // the radio is transmitting through the speaker
        }
        // Without the board the codec volume is that of the radio path; it is restored afterwards
        speaker->setOutputVolume(settings.voice_volume);
        switch (p) {
        case Phrase::NewStation: play(voice_new_station, voice_new_station_len); break;
        case Phrase::CallingYou: play(voice_calling_you, voice_calling_you_len); break;
        case Phrase::QsoStarted: play(voice_qso_started, voice_qso_started_len); break;
        case Phrase::QsoLogged: play(voice_qso_logged, voice_qso_logged_len); break;
        }
        if (shared) {
            speaker->setOutputVolume(settings.volume);
            xSemaphoreGive(speaker_lock);
        }
    }
}

bool announcedRecently(const char *call)
{
    for (const Announced &a : announced) {
        if (a.call[0] != '\0' && strcmp(a.call, call) == 0 && millis() - a.at < kSameStationGapMs) {
            return true;
        }
    }
    return false;
}

void remember(const char *call)
{
    Announced &a = announced[announced_next];
    announced_next = (announced_next + 1) % (int)(sizeof(announced) / sizeof(announced[0]));
    strncpy(a.call, call, sizeof(a.call) - 1);
    a.call[sizeof(a.call) - 1] = '\0';
    a.at = millis();
}

/// A station calling CQ that is not in the logbook for this band and was not announced lately
const char *newStation(const engine::SlotNews &n)
{
    if (last_new_station != 0 && millis() - last_new_station < kNewStationGapMs) {
        return nullptr;
    }
    const char *band = ft8::bandName(settings.dialHz((int)engine::mode()) / 1000);
    for (int i = 0; i < n.cqs; ++i) {
        const char *call = n.cq_call[i];
        if (logbook::worked(call, band ? band : "") != 2 && !announcedRecently(call)) {
            return call;
        }
    }
    return nullptr;
}

}  // namespace

void begin(AudioIo &speaker_io, bool shared_with_radio)
{
    speaker = &speaker_io;
    shared = shared_with_radio;
    opened = shared;  // the engine's audio path is already started
    speaker_lock = xSemaphoreCreateMutex();
    phrases = xQueueCreate(1, sizeof(Phrase));
    xTaskCreatePinnedToCore(task, "voice", 4096, nullptr, 3, nullptr, 1);
}

void poll()
{
    engine::SlotNews n;
    while (engine::nextSlotNews(&n)) {
        if (!settings.voice) {
            continue;
        }
        // One announcement per slot, the most important
        if (n.qso_logged) {
            say(Phrase::QsoLogged);
        } else if (n.qso_started) {
            say(Phrase::QsoStarted);
        } else if (n.called) {
            say(Phrase::CallingYou);
        } else if (const char *call = newStation(n)) {
            remember(call);
            last_new_station = millis();
            Serial.printf("[voice] new station %s\n", call);
            say(Phrase::NewStation);
        }
    }
}

void say(Phrase p)
{
    if (phrases != nullptr) {
        xQueueSend(phrases, &p, 0);
    }
}

void radioTx(bool on)
{
    if (!shared || speaker_lock == nullptr || on == radio_holds) {
        return;
    }
    if (on) {
        xSemaphoreTake(speaker_lock, portMAX_DELAY);  // an announcement lasts under two seconds
    } else {
        xSemaphoreGive(speaker_lock);
    }
    radio_holds = on;
}

}  // namespace voice
