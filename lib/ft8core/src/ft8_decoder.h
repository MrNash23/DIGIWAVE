// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include <common/monitor.h>
}

namespace ft8 {

constexpr int kSampleRate = 12000;

enum class Protocol { FT8, FT4 };

/// Timing of one protocol; all times in milliseconds
struct ProtocolInfo {
    const char *name;
    int slot_ms;        // length of a transmit/receive period
    int tx_ms;          // duration of the signal
    int symbols;
    float symbol_s;     // symbol period in seconds
};
const ProtocolInfo &info(Protocol p);

// A signal with DT = 0 starts this long after the slot boundary (WSJT-X convention)
constexpr float kNominalStart = 0.5f;

struct Decoded {
    int snr;         // estimated SNR in dB, 2500 Hz reference bandwidth
    float dt;        // time offset in seconds relative to the nominal start
    float freq;      // audio frequency of the lowest tone in Hz
    char text[36];   // full message text
    char call_to[14];
    char call_de[14];
    char extra[8];   // grid, report, RR73, 73 or empty
    bool is_cq;
};

/// Splits a decoded message text into call_to / call_de / extra and sets is_cq.
void parseMessage(Decoded &d);

class Decoder {
public:
    ~Decoder();

    /// May be called again to switch the protocol.
    bool begin(Protocol protocol = Protocol::FT8, int sample_rate = kSampleRate, float f_min = 200.0f, float f_max = 3000.0f);

    /// Start a new slot: drops the accumulated waterfall.
    void reset();

    /// Feed PCM samples; complete symbol blocks are added to the waterfall.
    void feed(const int16_t *samples, size_t count);

    /// Number of symbol blocks collected in this slot.
    int blocks() const { return _mon.wf.num_blocks; }
    int maxBlocks() const { return _mon.wf.max_blocks; }
    int bins() const { return _mon.wf.num_bins; }
    float binHz() const { return 1.0f / _mon.symbol_period; }
    float minHz() const { return _mon.min_bin / _mon.symbol_period; }

    /// Spectrum of one collected block, bins() values in 0.5 dB steps (0 = -120 dB).
    const uint8_t *row(int block) const { return _mon.wf.mag + (size_t)block * _mon.wf.block_stride; }

    /// Peak input level of the samples fed since the last call, 0..32767.
    int takePeak();

    /// Decode everything collected in this slot. Returns the number of messages written to out.
    int decode(Decoded *out, int max_out);

private:
    int estimateSnr(const ftx_candidate_t &cand, const ftx_message_t &msg, float noise_db) const;
    float noiseFloorDb() const;

    monitor_t _mon{};
    bool _ready = false;
    Protocol _protocol = Protocol::FT8;
    float *_block = nullptr;
    int _fill = 0;
    int _peak = 0;
};

}  // namespace ft8
