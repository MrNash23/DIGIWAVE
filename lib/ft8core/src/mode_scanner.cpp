// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "mode_scanner.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <fft/kiss_fftr.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace digi {

namespace {

constexpr int kRate = 12000;
constexpr int kFft = 2048;                         // 5.86 Hz per bin
constexpr int kHop = 1024;                         // a spectrum every 85 ms
constexpr float kBinHz = (float)kRate / kFft;
constexpr int kFirstBin = 34;                      // 199 Hz
constexpr int kBins = 581;                         // up to 3600 Hz: tones of DWD RTTY reach 3.1 kHz
constexpr int kRows = 352;                         // 30 s of spectra
constexpr int kRecentRows = 47;                    // 4 s
constexpr int kAudio = 8 * kRate;                  // 8 s for the fine analysis
constexpr int kDecim = 12;                         // fine analysis at 1 kHz
constexpr int kFine = kAudio / kDecim;
constexpr float kFineRate = (float)kRate / kDecim;
constexpr int kLineFft = 8192;                     // 0.12 Hz resolution of the symbol rate
constexpr float kTwoPi = 6.28318530718f;

void *bigAlloc(size_t bytes)
{
#if defined(ESP32)
    return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

float binHz(float bin)
{
    return (kFirstBin + bin) * kBinHz;
}

int hzBin(float hz)
{
    return (int)lrintf(hz / kBinHz) - kFirstBin;
}

bool near(float value, float nominal, float tolerance)
{
    return fabsf(value - nominal) <= tolerance * nominal;
}

/// The value below which the given share of the values lies; reorders the array.
float percentile(float *v, int n, float share)
{
    if (n <= 0) {
        return 0;
    }
    int k = std::min(n - 1, std::max(0, (int)(share * (n - 1) + 0.5f)));
    std::nth_element(v, v + k, v + n);
    return v[k];
}

/// Power of each 0.5 dB value; pow() takes long on the device
float power_of[256];

/// dB of the mean power of a set of 0.5 dB values
struct PowerMean {
    float sum = 0;
    int n = 0;
    void add(uint8_t v) { sum += power_of[v], ++n; }
    float db() const { return n ? 10.0f * log10f(sum / n + 1e-12f) : -120.0f; }
};

}  // namespace

const char *scanModeName(ScanMode mode)
{
    switch (mode) {
    case ScanMode::None: return "no signal";
    case ScanMode::Unknown: return "unknown";
    case ScanMode::Carrier: return "carrier";
    case ScanMode::CW: return "CW";
    case ScanMode::RTTY: return "RTTY";
    case ScanMode::NAVTEX: return "NAVTEX";
    case ScanMode::FT8: return "FT8";
    case ScanMode::FT4: return "FT4";
    case ScanMode::PSK31: return "PSK31";
    case ScanMode::PSK63: return "PSK63";
    case ScanMode::SSTV: return "SSTV";
    case ScanMode::WEFAX: return "WEFAX";
    case ScanMode::Voice: return "voice";
    }
    return "";
}

ModeScanner::~ModeScanner()
{
    free(_audio);
    free(_fft);
    free(_window);
    free(_time);
    free(_freq);
    free(_rows);
    free(_snap_audio);
    free(_snap_rows);
    free(_line_fft);
    free(_zi);
    free(_zq);
    free(_amp);
    free(_inst);
    free(_pr);
    free(_pi);
    free(_fsk_d);
    free(_fsk_t);
    free(_line_in);
    free(_scratch);
    free(_line_out);
}

bool ModeScanner::begin()
{
    if (_audio == nullptr) {
        _audio = (int16_t *)bigAlloc(kAudio * sizeof(int16_t));
        _fft = kiss_fftr_alloc(kFft, 0, nullptr, nullptr);
        _window = (float *)malloc(kFft * sizeof(float));
        _time = (float *)bigAlloc(kFft * sizeof(float));
        _freq = bigAlloc((kFft / 2 + 1) * sizeof(kiss_fft_cpx));
        _rows = (uint8_t *)bigAlloc((size_t)kRows * kBins);
        _snap_audio = (int16_t *)bigAlloc(kAudio * sizeof(int16_t));
        _snap_rows = (uint8_t *)bigAlloc((size_t)kRows * kBins);
        _line_fft = kiss_fftr_alloc(kLineFft, 0, nullptr, nullptr);
        _zi = (float *)bigAlloc(kFine * sizeof(float));
        _zq = (float *)bigAlloc(kFine * sizeof(float));
        _amp = (float *)bigAlloc(kFine * sizeof(float));
        _inst = (float *)bigAlloc(kFine * sizeof(float));
        _pr = (float *)bigAlloc(kFine * sizeof(float));
        _pi = (float *)bigAlloc(kFine * sizeof(float));
        _fsk_d = (float *)bigAlloc(kFine * sizeof(float));
        _fsk_t = (float *)bigAlloc(kFine * sizeof(float));
        _line_in = (float *)bigAlloc(kLineFft * sizeof(float));
        _scratch = (float *)bigAlloc(kLineFft * sizeof(float));
        _line_out = bigAlloc((kLineFft / 2 + 1) * sizeof(kiss_fft_cpx));
    }
    if (!_audio || !_snap_audio || !_snap_rows || !_fft || !_window || !_time || !_freq || !_rows || !_line_fft || !_zi || !_zq || !_amp || !_inst || !_pr || !_pi || !_fsk_d || !_fsk_t ||
        !_line_in || !_scratch || !_line_out) {
        return false;
    }
    for (int v = 0; v < 256; ++v) {
        power_of[v] = powf(10.0f, (v - 240) / 20.0f);
    }
    for (int i = 0; i < kFft; ++i) {
        float s = sinf((float)M_PI * i / kFft);
        _window[i] = 2.0f / kFft * s * s / 32768.0f;  // a full-scale sine gives 0 dB
    }
    reset();
    return true;
}

void ModeScanner::reset()
{
    _apos = 0;
    _total = 0;
    _since_hop = 0;
    _row_pos = 0;
    _row_count = 0;
    _snap_n = 0;
    _snap_row_count = 0;
}

void ModeScanner::snapshot()
{
    if (_audio == nullptr) {
        return;
    }
    // Oldest first, so that the analysis needs no ring arithmetic
    _snap_n = (int)std::min(_total, (size_t)kAudio);
    int start = (_apos - _snap_n + kAudio) % kAudio;
    int first = std::min(_snap_n, kAudio - start);
    memcpy(_snap_audio, _audio + start, first * sizeof(int16_t));
    memcpy(_snap_audio + first, _audio, (_snap_n - first) * sizeof(int16_t));
    _snap_row_count = _row_count;
    int row_start = (_row_pos - _row_count + kRows) % kRows;
    for (int a = 0; a < _row_count; ++a) {
        memcpy(_snap_rows + (size_t)a * kBins, _rows + (size_t)((row_start + a) % kRows) * kBins, kBins);
    }
}

void ModeScanner::feed(const int16_t *samples, size_t count)
{
    if (_audio == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        _audio[_apos] = samples[i];
        _apos = (_apos + 1) % kAudio;
        ++_total;
        if (++_since_hop == kHop) {
            _since_hop = 0;
            if (_total >= (size_t)kFft) {
                spectrumRow();
            }
        }
    }
}

void ModeScanner::spectrumRow()
{
    int start = (_apos - kFft + kAudio) % kAudio;
    for (int i = 0; i < kFft; ++i) {
        _time[i] = _window[i] * _audio[(start + i) % kAudio];
    }
    kiss_fft_cpx *freq = (kiss_fft_cpx *)_freq;
    kiss_fftr(_fft, _time, freq);
    uint8_t *row = _rows + (size_t)_row_pos * kBins;
    for (int b = 0; b < kBins; ++b) {
        const kiss_fft_cpx &c = freq[kFirstBin + b];
        float db = 10.0f * log10f(1e-12f + c.r * c.r + c.i * c.i);
        int scaled = (int)(2 * db + 240);  // 0.5 dB steps, 0 = -120 dB
        row[b] = (uint8_t)std::min(255, std::max(0, scaled));
    }
    _row_pos = (_row_pos + 1) % kRows;
    if (_row_count < kRows) {
        ++_row_count;
    }
}

float ModeScanner::rowDb(int age, int bin) const
{
    return (_snap_rows[(size_t)(_snap_row_count - 1 - age) * kBins + bin] - 240) * 0.5f;
}

/// Strongest periodic line between 4 and 130 Hz in x[0..n) at the fine rate.
/// Returns its frequency; strength is its height over the median of the spectrum.
float ModeScanner::lineSearch(const float *x, int n, float *strength)
{
    // Less a running mean over a quarter second: what changes slowly, like a signal
    // coming and going, is no symbol rate; 6.25 Hz still passes
    constexpr int kMean = 251;
    double run = 0;
    for (int i = 0; i < kLineFft; ++i) {
        _line_in[i] = 0;
    }
    for (int i = 0; i < n + kMean / 2; ++i) {
        if (i < n) {
            run += x[i];
        }
        if (i >= kMean) {
            run -= x[i - kMean];
        }
        int c = i - kMean / 2;
        if (c >= 0 && c < n) {
            int len = std::min(i, n - 1) - std::max(0, i - kMean + 1) + 1;
            _line_in[c] = (float)(x[c] - run / len);
        }
    }
    kiss_fft_cpx *out = (kiss_fft_cpx *)_line_out;
    kiss_fftr(_line_fft, _line_in, out);
    const float per_bin = kFineRate / kLineFft;
    int k0 = (int)(4.0f / per_bin), k1 = (int)(170.0f / per_bin);
    int lo = (int)(5.0f / per_bin), hi = (int)(160.0f / per_bin);
    int m = 0;
    for (int k = k0; k <= k1; ++k) {
        _scratch[m++] = sqrtf(out[k].r * out[k].r + out[k].i * out[k].i);
    }
    float median = percentile(_scratch, m, 0.5f) + 1e-9f;
    auto mag = [&](int k) { return sqrtf(out[k].r * out[k].r + out[k].i * out[k].i); };
    // Local maximum within a few bins of k
    auto peakNear = [&](float f, int *at) {
        int c = (int)lrintf(f / per_bin), best = c;
        for (int k = std::max(lo, c - 3); k <= std::min(hi, c + 3); ++k) {
            if (mag(k) > mag(best)) {
                best = k;
            }
        }
        *at = best;
        return mag(best) / median;
    };
    int best = lo;
    for (int k = lo; k <= hi; ++k) {
        if (mag(k) > mag(best)) {
            best = k;
        }
    }
    float best_strength = mag(best) / median;
    // A line at half or a third of the rate is the true rate, its harmonics are louder
    for (int d = 3; d >= 2; --d) {
        int at;
        float s = peakNear(best * per_bin / d, &at);
        if (at >= lo && s >= 4.0f && s >= 0.45f * best_strength) {
            best = at;
            best_strength = s;
            break;
        }
    }
    float f = best * per_bin;
    if (best > lo && best < hi) {
        float a = mag(best - 1), b = mag(best), c = mag(best + 1);
        float den = a - 2 * b + c;
        if (den < 0) {
            f += 0.5f * (a - c) / den * per_bin;
        }
    }
    *strength = best_strength;
    _line_median = median;
    return f;
}

/// Height of the last line search's spectrum near f, over its median
float ModeScanner::lineAt(float f) const
{
    const kiss_fft_cpx *out = (const kiss_fft_cpx *)_line_out;
    const float per_bin = kFineRate / kLineFft;
    int c = (int)lrintf(f / per_bin);
    float best = 0;
    for (int k = std::max(1, c - 2); k <= std::min(kLineFft / 2, c + 2); ++k) {
        best = std::max(best, sqrtf(out[k].r * out[k].r + out[k].i * out[k].i));
    }
    return best / _line_median;
}

ModeScanner::Fine ModeScanner::fine(float low_hz, float high_hz)
{
    Fine r;
    int n = _snap_n;
    int m = n / kDecim;
    if (m < 1000) {
        return r;
    }
    float centre = 0.5f * (low_hz + high_hz);
    float width = high_hz - low_hz;
    const int16_t *audio = _snap_audio + (n - m * kDecim);

    // Mixed to baseband, summed over 1 ms, then averaged to about the width of the
    // signal. The oscillator is a turning pointer: sines and cosines per sample
    // would take far too long on the device.
    const float step_c = cosf(kTwoPi * centre / kRate), step_s = -sinf(kTwoPi * centre / kRate);
    float pc = 1, ps = 0;
    for (int j = 0; j < m; ++j) {
        float si = 0, sq = 0;
        const int16_t *x = audio + j * kDecim;
        for (int i = 0; i < kDecim; ++i) {
            si += x[i] * pc;
            sq += x[i] * ps;
            float c = pc * step_c - ps * step_s;
            ps = pc * step_s + ps * step_c;
            pc = c;
        }
        // Keeps the pointer at length 1
        float g = 1.5f - 0.5f * (pc * pc + ps * ps);
        pc *= g;
        ps *= g;
        _zi[j] = si;
        _zq[j] = sq;
    }
    int len = std::min(25, std::max(1, (int)lrintf(kFineRate / (width + 15.0f))));
    float ai = 0, aq = 0;
    for (int j = 0; j < m; ++j) {
        ai += _zi[j];
        aq += _zq[j];
        if (j >= len) {
            ai -= _line_in[j % len];
            aq -= _scratch[j % len];
        }
        // The ring of the running sum lives in the two scratch arrays until the line search
        float pi = _zi[j], pq = _zq[j];
        _line_in[j % len] = pi;
        _scratch[j % len] = pq;
        _zi[j] = ai;
        _zq[j] = aq;
        _amp[j] = sqrtf(ai * ai + aq * aq);
    }

    // On and off
    memcpy(_inst, _amp, m * sizeof(float));
    float top = percentile(_inst, m, 0.95f);
    float floor_ = percentile(_inst, m, 0.10f);
    float on_level = 0.35f * top;
    int on = 0;
    for (int j = 0; j < m; ++j) {
        on += _amp[j] > on_level;
    }
    r.duty = (float)on / m;
    r.contrast = top > 0 ? floor_ / top : 0;

    // Phase steps from one millisecond to the next. Summed over a few of them they
    // give the frequency weighted by the strength of the signal, which stays
    // steady in noise where single phase steps scatter.
    _pr[0] = _pi[0] = 0;
    for (int j = 1; j < m; ++j) {
        _pr[j] = _zi[j] * _zi[j - 1] + _zq[j] * _zq[j - 1];
        _pi[j] = _zq[j] * _zi[j - 1] - _zi[j] * _zq[j - 1];
    }
    auto discriminate = [&](int k, float *out) {
        double sr = 0, si = 0;
        for (int j = 0; j < m; ++j) {
            sr += _pr[j];
            si += _pi[j];
            if (j >= k) {
                sr -= _pr[j - k];
                si -= _pi[j - k];
            }
            int c = j - k / 2;
            if (c >= 0) {
                out[c] = j >= k && _amp[c] > on_level ? atan2f((float)si, (float)sr) * kFineRate / kTwoPi : NAN;
            }
        }
        for (int c = std::max(0, m - k / 2); c < m; ++c) {
            out[c] = NAN;
        }
    };

    // The frequency over 4 ms where the signal is on
    discriminate(4, _inst);
    int valid = 0;
    for (int j = 0; j < m; ++j) {
        if (!isnan(_inst[j])) {
            _scratch[valid++] = _inst[j];
        }
    }
    if (valid < 200) {
        return r;
    }

    // Two strongest clusters of the frequency, in 2 Hz steps
    float lo_f = percentile(_scratch, valid, 0.05f);
    float hi_f = percentile(_scratch, valid, 0.95f);
    r.span_hz = hi_f - lo_f;
    r.centre_hz = centre + percentile(_scratch, valid, 0.5f);
    constexpr int kHist = 600;  // +-600 Hz
    static uint16_t hist[kHist];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < valid; ++i) {
        int k = (int)lrintf(_scratch[i] * 0.5f) + kHist / 2;
        if (k >= 0 && k < kHist) {
            ++hist[k];
        }
    }
    auto mass = [&](int c) {
        int s = 0;
        for (int k = std::max(0, c - 10); k <= std::min(kHist - 1, c + 10); ++k) {
            s += hist[k];
        }
        return s;
    };
    int ca = 0;
    for (int k = 0; k < kHist; ++k) {
        if (mass(k) > mass(ca)) {
            ca = k;
        }
    }
    int cb = -1;
    for (int k = 0; k < kHist; ++k) {
        if (abs(k - ca) >= 30 && (cb < 0 || mass(k) > mass(cb))) {
            cb = k;
        }
    }
    r.tone_a = centre + (ca - kHist / 2) * 2.0f;
    r.mass_a = (float)mass(ca) / valid;
    if (cb >= 0) {
        r.tone_b = centre + (cb - kHist / 2) * 2.0f;
        r.mass_b = (float)mass(cb) / valid;
    }

    // FSK: which of the two tones is the stronger, millisecond by millisecond, as a
    // teleprinter receiver decides. Its changes give the rate far more clearly
    // than the frequency over the whole band, which noise from both sides fills.
    if (r.mass_b > 0) {
        float shift = fabsf(r.tone_a - r.tone_b);
        int len = std::min(6, std::max(2, (int)lrintf(kFineRate / shift)));
        float ca_ = cosf(-kTwoPi * (r.tone_a - centre) / kFineRate), sa_ = sinf(-kTwoPi * (r.tone_a - centre) / kFineRate);
        float cb_ = cosf(-kTwoPi * (r.tone_b - centre) / kFineRate), sb_ = sinf(-kTwoPi * (r.tone_b - centre) / kFineRate);
        float pa = 1, qa = 0, pb = 1, qb = 0;  // the two turning pointers
        float ai = 0, aq = 0, bi = 0, bq = 0;
        for (int j = 0; j < m; ++j) {
            // Each tone moved to zero and summed over len ms
            float ra = _zi[j] * pa - _zq[j] * qa, ia = _zi[j] * qa + _zq[j] * pa;
            float rb = _zi[j] * pb - _zq[j] * qb, ib = _zi[j] * qb + _zq[j] * pb;
            _fsk_d[j] = ra;  // kept for the subtraction below, overwritten once used
            _fsk_t[j] = ia;
            ai += ra, aq += ia;
            float t = pa * ca_ - qa * sa_;
            qa = pa * sa_ + qa * ca_;
            pa = t;
            t = pb * cb_ - qb * sb_;
            qb = pb * sb_ + qb * cb_;
            pb = t;
            float g = 1.5f - 0.5f * (pa * pa + qa * qa);
            pa *= g, qa *= g;
            g = 1.5f - 0.5f * (pb * pb + qb * qb);
            pb *= g, qb *= g;
            bi += rb, bq += ib;
            if (j >= len) {
                ai -= _fsk_d[j - len], aq -= _fsk_t[j - len];
            }
            // The tone b sums need their own history: kept in the line search's input for now
            _line_in[j % 8] = rb;
            _scratch[j % 8] = ib;
            if (j >= len) {
                bi -= _line_in[(j - len) % 8], bq -= _scratch[(j - len) % 8];
            }
            _zi[j] = ai * ai + aq * aq;  // z is not needed past this point
            _zq[j] = bi * bi + bq * bq;
        }
        for (int j = 0; j < m; ++j) {
            _fsk_d[j] = _amp[j] > on_level ? (_zi[j] > _zq[j] ? 1.0f : -1.0f) : 0.0f;
        }
        for (int j = 1; j < m; ++j) {
            _fsk_t[j] = _fsk_d[j] != 0 && _fsk_d[j - 1] != 0 && _fsk_d[j] != _fsk_d[j - 1] ? 1.0f : 0.0f;
        }
        _fsk_t[0] = 0;
        float strength;
        lineSearch(_fsk_t, m, &strength);
        for (float rate : {45.45f, 50.0f, 75.0f, 90.9f, 100.0f, 110.0f, 150.0f}) {
            float s = lineAt(rate);
            if (s > r.fsk_strength) {
                r.fsk_strength = s;
                r.fsk_line = rate;
            }
        }
        // Is the line the bit rate, or twice it? Teleprinters send 1.5 stop bits,
        // which puts the bits on a grid of half bits. Stays on one tone of a whole
        // line period then outnumber those of half of one.
        if (r.fsk_line > 0) {
            float period = kFineRate / r.fsk_line;  // in samples
            int halves = 0, wholes = 0, run = 0;
            float side = 0;
            for (int j = 0; j < m; ++j) {
                if (_fsk_d[j] == side && side != 0) {
                    ++run;
                    continue;
                }
                if (side != 0 && _fsk_d[j] != 0) {
                    halves += fabsf(run - period) <= 0.3f * period;
                    wholes += fabsf(run - 2 * period) <= 0.3f * 2 * period;
                }
                side = _fsk_d[j];
                run = 1;
            }
            r.half_rate = wholes > 1.5f * halves && wholes >= 10;
        }
    }

    // Symbol rates. Changes of frequency mark the symbols of FSK, seen through
    // several smoothings: fast symbols need little, the soft changes of FT8 much.
    // The mixer's output is not needed any more; its buffers take the work.
    for (int k : {1, 4, 16}) {
        float *f = _inst;
        if (k != 4) {
            discriminate(k, _zi);
            f = _zi;
        }
        for (int j = 0; j < m; ++j) {
            float a = f[j], b = j >= k ? f[j - k] : NAN;
            _zq[j] = isnan(a) || isnan(b) ? 0.0f : std::min(fabsf(a - b), 100.0f);
        }
        float strength;
        float baud = lineSearch(_zq, m, &strength);
        if (strength > r.line_f) {
            r.line_f = strength;
            r.baud_f = baud;
        }
    }
    // Changes of the envelope mark those of PSK
    for (int j = 1; j < m; ++j) {
        _zq[j] = fabsf(_amp[j] - _amp[j - 1]);
    }
    _zq[0] = 0;
    r.baud_e = lineSearch(_zq, m, &r.line_e);

    // CW: the shortest elements are the dots
    int runs = 0, run = 0;
    for (int j = 0; j < m; ++j) {
        if (_amp[j] > on_level) {
            ++run;
        } else {
            if (run >= 15) {
                _scratch[runs++] = (float)run;
            }
            run = 0;
        }
    }
    if (runs >= 6) {
        float dot_ms = percentile(_scratch, runs, 0.25f) * 1000.0f / kFineRate;
        r.wpm = (int)lrintf(1200.0f / dot_ms);
    }
    return r;
}

ScanResult ModeScanner::analyse()
{
    snapshot();
    return analyseSnapshot();
}

ScanResult ModeScanner::analyseSnapshot()
{
    ScanResult r;
    if (_audio == nullptr || _snap_row_count < 24) {
        return r;
    }

    // Spectrum of the last seconds and the noise under it
    const int recent = std::min(_snap_row_count, kRecentRows);
    static float avg[kBins], sm[kBins], sorted[kBins];
    for (int b = 0; b < kBins; ++b) {
        PowerMean p;
        for (int a = 0; a < recent; ++a) {
            p.add(_snap_rows[(size_t)(_snap_row_count - 1 - a) * kBins + b]);
        }
        avg[b] = p.db();
    }
    for (int b = 0; b < kBins; ++b) {
        sm[b] = (avg[std::max(0, b - 1)] + avg[b] + avg[std::min(kBins - 1, b + 1)]) / 3.0f;
    }
    // The noise where every receiver passes audio; beyond its filter it is far lower
    int quiet = 0;
    for (int b = hzBin(500); b <= hzBin(2500); ++b) {
        sorted[quiet++] = sm[b];
    }
    const float noise = percentile(sorted, quiet, 0.25f);

    // The signal to look at
    int pk = 1;
    if (_target_hz > 0) {
        int t = std::min(kBins - 2, std::max(1, hzBin(_target_hz)));
        pk = t;
        for (int b = std::max(1, t - 12); b <= std::min(kBins - 2, t + 12); ++b) {
            if (sm[b] > sm[pk]) {
                pk = b;
            }
        }
    } else {
        for (int b = 1; b < kBins - 1; ++b) {
            if (sm[b] > sm[pk]) {
                pk = b;
            }
        }
    }
    const float peak = sm[pk];
    r.level_db = peak - noise;
    r.peak_hz = binHz((float)pk);
    // A signal 70 dB under full scale is no more than a few bits: crosstalk, not reception
    r.quiet = peak < -70.0f;
    if (r.level_db < 6.0f || r.quiet) {
        return r;
    }

    // The band around it that stands above the noise, with small gaps bridged
    const float thr = std::max(noise + 4.0f, peak - 15.0f);
    constexpr int kMaxReach = 102;  // 600 Hz to each side
    auto grow = [&](int dir) {
        int edge = pk, gap = 0;
        for (int b = pk + dir; b >= 0 && b < kBins && abs(b - pk) <= kMaxReach; b += dir) {
            if (sm[b] >= thr) {
                edge = b;
                gap = 0;
            } else if (++gap > 2) {
                break;
            }
        }
        return edge;
    };
    int lo = grow(-1), hi = grow(+1);
    bool capped = pk - lo >= kMaxReach - 3 || hi - pk >= kMaxReach - 3;
    r.low_hz = binHz(lo - 0.5f);
    r.high_hz = binHz(hi + 0.5f);
    const float width = r.high_hz - r.low_hz;

    // Bursts: complete transmissions in the last 30 s, from the spectra over the band
    {
        int longest = 0, run = 0, gap = 0;
        bool touched_start = true;
        for (int a = _snap_row_count - 1; a >= 0; --a) {
            float v = -200;
            for (int b = lo; b <= hi; ++b) {
                v = std::max(v, rowDb(a, b));
            }
            bool on = v > noise + 8.0f;
            if (on) {
                run += gap + 1;
                gap = 0;
            } else if (run > 0 && ++gap > 1) {
                if (!touched_start) {
                    longest = std::max(longest, run);
                }
                run = 0;
                gap = 0;
                touched_start = false;
            } else if (run == 0) {
                touched_start = false;
            }
        }
        r.burst_s = longest * (float)kHop / kRate;
    }

    // A partner tone of FSK at one of the usual shifts
    float partner_lo = r.low_hz, partner_hi = r.high_hz;
    bool partner = false;
    if (width < 120.0f) {
        static const float shifts[][2] = {{140, 200}, {400, 480}, {800, 900}};
        float best = -200;
        for (const auto &s : shifts) {
            for (int dir = -1; dir <= 1; dir += 2) {
                for (int b = pk + dir * (int)(s[0] / kBinHz); dir * (b - pk) <= (int)(s[1] / kBinHz); b += dir) {
                    if (b < 1 || b >= kBins - 1) {
                        break;
                    }
                    if (sm[b] >= sm[b - 1] && sm[b] >= sm[b + 1] && sm[b] >= peak - 10.0f && sm[b] >= noise + 6.0f &&
                        sm[b] > best) {
                        best = sm[b];
                        partner = true;
                        partner_lo = std::min(r.low_hz, binHz(b - 3.0f));
                        partner_hi = std::max(r.high_hz, binHz(b + 3.0f));
                    }
                }
            }
        }
    }

    // Where the energy lies: SSTV and WEFAX fill 1500 to 2300 Hz, SSTV adds its 1200 Hz pulses
    auto share = [&](float from, float to) {
        int n = 0, above = 0;
        for (int b = std::max(0, hzBin(from)); b <= std::min(kBins - 1, hzBin(to)); ++b) {
            ++n;
            above += sm[b] > noise + 6.0f;
        }
        return n ? (float)above / n : 0.0f;
    };
    const float picture = share(1500, 2300), low = share(300, 1400);
    float sync = -200;
    for (int b = hzBin(1188); b <= hzBin(1212); ++b) {
        sync = std::max(sync, sm[b]);
    }
    PowerMean valley_mean;
    for (int b = hzBin(1300); b <= hzBin(1420); ++b) {
        valley_mean.add((uint8_t)std::min(255.0f, std::max(0.0f, 2 * sm[b] + 240)));
    }
    const float valley = valley_mean.db();
    // The short pulses spread beside 1200 Hz too; voice would fill the low band entirely
    if (picture > 0.4f && sync - noise >= 10.0f && sync >= valley && low < 0.6f) {
        r.mode = ScanMode::SSTV;
        r.low_hz = 1150;
        r.high_hz = 2350;
        r.confidence = (int)std::min(95.0f, 40.0f + 2.0f * (sync - noise));
        return r;
    }

    // Narrow signals: tones and symbol rate decide
    if (!capped && width < 500.0f) {
        Fine f = fine(r.low_hz - 10.0f, r.high_hz + 10.0f);
        r.duty = f.duty;
        r.baud = f.line_f >= f.line_e ? f.baud_f : f.baud_e;
        bool line_f = f.line_f >= 5.0f, line_e = f.line_e >= 5.0f;
        auto confidence = [](float line, float extra) {
            return (int)std::min(99.0f, 45.0f + 4.0f * line + extra);
        };
        // FSK with both tones in the band, or with the partner found beside it
        Fine g = f;
        if (partner && !(f.mass_b >= 0.15f && fabsf(f.tone_a - f.tone_b) >= 60.0f)) {
            g = fine(partner_lo - 10.0f, partner_hi + 10.0f);
        }
        bool fsk_rate = g.fsk_strength >= 5.0f;
        if (fsk_rate && g.mass_a >= 0.15f && g.mass_b >= 0.15f && g.mass_a + g.mass_b >= 0.55f &&
            fabsf(g.tone_a - g.tone_b) >= 60.0f) {
            // The tones from the spectrum: the instantaneous frequency is pulled inwards by the changes
            auto spectralPeak = [&](float hz) {
                int c = hzBin(hz), best = c;
                for (int b = std::max(1, c - 5); b <= std::min(kBins - 2, c + 5); ++b) {
                    if (sm[b] > sm[best]) {
                        best = b;
                    }
                }
                float den = sm[best - 1] - 2 * sm[best] + sm[best + 1];
                float off = den < 0 ? 0.5f * (sm[best - 1] - sm[best + 1]) / den : 0.0f;
                return binHz(best + off);
            };
            float ta = spectralPeak(g.tone_a), tb = spectralPeak(g.tone_b);
            r.tones = 2;
            r.shift_hz = fabsf(ta - tb);
            r.low_hz = std::min(ta, tb) - 20.0f;
            r.high_hz = std::max(ta, tb) + 20.0f;
            r.duty = g.duty;
            // With 1.5 stop bits the bits lie on a grid of half bits, and the line shows at
            // twice the rate: 90.9 Hz is 45.45 Bd, and 100 Hz is either NAVTEX or 50 Bd
            float line = g.fsk_line;
            r.baud = line;
            if (near(line, 90.9f, 0.01f)) {
                r.baud = 45.45f;
            } else if (g.half_rate && (near(line, 100.0f, 0.01f) || near(line, 150.0f, 0.01f))) {
                r.baud = 0.5f * line;
            }
            bool narrow_shift = r.shift_hz > 140 && r.shift_hz < 200;
            if (r.baud > 0 && near(r.baud, 100.0f, 0.05f) && narrow_shift) {
                r.mode = ScanMode::NAVTEX;
            } else {
                r.mode = ScanMode::RTTY;  // any other rate or shift, which the numbers tell
            }
            r.confidence = confidence(g.fsk_strength, 0);
            return r;
        }
        // Slot modes need their width and an exact rate (characters of RTTY at 45.45 Bd
        // put a line at 6.06 Hz), and either their length of transmission or a clear line
        bool ft8_burst = r.burst_s > 11.0f && r.burst_s < 14.5f, ft4_burst = r.burst_s > 4.0f && r.burst_s < 6.5f;
        if (line_f && near(f.baud_f, 6.25f, 0.02f) && f.span_hz > 20 && f.span_hz < 70 && width > 30 && width < 120 &&
            (ft8_burst || f.line_f >= 12.0f)) {
            r.mode = ScanMode::FT8;
            r.baud = f.baud_f;
            r.tones = 8;
            r.confidence = confidence(f.line_f, ft8_burst ? 15.0f : 0.0f);
            return r;
        }
        if (line_f && near(f.baud_f, 20.83f, 0.03f) && f.span_hz > 35 && f.span_hz < 110 && width > 50 && width < 160 &&
            (ft4_burst || f.line_f >= 12.0f)) {
            r.mode = ScanMode::FT4;
            r.baud = f.baud_f;
            r.tones = 4;
            r.confidence = confidence(f.line_f, ft4_burst ? 15.0f : 0.0f);
            return r;
        }
        if (line_e && f.span_hz < 45) {
            if (near(f.baud_e, 31.25f, 0.06f)) {
                r.mode = ScanMode::PSK31;
            } else if (near(f.baud_e, 62.5f, 0.06f)) {
                r.mode = ScanMode::PSK63;
            }
            if (r.mode != ScanMode::None) {
                r.baud = f.baud_e;
                r.tones = 1;
                r.confidence = confidence(f.line_e, 0);
                return r;
            }
        }
        if (f.span_hz < 30 && f.duty > 0.15f && f.duty < 0.9f && f.contrast < 0.25f && f.wpm > 0) {
            r.mode = ScanMode::CW;
            r.tones = 1;
            r.baud = 0;
            r.wpm = f.wpm;
            r.confidence = f.wpm >= 5 && f.wpm <= 60 ? 75 : 50;
            return r;
        }
        if (f.span_hz < 12 && f.duty > 0.9f) {
            r.mode = ScanMode::Carrier;
            r.tones = 1;
            r.baud = 0;
            r.confidence = 70;
            return r;
        }
    }

    if (capped || width >= 300.0f || picture > 0.5f) {
        r.low_hz = binHz(lo - 0.5f);
        r.high_hz = binHz(hi + 0.5f);
        r.baud = 0;
        r.duty = 0;
        if (picture > 0.5f && low < 0.3f) {
            r.mode = ScanMode::WEFAX;
            r.confidence = (int)std::min(90.0f, 40.0f + 60.0f * picture);
        } else if (low >= 0.3f) {
            r.mode = ScanMode::Voice;
            r.confidence = 40;
        } else {
            r.mode = ScanMode::Unknown;
        }
        return r;
    }
    r.mode = ScanMode::Unknown;
    return r;
}

}  // namespace digi
