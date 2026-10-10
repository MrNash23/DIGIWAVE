#include "cw_codec.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

namespace digi {

namespace {

constexpr float kTwoPi = 6.28318530718f;

struct Morse {
    char c;
    const char *code;
};

const Morse kMorse[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},      {'F', "..-."},
    {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},
    {'M', "--"},     {'N', "-."},     {'O', "---"},    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},
    {'S', "..."},    {'T', "-"},      {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},
    {'Y', "-.--"},   {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},  {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},  {'=', "-...-"},  {'+', ".-.-."},
    {'-', "-....-"}, {'@', ".--.-."}, {':', "---..."}, {'\'', ".----."}, {'(', "-.--."}, {')', "-.--.-"},
    {'!', "-.-.--"}, {'"', ".-..-."},
};

/// Power of one frequency in a block; taper (optional) weights the samples.
float goertzel(const int16_t *x, int n, float hz, int rate, const float *taper = nullptr)
{
    float w = kTwoPi * hz / rate;
    float coeff = 2.0f * cosf(w);
    float s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        float s0 = (taper ? taper[i] * x[i] : (float)x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power < 0 ? 0 : power;
}

}  // namespace

const char *morseFor(char c)
{
    c = (char)toupper((unsigned char)c);
    for (const Morse &m : kMorse) {
        if (m.c == c) {
            return m.code;
        }
    }
    return nullptr;
}

// --- decoder -------------------------------------------------------------------

void CwDecoder::begin(int sample_rate)
{
    *this = CwDecoder();
    _rate = sample_rate;
    _hop_ms = 1000.0f * kHop / sample_rate;
    // The taper keeps signals beside the window out of the envelope
    for (int i = 0; i < kWindow; ++i) {
        _taper[i] = 0.5f - 0.5f * cosf(kTwoPi * (i + 0.5f) / kWindow);
    }
    for (int i = 0; i < kBankBlock; ++i) {
        _bank_taper[i] = 0.5f - 0.5f * cosf(kTwoPi * (i + 0.5f) / kBankBlock);
    }
}

void CwDecoder::setWindow(float centre_hz, float half_width_hz)
{
    _centre = centre_hz;
    _half = half_width_hz;
    _tone = centre_hz;
    for (float &p : _bank_power) {
        p = 0;
    }
}

void CwDecoder::feed(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        // Sliding analysis window, evaluated every kHop samples
        if (_win_fill < kWindow) {
            _win[_win_fill++] = samples[i];
        } else {
            memmove(_win, _win + 1, sizeof(int16_t) * (kWindow - 1));
            _win[kWindow - 1] = samples[i];
        }
        if (++_hop_fill >= kHop && _win_fill == kWindow) {
            _hop_fill = 0;
            hop();
        }

        _bank_buf[_bank_fill++] = samples[i];
        if (_bank_fill == kBankBlock) {
            updateTone();
            memmove(_bank_buf, _bank_buf + kBankHop, sizeof(int16_t) * (kBankBlock - kBankHop));
            _bank_fill = kBankBlock - kBankHop;
        }
    }
}

void CwDecoder::updateTone()
{
    // Filters across the window find the tone; filters beside it measure the noise
    float best = 0, block_best = 0;
    int best_i = 0;
    const float step = _half / 2.0f;
    for (int i = 0; i < kBank; ++i) {
        float p = goertzel(_bank_buf, kBankBlock, _centre + (i - kBank / 2) * step, _rate, _bank_taper);
        block_best = fmaxf(block_best, p);
        _bank_power[i] = 0.7f * _bank_power[i] + 0.3f * p;
        if (_bank_power[i] > best) {
            best = _bank_power[i];
            best_i = i;
        }
    }
    float sum = 0, highest = 0;
    for (int i = 0; i < kRefs; ++i) {
        float offset = _half + 100.0f + 75.0f * (i / 2);
        float p = goertzel(_bank_buf, kBankBlock, _centre + ((i & 1) ? offset : -offset), _rate, _bank_taper);
        sum += p;
        highest = fmaxf(highest, p);
    }
    // The highest value is left out: a station beside the window must not count as noise
    float block_ref = (sum - highest) / (kRefs - 1);
    // The first block sets the average; starting from zero would open the decoder at once
    _ref_power = _ref_power == 0 ? block_ref : 0.9f * _ref_power + 0.1f * block_ref;

    // A tone in the window stands well above the noise beside it, in this block
    // and on average. A click raises window and surroundings alike within its
    // block; plain noise does not hold the average ratio.
    bool was_open = _hold_ms > 0;
    if (block_best > 5.0f * block_ref + 1.0f && best > 10.0f * _ref_power + 1.0f) {
        _hold_ms = 2500.0f;  // bridges the gaps between words, also at low speed
        if (!was_open && _mark_count > 1) {
            // What was keyed before belongs to noise, except the element just received
            _marks[0] = _marks[_mark_count - 1];
            _mark_count = 1;
        }
        if (!was_open) {
            // The same for the character being collected: keep only an element that just ended
            bool recent = !_key && _state_ms < 100.0f && _element_count > 0;
            _elements[0] = recent ? _elements[_element_count - 1] : 0;
            _element_count = recent ? 1 : 0;
        }
        if (_tracking) {
            float left = best_i > 0 ? _bank_power[best_i - 1] : 0;
            float right = best_i < kBank - 1 ? _bank_power[best_i + 1] : 0;
            float offset = (right - left) / (left + best + right);
            float target = _centre + (best_i - kBank / 2 + offset) * step;
            target = fminf(fmaxf(target, _centre - _half), _centre + _half);
            _tone += 0.3f * (target - _tone);
        }
    } else if (_hold_ms > 0) {
        _hold_ms -= 1000.0f * kBankHop / _rate;
    }
    if (!_tracking) {
        _tone = _centre;
    }
}

void CwDecoder::hop()
{
    _env = sqrtf(goertzel(_win, kWindow, _tone, _rate, _taper));

    if (_warmup > 0) {
        --_warmup;
        _noise = _noise == 0 ? _env : _noise + (_env - _noise) * 0.1f;
        _peak = _noise;
        return;
    }
    if (_env > _peak) {
        _peak = _env;
    } else {
        _peak -= (_peak - _noise) * 0.0006f;  // forgets a strong signal within a few seconds
    }
    // The noise estimate falls quickly and rises slowly, so that it settles on the
    // level between the elements and is not pulled up by the keying itself
    _noise += (_env - _noise) * (_env < _noise ? 0.05f : 0.003f);
    if (_key && _state_ms > 3000.0f) {
        // No Morse element lasts this long: a carrier or a rise in noise, not keying
        _noise = _env;
        _peak = _env;
    }
    // Keying is followed whenever something stands out of the envelope. Whether it
    // is a signal in the window is decided in updateTone(); text is only given out
    // then. Keeping the two apart saves the first element of a transmission, which
    // arrives before the window detector has made up its mind.
    _squelch_open = _peak > 3.0f * _noise + 1.0f;

    // Hysteresis around the middle between noise and peak
    float span = _peak - _noise;
    bool key = _key ? (_env > _noise + 0.3f * span) : (_env > _noise + 0.5f * span);
    if (!_squelch_open) {
        key = false;
    }

    _state_ms += _hop_ms;
    if (key != _key) {
        if (_key) {
            onMark(_state_ms);
        } else {
            onSpace(_state_ms);
        }
        _key = key;
        _state_ms = 0;
    } else if (!_key) {
        // Close the character and the word without waiting for the next mark
        if (_char_open && _state_ms > 2.0f * _dot_ms) {
            flushChar();
        }
        if (!_space_sent && _state_ms > 5.0f * _dot_ms) {
            if (_hold_ms > 0) {
                emit(' ');
            }
            _space_sent = true;
        }
    }
}

void CwDecoder::onMark(float ms)
{
    if (ms < 0.3f * _dot_ms && ms < 12.0f) {
        return;  // click or noise burst
    }

    // The last marks tell dots from dashes: with both present the gap between
    // the shortest and the longest is at least a factor of two.
    if (_mark_count < 12) {
        _marks[_mark_count++] = ms;
    } else {
        memmove(_marks, _marks + 1, sizeof(float) * 11);
        _marks[11] = ms;
    }
    float lo = _marks[0], hi = _marks[0], sum = 0;
    for (int i = 0; i < _mark_count; ++i) {
        lo = fminf(lo, _marks[i]);
        hi = fmaxf(hi, _marks[i]);
        sum += _marks[i];
    }
    float avg = sum / _mark_count;
    if (hi >= 2.2f * lo) {
        float estimate = (lo + hi / 3.0f) / 2.0f;
        _dot_ms += 0.5f * (estimate - _dot_ms);
    } else if (ms > 5.0f * _dot_ms) {
        // Longer than any dash at the assumed speed: the sender is much slower.
        // Corrected at once, otherwise the gaps inside the character would end it.
        _dot_ms = ms / 3.0f;
    } else if (_mark_count >= 3) {
        // Only one kind of element seen: check the assumed speed against it
        if (hi < 0.6f * _dot_ms) {
            _dot_ms = avg;  // all much shorter than a dot: the sender is faster
        } else if (lo > 4.0f * _dot_ms) {
            _dot_ms = avg / 3.0f;  // all much longer than a dash: the sender is slower
        }
    }
    _dot_ms = fminf(fmaxf(_dot_ms, 20.0f), 300.0f);  // 60 to 4 WPM

    // Dots and dashes are told apart when the character is complete, with the
    // speed estimate as it stands then: the first elements often arrive before
    // the estimate has settled.
    if (_element_count < 10) {
        _elements[_element_count++] = ms;
    }
    _char_open = true;
    _space_sent = false;
}

void CwDecoder::onSpace(float ms)
{
    // Gaps are handled while they last, in hop(); a very short one is part of a mark
    (void)ms;
}

void CwDecoder::flushChar()
{
    _char_open = false;
    if (_element_count == 0) {
        return;
    }
    if (_hold_ms <= 0) {
        _element_count = 0;  // keyed by noise, no signal in the window
        return;
    }
    char symbol[11];
    for (int i = 0; i < _element_count; ++i) {
        symbol[i] = _elements[i] < 2.0f * _dot_ms ? '.' : '-';
    }
    symbol[_element_count] = '\0';
    _element_count = 0;
    char c = 0;
    for (const Morse &m : kMorse) {
        if (strcmp(m.code, symbol) == 0) {
            c = m.c;
            break;
        }
    }
    emit(c ? c : '*');
}

void CwDecoder::emit(char c)
{
    size_t next = (_out_head + 1) % sizeof(_out);
    if (next != _out_tail) {
        _out[_out_head] = c;
        _out_head = next;
    }
}

size_t CwDecoder::read(char *out, size_t max)
{
    size_t n = 0;
    while (n < max && _out_tail != _out_head) {
        out[n++] = _out[_out_tail];
        _out_tail = (_out_tail + 1) % sizeof(_out);
    }
    return n;
}

// --- keyer ---------------------------------------------------------------------

void CwKeyer::begin(int wpm, float tone_hz, float amplitude, int sample_rate)
{
    _rate = sample_rate;
    _dot_samples = 1.2f / wpm * sample_rate;
    _tone = tone_hz;
    _amp = amplitude;
    _ramp_step = 1.0f / (0.005f * sample_rate);  // 5 ms edges keep the signal narrow
}

void CwKeyer::clear()
{
    _q_head = _q_tail = 0;
    _pattern = nullptr;
    _seg_left = 0;
    _seg_on = false;
    _gap_pending = 0;
}

size_t CwKeyer::send(const char *text)
{
    size_t accepted = 0;
    for (; *text; ++text) {
        if (*text != ' ' && morseFor(*text) == nullptr) {
            continue;
        }
        size_t next = (_q_head + 1) % sizeof(_queue);
        if (next == _q_tail) {
            break;
        }
        _queue[_q_head] = *text;
        _q_head = next;
        ++accepted;
    }
    return accepted;
}

bool CwKeyer::nextSegment()
{
    if (_gap_pending > 0) {
        _seg_on = false;
        _seg_left = (int)(_gap_pending * _dot_samples);
        _gap_pending = 0;
        return true;
    }
    if (_pattern != nullptr && *_pattern != '\0') {
        _seg_on = true;
        _seg_left = (int)((*_pattern == '-' ? 3 : 1) * _dot_samples);
        ++_pattern;
        // One dot between elements, three after the character
        _gap_pending = *_pattern != '\0' ? 1 : 3;
        return true;
    }
    _pattern = nullptr;
    if (_q_tail == _q_head) {
        return false;
    }
    char c = _queue[_q_tail];
    _q_tail = (_q_tail + 1) % sizeof(_queue);
    if (c == ' ') {
        // A word gap is seven dots; three were already sent after the last character
        _seg_on = false;
        _seg_left = (int)(4 * _dot_samples);
        return true;
    }
    _pattern = morseFor(c);
    return nextSegment();
}

size_t CwKeyer::read(int16_t *out, size_t count)
{
    const float dphi = kTwoPi * _tone / _rate;
    size_t n = 0;
    while (n < count) {
        if (_seg_left <= 0 && !nextSegment()) {
            // Let the envelope fall before reporting the end
            if (_gain <= 0) {
                break;
            }
            _seg_on = false;
        }
        float target = _seg_on ? 1.0f : 0.0f;
        if (_gain < target) {
            _gain = fminf(_gain + _ramp_step, target);
        } else if (_gain > target) {
            _gain = fmaxf(_gain - _ramp_step, target);
        }
        // Raised-cosine shape of the linear ramp
        float shaped = 0.5f - 0.5f * cosf(3.14159265f * _gain);
        out[n++] = (int16_t)lroundf(32767.0f * _amp * shaped * sinf(_phi));
        _phi += dphi;
        if (_phi > kTwoPi) {
            _phi -= kTwoPi;
        }
        if (_seg_left > 0) {
            --_seg_left;
        }
    }
    return n;
}

}  // namespace digi
