#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ft8_decoder.h"

namespace ft8 {

constexpr int kMaxTones = 105;  // FT4; FT8 has 79

/// How a message text is packed into the 77 bits.
enum class Packing {
    Auto,         // standard message if the callsigns allow it
    NonStandard,  // "CQ CALL" or "<OTHER> CALL [RR73|73]": CALL in clear, OTHER as hash
    FreeText,     // up to 13 characters, sent as they are
};

/// Streaming GFSK synthesizer for one FT8 or FT4 transmission.
class Synth {
public:
    ~Synth();

    /// Packs and encodes the message. Returns false if the text is not a valid FT8 message.
    /// @param f0 audio frequency of the lowest tone in Hz
    /// @param amplitude peak output level, 0..1
    bool begin(const char *text, float f0, float amplitude = 0.5f, Protocol protocol = Protocol::FT8, int sample_rate = 12000,
               Packing packing = Packing::Auto);

    /// Writes up to count samples; returns the number written (0 once the message is complete).
    size_t read(int16_t *out, size_t count);

    size_t totalSamples() const { return (size_t)_num_tones * _spsym; }
    bool done() const { return _pos >= totalSamples(); }
    const uint8_t *tones() const { return _tones; }

private:
    uint8_t _tones[kMaxTones]{};
    int _num_tones = 0;
    float *_pulse = nullptr;  // 3 * _spsym samples
    int _spsym = 0;
    int _rate = 0;
    float _f0 = 0;
    float _amp = 0;
    float _phi = 0;
    size_t _pos = 0;
};

}  // namespace ft8
