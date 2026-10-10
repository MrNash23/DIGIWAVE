#include "fsk_modem.h"

#include <initializer_list>

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace digi {

namespace {
constexpr float kTwoPi = 6.28318530718f;
}

FskDemod::~FskDemod()
{
    free(_mark.ring_i);
    free(_mark.ring_q);
    free(_space.ring_i);
    free(_space.ring_q);
}

bool FskDemod::begin(float mark_hz, float space_hz, float baud, int sample_rate)
{
    _spb = sample_rate / baud;
    // Averaging over 80 % of a bit rejects the other tone and leaves the edges sharp enough
    int len = (int)(0.8f * _spb);
    for (Tone *t : {&_mark, &_space}) {
        free(t->ring_i);
        free(t->ring_q);
        t->ring_i = (float *)calloc(len, sizeof(float));
        t->ring_q = (float *)calloc(len, sizeof(float));
        t->sum_i = t->sum_q = 0;
        t->phi = 0;
        if (t->ring_i == nullptr || t->ring_q == nullptr) {
            _len = 0;
            return false;
        }
    }
    _mark.dphi = kTwoPi * mark_hz / sample_rate;
    _space.dphi = kTwoPi * space_hz / sample_rate;
    _len = len;
    _pos = 0;
    _since_resync = 0;
    _level = 0;
    return true;
}

float FskDemod::mix(Tone &t, float x)
{
    float i = x * cosf(t.phi);
    float q = x * sinf(t.phi);
    t.phi += t.dphi;
    if (t.phi > kTwoPi) {
        t.phi -= kTwoPi;
    }
    t.sum_i += i - t.ring_i[_pos];
    t.sum_q += q - t.ring_q[_pos];
    t.ring_i[_pos] = i;
    t.ring_q[_pos] = q;
    return sqrtf(t.sum_i * t.sum_i + t.sum_q * t.sum_q) / _len;
}

float FskDemod::process(int16_t sample)
{
    if (_len == 0) {
        return 0;
    }
    float x = sample / 32768.0f;
    float m = mix(_mark, x);
    float s = mix(_space, x);
    _pos = (_pos + 1) % _len;

    // The running sums collect rounding errors; rebuild them now and then
    if (++_since_resync >= 48000) {
        _since_resync = 0;
        for (Tone *t : {&_mark, &_space}) {
            t->sum_i = t->sum_q = 0;
            for (int k = 0; k < _len; ++k) {
                t->sum_i += t->ring_i[k];
                t->sum_q += t->ring_q[k];
            }
        }
    }

    float total = m + s;
    _level += 0.001f * (2.0f * total - _level);
    return total > 1e-6f ? (m - s) / total : 0.0f;
}

void FskMod::begin(float mark_hz, float space_hz, float baud, float amplitude, int sample_rate)
{
    _mark_dphi = kTwoPi * mark_hz / sample_rate;
    _space_dphi = kTwoPi * space_hz / sample_rate;
    _spb = sample_rate / baud;
    _carry = 0;
    _phi = 0;
    _amp = amplitude;
}

size_t FskMod::emitBit(bool mark, int16_t *out, float bits)
{
    // Bits are rarely a whole number of samples long; the fraction is carried over
    float exact = _spb * bits + _carry;
    size_t n = (size_t)exact;
    _carry = exact - n;
    float dphi = mark ? _mark_dphi : _space_dphi;
    for (size_t i = 0; i < n; ++i) {
        out[i] = (int16_t)lroundf(32767.0f * _amp * sinf(_phi));
        _phi += dphi;
        if (_phi > kTwoPi) {
            _phi -= kTwoPi;
        }
    }
    return n;
}

}  // namespace digi
