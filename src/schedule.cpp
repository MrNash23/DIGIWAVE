// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "schedule.h"

#include <LilyGoLib.h>
#include <SD.h>

#include <algorithm>

namespace schedule {
namespace {

// Deutscher Wetterdienst, WEFAX, as published in the NOAA list of radiofacsimile
// broadcasts (information dated 24 July 2024, after the DWD plan of October 2020).
// Between the charts there are voice broadcasts on 5905 and 6180 kHz.
const char kWefaxDefault[] =
    "# DIGIWAVE WEFAX schedule. One chart per line: start in UTC, then what it is.\n"
    "# Lines starting with # are notes. Source: DWD, NOAA list of 24 July 2024.\n"
    "Station: DWD Pinneberg\n"
    "Frequencies: 3855 7880 13882.5 kHz\n"
    "04:30 Surface analysis N Atlantic, Europe\n"
    "05:12 36 h forecast surface pressure\n"
    "05:25 Surface analysis, movement, weather, ice\n"
    "06:38 Tropical storms N Atlantic (in season)\n"
    "06:51 12/24 h 500 hPa, surface pressure\n"
    "07:04 12/24 h 850 hPa, 700 hPa humidity\n"
    "07:17 Repeat of 05:12\n"
    "07:30 48 h forecast surface pressure\n"
    "07:43 60 h forecast surface pressure\n"
    "08:04 84 h forecast surface pressure\n"
    "08:17 108 h forecast surface pressure\n"
    "08:30 24 h wind sea and swell, wind\n"
    "08:42 48 h wind sea and swell, wind\n"
    "08:54 72 h wind sea and swell, wind\n"
    "09:06 96 h wind sea and swell, wind\n"
    "09:30 36/48 h 500 hPa, surface pressure\n"
    "09:45 Sea surface temperature North Sea\n"
    "10:07 Ice chart western Baltic (if ice)\n"
    "10:29 48 h wave forecast N Atlantic\n"
    "10:50 Surface analysis N Atlantic, Europe\n"
    "11:11 36/48 h 850 hPa, 700 hPa humidity\n"
    "11:23 60/72 h 500 hPa, surface pressure\n"
    "11:35 60/72 h 850 hPa, 700 hPa humidity\n"
    "12:36 Repeat of 10:50\n"
    "12:56 Repeat of 05:12\n"
    "13:08 Repeat of 07:30\n"
    "13:20 Repeat of 07:43\n"
    "13:32 Repeat of 08:04\n"
    "13:44 Repeat of 08:17\n"
    "13:56 Repeat of 10:50\n"
    "14:25 Broadcast schedule, part 1\n"
    "14:45 Broadcast schedule, part 2\n"
    "15:08 Ice chart NW Atlantic\n"
    "15:20 Ice chart Baltic Sea\n"
    "15:40 Ice chart European Arctic\n"
    "16:36 Surface analysis N Atlantic, Europe\n"
    "18:00 Surface analysis, movement, weather, ice\n"
    "18:21 Tropical storms N Atlantic (in season)\n"
    "18:34 36 h forecast surface pressure\n"
    "18:47 48 h forecast surface pressure\n"
    "19:00 84 h forecast surface pressure\n"
    "19:13 24 h wind sea and swell, wind\n"
    "19:26 48 h wind sea and swell, wind\n"
    "19:39 72 h wind sea and swell, wind\n"
    "21:00 Ice chart NW Atlantic\n"
    "21:15 Ice chart Baltic Sea\n"
    "21:36 48 h wave forecast N Atlantic\n"
    "22:00 Surface analysis N Atlantic, Europe\n";

// Deutscher Wetterdienst, RTTY programme 1 in English, plan of September 2023.
// Warnings for the Baltic and the North Sea also come in between, as they are issued.
const char kRttyDefault[] =
    "# DIGIWAVE RTTY schedule. One broadcast per line: start in UTC, then what it is.\n"
    "# Lines starting with # are notes. Source: DWD programme 1 (English), September 2023.\n"
    "# 50 baud, 450 Hz shift. Warnings for the Baltic and North Sea come as issued.\n"
    "Station: DWD Pinneberg\n"
    "Frequencies: 4583 7646 10100.8 kHz  50 Bd 450 Hz\n"
    "00:00 Gale and storm warnings German waters\n"
    "00:05 Weather report North Sea and Baltic\n"
    "00:20 Weather report German coast\n"
    "00:30 Advice on the use of weather data\n"
    "00:35 SYNOP station reports (coded)\n"
    "02:00 SHIP reports (coded)\n"
    "03:00 Warnings for the sea areas\n"
    "03:05 Weather report North Sea and Baltic\n"
    "03:20 Weather report German coast\n"
    "03:30 5-day report Baltic Sea\n"
    "03:55 5-day report North Sea\n"
    "04:15 5-day report Mediterranean\n"
    "04:40 SHIP reports (coded)\n"
    "05:15 Navigational warnings North Sea, Baltic\n"
    "05:35 Weather report North Sea and Baltic\n"
    "05:50 Weather report German coast\n"
    "06:00 Warnings for the sea areas\n"
    "06:03 Advice on the use of weather data\n"
    "06:04 5-day report eastern Atlantic\n"
    "06:30 SYNOP station reports (coded)\n"
    "07:35 SHIP reports (coded)\n"
    "08:15 BUOY reports (coded)\n"
    "08:35 Weather report North Sea and Baltic\n"
    "08:50 Weather report German coast\n"
    "09:00 Warnings for the sea areas\n"
    "09:05 Weather report Norwegian Sea and Baltic\n"
    "09:30 Weather report North Atlantic\n"
    "09:55 Weather report Western European Sea\n"
    "10:15 Weather report Western Mediterranean\n"
    "10:35 SHIP reports (coded)\n"
    "11:10 Notices\n"
    "11:15 Weather report Eastern Mediterranean\n"
    "11:35 Weather report North Sea and Baltic\n"
    "11:50 Weather report German coast\n"
    "12:00 Warnings for the sea areas\n"
    "12:05 Advice on the use of weather data\n"
    "12:10 SYNOP station reports (coded)\n"
    "13:35 SHIP reports (coded)\n"
    "14:13 5-day report Mediterranean\n"
    "14:35 Weather report North Sea and Baltic\n"
    "14:50 Weather report German coast\n"
    "15:00 Warnings for the sea areas\n"
    "15:05 5-day report Baltic Sea\n"
    "15:30 5-day report North Sea\n"
    "15:50 Weather report Mediterranean, 24 h\n"
    "15:57 Weather report E Mediterranean (repeat)\n"
    "16:10 5-day report Mediterranean (repeat)\n"
    "16:35 SHIP reports (coded)\n"
    "17:15 Navigational warnings North Sea, Baltic\n"
    "17:35 Weather report North Sea and Baltic\n"
    "17:50 Weather report German coast\n"
    "18:00 Warnings for the sea areas\n"
    "18:03 Advice on the use of weather data\n"
    "18:04 5-day report eastern Atlantic\n"
    "18:30 SYNOP station reports (coded)\n"
    "19:35 SHIP reports (coded)\n"
    "20:15 BUOY reports (coded)\n"
    "20:35 Weather report North Sea and Baltic\n"
    "20:50 Weather report German coast\n"
    "21:00 Warnings for the sea areas\n"
    "21:05 Weather report Norwegian Sea and Baltic\n"
    "21:30 Weather report North Atlantic\n"
    "21:55 Weather report Western European Sea\n"
    "22:15 Weather report Western Mediterranean\n"
    "22:35 SHIP reports (coded)\n"
    "23:15 Weather report Eastern Mediterranean\n";

struct PlanData {
    const char *id;
    const char *path;
    const char *built_in;
    int longest;  // minutes
    Entry entries[kMaxEntries];
    int count = 0;
    char station[40] = "";
    char freqs[52] = "";
    bool on_card = false;
};

// A chart takes about ten minutes; the RTTY programme runs without a break
PlanData plans[kPlans] = {
    {"wefax", "/wefax_schedule.txt", kWefaxDefault, 15},
    {"rtty", "/rtty_schedule.txt", kRttyDefault, 90},
};

PlanData &data(Plan p)
{
    return plans[(int)p];
}

/// Takes one line of the text form. Returns false for a line that is neither a
/// note, a name nor a broadcast.
bool parseLine(PlanData &d, char *line)
{
    // Trim both ends
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    if (n == 0 || line[0] == '#') {
        return true;
    }
    if (strncasecmp(line, "Station:", 8) == 0) {
        const char *v = line + 8;
        while (*v == ' ') {
            ++v;
        }
        strlcpy(d.station, v, sizeof(d.station));
        return true;
    }
    if (strncasecmp(line, "Frequencies:", 12) == 0) {
        const char *v = line + 12;
        while (*v == ' ') {
            ++v;
        }
        strlcpy(d.freqs, v, sizeof(d.freqs));
        return true;
    }
    // "H:MM" or "HH:MM", also with a point, then the description
    int h = 0, m = 0, used = 0;
    if (sscanf(line, "%d%*[:.]%d%n", &h, &m, &used) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    if (d.count == kMaxEntries) {
        return true;  // the rest is dropped
    }
    const char *v = line + used;
    while (*v == ' ' || *v == '\t') {
        ++v;
    }
    Entry &e = d.entries[d.count++];
    e.minute = (uint16_t)(h * 60 + m);
    strlcpy(e.text, v, sizeof(e.text));
    return true;
}

/// Parses a whole text into the plan; returns the number of lines that were not understood.
int parseText(PlanData &d, const char *text)
{
    d.count = 0;
    d.station[0] = d.freqs[0] = '\0';
    int bad = 0;
    char line[128];
    while (*text) {
        size_t len = strcspn(text, "\n");
        size_t take = min(len, sizeof(line) - 1);
        memcpy(line, text, take);
        line[take] = '\0';
        if (!parseLine(d, line)) {
            ++bad;
        }
        text += len;
        if (*text == '\n') {
            ++text;
        }
    }
    std::sort(d.entries, d.entries + d.count, [](const Entry &a, const Entry &b) { return a.minute < b.minute; });
    return bad;
}

String readCard(const PlanData &d)
{
    String out;
    instance.lockSPI();
    if (SD.cardType() != CARD_NONE && SD.exists(d.path)) {
        File f = SD.open(d.path, FILE_READ);
        if (f) {
            out = f.readString();
            f.close();
        }
    }
    instance.unlockSPI();
    return out;
}

}  // namespace

const char *id(Plan p)
{
    return data(p).id;
}

bool fromId(const char *id, Plan *p)
{
    for (int i = 0; i < kPlans; ++i) {
        if (strcmp(id, plans[i].id) == 0) {
            *p = (Plan)i;
            return true;
        }
    }
    return false;
}

void load(Plan p)
{
    PlanData &d = data(p);
    String card = readCard(d);
    d.on_card = false;
    if (card.length() > 0) {
        parseText(d, card.c_str());
        d.on_card = d.count > 0;
    }
    if (!d.on_card) {
        parseText(d, d.built_in);
    }
}

int count(Plan p)
{
    return data(p).count;
}

const Entry &entry(Plan p, int i)
{
    const PlanData &d = data(p);
    return d.entries[constrain(i, 0, max(0, d.count - 1))];
}

const char *station(Plan p)
{
    return data(p).station;
}

const char *frequencies(Plan p)
{
    return data(p).freqs;
}

bool fromCard(Plan p)
{
    return data(p).on_card;
}

int longestMinutes(Plan p)
{
    return data(p).longest;
}

int next(Plan p, int minute_now)
{
    const PlanData &d = data(p);
    if (d.count == 0) {
        return -1;
    }
    for (int i = 0; i < d.count; ++i) {
        if (d.entries[i].minute > minute_now) {
            return i;
        }
    }
    return 0;  // the first one tomorrow
}

String text(Plan p)
{
    const PlanData &d = data(p);
    if (d.on_card) {
        String card = readCard(d);
        if (card.length() > 0) {
            return card;
        }
    }
    return String(d.built_in);
}

const char *save(Plan p, const String &text, bool *ok)
{
    PlanData &d = data(p);
    *ok = false;
    // Checked first; the plan in use is read again afterwards, so that a broken
    // text does not replace a working plan
    int bad = parseText(d, text.c_str());
    int found = d.count;
    load(p);
    if (found == 0) {
        return "Not saved: no line with a time like 04:30";
    }
    if (bad > 0) {
        static char msg[64];
        snprintf(msg, sizeof(msg), "Not saved: %d line%s not understood", bad, bad == 1 ? "" : "s");
        return msg;
    }
    instance.lockSPI();
    bool written = false;
    if (SD.cardType() != CARD_NONE) {
        File f = SD.open(d.path, FILE_WRITE);
        if (f) {
            written = f.print(text) == text.length();
            f.close();
        }
    }
    instance.unlockSPI();
    if (!written) {
        return "Not saved - SD card?";
    }
    load(p);
    *ok = true;
    static char msg[48];
    snprintf(msg, sizeof(msg), "Saved, %d broadcasts", d.count);
    return msg;
}

const char *restoreDefault(Plan p)
{
    const PlanData &d = data(p);
    instance.lockSPI();
    if (SD.cardType() != CARD_NONE && SD.exists(d.path)) {
        SD.remove(d.path);
    }
    instance.unlockSPI();
    load(p);
    return "Built-in plan restored";
}

}  // namespace schedule
