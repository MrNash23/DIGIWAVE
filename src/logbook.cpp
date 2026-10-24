// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "logbook.h"

#include <LilyGoLib.h>
#include <SD.h>

#include <cb_mode.h>
#include <ft8_adif.h>
#include <ft8_country.h>

#include "settings.h"

namespace logbook {

const char kPath[] = "/digiwave.adi";
const char kStationPath[] = "/station.adi";

namespace {

Row *rows = nullptr;
int row_count = 0;
int total_count = 0;

Row *station_rows = nullptr;
int station_row_count = 0;
int station_count = 0;

// Both lists merged by time, built when first asked for after a change
const Row **merged = nullptr;
int merged_count = 0;
bool merged_dirty = true;

// Every station in the file with the band it was worked on, for marking it
// in the list of decodes. Kept for the whole file, not only for the rows shown.
struct Worked {
    char call[14];
    char band[6];
};
constexpr int kMaxWorked = 6000;
Worked *worked_list = nullptr;
int worked_count = 0;

void addWorked(const char *call, const char *record)
{
    if (worked_list == nullptr) {
        worked_list = (Worked *)ps_calloc(kMaxWorked, sizeof(Worked));
    }
    if (worked_list != nullptr && worked_count < kMaxWorked) {
        Worked &w = worked_list[worked_count++];
        strncpy(w.call, call, sizeof(w.call) - 1);
        w.call[sizeof(w.call) - 1] = '\0';
        ft8::adifField(record, "band", w.band, sizeof(w.band));
    }
}

Stats st{};
char st_today[9] = "";
constexpr int kMaxCountries = 400;
const char **seen_countries = nullptr;  // names from the country table, compared by address

/// Takes one record into the figures.
void tally(const char *record)
{
    char v[16], my[8], dx[8];
    ++st.total;
    if (ft8::adifField(record, "qso_date", v, sizeof(v))) {
        if (st.first_date[0] == '\0' || strcmp(v, st.first_date) < 0) {
            strcpy(st.first_date, v);
        }
        if (strcmp(v, st.last_date) > 0) {
            strcpy(st.last_date, v);
        }
        if (strcmp(v, st_today) == 0) {
            ++st.today;
        }
    }
    if (ft8::adifField(record, "band", v, sizeof(v))) {
        int i = 0;
        while (i < st.bands && strcmp(st.band[i].name, v) != 0) {
            ++i;
        }
        if (i == st.bands && st.bands < 12) {
            strncpy(st.band[st.bands++].name, v, sizeof(st.band[0].name) - 1);
        }
        if (i < st.bands) {
            ++st.band[i].count;
        }
    }
    if (ft8::adifField(record, "submode", v, sizeof(v)) && strcmp(v, "FT4") == 0) {
        ++st.ft4;
    } else {
        ++st.ft8;
    }
    char call[14];
    if (ft8::adifField(record, "call", call, sizeof(call))) {
        addWorked(call, record);
        const char *country = ft8::isCbCallsign(call) ? ft8::cbCountryForCall(call) : ft8::countryForCall(call);
        if (seen_countries == nullptr) {
            seen_countries = (const char **)ps_calloc(kMaxCountries, sizeof(char *));
        }
        if (country != nullptr && country[0] != '\0' && seen_countries != nullptr) {
            int i = 0;
            while (i < st.countries && seen_countries[i] != country) {
                ++i;
            }
            if (i == st.countries && st.countries < kMaxCountries) {
                seen_countries[st.countries++] = country;
            }
        }
        ft8::adifField(record, "my_gridsquare", my, sizeof(my));
        ft8::adifField(record, "gridsquare", dx, sizeof(dx));
        int km = ft8::gridDistanceKm(my, dx);
        if (km >= 0) {
            ++st.with_distance;
            st.km_sum += km;
            if (km > st.km_max) {
                st.km_max = km;
                strcpy(st.km_max_call, call);
            }
        }
    }
}

/// Takes a record into a list that keeps the newest kMaxRows. Returns the row, nullptr without memory.
Row *keepRow(Row *&list, int &n, const char *record)
{
    if (list == nullptr) {
        list = (Row *)ps_calloc(kMaxRows, sizeof(Row));
        if (list == nullptr) {
            return nullptr;
        }
    }
    if (n == kMaxRows) {
        memmove(list, list + 1, sizeof(Row) * (kMaxRows - 1));
        --n;
    }
    Row &r = list[n++];
    ft8::adifField(record, "qso_date", r.date, sizeof(r.date));
    ft8::adifField(record, "time_on", r.time, sizeof(r.time));
    ft8::adifField(record, "call", r.call, sizeof(r.call));
    ft8::adifField(record, "gridsquare", r.grid, sizeof(r.grid));
    ft8::adifField(record, "rst_sent", r.sent, sizeof(r.sent));
    ft8::adifField(record, "rst_rcvd", r.rcvd, sizeof(r.rcvd));
    ft8::adifField(record, "band", r.band, sizeof(r.band));
    char my[8];
    ft8::adifField(record, "my_gridsquare", my, sizeof(my));
    r.km = ft8::gridDistanceKm(my, r.grid);
    // FT4 is logged as submode of MFSK
    if (!ft8::adifField(record, "submode", r.mode, sizeof(r.mode))) {
        ft8::adifField(record, "mode", r.mode, sizeof(r.mode));
    }
    r.station = false;
    merged_dirty = true;
    return &r;
}

void addRow(const char *record)
{
    keepRow(rows, row_count, record);
    ++total_count;
    tally(record);
}

void addStationRow(const char *record, const char *call)
{
    Row *r = keepRow(station_rows, station_row_count, record);
    if (r != nullptr) {
        r->station = true;
    }
    ++station_count;
    addWorked(call, record);
}

/// Reads one file of records, one to a line as append() and CBScope write them.
void readFile(const char *path, bool station)
{
    File f = SD.open(path, FILE_READ);
    if (!f) {
        return;
    }
    static char line[400];
    while (f.available()) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        char call[14];
        if (ft8::adifField(line, "call", call, sizeof(call))) {
            if (station) {
                addStationRow(line, call);
            } else {
                addRow(line);
            }
        }
    }
    f.close();
}

bool before(const Row *a, const Row *b)
{
    int d = strcmp(a->date, b->date);
    return d < 0 || (d == 0 && strcmp(a->time, b->time) < 0);
}

void buildMerged()
{
    merged_dirty = false;
    merged_count = 0;
    if (merged == nullptr) {
        merged = (const Row **)ps_calloc(2 * kMaxRows, sizeof(Row *));
        if (merged == nullptr) {
            return;
        }
    }
    int a = 0, b = 0;
    while (a < row_count || b < station_row_count) {
        bool take_own = b == station_row_count || (a < row_count && !before(&station_rows[b], &rows[a]));
        merged[merged_count++] = take_own ? &rows[a++] : &station_rows[b++];
    }
}

}  // namespace

bool load()
{
    row_count = 0;
    total_count = 0;
    station_row_count = 0;
    station_count = 0;
    merged_dirty = true;
    worked_count = 0;
    st = Stats{};
    // The card shares the SPI bus with the display
    instance.lockSPI();
    bool ok = SD.cardType() != CARD_NONE;
    if (ok) {
        readFile(kPath, false);
        readFile(kStationPath, true);
    }
    instance.unlockSPI();
    return ok;
}

bool append(const ft8::QsoRecord &rec, ft8::Protocol protocol)
{
    ft8::QsoConfig cfg;
    strncpy(cfg.my_call, settings.call, sizeof(cfg.my_call) - 1);
    strncpy(cfg.my_grid, settings.grid, sizeof(cfg.my_grid) - 1);
    char line[400];
    ft8::formatAdif(rec, cfg, settings.dialHz(protocol == ft8::Protocol::FT4 ? 1 : 0), line, sizeof(line), protocol);
    Serial.printf("[log] %s", line);
    addRow(line);

    instance.lockSPI();
    bool ok = false;
    if (SD.cardType() != CARD_NONE) {
        bool is_new = !SD.exists(kPath);
        File f = SD.open(kPath, FILE_APPEND);
        if (f) {
            if (is_new) {
                f.print(ft8::kAdifHeader);
            }
            ok = f.print(line) == strlen(line);
            f.close();
        }
    }
    instance.unlockSPI();
    return ok;
}

int total()
{
    return total_count;
}

int count()
{
    return row_count;
}

int stationTotal()
{
    return station_count;
}

int countAll()
{
    if (merged_dirty) {
        buildMerged();
    }
    return merged_count;
}

const Row &rowAll(int index)
{
    if (merged_dirty) {
        buildMerged();
    }
    return *merged[index];
}

const Row &row(int index)
{
    return rows[index];
}

int worked(const char *call, const char *band)
{
    int result = 0;
    for (int i = 0; i < worked_count && result < 2; ++i) {
        if (strcmp(worked_list[i].call, call) == 0) {
            result = strcmp(worked_list[i].band, band) == 0 ? 2 : 1;
        }
    }
    return result;
}

const Stats &stats(const char *today)
{
    // The count of the day belongs to one date; on another one the file is read again
    if (strcmp(today, st_today) != 0) {
        strncpy(st_today, today, sizeof(st_today) - 1);
        load();
    }
    return st;
}

}  // namespace logbook
