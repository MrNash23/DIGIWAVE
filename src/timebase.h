// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <stdint.h>

/// UTC clock for FT8 slot timing. GPS with its PPS pulse is the reference;
/// the battery-backed RTC bridges the time without a fix.
namespace timebase {

enum class Source { None, Rtc, Gps, GpsPps };

void begin();

/// Reads the GPS and updates the clock. Call regularly from the main loop.
void poll();

bool valid();
Source source();
const char *sourceName();

/// UTC in milliseconds since 1970, including the manual correction.
int64_t utcMs();

/// Manual correction added to the clock, for operation without GPS.
void setOffsetMs(int32_t ms);

int satellites();

/// Prints the state of GPS reception and of the clock to the serial log.
void printDiagnostics();

}  // namespace timebase
