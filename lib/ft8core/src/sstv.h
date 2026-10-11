#pragma once

#include <stddef.h>
#include <stdint.h>

namespace digi {

/// Slow-scan television: colour pictures sent line by line as a tone between
/// 1500 Hz (black) and 2300 Hz (white), with 1200 Hz pulses for synchronisation.
/// A header (VIS code) tells the receiver which of the many formats follows.
enum class SstvMode { None, MartinM1, MartinM2, ScottieS1, ScottieS2, Robot36, PD120, PD180 };
constexpr int kSstvModes = 8;

struct SstvInfo {
    const char *name;
    uint8_t vis;    // code in the header
    int lines;      // lines delivered (PD: half of the 496 sent, see sstv.cpp)
    float line_ms;  // duration of one transmitted line
};

const SstvInfo &sstvInfo(SstvMode mode);
SstvMode sstvModeForVis(uint8_t vis);

/// All pictures are delivered this wide; modes with 640 pixels per line are reduced to it.
constexpr int kSstvWidth = 320;

class SstvDecoder {
public:
    enum class Event { None, Start, End };

    /// Called with a finished picture line: kSstvWidth pixels of red, green, blue.
    using LineFn = void (*)(int line, const uint8_t *rgb, void *ctx);

    ~SstvDecoder();

    bool begin(int sample_rate = 12000);
    void onLine(LineFn fn, void *ctx);
    void feed(const int16_t *samples, size_t count);

    /// A picture began or ended since the last call. It begins with its header,
    /// or - if that was missed or unreadable - once the rhythm of its
    /// synchronisation pulses has shown the format, or with start().
    Event takeEvent();

    /// Format of the picture being received, None between pictures.
    SstvMode mode() const { return _mode; }
    int line() const { return _line; }

    /// Lines delivered so far.
    int goodLines() const { return _good; }

    /// Receives a picture whose header was missed: waits for the next
    /// synchronisation pulse and takes it as the start of a line.
    void start(SstvMode mode);
    void stop();

private:
    float mean(double from, double to) const;
    void scan(double from, double ms, uint8_t *out) const;
    void readVis();
    void beginPicture(SstvMode mode, double first_line);
    void decodeLine();
    bool trackSync(double expected);

    int _rate = 12000;
    double _per_ms = 12;

    // FM demodulator
    float _phi = 0;
    float _bi[6]{}, _bq[6]{}, _ci[6]{}, _cq[6]{};
    float _si = 0, _sq = 0, _ti = 0, _tq = 0;
    int _fpos = 0;
    float _prev_i = 0, _prev_q = 0;

    // The turn of the phase from each sample to the next, as a vector, for the
    // last second and a bit, by sample number
    float *_dot = nullptr, *_cross = nullptr;
    uint64_t _n = 0;
    float _smooth = 1900;
    float _sdot = 1, _scross = 0;

    // Header
    int _leader = 0;         // samples of leader tone seen in a row
    int _leader_at_drop = 0;
    int _since_leader = 0;   // samples since the leader tone was last seen
    int _start_run = 0;      // samples of the start bit so far, 0 when not in one
    uint64_t _vis_at = 0;    // sample at which the code can be read, 0 if none is pending
    uint64_t _vis_start = 0;

    // Pulses seen lately, for recognising a format by their rhythm
    static constexpr int kPulses = 4;
    static constexpr int kPulseWindow = 54;  // 4.5 ms at 12 kHz, a little shorter than the shortest pulse
    float _pi[kPulseWindow]{}, _pq[kPulseWindow]{}, _pp[kPulseWindow]{};
    float _wi = 0, _wq = 0, _wp = 0, _pphi = 0;
    int _ppos = 0;
    bool _in_pulse = false;
    uint64_t _pulse_from = 0;
    double _pulse_at[kPulses] = {0, 0, 0, 0};

    // Waiting for a pulse after start()
    SstvMode _hunt = SstvMode::None;
    int _pulse = 0;

    SstvMode _mode = SstvMode::None;
    int _line = 0;           // next picture line to deliver
    int _good = 0;
    double _line_start = 0;  // sample at which the transmitted line begins
    uint8_t *_chroma = nullptr;  // Robot 36: the colour difference of the line before, and its brightness
    uint8_t *_row = nullptr;

    Event _event = Event::None;
    LineFn _fn = nullptr;
    void *_ctx = nullptr;
};

/// Produces the audio of a picture, for tests and for sending.
class SstvEncoder {
public:
    /// Gives the colour of a pixel; x runs to kSstvWidth, y to the lines of the mode.
    using PixelFn = void (*)(int x, int y, uint8_t *rgb, void *ctx);

    void begin(SstvMode mode, PixelFn fn, void *ctx, int sample_rate = 12000, float amplitude = 0.5f);

    /// Writes up to count samples; returns the number written, 0 once the picture is complete.
    size_t read(int16_t *out, size_t count);

private:
    bool next();
    void tone(float hz, double ms);

    SstvMode _mode = SstvMode::None;
    PixelFn _fn = nullptr;
    void *_ctx = nullptr;
    int _rate = 12000;
    float _amp = 0.5f;
    float _phase = 0;
    int _step = 0;       // part of the header or of the line
    int _line = 0;
    int _pixel = 0;
    float _hz = 1900;
    double _until = 0;   // sample at which the current tone ends
    double _clock = 0;   // samples written so far
    bool _done = false;
};

}  // namespace digi
