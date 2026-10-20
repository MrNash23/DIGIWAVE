// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <stddef.h>
#include <stdint.h>

/// Audio path between the FT8 engine and a radio: 12 kHz, mono, 16 bit.
/// Backends: the built-in codec, and the interface board with line in and line out.
class AudioIo {
public:
    virtual ~AudioIo() = default;

    virtual bool begin() = 0;

    /// Blocks until count samples are captured. Returns false on a driver error.
    virtual bool read(int16_t *samples, size_t count) = 0;

    /// Queues count samples for playback; blocks while the output buffer is full.
    virtual bool write(const int16_t *samples, size_t count) = 0;

    /// Switches the output stage (amplifier or line driver) on or off.
    virtual void setOutputEnabled(bool on) = 0;

    virtual void setInputGain(float db) = 0;
    virtual void setOutputVolume(int percent) = 0;

    /// Delay in ms between write() and the sound leaving the device.
    virtual int outputLatencyMs() const = 0;
};

/// Built-in ES8311 codec with microphone and speaker.
AudioIo &es8311Audio();

/// Whether the interface board on the 12-pin port answers on the I2C bus.
bool interfaceBoardPresent();

/// Line in and line out of the interface board: its own ES8311 at 0x19 on
/// I2S1, one data pin shared by both directions and switched with the PTT.
AudioIo &interfaceAudio();

/// Which backend was chosen at the start.
extern bool audio_on_board;
