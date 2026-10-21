// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <Arduino.h>
#include <stdint.h>

/// Broadcast times of the weather stations: the WEFAX charts and the RTTY
/// programme of the DWD are built in; a plan saved over Wi-Fi lives on the SD
/// card and replaces the built-in one. The text form is one broadcast per line,
/// "HH:MM what it is", in UTC.
namespace schedule {

enum class Plan { Wefax, Rtty };
constexpr int kPlans = 2;

constexpr int kMaxEntries = 96;

struct Entry {
    uint16_t minute;  // of the day, UTC
    char text[52];
};

/// Name used in the web interface ("wefax", "rtty").
const char *id(Plan p);
bool fromId(const char *id, Plan *p);

/// Reads the plan from the SD card, or takes the built-in one. Called again after a change.
void load(Plan p);

int count(Plan p);
const Entry &entry(Plan p, int i);
const char *station(Plan p);
const char *frequencies(Plan p);

/// Whether the plan comes from the SD card rather than the firmware.
bool fromCard(Plan p);

/// How long a broadcast may run at most, in minutes; it ends earlier when the next begins.
int longestMinutes(Plan p);

/// The first broadcast that starts after the given minute of the day, wrapping
/// to the next day. -1 without entries.
int next(Plan p, int minute_now);

/// The plan in its text form, for editing.
String text(Plan p);

/// Checks the text, saves it to the SD card and uses it. Returns a message for the user.
const char *save(Plan p, const String &text, bool *ok);

/// Removes the saved plan, so that the built-in one is used again.
const char *restoreDefault(Plan p);

}  // namespace schedule
