// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "ft8_encoder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft8_hashtable.h"

extern "C" {
#include <ft8/constants.h>
#include <ft8/encode.h>
#include <ft8/message.h>
}

// The pulse shape and phase accumulation follow synth_gfsk() in demo/gen_ft8.c
// of ft8_lib, reworked to produce samples on demand instead of a whole slot.

namespace ft8 {

namespace {
constexpr float kGfskK = 5.336446f;  // pi * sqrt(2 / ln 2)
constexpr float kTwoPi = 6.28318530718f;
}  // namespace

Synth::~Synth()
{
    free(_pulse);
}

namespace {

bool pack(const char *text, Packing packing, ftx_message_t *msg)
{
    switch (packing) {
    case Packing::FreeText:
        return strlen(text) <= 13 && ftx_message_encode_free(msg, text) == FTX_MESSAGE_RC_OK;
    case Packing::NonStandard: {
        // "CQ CALL" or "<OTHER> CALL [EXTRA]"; the library wants the hashed call without brackets
        char buf[40];
        strncpy(buf, text, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *to = strtok(buf, " ");
        char *de = strtok(nullptr, " ");
        char *extra = strtok(nullptr, " ");
        if (to == nullptr || de == nullptr) {
            return false;
        }
        size_t n = strlen(to);
        if (to[0] == '<' && n > 2 && to[n - 1] == '>') {
            to[n - 1] = '\0';
            ++to;
        }
        return ftx_message_encode_nonstd(msg, hashInterface(), to, de, extra ? extra : "") == FTX_MESSAGE_RC_OK;
    }
    case Packing::Auto:
        break;
    }
    return ftx_message_encode(msg, hashInterface(), text) == FTX_MESSAGE_RC_OK;
}

}  // namespace

bool Synth::begin(const char *text, float f0, float amplitude, Protocol protocol, int sample_rate, Packing packing)
{
    ftx_message_t msg;
    if (!pack(text, packing, &msg)) {
        return false;
    }
    const ProtocolInfo &pi = info(protocol);
    // Smoothing filter bandwidth: FT8 uses BT = 2, FT4 BT = 1
    const float symbol_bt = protocol == Protocol::FT4 ? 1.0f : 2.0f;
    if (protocol == Protocol::FT4) {
        ft4_encode(msg.payload, _tones);
    } else {
        ft8_encode(msg.payload, _tones);
    }
    _num_tones = pi.symbols;

    int spsym = (int)(0.5f + sample_rate * pi.symbol_s);
    if (spsym != _spsym || _pulse == nullptr) {
        free(_pulse);
        _pulse = (float *)malloc(3 * spsym * sizeof(float));
        if (_pulse == nullptr) {
            _spsym = 0;
            return false;
        }
        _spsym = spsym;
        for (int i = 0; i < 3 * spsym; ++i) {
            float t = i / (float)spsym - 1.5f;
            _pulse[i] = (erff(kGfskK * symbol_bt * (t + 0.5f)) - erff(kGfskK * symbol_bt * (t - 0.5f))) / 2;
        }
    }
    _rate = sample_rate;
    _f0 = f0;
    _amp = amplitude;
    _phi = 0;
    _pos = 0;
    return true;
}

size_t Synth::read(int16_t *out, size_t count)
{
    const size_t total = totalSamples();
    const float dphi_base = kTwoPi * _f0 / _rate;
    const float dphi_peak = kTwoPi / _spsym;
    const int n_ramp = _spsym / 8;

    size_t written = 0;
    while (written < count && _pos < total) {
        // Each sample is shaped by the pulses of the previous, current and next symbol.
        // Beyond the ends the first and last symbols are repeated.
        int sym = (int)(_pos / _spsym);
        int offset = (int)(_pos % _spsym);
        float dphi = dphi_base;
        for (int d = -1; d <= 1; ++d) {
            int s = sym + d;
            int tone = _tones[s < 0 ? 0 : (s >= _num_tones ? _num_tones - 1 : s)];
            dphi += dphi_peak * tone * _pulse[offset + (1 - d) * _spsym];
        }

        float env = 1.0f;
        if (_pos < (size_t)n_ramp) {
            env = (1 - cosf(kTwoPi * _pos / (2 * n_ramp))) / 2;
        } else if (total - 1 - _pos < (size_t)n_ramp) {
            env = (1 - cosf(kTwoPi * (total - 1 - _pos) / (2 * n_ramp))) / 2;
        }

        out[written++] = (int16_t)lroundf(32767.0f * _amp * env * sinf(_phi));
        _phi += dphi;
        if (_phi > kTwoPi) {
            _phi -= kTwoPi;
        }
        ++_pos;
    }
    return written;
}

}  // namespace ft8
