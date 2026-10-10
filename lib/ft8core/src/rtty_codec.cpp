#include "rtty_codec.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

namespace digi {

namespace {

constexpr int kLtrs = 0x1F;
constexpr int kFigs = 0x1B;

// ITA2 with US figures; index is the 5-bit code, first transmitted bit = bit 0
const char kLetters[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
                           'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
const char kFigures[32] = {0, '3', '\n', '-', ' ', 0 /*bell*/, '8', '7', '\r', '$', '4', '\'', ',', '!', ':', '(',
                           '5', '"', ')', '2', '#', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};

}  // namespace

// --- decoder -------------------------------------------------------------------

bool RttyDecoder::begin(const RttyConfig &cfg, int sample_rate)
{
    float mark = cfg.reverse ? cfg.space_hz : cfg.mark_hz;
    float space = cfg.reverse ? cfg.mark_hz : cfg.space_hz;
    if (!_demod.begin(mark, space, cfg.baud, sample_rate)) {
        return false;
    }
    _spb = _demod.samplesPerBit();
    _was_mark = true;
    _in_char = false;
    _figures = false;
    _quality = 0;
    _smooth = 0;
    _clarity = 0;
    _out_head = _out_tail = 0;
    return true;
}

void RttyDecoder::emit(char c)
{
    size_t next = (_out_head + 1) % sizeof(_out);
    if (next != _out_tail) {
        _out[_out_head] = c;
        _out_head = next;
    }
}

size_t RttyDecoder::read(char *out, size_t max)
{
    size_t n = 0;
    while (n < max && _out_tail != _out_head) {
        out[n++] = _out[_out_tail];
        _out_tail = (_out_tail + 1) % sizeof(_out);
    }
    return n;
}

void RttyDecoder::character(int code)
{
    if (code == kLtrs) {
        _figures = false;
        return;
    }
    if (code == kFigs) {
        _figures = true;
        return;
    }
    char c = _figures ? kFigures[code] : kLetters[code];
    if (c == ' ') {
        _figures = false;  // "unshift on space", the usual amateur convention
    }
    if (c == '\r') {
        return;  // the line feed that follows ends the line
    }
    if (c != 0) {
        emit(c);
    }
}

void RttyDecoder::feed(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        float d = _demod.process(samples[i]);
        // Hysteresis keeps noise around zero from producing edges
        bool mark = _was_mark ? d > -0.15f : d > 0.15f;
        // With a signal one tone clearly wins (|d| near 1); in noise both filters
        // see about the same and |d| averages around a third.
        _clarity += (fabsf(d) - _clarity) / (3.0f * _spb);

        if (!_in_char) {
            // A character begins with the change from mark (idle) to the space of the start bit
            if (_was_mark && !mark && _clarity > 0.55f) {
                _in_char = true;
                _t = 0;
                _bit = 0;
                _code = 0;
                _weak = false;
            }
        } else {
            _t += 1.0f;
            // Bits are read in their middle
            if (_t >= (_bit + 0.5f) * _spb) {
                if (fabsf(d) < 0.25f) {
                    _weak = true;
                }
                if (_bit == 0) {
                    if (mark) {
                        _in_char = false;  // a glitch, not a start bit
                    }
                } else if (_bit <= 5) {
                    if (mark) {
                        _code |= 1 << (_bit - 1);
                    }
                } else {
                    // A valid character ends in mark
                    _smooth += 0.1f * ((mark ? 100.0f : 0.0f) - _smooth);
                    _quality = (int)(_smooth + 0.5f);
                    // Noise rarely gives seven clear bits in a row; a real signal does
                    if (mark && !_weak) {
                        character(_code);
                    }
                    _in_char = false;
                }
                ++_bit;
            }
        }
        _was_mark = mark;
    }
}

// --- encoder -------------------------------------------------------------------

void RttyEncoder::begin(const RttyConfig &cfg, float amplitude, int sample_rate)
{
    float mark = cfg.reverse ? cfg.space_hz : cfg.mark_hz;
    float space = cfg.reverse ? cfg.mark_hz : cfg.space_hz;
    _mod.begin(mark, space, cfg.baud, amplitude, sample_rate);
    clear();
}

void RttyEncoder::clear()
{
    _q_head = _q_tail = 0;
    _buf_len = _buf_pos = 0;
    _figures = false;
    _started = false;
    _tail_pending = false;
}

size_t RttyEncoder::send(const char *text)
{
    size_t accepted = 0;
    for (; *text; ++text) {
        size_t next = (_q_head + 1) % sizeof(_queue);
        if (next == _q_tail) {
            break;
        }
        _queue[_q_head] = (char)toupper((unsigned char)*text);
        _q_head = next;
        ++accepted;
    }
    return accepted;
}

void RttyEncoder::append(bool mark, float bits)
{
    size_t needed = (size_t)(_mod.maxSamplesPerBit() * bits) + 1;
    if (_buf_len + needed <= sizeof(_buf) / sizeof(_buf[0])) {
        _buf_len += _mod.emitBit(mark, _buf + _buf_len, bits);
    }
}

void RttyEncoder::frame(int code)
{
    append(false, 1.0f);  // start bit
    for (int b = 0; b < 5; ++b) {
        append((code >> b) & 1, 1.0f);
    }
    append(true, 1.5f);  // stop
}

size_t RttyEncoder::read(int16_t *out, size_t count)
{
    size_t n = 0;
    while (n < count) {
        if (_buf_pos < _buf_len) {
            size_t take = _buf_len - _buf_pos;
            if (take > count - n) {
                take = count - n;
            }
            memcpy(out + n, _buf + _buf_pos, take * sizeof(int16_t));
            _buf_pos += take;
            n += take;
            continue;
        }
        _buf_len = _buf_pos = 0;

        if (_q_tail == _q_head) {
            if (_tail_pending) {
                // A short mark tail lets the receiver finish the last stop bit cleanly
                _tail_pending = false;
                append(true, 3.0f);
                continue;
            }
            _started = false;
            break;
        }
        if (!_started) {
            // Idle mark and two LTRS characters bring the receiver into step
            _started = true;
            _figures = false;
            append(true, 8.0f);
            frame(kLtrs);
            frame(kLtrs);
            continue;
        }

        char c = _queue[_q_tail];
        _q_tail = (_q_tail + 1) % sizeof(_queue);
        _tail_pending = true;

        if (c == '\n') {
            frame(0x08);  // carriage return, then line feed
            frame(0x02);
            continue;
        }
        int code = -1;
        bool figures = _figures;
        for (int k = 0; k < 32; ++k) {
            if (kLetters[k] == c && c != 0) {
                code = k;
                figures = (c == ' ') ? _figures : false;
                break;
            }
        }
        if (code < 0) {
            for (int k = 0; k < 32; ++k) {
                if (kFigures[k] == c && c != 0) {
                    code = k;
                    figures = true;
                    break;
                }
            }
        }
        if (code < 0) {
            continue;  // no Baudot code for this character
        }
        if (figures != _figures) {
            frame(figures ? kFigs : kLtrs);
            _figures = figures;
        }
        frame(code);
        if (c == ' ') {
            _figures = false;  // the receiver unshifts on space
        }
    }
    return n;
}

}  // namespace digi
