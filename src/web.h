// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

/// Wi-Fi access point with a small web interface for a phone: decodes, the main
/// controls, station settings and download of the logbook.
/// All functions must be called from the main loop.
namespace web {

extern const char kSsid[];

/// Starts or stops the access point according to the settings.
void apply();

void loop();

bool running();

/// Number of phones connected to the access point.
int clients();

/// Switches Wi-Fi off without changing the settings, before the device goes to sleep.
void shutdown();

/// Address to open in the browser, or an empty string while Wi-Fi is off.
const char *address();

/// Password of the access point.
const char *password();

/// State of the access point and the home network on the serial line, for the command "w".
void printDiagnostics();

/// Address in the home network (such as "192.168.1.40"), or an empty string while not joined.
const char *homeAddress();

/// How joining the home network goes: "" (none set or Wi-Fi off), "joining", "joined",
/// "notfound" (not in reach) or "failed" (most likely the password).
const char *homeState();

}  // namespace web
