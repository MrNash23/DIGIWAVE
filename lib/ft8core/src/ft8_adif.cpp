#include "ft8_adif.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace ft8 {

const char kAdifHeader[] = "DIGIWAVE log\n<adif_ver:5>3.1.4 <programid:8>DIGIWAVE <eoh>\n";

namespace {

struct Band {
    uint32_t lo, hi;
    const char *name;
};

const Band kBands[] = {
    {1800, 2000, "160m"},   {3500, 4000, "80m"},    {5351, 5367, "60m"},    {7000, 7300, "40m"},
    {10100, 10150, "30m"},  {14000, 14350, "20m"},  {18068, 18168, "17m"},  {21000, 21450, "15m"},
    {24890, 24990, "12m"},  {26965, 27405, "11m"},  {28000, 29700, "10m"},  {50000, 54000, "6m"},   {70000, 70500, "4m"},
    {144000, 148000, "2m"}, {430000, 440000, "70cm"},
};

size_t field(char *out, size_t size, size_t pos, const char *name, const char *value)
{
    if (value[0] == '\0' || pos >= size) {
        return pos;
    }
    int n = snprintf(out + pos, size - pos, "<%s:%u>%s ", name, (unsigned)strlen(value), value);
    return n < 0 ? pos : pos + (size_t)n;
}

}  // namespace

bool adifField(const char *record, const char *name, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t name_len = strlen(name);
    for (const char *p = strchr(record, '<'); p != nullptr; p = strchr(p + 1, '<')) {
        const char *tag = p + 1;
        bool match = true;
        for (size_t i = 0; i < name_len; ++i) {
            if (tolower((unsigned char)tag[i]) != tolower((unsigned char)name[i])) {
                match = false;
                break;
            }
        }
        if (!match || tag[name_len] != ':') {
            continue;
        }
        const char *close = strchr(tag, '>');
        if (close == nullptr) {
            return false;
        }
        size_t len = (size_t)atoi(tag + name_len + 1);
        size_t avail = strlen(close + 1);
        if (len > avail) {
            len = avail;
        }
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out, close + 1, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

bool gridFromLatLon(double lat, double lon, char *out)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        out[0] = '\0';
        return false;
    }
    // The poles and the date line belong to the last field, not to a 19th one
    double x = lon + 180.0;
    double y = lat + 90.0;
    if (x >= 360.0) {
        x = 359.999999;
    }
    if (y >= 180.0) {
        y = 179.999999;
    }
    int field_x = (int)(x / 20.0);
    int field_y = (int)(y / 10.0);
    out[0] = (char)('A' + field_x);
    out[1] = (char)('A' + field_y);
    out[2] = (char)('0' + (int)((x - field_x * 20.0) / 2.0));
    out[3] = (char)('0' + (int)(y - field_y * 10.0));
    out[4] = '\0';
    return true;
}

const char *bandName(uint32_t khz)
{
    for (const Band &b : kBands) {
        if (khz >= b.lo && khz <= b.hi) {
            return b.name;
        }
    }
    return nullptr;
}

size_t formatAdif(const QsoRecord &rec, const QsoConfig &cfg, uint32_t dial_hz, char *out, size_t out_size, Protocol protocol)
{
    char date_on[9], time_on[7], date_off[9], time_off[7], sent[4], rcvd[4];
    time_t t_on = rec.time_on;
    time_t t_off = rec.time_off;
    struct tm tm_on, tm_off;
    gmtime_r(&t_on, &tm_on);
    gmtime_r(&t_off, &tm_off);
    strftime(date_on, sizeof(date_on), "%Y%m%d", &tm_on);
    strftime(time_on, sizeof(time_on), "%H%M%S", &tm_on);
    strftime(date_off, sizeof(date_off), "%Y%m%d", &tm_off);
    strftime(time_off, sizeof(time_off), "%H%M%S", &tm_off);
    formatReport(rec.rst_sent, sent);
    formatReport(rec.rst_rcvd, rcvd);

    size_t pos = 0;
    out[0] = '\0';
    pos = field(out, out_size, pos, "call", rec.dx_call);
    pos = field(out, out_size, pos, "gridsquare", rec.dx_grid);
    if (protocol == Protocol::FT4) {
        // ADIF lists FT4 as a submode of MFSK
        pos = field(out, out_size, pos, "mode", "MFSK");
        pos = field(out, out_size, pos, "submode", "FT4");
    } else {
        pos = field(out, out_size, pos, "mode", "FT8");
    }
    pos = field(out, out_size, pos, "rst_sent", sent);
    pos = field(out, out_size, pos, "rst_rcvd", rcvd);
    pos = field(out, out_size, pos, "qso_date", date_on);
    pos = field(out, out_size, pos, "time_on", time_on);
    pos = field(out, out_size, pos, "qso_date_off", date_off);
    pos = field(out, out_size, pos, "time_off", time_off);
    if (dial_hz != 0) {
        const char *band = bandName(dial_hz / 1000);
        if (band != nullptr) {
            pos = field(out, out_size, pos, "band", band);
        }
        char freq[16];
        snprintf(freq, sizeof(freq), "%.6f", (dial_hz + (double)rec.audio_freq) / 1e6);
        pos = field(out, out_size, pos, "freq", freq);
    }
    pos = field(out, out_size, pos, "station_callsign", cfg.my_call);
    pos = field(out, out_size, pos, "my_gridsquare", cfg.my_grid);
    if (pos < out_size) {
        int n = snprintf(out + pos, out_size - pos, "<eor>\n");
        if (n > 0) {
            pos += (size_t)n;
        }
    }
    return pos < out_size ? pos : out_size - 1;
}

}  // namespace ft8
