#pragma once

#include <stddef.h>
#include <stdint.h>

/// Audio path between the FT8 engine and a radio: 12 kHz, mono, 16 bit.
/// The built-in codec is the first backend; a line-in/line-out interface
/// becomes another implementation of this class.
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
