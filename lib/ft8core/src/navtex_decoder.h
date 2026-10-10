#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fsk_modem.h"

namespace digi {

/// CCIR 476 codes: 7 bits of which exactly 4 are "B" (the higher tone), first
/// transmitted bit in bit 0.
namespace ccir476 {
constexpr uint8_t kAlpha = 0x0F;  // idle / phasing signal in the repeat position
constexpr uint8_t kBeta = 0x33;
constexpr uint8_t kRep = 0x66;    // phasing signal in the first-transmission position
constexpr uint8_t kLtrs = 0x5A;
constexpr uint8_t kFigs = 0x36;
constexpr uint8_t kCr = 0x78;
constexpr uint8_t kLf = 0x6C;
constexpr uint8_t kSpace = 0x5C;

bool valid(uint8_t code);
/// Code for a letter, or for a figure (sets *figure). Returns 0 if there is none.
uint8_t encode(char c, bool *figure);
/// Character for a code in the given shift, 0 for control codes and unused ones.
char decode(uint8_t code, bool figures);
}  // namespace ccir476

struct NavtexConfig {
    float center_hz = 1000.0f;  // middle between the two tones, 170 Hz apart
    bool reverse = false;       // for a receiver on the other sideband
};

/// NAVTEX / SITOR-B receiver: 100 baud FSK, every character sent twice with
/// 280 ms in between, so that a character lost in one pass is taken from the other.
class NavtexDecoder {
public:
    bool begin(const NavtexConfig &cfg, int sample_rate = 12000);
    void feed(const int16_t *samples, size_t count);
    size_t read(char *out, size_t max);

    /// True while the receiver is in step with the character stream.
    bool locked() const { return _locked; }
    float level() const { return _demod.level(); }

private:
    void onBit(bool b);
    void onCode(uint8_t code);
    void output(uint8_t code);
    void emit(char c);

    FskDemod _demod;
    float _spb = 120;
    float _phase = 0;
    bool _last_positive = false;

    uint8_t _window = 0;
    uint32_t _bit_count = 0;
    int _score[7]{};
    bool _locked = false;
    int _align = 0;

    uint8_t _codes[8]{};   // the last codes, 0xFF for an invalid one
    uint32_t _code_count = 0;
    int _vote[2]{};        // evidence that odd / even positions carry the repeats
    bool _figures = false;
    int _lost = 0;         // characters lost in both transmissions, shown once text continues

    char _out[256]{};
    size_t _out_head = 0, _out_tail = 0;
};

}  // namespace digi
