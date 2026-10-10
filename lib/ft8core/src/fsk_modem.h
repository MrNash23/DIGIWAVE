#pragma once

#include <stddef.h>
#include <stdint.h>

namespace digi {

/// Two-tone FSK demodulator shared by RTTY and NAVTEX. Each tone is mixed to
/// baseband and averaged over most of a bit; the output compares the two.
class FskDemod {
public:
    ~FskDemod();

    /// @param mark_hz, space_hz audio frequencies of the two tones
    bool begin(float mark_hz, float space_hz, float baud, int sample_rate = 12000);

    /// Returns a value between -1 (space) and +1 (mark) for one input sample.
    float process(int16_t sample);

    /// Smoothed total tone level relative to full scale, for a signal indicator.
    float level() const { return _level; }

    float samplesPerBit() const { return _spb; }

private:
    struct Tone {
        float dphi = 0, phi = 0;
        float *ring_i = nullptr, *ring_q = nullptr;
        float sum_i = 0, sum_q = 0;
    };
    float mix(Tone &t, float x);

    Tone _mark, _space;
    int _len = 0;   // averaging length in samples
    int _pos = 0;
    int _since_resync = 0;
    float _spb = 0;
    float _level = 0;
};

/// Phase-continuous two-tone FSK generator.
class FskMod {
public:
    void begin(float mark_hz, float space_hz, float baud, float amplitude = 0.5f, int sample_rate = 12000);

    /// Writes the samples of one bit (or a fraction of one, for 1.5 stop bits).
    /// @return number of samples written; out must hold samplesPerBit() * bits + 1
    size_t emitBit(bool mark, int16_t *out, float bits = 1.0f);

    int maxSamplesPerBit() const { return (int)_spb + 1; }

private:
    float _mark_dphi = 0, _space_dphi = 0;
    float _spb = 0, _carry = 0;
    float _phi = 0, _amp = 0;
};

}  // namespace digi
