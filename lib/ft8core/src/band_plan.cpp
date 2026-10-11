#include "band_plan.h"

#include <stdio.h>

namespace ft8 {

namespace {

const BandPreset kAmateur[] = {
    {"160 m", 1840000, 0, ""},
    {"80 m", 3573000, 3575000, ""},
    {"60 m", 5357000, 0, ""},
    {"40 m", 7074000, 7047500, ""},
    {"30 m", 10136000, 10140000, ""},
    {"20 m", 14074000, 14080000, ""},
    {"17 m", 18100000, 18104000, ""},
    {"15 m", 21074000, 21140000, ""},
    {"12 m", 24915000, 24919000, ""},
    {"10 m", 28074000, 28180000, ""},
    {"6 m", 50313000, 50318000, ""},
    {"2 m", 144174000, 144170000, ""},
};

// As used with WSJT-CB
const BandPreset kCb[] = {
    {"11 m", 27265000, 27265000, "channel 26"},
};

}  // namespace

int bandPresetCount(bool cb)
{
    return cb ? (int)(sizeof(kCb) / sizeof(kCb[0])) : (int)(sizeof(kAmateur) / sizeof(kAmateur[0]));
}

const BandPreset &bandPreset(bool cb, int index)
{
    return cb ? kCb[index] : kAmateur[index];
}

void formatMhz(uint32_t hz, char *out)
{
    if (hz % 1000 == 0) {
        snprintf(out, 12, "%u.%03u", (unsigned)(hz / 1000000), (unsigned)(hz / 1000 % 1000));
    } else {
        snprintf(out, 12, "%u.%04u", (unsigned)(hz / 1000000), (unsigned)(hz / 100 % 10000));
    }
}

}  // namespace ft8
