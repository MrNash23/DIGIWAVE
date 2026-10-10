#include "wefax_decoder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace digi {

namespace {
constexpr float kTwoPi = 6.28318530718f;
constexpr float kBlackHz = 1500.0f;
constexpr float kWhiteHz = 2300.0f;
constexpr float kCenterHz = (kBlackHz + kWhiteHz) / 2;
}  // namespace

WefaxDecoder::~WefaxDecoder()
{
    free(_line);
    free(_acc);
    free(_acc_n);
}

bool WefaxDecoder::begin(int width, int lines_per_minute, int sample_rate)
{
    free(_line);
    free(_acc);
    free(_acc_n);
    _line = (uint8_t *)calloc(width, 1);
    _acc = (float *)calloc(width, sizeof(float));
    _acc_n = (uint16_t *)calloc(width, sizeof(uint16_t));
    if (_line == nullptr || _acc == nullptr || _acc_n == nullptr) {
        _width = 0;
        return false;
    }
    _rate = sample_rate;
    _width = width;
    _nominal_spl = sample_rate * 60.0f / lines_per_minute;
    _spl = _nominal_spl;
    _pos = 0;
    _phi = 0;
    memset(_bi, 0, sizeof(_bi));
    memset(_bq, 0, sizeof(_bq));
    memset(_ci, 0, sizeof(_ci));
    memset(_cq, 0, sizeof(_cq));
    _si = _sq = _ti = _tq = 0;
    _fpos = 0;
    _prev_i = _prev_q = 0;
    _tone_crossings = _tone_samples = 0;
    _start_windows = _stop_windows = 0;
    _phased = false;
    _phasing_run = 0;
    _event = Event::None;
    return true;
}

void WefaxDecoder::onLine(LineFn fn, void *ctx)
{
    _fn = fn;
    _ctx = ctx;
}

void WefaxDecoder::setSlantPpm(float ppm)
{
    _spl = _nominal_spl * (1.0f + ppm * 1e-6f);
}

void WefaxDecoder::shift(int pixels)
{
    // Moving the picture left means being earlier in the line for the same sample
    _pos -= pixels * _spl / _width;
    while (_pos < 0) {
        _pos += _spl;
    }
    while (_pos >= _spl) {
        _pos -= _spl;
    }
}

WefaxDecoder::Event WefaxDecoder::takeEvent()
{
    Event e = _event;
    _event = Event::None;
    return e;
}

void WefaxDecoder::feed(const int16_t *samples, size_t count)
{
    if (_width == 0) {
        return;
    }
    const float dphi = kTwoPi * kCenterHz / _rate;
    for (size_t n = 0; n < count; ++n) {
        // Shift the signal down by the centre frequency and remove what is left of the mirror image
        float x = samples[n] / 32768.0f;
        float i = x * cosf(_phi);
        float q = -x * sinf(_phi);
        _phi += dphi;
        if (_phi > kTwoPi) {
            _phi -= kTwoPi;
        }
        _si += i - _bi[_fpos];
        _sq += q - _bq[_fpos];
        _bi[_fpos] = i;
        _bq[_fpos] = q;
        _ti += _si - _ci[_fpos];
        _tq += _sq - _cq[_fpos];
        _ci[_fpos] = _si;
        _cq[_fpos] = _sq;
        _fpos = (_fpos + 1) % 6;

        // The rotation between two samples is the frequency offset from the centre
        float dot = _ti * _prev_i + _tq * _prev_q;
        float cross = _tq * _prev_i - _ti * _prev_q;
        _prev_i = _ti;
        _prev_q = _tq;
        float hz = kCenterHz + atan2f(cross, dot) * _rate / kTwoPi;
        float value = (hz - kBlackHz) / (kWhiteHz - kBlackHz);
        if (value < 0) {
            value = 0;
        }
        if (value > 1) {
            value = 1;
        }

        tone(value);

        int px = (int)(_pos * _width / _spl);
        if (px >= _width) {
            px = _width - 1;
        }
        _acc[px] += value;
        ++_acc_n[px];

        _pos += 1.0f;
        if (_pos >= _spl) {
            _pos -= _spl;
            finishLine();
        }
    }
}

void WefaxDecoder::tone(float value)
{
    // Start and stop are announced by the picture signal switching between black
    // and white 300 or 450 times a second.
    if (_tone_high ? value < 0.3f : value > 0.7f) {
        _tone_high = !_tone_high;
        ++_tone_crossings;
    }
    if (++_tone_samples < _rate / 2) {
        return;
    }
    int hz = _tone_crossings;  // crossings in half a second = cycles per second
    _tone_crossings = 0;
    _tone_samples = 0;
    _start_windows = (hz >= 285 && hz <= 315) ? _start_windows + 1 : 0;
    _stop_windows = (hz >= 430 && hz <= 470) ? _stop_windows + 1 : 0;
    if (_start_windows == 3) {
        _event = Event::Start;
        rearmPhasing();
    }
    if (_stop_windows == 3) {
        _event = Event::Stop;
    }
}

void WefaxDecoder::finishLine()
{
    uint8_t last = 0;
    for (int x = 0; x < _width; ++x) {
        if (_acc_n[x] > 0) {
            last = (uint8_t)lroundf(255.0f * _acc[x] / _acc_n[x]);
        }
        _line[x] = last;
        _acc[x] = 0;
        _acc_n[x] = 0;
    }
    if (!_phased) {
        checkPhasing();
    }
    if (_fn != nullptr) {
        _fn(_line, _width, _ctx);
    }
}

void WefaxDecoder::checkPhasing()
{
    // A phasing line is one shade over nearly its whole length, with a pulse of the
    // other shade about 5 % wide where one line ends and the next begins.
    int white = 0;
    for (int x = 0; x < _width; ++x) {
        white += _line[x] > 127;
    }
    bool pulse_is_white = white < _width / 2;
    int minority = pulse_is_white ? white : _width - white;
    if (minority < _width * 2 / 100 || minority > _width * 9 / 100) {
        _phasing_run = 0;
        return;
    }
    // The pulse must be one run (it may wrap around the end of the line)
    int runs = 0, first = -1;
    for (int x = 0; x < _width; ++x) {
        bool cur = (_line[x] > 127) == pulse_is_white;
        bool prev = (_line[(x + _width - 1) % _width] > 127) == pulse_is_white;
        if (cur && !prev) {
            ++runs;
            first = x;
        }
    }
    if (runs != 1) {
        _phasing_run = 0;
        return;
    }
    int centre = (first + minority / 2) % _width;
    int diff = abs(centre - _phasing_pos);
    if (diff > _width / 2) {
        diff = _width - diff;
    }
    _phasing_run = (_phasing_run > 0 && diff <= _width / 100 + 1) ? _phasing_run + 1 : 1;
    _phasing_pos = centre;
    if (_phasing_run >= 5) {
        // Put the middle of the pulse on the line boundary
        shift(centre);
        _phased = true;
        _phasing_run = 0;
        _event = Event::Phased;
    }
}

}  // namespace digi
