// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <stdint.h>

/// UTC clock for FT8 slot timing. GPS with its PPS pulse is the reference;
/// without a fix the time of a phone on the web interface stands in, and the
/// battery-backed RTC bridges the time in between.
namespace timebase {

enum class Source { None, Rtc, Phone, Gps, GpsPps };

void begin();

/// Reads the GPS and updates the clock. Call regularly from the main loop.
void poll();

bool valid();
Source source();
const char *sourceName();

/// UTC in milliseconds since 1970, including the manual correction.
int64_t utcMs();

/// Corrects the clock by what a phone measured against utcMs(), and writes the RTC.
/// Ignored while GPS sets the clock; returns whether the correction was taken.
bool setFromPhone(int64_t adjust_ms);

/// Manual correction added to the clock while it runs on the RTC.
void setOffsetMs(int32_t ms);

int satellites();

/// Prints the state of GPS reception and of the clock to the serial log.
void printDiagnostics();

}  // namespace timebase
