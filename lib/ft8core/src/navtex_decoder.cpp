#include "navtex_decoder.h"

#include <ctype.h>
#include <string.h>

namespace digi {

namespace ccir476 {

namespace {

struct Entry {
    uint8_t code;
    char letter;
    char figure;
};

// ITU-R M.476, with the international figure set
const Entry kTable[] = {
    {0x47, 'A', '-'}, {0x72, 'B', '?'}, {0x1D, 'C', ':'}, {0x53, 'D', 0},   {0x56, 'E', '3'}, {0x1B, 'F', 0},
    {0x35, 'G', 0},   {0x69, 'H', 0},   {0x4D, 'I', '8'}, {0x17, 'J', 0},   {0x1E, 'K', '('}, {0x65, 'L', ')'},
    {0x39, 'M', '.'}, {0x59, 'N', ','}, {0x71, 'O', '9'}, {0x2D, 'P', '0'}, {0x2E, 'Q', '1'}, {0x55, 'R', '4'},
    {0x4B, 'S', '\''}, {0x74, 'T', '5'}, {0x4E, 'U', '7'}, {0x3C, 'V', '='}, {0x27, 'W', '2'}, {0x3A, 'X', '/'},
    {0x2B, 'Y', '6'}, {0x63, 'Z', '+'}, {kSpace, ' ', ' '},
};

}  // namespace

bool valid(uint8_t code)
{
    return code < 0x80 && __builtin_popcount(code) == 4;
}

uint8_t encode(char c, bool *figure)
{
    c = (char)toupper((unsigned char)c);
    for (const Entry &e : kTable) {
        if (e.letter == c) {
            *figure = false;
            return e.code;
        }
    }
    for (const Entry &e : kTable) {
        if (e.figure == c && c != 0) {
            *figure = true;
            return e.code;
        }
    }
    return 0;
}

char decode(uint8_t code, bool figures)
{
    for (const Entry &e : kTable) {
        if (e.code == code) {
            return figures ? e.figure : e.letter;
        }
    }
    return 0;
}

}  // namespace ccir476

using namespace ccir476;

bool NavtexDecoder::begin(const NavtexConfig &cfg, int sample_rate)
{
    // "B" is the higher radio frequency, which a receiver in USB turns into the higher tone
    float high = cfg.center_hz + 85.0f;
    float low = cfg.center_hz - 85.0f;
    if (!_demod.begin(cfg.reverse ? low : high, cfg.reverse ? high : low, 100.0f, sample_rate)) {
        return false;
    }
    _spb = _demod.samplesPerBit();
    _phase = 0;
    _last_positive = false;
    _window = 0;
    _bit_count = 0;
    memset(_score, 0, sizeof(_score));
    _locked = false;
    memset(_codes, 0xFF, sizeof(_codes));
    _code_count = 0;
    _vote[0] = _vote[1] = 0;
    _figures = false;
    _lost = 0;
    _out_head = _out_tail = 0;
    return true;
}

void NavtexDecoder::emit(char c)
{
    size_t next = (_out_head + 1) % sizeof(_out);
    if (next != _out_tail) {
        _out[_out_head] = c;
        _out_head = next;
    }
}

size_t NavtexDecoder::read(char *out, size_t max)
{
    size_t n = 0;
    while (n < max && _out_tail != _out_head) {
        out[n++] = _out[_out_tail];
        _out_tail = (_out_tail + 1) % sizeof(_out);
    }
    return n;
}

void NavtexDecoder::feed(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        float d = _demod.process(samples[i]);
        bool positive = d > 0;

        // The stream has no start bits: the bit clock follows the tone changes.
        // A change should fall on phase 0; the clock is nudged towards that.
        if (positive != _last_positive) {
            float error = _phase < _spb / 2 ? _phase : _phase - _spb;
            _phase -= 0.12f * error;
            _last_positive = positive;
        }
        float before = _phase;
        _phase += 1.0f;
        if (before < _spb / 2 && _phase >= _spb / 2) {
            onBit(positive);  // middle of the bit
        }
        if (_phase >= _spb) {
            _phase -= _spb;
        }
    }
}

void NavtexDecoder::onBit(bool b)
{
    // The window holds the last seven bits, the oldest in bit 0
    _window = (uint8_t)((_window >> 1) | (b ? 0x40 : 0));
    int a = (int)(_bit_count % 7);
    ++_bit_count;

    // Every valid character has four "B" bits. Only one of the seven possible
    // character boundaries produces valid characters all the time.
    bool ok = valid(_window);
    _score[a] += ok ? 1 : -2;
    if (_score[a] > 20) {
        _score[a] = 20;
    }
    if (_score[a] < 0) {
        _score[a] = 0;
    }

    if (!_locked) {
        if (_score[a] >= 12) {
            _locked = true;
            _align = a;
            _code_count = 0;
            _vote[0] = _vote[1] = 0;
            _figures = false;
            _lost = 0;
            memset(_codes, 0xFF, sizeof(_codes));
        }
        return;
    }
    if (a != _align) {
        return;
    }
    if (_score[a] < 4) {
        _locked = false;  // signal gone or out of step
        return;
    }
    onCode(ok ? _window : 0xFF);
}

void NavtexDecoder::onCode(uint8_t code)
{
    uint32_t n = _code_count++;
    int parity = (int)(n & 1);
    _codes[n % 8] = code;
    if (n < 5) {
        return;
    }
    uint8_t earlier = _codes[(n - 5) % 8];

    // Which positions carry the repeats? A repeat equals the character sent five
    // positions before. During phasing the repeat position carries alpha and the
    // other one "rep".
    if (code != 0xFF && code == earlier && code != kAlpha && code != kRep) {
        _vote[parity] += 2;
    }
    if (code == kAlpha) {
        _vote[parity] += 1;
    } else if (code == kRep) {
        _vote[1 - parity] += 1;
    }
    for (int &v : _vote) {
        if (v > 40) {
            v = 40;
        }
    }
    if ((n & 31) == 31) {
        // Old evidence fades, so that a wrong start can be corrected
        _vote[0] -= _vote[0] / 4;
        _vote[1] -= _vote[1] / 4;
    }

    int repeat_parity = _vote[1] > _vote[0] ? 1 : 0;
    if (_vote[repeat_parity] - _vote[1 - repeat_parity] < 3 || parity != repeat_parity) {
        return;
    }

    // Both transmissions of one character are in hand
    auto phasing = [](uint8_t c) { return c == kAlpha || c == kRep || c == kBeta; };
    if (phasing(earlier) || phasing(code)) {
        return;  // phasing or idle: the two positions differ by design
    }
    if (earlier != 0xFF && code != 0xFF && earlier != code) {
        ++_lost;  // both look valid but differ
    } else if (earlier != 0xFF) {
        output(earlier);
    } else if (code != 0xFF) {
        output(code);
    } else {
        ++_lost;
    }
}

void NavtexDecoder::output(uint8_t code)
{
    // Gaps are marked only inside running text, not when the signal fades out
    for (; _lost > 0; --_lost) {
        emit('_');
    }
    switch (code) {
    case kAlpha:
    case kBeta:
    case kRep:
    case kCr:
        return;
    case kLtrs:
        _figures = false;
        return;
    case kFigs:
        _figures = true;
        return;
    case kLf:
        emit('\n');
        return;
    default:
        break;
    }
    char c = decode(code, _figures);
    if (c != 0) {
        emit(c);
    }
}

}  // namespace digi
