// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <stddef.h>
#include <stdint.h>

struct kiss_fftr_state;

namespace digi {

/// What the scanner takes a signal for.
enum class ScanMode { None, Unknown, Carrier, CW, RTTY, NAVTEX, FT8, FT4, PSK31, PSK63, SSTV, WEFAX, Voice };

const char *scanModeName(ScanMode mode);

struct ScanResult {
    ScanMode mode = ScanMode::None;
    int confidence = 0;      // 0..100
    float low_hz = 0;        // band the signal occupies
    float high_hz = 0;
    float peak_hz = 0;       // its strongest frequency
    float level_db = 0;      // peak over the noise, in a 5.9 Hz wide bin
    int tones = 0;           // FSK 2, FT8 8, FT4 4, single-tone modes 1; 0 if not known
    float shift_hz = 0;      // FSK: distance of the two tones
    float baud = 0;          // symbol rate found, 0 if none
    float duty = 0;          // share of the time the signal is on, 0..1
    float burst_s = 0;       // longest transmission seen from start to end, 0 if none
    int wpm = 0;             // CW speed
    bool quiet = false;      // the input carries next to nothing: a cable or a level to check
};

/// Guesses the mode of a received signal from its spectrum, its tones, its
/// symbol rate and its timing. It looks at the strongest signal in the
/// passband or at the one nearest to a chosen frequency. 12 kHz audio.
class ModeScanner {
public:
    ~ModeScanner();

    bool begin();
    void reset();
    void feed(const int16_t *samples, size_t count);

    /// Analyses the signal nearest to this audio frequency; 0 picks the strongest.
    void setTarget(float hz) { _target_hz = hz; }

    /// snapshot() and analyseSnapshot() in one.
    ScanResult analyse();

    /// Copies the audio and spectra collected so far; quick. analyseSnapshot()
    /// then works on the copy and may run in another task while feed() goes on.
    void snapshot();
    ScanResult analyseSnapshot();

    /// Seconds of audio collected since begin() or reset().
    float seconds() const { return _total / 12000.0f; }

    /// Analysis of the signal between two frequencies, from the recent audio
    struct Fine {
        float span_hz = 0;       // 5 to 95 % of the instantaneous frequency
        float centre_hz = 0;     // median instantaneous frequency
        float baud_f = 0, line_f = 0;  // rate and strength of the strongest line in the frequency changes
        float baud_e = 0, line_e = 0;  // the same for the envelope
        float duty = 0;
        float contrast = 0;      // envelope when off relative to when on, 0..1
        float tone_a = 0, tone_b = 0;  // the two strongest frequency clusters
        float mass_a = 0, mass_b = 0;  // their shares of the time the signal is on
        float fsk_line = 0, fsk_strength = 0;  // strongest line at a teleprinter rate
        bool half_rate = false;  // FSK: the bits last two periods of the line (1.5 stop bits)
        int wpm = 0;
    };

private:
    void spectrumRow();
    float rowDb(int age, int bin) const;  // age 0 = newest row
    Fine fine(float low_hz, float high_hz);
    float lineSearch(const float *x, int n, float *strength);
    float lineAt(float f) const;

    int16_t *_audio = nullptr;
    int _apos = 0;
    size_t _total = 0;
    int _since_hop = 0;

    kiss_fftr_state *_fft = nullptr;
    float *_window = nullptr, *_time = nullptr;
    void *_freq = nullptr;

    uint8_t *_rows = nullptr;
    int _row_pos = 0, _row_count = 0;

    // The copy the analysis works on, oldest first
    int16_t *_snap_audio = nullptr;
    uint8_t *_snap_rows = nullptr;
    int _snap_n = 0, _snap_row_count = 0;

    // Fine analysis
    kiss_fftr_state *_line_fft = nullptr;
    float *_zi = nullptr, *_zq = nullptr, *_amp = nullptr, *_inst = nullptr, *_pr = nullptr, *_pi = nullptr, *_fsk_d = nullptr, *_fsk_t = nullptr, *_line_in = nullptr, *_scratch = nullptr;
    void *_line_out = nullptr;
    float _line_median = 1;

    float _target_hz = 0;
};

}  // namespace digi
