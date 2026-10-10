#pragma once

#include <stddef.h>
#include <stdint.h>

namespace digi {

/// Morse code for a character ('.' and '-'), or nullptr if it has none.
const char *morseFor(char c);

/// Morse decoder for audio at 12 kHz. Listens in a narrow window around a set
/// frequency, follows the sending speed and turns the keying into text. The
/// operator tunes the radio so that the wanted signal falls into the window;
/// signals outside it are ignored.
class CwDecoder {
public:
    void begin(int sample_rate = 12000);

    void feed(const int16_t *samples, size_t count);

    /// Fetches decoded characters. Returns the number written to out.
    size_t read(char *out, size_t max);

    /// The decoder listens between centre - half_width and centre + half_width.
    void setWindow(float centre_hz, float half_width_hz = 50.0f);
    float centre() const { return _centre; }
    float halfWidth() const { return _half; }

    /// With tracking on the decoder follows the tone inside the window, otherwise it stays on the centre.
    void setTracking(bool on) { _tracking = on; }
    bool tracking() const { return _tracking; }

    /// Frequency the decoder is listening on right now.
    float tone() const { return _tone; }
    int wpm() const { return _dot_ms > 0 ? (int)(1200.0f / _dot_ms + 0.5f) : 0; }
    bool keyDown() const { return _key; }
    /// True while a signal clearly above the noise is present.
    bool signal() const { return _hold_ms > 0; }

private:
    void hop();
    void onMark(float ms);
    void onSpace(float ms);
    void emit(char c);
    void flushChar();
    void updateTone();

    static constexpr int kHop = 48;       // 4 ms
    static constexpr int kWindow = 384;   // 32 ms analysis window, tapered: about 16 ms effective, 60 Hz wide
    static constexpr int kBank = 5;       // tone search inside the window
    static constexpr int kRefs = 6;       // noise measured beside the window
    static constexpr int kBankBlock = 480;
    static constexpr int kBankHop = 240;  // blocks overlap by half

    int _rate = 12000;
    float _hop_ms = 4.0f;
    int16_t _win[kWindow]{};
    int _win_fill = 0;
    int _hop_fill = 0;

    float _centre = 700.0f;
    float _half = 50.0f;
    float _tone = 700.0f;
    bool _tracking = true;
    float _taper[kWindow]{};
    float _bank_taper[kBankBlock]{};
    int16_t _bank_buf[kBankBlock]{};
    int _bank_fill = 0;
    float _bank_power[kBank]{};
    float _ref_power = 0;
    float _hold_ms = 0;    // time left in which a signal in the window counts as present

    int _warmup = 50;      // hops used to learn the noise level before keying is evaluated
    float _env = 0;
    float _peak = 0;
    float _noise = 0;
    bool _key = false;
    bool _squelch_open = false;
    float _state_ms = 0;   // time in the current key state
    bool _char_open = false;
    bool _space_sent = true;

    float _dot_ms = 60.0f;  // 20 WPM until the signal says otherwise
    float _marks[12]{};
    int _mark_count = 0;

    float _elements[10]{};  // mark durations of the character being received
    int _element_count = 0;

    char _out[128]{};
    size_t _out_head = 0, _out_tail = 0;
};

/// Morse keyer: turns text into keyed tone samples.
class CwKeyer {
public:
    void begin(int wpm, float tone_hz, float amplitude = 0.5f, int sample_rate = 12000);

    /// Queues text for sending. Characters without Morse code are skipped.
    /// Returns the number of characters accepted.
    size_t send(const char *text);

    /// Writes up to count samples. Returns the number written, 0 when nothing is left to send.
    size_t read(int16_t *out, size_t count);

    bool busy() const { return _q_head != _q_tail || _seg_left > 0 || _pattern != nullptr; }
    /// State of the key for the last sample written, for a keying line.
    bool keyDown() const { return _seg_on; }
    void clear();

private:
    bool nextSegment();

    int _rate = 12000;
    float _dot_samples = 720;
    float _tone = 700;
    float _amp = 0.5f;
    float _phi = 0;
    float _gain = 0;        // current envelope value, ramps towards the key state
    float _ramp_step = 0;

    char _queue[256]{};
    size_t _q_head = 0, _q_tail = 0;
    const char *_pattern = nullptr;  // rest of the character being sent
    bool _seg_on = false;
    int _seg_left = 0;
    int _gap_pending = 0;            // dots of silence owed before the next element
};

}  // namespace digi
