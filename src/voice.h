// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include "hal/audio_if.h"

/// Spoken announcements in FT8 and FT4 over the speaker of the built-in codec:
/// a new station calling CQ, a station calling us, a QSO started and logged.
/// They are switched on and off in the setup.
namespace voice {

enum class Phrase { NewStation, CallingYou, QsoStarted, QsoLogged };

/// speaker is the built-in codec. shared: it is also the audio path to the
/// radio, so announcements and transmissions take turns. Call before engine::begin().
void begin(AudioIo &speaker, bool shared);

/// Turns the news of the decoded slots into announcements. Call from the main loop.
void poll();

/// Queues an announcement; it is dropped while another one is still waiting.
void say(Phrase p);

/// Called by the engine when it keys and unkeys the radio. With a shared
/// speaker it waits for an announcement to end and holds the speaker until
/// the radio is unkeyed.
void radioTx(bool on);

}  // namespace voice
