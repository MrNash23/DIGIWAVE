// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "audio_if.h"

#include <LilyGoLib.h>

#include <ft8_decoder.h>

namespace {

constexpr int kCodecRate = 48000;
constexpr int kFactor = kCodecRate / ft8::kSampleRate;  // 4
constexpr int kTaps = 48;

// The codec runs at 48 kHz, a rate every ES8311 clock configuration supports;
// the FT8 engine works at 12 kHz. One low-pass filter serves both directions.
class Es8311Audio : public AudioIo {
public:
    bool begin() override
    {
        // Windowed-sinc low-pass, cutoff 5 kHz at 48 kHz
        const float fc = 5000.0f / kCodecRate;
        float sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            float x = i - (kTaps - 1) / 2.0f;
            float sinc = (x == 0) ? 2 * fc : sinf(2 * PI * fc * x) / (PI * x);
            float window = 0.54f - 0.46f * cosf(2 * PI * i / (kTaps - 1));
            _fir[i] = sinc * window;
            sum += _fir[i];
        }
        for (float &c : _fir) {
            c /= sum;
        }
        _ok = instance.codec.open(16, 1, kCodecRate) == 0;
        setOutputEnabled(false);
        return _ok;
    }

    bool read(int16_t *samples, size_t count) override
    {
        if (!_ok) {
            return false;
        }
        size_t done = 0;
        while (done < count) {
            size_t n = min(count - done, (size_t)kChunk);
            if (instance.codec.read((uint8_t *)_io, n * kFactor * sizeof(int16_t)) != 0) {
                return false;
            }
            for (size_t i = 0; i < n; ++i) {
                // Shift four new input samples into the history, then filter once
                memmove(_rx_hist, _rx_hist + kFactor, (kTaps - kFactor) * sizeof(float));
                for (int k = 0; k < kFactor; ++k) {
                    _rx_hist[kTaps - kFactor + k] = _io[i * kFactor + k];
                }
                float acc = 0;
                for (int t = 0; t < kTaps; ++t) {
                    acc += _fir[t] * _rx_hist[t];
                }
                samples[done + i] = (int16_t)constrain(acc, -32768.0f, 32767.0f);
            }
            done += n;
        }
        return true;
    }

    bool write(const int16_t *samples, size_t count) override
    {
        if (!_ok) {
            return false;
        }
        size_t done = 0;
        while (done < count) {
            size_t n = min(count - done, (size_t)kChunk);
            for (size_t i = 0; i < n; ++i) {
                // Zero-stuffing interpolation: one input sample, three zeros, filtered
                for (int k = 0; k < kFactor; ++k) {
                    memmove(_tx_hist, _tx_hist + 1, (kTaps - 1) * sizeof(float));
                    _tx_hist[kTaps - 1] = (k == 0) ? samples[done + i] * (float)kFactor : 0.0f;
                    float acc = 0;
                    for (int t = 0; t < kTaps; ++t) {
                        acc += _fir[t] * _tx_hist[t];
                    }
                    _io[i * kFactor + k] = (int16_t)constrain(acc, -32768.0f, 32767.0f);
                }
            }
            if (instance.codec.write((uint8_t *)_io, n * kFactor * sizeof(int16_t)) != 0) {
                return false;
            }
            done += n;
        }
        return true;
    }

    void setOutputEnabled(bool on) override
    {
        // The codec keeps the speaker amplifier on while the stream is open;
        // it is only wanted while transmitting.
        instance.io.digitalWrite(EXPANDS_AMP_EN, on ? HIGH : LOW);
    }

    void setInputGain(float db) override { instance.codec.setGain(db); }
    void setOutputVolume(int percent) override { instance.codec.setVolume(constrain(percent, 0, 100)); }

    // I2S DMA buffering plus the filter delay; refined by comparing DT against WSJT-X
    int outputLatencyMs() const override { return 40; }

private:
    static constexpr int kChunk = 240;  // 20 ms at 12 kHz
    bool _ok = false;
    float _fir[kTaps];
    float _rx_hist[kTaps] = {0};
    float _tx_hist[kTaps] = {0};
    int16_t _io[kChunk * kFactor];
};

Es8311Audio audio;

}  // namespace

AudioIo &es8311Audio()
{
    return audio;
}
