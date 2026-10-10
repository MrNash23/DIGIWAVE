#pragma once

#include <stdint.h>

namespace ft8 {

/// CB (11 m) callsigns as used by the DX clubs and by WSJT-CB: a division number
/// of 1-3 digits, 1-2 club letters and a unit number of 1-3 digits (26AT016), four
/// digits with a one-digit division (1AT1000), or a two-letter suffix (26AT/DX).
bool isCbCallsign(const char *call);

/// Division number at the start of a CB callsign, 0 if it has none.
int cbDivision(const char *call);

/// Country of a CB callsign by its division number, empty if unknown.
const char *cbCountryForCall(const char *call);

}  // namespace ft8
