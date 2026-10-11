#pragma once

#include <stdint.h>

namespace ft8 {

/// What the list of frequencies is for; the order is that of the modes of the firmware.
enum class BandUse { FT8, FT4, CW, RTTY, NAVTEX, WEFAX };

/// A frequency where a mode is usually found, or a station to listen to. The
/// radio is tuned by hand, so these are offered for telling the device (and
/// the logbook) which frequency is in use.
struct BandPreset {
    const char *label;   // band ("40 m") or station
    uint32_t hz;         // FT8, FT4, CW, RTTY: dial frequency. NAVTEX, WEFAX: frequency of the station
    const char *remark;  // shown behind the frequency, may be empty
};

/// The usual frequencies for a mode. With cb, FT8 and FT4 get the one used on
/// the citizens band instead of the amateur bands.
int bandPresetCount(BandUse use, bool cb);
const BandPreset &bandPreset(BandUse use, bool cb, int index);

/// "14.074", or with a fourth decimal where the frequency needs it ("7.0475"). out must hold 12 bytes.
void formatMhz(uint32_t hz, char *out);

/// "518" or "13882.5": kilohertz, as broadcast stations are listed. out must hold 12 bytes.
void formatKhz(uint32_t hz, char *out);

}  // namespace ft8
