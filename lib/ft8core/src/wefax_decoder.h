#pragma once

#include <stddef.h>
#include <stdint.h>

namespace digi {

/// HF radiofax receiver (IOC 576): the picture is frequency modulated between
/// 1500 Hz (black) and 2300 Hz (white), 120 or 60 lines per minute.
class WefaxDecoder {
public:
    enum class Event { None, Start, Phased, Stop };

    /// Called with one line of grey values, 0 = black, 255 = white.
    using LineFn = void (*)(const uint8_t *pixels, int width, void *ctx);

    ~WefaxDecoder();

    /// @param width pixels per line in the output
    bool begin(int width = 904, int lines_per_minute = 120, int sample_rate = 12000);
    void onLine(LineFn fn, void *ctx);

    void feed(const int16_t *samples, size_t count);

    /// Start tone, phasing complete or stop tone seen since the last call.
    Event takeEvent();

    /// Moves the left edge by the given number of output pixels (positive: the picture moves left).
    void shift(int pixels);

    /// Corrects a slanted picture: difference of the sender's line rate in parts per million.
    void setSlantPpm(float ppm);

    /// Lines are searched for the phasing pulse until one alignment was made; this allows another.
    void rearmPhasing() { _phased = false; _phasing_run = 0; }
    bool phased() const { return _phased; }

    int width() const { return _width; }

private:
    void finishLine();
    void checkPhasing();
    void tone(float value);

    int _rate = 12000;
    int _width = 0;
    float _nominal_spl = 6000;  // samples per line
    float _spl = 6000;
    float _pos = 0;             // position in the current line, in samples
    uint8_t *_line = nullptr;
    float *_acc = nullptr;
    uint16_t *_acc_n = nullptr;

    // FM demodulator
    float _phi = 0;
    float _bi[6]{}, _bq[6]{}, _ci[6]{}, _cq[6]{};  // two cascaded averaging filters
    float _si = 0, _sq = 0, _ti = 0, _tq = 0;
    int _fpos = 0;
    float _prev_i = 0, _prev_q = 0;

    // start / stop tone
    bool _tone_high = false;
    int _tone_crossings = 0;
    int _tone_samples = 0;
    int _start_windows = 0, _stop_windows = 0;

    bool _phased = false;
    int _phasing_run = 0;
    int _phasing_pos = 0;

    Event _event = Event::None;
    LineFn _fn = nullptr;
    void *_ctx = nullptr;
};

}  // namespace digi
