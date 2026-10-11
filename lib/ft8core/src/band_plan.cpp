#include "band_plan.h"

#include <stdio.h>

namespace ft8 {

namespace {

const BandPreset kFt8[] = {
    {"160 m", 1840000, ""}, {"80 m", 3573000, ""},  {"60 m", 5357000, ""},  {"40 m", 7074000, ""},
    {"30 m", 10136000, ""}, {"20 m", 14074000, ""}, {"17 m", 18100000, ""}, {"15 m", 21074000, ""},
    {"12 m", 24915000, ""}, {"10 m", 28074000, ""}, {"6 m", 50313000, ""},  {"2 m", 144174000, ""},
};

// FT4 has no usual place on 160 m and 60 m
const BandPreset kFt4[] = {
    {"80 m", 3575000, ""},  {"40 m", 7047500, ""},  {"30 m", 10140000, ""}, {"20 m", 14080000, ""}, {"17 m", 18104000, ""},
    {"15 m", 21140000, ""}, {"12 m", 24919000, ""}, {"10 m", 28180000, ""}, {"6 m", 50318000, ""},  {"2 m", 144170000, ""},
};

// As used with WSJT-CB
const BandPreset kCb[] = {
    {"11 m", 27265000, "channel 26"},
};

// Centres of low-power CW activity; on 6 m and 2 m the calling frequencies
const BandPreset kCw[] = {
    {"160 m", 1836000, ""}, {"80 m", 3560000, ""},  {"40 m", 7030000, ""},  {"30 m", 10116000, ""},
    {"20 m", 14060000, ""}, {"17 m", 18086000, ""}, {"15 m", 21060000, ""}, {"12 m", 24906000, ""},
    {"10 m", 28060000, ""}, {"6 m", 50090000, ""},  {"2 m", 144050000, ""},
};

// Amateur RTTY has segments rather than fixed frequencies; these lie in them.
// The weather service DWD sends on the others around the clock.
const BandPreset kRtty[] = {
    {"80 m", 3590000, ""},
    {"40 m", 7040000, ""},
    {"30 m", 10142000, ""},
    {"20 m", 14085000, ""},
    {"17 m", 18105000, ""},
    {"15 m", 21090000, ""},
    {"12 m", 24925000, ""},
    {"10 m", 28090000, ""},
    {"DWD weather", 4583000, "50 Bd, shift 450 Hz"},
    {"DWD weather", 7646000, "50 Bd, shift 450 Hz"},
    {"DWD weather", 10100800, "50 Bd, shift 450 Hz"},
    {"DWD weather", 11039000, "50 Bd, shift 450 Hz"},
    {"DWD weather", 14467300, "50 Bd, shift 450 Hz"},
};

const BandPreset kNavtex[] = {
    {"International", 518000, "English"},
    {"National", 490000, "local language"},
    {"Shortwave", 4209500, ""},
};

const BandPreset kWefax[] = {
    {"DWD Hamburg", 3855000, ""},     {"DWD Hamburg", 7880000, ""},     {"DWD Hamburg", 13882500, ""},
    {"Northwood", 2618500, ""},       {"Northwood", 4610000, ""},       {"Northwood", 8040000, ""},
    {"Northwood", 11086500, ""},
};

struct List {
    const BandPreset *items;
    int count;
};

template <int N>
constexpr List listOf(const BandPreset (&items)[N])
{
    return {items, N};
}

List listFor(BandUse use, bool cb)
{
    switch (use) {
    case BandUse::FT8: return cb ? listOf(kCb) : listOf(kFt8);
    case BandUse::FT4: return cb ? listOf(kCb) : listOf(kFt4);
    case BandUse::CW: return listOf(kCw);
    case BandUse::RTTY: return listOf(kRtty);
    case BandUse::NAVTEX: return listOf(kNavtex);
    case BandUse::WEFAX: break;
    }
    return listOf(kWefax);
}

}  // namespace

int bandPresetCount(BandUse use, bool cb)
{
    return listFor(use, cb).count;
}

const BandPreset &bandPreset(BandUse use, bool cb, int index)
{
    return listFor(use, cb).items[index];
}

void formatMhz(uint32_t hz, char *out)
{
    if (hz % 1000 == 0) {
        snprintf(out, 12, "%u.%03u", (unsigned)(hz / 1000000), (unsigned)(hz / 1000 % 1000));
    } else {
        snprintf(out, 12, "%u.%04u", (unsigned)(hz / 1000000), (unsigned)(hz / 100 % 10000));
    }
}

void formatKhz(uint32_t hz, char *out)
{
    if (hz % 1000 == 0) {
        snprintf(out, 12, "%u", (unsigned)(hz / 1000));
    } else {
        snprintf(out, 12, "%u.%u", (unsigned)(hz / 1000), (unsigned)(hz / 100 % 10));
    }
}

}  // namespace ft8
