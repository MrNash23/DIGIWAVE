// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "timebase.h"

#include <LilyGoLib.h>
#include <esp_timer.h>

namespace timebase {

namespace {

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// utc = base_ms + (now_us - ref_us) / 1000
int64_t base_ms = 0;
int64_t ref_us = 0;
Source src = Source::None;
int32_t offset_ms = 0;

volatile int64_t pps_us = 0;
volatile uint32_t pps_count = 0;
uint32_t pps_seen = 0;
uint32_t last_rtc_write = 0;
bool rtc_written = false;
uint32_t last_gps_sync = 0;  // millis() of the last time the clock was set from GPS

// GPS time is only trusted together with a position fix in the same second or the one before
constexpr uint32_t kMaxFixAgeMs = 2500;
// After the last fix the clock keeps running on the processor's crystal, which drifts
// by about 20 ms in this time. Then it counts as RTC quality again.
constexpr uint32_t kGpsHoldMs = 20UL * 60UL * 1000UL;
// Delay between the start of a second and its NMEA sentence, used without PPS
constexpr int kNmeaDelayMs = 150;

void IRAM_ATTR onPps()
{
    pps_us = esp_timer_get_time();
    pps_count = pps_count + 1;
}

int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t epochSeconds(int y, int mo, int d, int h, int mi, int s)
{
    return daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

void setClock(int64_t utc_ms, int64_t at_us, Source s)
{
    portENTER_CRITICAL(&mux);
    base_ms = utc_ms;
    ref_us = at_us;
    src = s;
    portEXIT_CRITICAL(&mux);
}

void readRtc()
{
    // The RTC counts whole seconds; wait for a tick to get the sub-second phase
    struct tm first, now;
    instance.rtc.getDateTime(&first);
    now = first;
    uint32_t start = millis();
    while (now.tm_sec == first.tm_sec && millis() - start < 1200) {
        delay(5);
        instance.rtc.getDateTime(&now);
    }
    int64_t at_us = esp_timer_get_time();
    if (now.tm_year + 1900 < 2025 || now.tm_sec == first.tm_sec) {
        return;  // never set, or not running
    }
    int64_t secs = epochSeconds(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
    setClock(secs * 1000, at_us, Source::Rtc);
}

void writeRtc()
{
    // Set the RTC right after a second boundary so that it ticks in phase with UTC.
    // Only called while GPS is the source, so utcMs() is the plain GPS time.
    int64_t ms = utcMs();
    if (ms % 1000 > 50) {
        return;
    }
    time_t secs = ms / 1000;
    struct tm t;
    gmtime_r(&secs, &t);
    instance.rtc.setDateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    last_rtc_write = millis();
    rtc_written = true;
}

}  // namespace

void begin()
{
    readRtc();
    pinMode(GPS_PPS, INPUT);
    attachInterrupt(digitalPinToInterrupt(GPS_PPS), onPps, RISING);
}

void poll()
{
    GPS &gps = instance.gps;
    gps.loop();

    if (gps.time.isUpdated()) {
        (void)gps.time.value();  // clears the updated flag
        // The receiver also reports a time without reception, from its own clock, which
        // may be seconds off. Only a sentence that carries a position fix is trusted:
        // the library updates the position from such sentences alone, in the same
        // step as the time.
        bool fresh_fix = gps.location.isUpdated();
        if (fresh_fix) {
            (void)gps.location.lat();  // clears the updated flag
        }
        bool fix = fresh_fix && gps.location.isValid() && gps.location.age() < kMaxFixAgeMs && gps.date.isValid() &&
                   gps.date.year() >= 2025;
        if (fix) {
            int64_t now_us = esp_timer_get_time();
            int64_t secs = epochSeconds(gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(),
                                        gps.time.second());
            int64_t sentence_ms = secs * 1000 + gps.time.centisecond() * 10;

            portENTER_CRITICAL(&mux);
            int64_t pps = pps_us;
            uint32_t count = pps_count;
            portEXIT_CRITICAL(&mux);

            int64_t since_pps_us = now_us - pps;
            if (count != pps_seen && since_pps_us >= 0 && since_pps_us < 900000 && gps.time.centisecond() == 0) {
                // The sentence names the second that began with the last pulse
                pps_seen = count;
                setClock(sentence_ms, pps, Source::GpsPps);
                last_gps_sync = millis();
            } else if (source() != Source::GpsPps || since_pps_us > 3000000) {
                setClock(sentence_ms, now_us - (int64_t)gps.time.age() * 1000 - kNmeaDelayMs * 1000, Source::Gps);
                last_gps_sync = millis();
            }
        }
    }

    // Without a fix the module keeps sending sentences, so their arrival says nothing;
    // what counts is how long ago the clock was last set from a fix.
    if ((src == Source::Gps || src == Source::GpsPps) && millis() - last_gps_sync > kGpsHoldMs) {
        portENTER_CRITICAL(&mux);
        src = Source::Rtc;
        portEXIT_CRITICAL(&mux);
    }

    if ((src == Source::Gps || src == Source::GpsPps) && (!rtc_written || millis() - last_rtc_write > 3600000UL)) {
        writeRtc();
    }
}

bool valid()
{
    return source() != Source::None;
}

Source source()
{
    portENTER_CRITICAL(&mux);
    Source s = src;
    portEXIT_CRITICAL(&mux);
    return s;
}

const char *sourceName()
{
    switch (source()) {
    case Source::GpsPps: return "GPS+";
    case Source::Gps: return "GPS";
    case Source::Rtc: return "RTC";
    case Source::None: break;
    }
    return "----";
}

int64_t utcMs()
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&mux);
    int64_t ms = base_ms + (now_us - ref_us) / 1000;
    // The manual correction is for the RTC; GPS time needs none
    if (src != Source::Gps && src != Source::GpsPps) {
        ms += offset_ms;
    }
    portEXIT_CRITICAL(&mux);
    return ms;
}

void setOffsetMs(int32_t ms)
{
    portENTER_CRITICAL(&mux);
    offset_ms = ms;
    portEXIT_CRITICAL(&mux);
}

void printDiagnostics()
{
    GPS &gps = instance.gps;
    portENTER_CRITICAL(&mux);
    uint32_t pulses = pps_count;
    int64_t last_pps = pps_us;
    portEXIT_CRITICAL(&mux);
    int64_t now = utcMs();
    Serial.printf("[gps] utc_ms %lld, last sync %d s ago\n", (long long)now, last_gps_sync ? (int)((millis() - last_gps_sync) / 1000) : -1);
    Serial.printf("[gps] chars %u, sentences ok %u / failed %u, sats %d, fix %s (age %u ms), time %02d:%02d:%02d (age %u ms), "
                  "date %s, pps %u (last %d ms ago), clock %s\n",
                  (unsigned)gps.charsProcessed(), (unsigned)gps.passedChecksum(), (unsigned)gps.failedChecksum(), satellites(),
                  gps.location.isValid() ? "yes" : "no", (unsigned)gps.location.age(), gps.time.hour(), gps.time.minute(),
                  gps.time.second(), (unsigned)gps.time.age(), gps.date.isValid() && gps.date.year() >= 2025 ? "ok" : "none",
                  (unsigned)pulses, pulses ? (int)((esp_timer_get_time() - last_pps) / 1000) : -1, sourceName());
}

int satellites()
{
    return instance.gps.satellites.isValid() ? (int)instance.gps.satellites.value() : 0;
}

}  // namespace timebase
