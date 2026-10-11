// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fsk_modem.h"

namespace digi {

struct RttyConfig {
    float baud = 45.45f;
    float mark_hz = 2125.0f;   // the usual AFSK tones
    float space_hz = 2295.0f;
    bool reverse = false;      // swaps mark and space, for the other sideband
};

/// Baudot (ITA2, US figures) teleprinter decoder: 5 data bits, start and stop bit.
class RttyDecoder {
public:
    bool begin(const RttyConfig &cfg, int sample_rate = 12000);
    void feed(const int16_t *samples, size_t count);
    size_t read(char *out, size_t max);

    float level() const { return _demod.level(); }
    /// Share of recently received characters with a valid stop bit, 0..100.
    int quality() const { return _quality; }

private:
    void emit(char c);
    void character(int code);

    FskDemod _demod;
    float _spb = 0;
    bool _was_mark = true;
    bool _in_char = false;
    float _t = 0;        // samples since the start bit began
    int _bit = 0;        // next bit to sample: 0 = start, 1..5 data, 6 = stop
    int _code = 0;
    bool _weak = false;  // a bit of the current character was not clearly mark or space
    bool _figures = false;
    int _quality = 0;
    float _smooth = 0;
    float _clarity = 0;  // how clearly one tone dominates, averaged over a few bits

    char _out[128]{};
    size_t _out_head = 0, _out_tail = 0;
};

/// Baudot teleprinter encoder.
class RttyEncoder {
public:
    void begin(const RttyConfig &cfg, float amplitude = 0.5f, int sample_rate = 12000);

    /// Queues text. Characters outside the Baudot set are skipped.
    size_t send(const char *text);

    /// Writes up to count samples; returns 0 when nothing is left to send.
    size_t read(int16_t *out, size_t count);

    bool busy() const { return _q_head != _q_tail || _buf_pos < _buf_len || _tail_pending; }
    void clear();

private:
    void frame(int code);
    void append(bool mark, float bits);

    FskMod _mod;
    char _queue[256]{};
    size_t _q_head = 0, _q_tail = 0;
    bool _figures = false;
    bool _started = false;
    bool _tail_pending = false;

    int16_t _buf[8192]{};  // samples of what is being sent: at 45 baud the lead-in needs about 6100
    size_t _buf_len = 0, _buf_pos = 0;
};

}  // namespace digi
