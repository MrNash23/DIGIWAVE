// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

/// Screens and input handling. All functions must be called from the main loop.
namespace ui {

/// Shows the boot picture. Call right after the display is up; begin() replaces it.
void splash();

void begin();
void loop();

/// Switches the mode and shows its screen (0..5, see engine::Mode).
void selectMode(int mode);

/// Text received in CW, RTTY and NAVTEX, most recent at the end.
const char *recentText();

/// Sends text in CW or RTTY and shows it in the text window. Returns false if the mode cannot send.
bool submitText(const char *text);

/// Saves the WEFAX picture received so far, as Enter does. Returns the number of lines written, 0 if none.
unsigned saveFaxNowForTest();

/// Saved WEFAX pictures are in this directory on the SD card.
extern const char kFaxDir[];
extern const char kSstvDir[];

}  // namespace ui
