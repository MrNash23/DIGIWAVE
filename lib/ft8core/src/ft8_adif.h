#pragma once

#include <stddef.h>

#include "ft8_qso.h"

namespace ft8 {

/// Header written once at the top of a new ADIF file.
extern const char kAdifHeader[];

/// Formats one QSO as an ADIF record (one line, terminated by <eor> and newline).
/// @param dial_hz dial frequency in Hz, 0 to omit band and frequency
/// @return number of characters written (excluding the terminator)
size_t formatAdif(const QsoRecord &rec, const QsoConfig &cfg, uint32_t dial_hz, char *out, size_t out_size,
                  Protocol protocol = Protocol::FT8);

/// Reads one field of an ADIF record ("<name:len>value", name compared without case).
/// Returns false if the field is missing; out is then empty.
bool adifField(const char *record, const char *name, char *out, size_t out_size);

/// Maidenhead locator (4 characters, e.g. "JO62") for a position in degrees.
/// out must hold 5 bytes. Returns false for coordinates outside the globe.
bool gridFromLatLon(double lat, double lon, char *out);

/// ADIF band name for a frequency in kHz ("20m"), or nullptr outside the amateur HF/VHF bands.
const char *bandName(uint32_t khz);

}  // namespace ft8
