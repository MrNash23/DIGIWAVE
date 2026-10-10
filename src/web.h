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

/// Address to open in the browser, or an empty string while Wi-Fi is off.
const char *address();

}  // namespace web
