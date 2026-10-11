#pragma once

#include <stdint.h>

namespace ft8 {

/// Where FT8 and FT4 are usually found on a band. The radio is tuned by hand,
/// so these are offered for telling the logbook which frequency is in use.
struct BandPreset {
    const char *band;    // "40 m"
    uint32_t ft8_hz;     // dial frequency (USB), 0 if the mode has no usual place on the band
    uint32_t ft4_hz;
    const char *remark;  // shown behind the frequency, may be empty
};

/// The usual frequencies of the amateur bands from 160 m to 2 m, or with cb the
/// one used on the citizens band.
int bandPresetCount(bool cb);
const BandPreset &bandPreset(bool cb, int index);

/// "14.074", or with a fourth decimal where the frequency needs it ("7.0475"). out must hold 12 bytes.
void formatMhz(uint32_t hz, char *out);

}  // namespace ft8
