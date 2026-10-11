// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

namespace ft8 {

/// Country (DXCC entity, short English name) for a callsign, derived from its prefix.
/// Returns an empty string if the prefix is not in the table.
///
/// The table follows the ITU prefix allocations plus the common exceptions
/// (islands and regions that count separately). It is not a full cty.dat.
const char *countryForCall(const char *call);

}  // namespace ft8
