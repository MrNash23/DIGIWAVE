#include "sstv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace digi {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kSyncHz = 1200, kBlackHz = 1500, kWhiteHz = 2300, kLeaderHz = 1900;
constexpr int kRing = 16384;  // samples of demodulated frequency kept; a power of two

// Line layout in milliseconds. Martin and Scottie send green, blue, red;
// Robot and PD send brightness and colour differences.
struct Timing {
    float sync, porch, scan, gap;
};

const SstvInfo kInfo[kSstvModes] = {
    {"none", 0, 0, 0},
    {"Martin 1", 44, 256, 446.446f},
    {"Martin 2", 40, 256, 226.798f},
    {"Scottie 1", 60, 256, 428.220f},
    {"Scottie 2", 56, 256, 277.692f},
    {"Robot 36", 8, 240, 150.0f},
    {"PD 120", 95, 496, 508.480f},
    {"PD 180", 96, 496, 754.240f},
};

const Timing kTiming[kSstvModes] = {
    {0, 0, 0, 0},
    {4.862f, 0.572f, 146.432f, 0.572f},
    {4.862f, 0.572f, 73.216f, 0.572f},
    {9.0f, 1.5f, 138.240f, 1.5f},
    {9.0f, 1.5f, 88.064f, 1.5f},
    {9.0f, 3.0f, 88.0f, 0},
    {20.0f, 2.08f, 121.6f, 0},
    {20.0f, 2.08f, 183.04f, 0},
};

bool isScottie(SstvMode m)
{
    return m == SstvMode::ScottieS1 || m == SstvMode::ScottieS2;
}

bool isPd(SstvMode m)
{
    return m == SstvMode::PD120 || m == SstvMode::PD180;
}

uint8_t level(float hz)
{
    float v = (hz - kBlackHz) * (255.0f / (kWhiteHz - kBlackHz));
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
}

float toneOf(uint8_t value)
{
    return kBlackHz + value * ((kWhiteHz - kBlackHz) / 255.0f);
}

uint8_t clamp8(float v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
}

/// Brightness and colour differences as sent by Robot and PD modes, to red, green, blue.
void toRgb(uint8_t y, uint8_t ry, uint8_t by, uint8_t *rgb)
{
    float c = 298.082f * (y - 16);
    rgb[0] = clamp8((c + 408.583f * (ry - 128)) / 256);
    rgb[1] = clamp8((c - 100.291f * (by - 128) - 208.120f * (ry - 128)) / 256);
    rgb[2] = clamp8((c + 516.411f * (by - 128)) / 256);
}

void fromRgb(const uint8_t *rgb, uint8_t *y, uint8_t *ry, uint8_t *by)
{
    float r = rgb[0], g = rgb[1], b = rgb[2];
    *y = clamp8(16 + (65.738f * r + 129.057f * g + 25.064f * b) / 256);
    *ry = clamp8(128 + (112.439f * r - 94.154f * g - 18.285f * b) / 256);
    *by = clamp8(128 + (-37.945f * r - 74.494f * g + 112.439f * b) / 256);
}

}  // namespace

const SstvInfo &sstvInfo(SstvMode mode)
{
    return kInfo[(int)mode];
}

SstvMode sstvModeForVis(uint8_t vis)
{
    for (int i = 1; i < kSstvModes; ++i) {
        if (kInfo[i].vis == vis) {
            return (SstvMode)i;
        }
    }
    return SstvMode::None;
}

// --- decoder -----------------------------------------------------------------------

SstvDecoder::~SstvDecoder()
{
    free(_f);
    free(_chroma);
    free(_row);
}

bool SstvDecoder::begin(int sample_rate)
{
    if (_f == nullptr) {
        _f = (float *)malloc(kRing * sizeof(float));
        _chroma = (uint8_t *)malloc(2 * kSstvWidth);
        _row = (uint8_t *)malloc(2 * 3 * kSstvWidth);
    }
    if (_f == nullptr || _chroma == nullptr || _row == nullptr) {
        return false;
    }
    _rate = sample_rate;
    _per_ms = sample_rate / 1000.0;
    for (int i = 0; i < kRing; ++i) {
        _f[i] = kLeaderHz;
    }
    _phi = 0;
    memset(_bi, 0, sizeof(_bi));
    memset(_bq, 0, sizeof(_bq));
    memset(_ci, 0, sizeof(_ci));
    memset(_cq, 0, sizeof(_cq));
    _si = _sq = _ti = _tq = 0;
    _fpos = 0;
    _prev_i = _prev_q = 0;
    _n = 0;
    _smooth = kLeaderHz;
    _leader = _leader_at_drop = _start_run = _since_leader = 0;
    _vis_at = 0;
    _hunt = SstvMode::None;
    _pulse = 0;
    _mode = SstvMode::None;
    _line = 0;
    _event = Event::None;
    return true;
}

void SstvDecoder::onLine(LineFn fn, void *ctx)
{
    _fn = fn;
    _ctx = ctx;
}

SstvDecoder::Event SstvDecoder::takeEvent()
{
    Event e = _event;
    _event = Event::None;
    return e;
}

void SstvDecoder::start(SstvMode mode)
{
    _mode = SstvMode::None;
    _hunt = mode;
    _pulse = 0;
}

void SstvDecoder::stop()
{
    if (_mode != SstvMode::None) {
        _event = Event::End;
    }
    _mode = SstvMode::None;
    _hunt = SstvMode::None;
}

/// Mean frequency between two sample numbers (which must still be in the ring).
float SstvDecoder::mean(double from, double to) const
{
    int64_t a = (int64_t)floor(from + 0.5), b = (int64_t)floor(to + 0.5);
    if (b <= a) {
        b = a + 1;
    }
    float sum = 0;
    for (int64_t i = a; i < b; ++i) {
        sum += _f[(uint64_t)i & (kRing - 1)];
    }
    return sum / (float)(b - a);
}

/// One scan of a line: kSstvWidth values from the stretch that begins at from and lasts ms.
void SstvDecoder::scan(double from, double ms, uint8_t *out) const
{
    double step = ms * _per_ms / kSstvWidth;
    for (int x = 0; x < kSstvWidth; ++x) {
        out[x] = level(mean(from + x * step, from + (x + 1) * step));
    }
}

void SstvDecoder::beginPicture(SstvMode mode, double first_line)
{
    _mode = mode;
    _hunt = SstvMode::None;
    _line = 0;
    _line_start = first_line;
    _event = Event::Start;
}

/// Reads the eight bits that follow the start bit: seven of the code, lowest first, and the parity.
void SstvDecoder::readVis()
{
    const double bit = 30 * _per_ms;
    int code = 0, ones = 0;
    for (int k = 0; k < 8; ++k) {
        double centre = (double)_vis_start + bit * (1.5 + k);
        bool one = mean(centre - 8 * _per_ms, centre + 8 * _per_ms) < kSyncHz;  // 1100 Hz is a one, 1300 Hz a zero
        ones += one;
        if (k < 7 && one) {
            code |= 1 << k;
        }
    }
    SstvMode mode = sstvModeForVis((uint8_t)code);
    if (mode == SstvMode::None || (ones & 1) != 0) {
        return;  // unknown format or damaged header
    }
    // The picture follows the stop bit. Scottie begins with one extra synchronisation pulse.
    double first = (double)_vis_start + bit * 10;
    if (isScottie(mode)) {
        first += kTiming[(int)mode].sync * _per_ms;
    }
    beginPicture(mode, first);
}

/// Pulls the line clock towards a synchronisation pulse found near where it is expected.
void SstvDecoder::trackSync(double expected)
{
    const Timing &t = kTiming[(int)_mode];
    const double len = t.sync * _per_ms, window = 6 * _per_ms;
    int64_t a = (int64_t)(expected - window), b = (int64_t)(expected + len + window);
    double sum = 0;
    int count = 0;
    for (int64_t i = a; i < b; ++i) {
        if (_f[(uint64_t)i & (kRing - 1)] < 1350) {
            sum += (double)i;
            ++count;
        }
    }
    if (count < 0.6 * len || count > 1.5 * len) {
        return;  // no clear pulse: keep the clock as it is
    }
    double error = sum / count + 0.5 - (expected + len / 2);
    if (fabs(error) < window) {
        _line_start += 0.25 * error;
    }
}

void SstvDecoder::decodeLine()
{
    const Timing &t = kTiming[(int)_mode];
    const SstvInfo &info = kInfo[(int)_mode];
    const double ms = _per_ms, s = _line_start;
    uint8_t *rgb = _row;
    static uint8_t a[kSstvWidth], b[kSstvWidth], c[kSstvWidth], d[kSstvWidth];

    if (isPd(_mode)) {
        // Brightness of one line, both colour differences, brightness of the next line
        double at = s + (t.sync + t.porch) * ms;
        scan(at, t.scan, a);
        scan(at + t.scan * ms, t.scan, b);
        scan(at + 2 * t.scan * ms, t.scan, c);
        scan(at + 3 * t.scan * ms, t.scan, d);
        for (int x = 0; x < kSstvWidth; ++x) {
            toRgb(a[x], b[x], c[x], rgb + 3 * x);
            toRgb(d[x], b[x], c[x], rgb + 3 * (kSstvWidth + x));
        }
        if (_fn != nullptr) {
            _fn(_line, rgb, _ctx);
            _fn(_line + 1, rgb + 3 * kSstvWidth, _ctx);
        }
        _line += 2;
    } else if (_mode == SstvMode::Robot36) {
        // Brightness on every line; red difference on even lines, blue difference on odd ones
        scan(s + (t.sync + t.porch) * ms, t.scan, a);
        scan(s + (t.sync + t.porch + t.scan + 4.5 + 1.5) * ms, t.scan / 2, b);
        if ((_line & 1) == 0) {
            memcpy(_chroma, a, kSstvWidth);
            memcpy(_chroma + kSstvWidth, b, kSstvWidth);
        } else {
            for (int x = 0; x < kSstvWidth; ++x) {
                toRgb(_chroma[x], _chroma[kSstvWidth + x], b[x], rgb + 3 * x);
                toRgb(a[x], _chroma[kSstvWidth + x], b[x], rgb + 3 * (kSstvWidth + x));
            }
            if (_fn != nullptr) {
                _fn(_line - 1, rgb, _ctx);
                _fn(_line, rgb + 3 * kSstvWidth, _ctx);
            }
        }
        ++_line;
    } else {
        // Green, blue, red. Martin: pulse, then the three scans. Scottie: the pulse comes before the red scan.
        double g_at, b_at, r_at;
        if (isScottie(_mode)) {
            g_at = s + t.gap * ms;
            b_at = g_at + (t.scan + t.gap) * ms;
            r_at = b_at + (t.scan + t.sync + t.porch) * ms;
        } else {
            g_at = s + (t.sync + t.porch) * ms;
            b_at = g_at + (t.scan + t.gap) * ms;
            r_at = b_at + (t.scan + t.gap) * ms;
        }
        scan(g_at, t.scan, a);
        scan(b_at, t.scan, b);
        scan(r_at, t.scan, c);
        for (int x = 0; x < kSstvWidth; ++x) {
            rgb[3 * x] = c[x];
            rgb[3 * x + 1] = a[x];
            rgb[3 * x + 2] = b[x];
        }
        if (_fn != nullptr) {
            _fn(_line, rgb, _ctx);
        }
        ++_line;
    }

    // Where the pulse of this line (Scottie) or of the next one is expected
    double line_len = info.line_ms * ms;
    if (isScottie(_mode)) {
        trackSync(s + (2 * t.gap + 2 * t.scan) * ms);
    } else {
        trackSync(s + line_len);
    }
    _line_start += line_len;
    if (_line >= info.lines) {
        _mode = SstvMode::None;
        _event = Event::End;
    }
}

void SstvDecoder::feed(const int16_t *samples, size_t count)
{
    if (_f == nullptr) {
        return;
    }
    const float dphi = kTwoPi * kLeaderHz / _rate;
    for (size_t k = 0; k < count; ++k) {
        // Mix the tone range down to zero and low-pass with two averaging filters
        float x = samples[k] * (1.0f / 32768);
        float i = x * cosf(_phi), q = -x * sinf(_phi);
        _phi += dphi;
        if (_phi > kTwoPi) {
            _phi -= kTwoPi;
        }
        _si += i - _bi[_fpos];
        _sq += q - _bq[_fpos];
        _bi[_fpos] = i;
        _bq[_fpos] = q;
        float i1 = _si / 6, q1 = _sq / 6;
        _ti += i1 - _ci[_fpos];
        _tq += q1 - _cq[_fpos];
        _ci[_fpos] = i1;
        _cq[_fpos] = q1;
        _fpos = (_fpos + 1) % 6;
        float fi = _ti / 6, fq = _tq / 6;
        // The frequency is the turn of the phase from one sample to the next
        float dot = fi * _prev_i + fq * _prev_q, cross = fq * _prev_i - fi * _prev_q;
        _prev_i = fi;
        _prev_q = fq;
        float hz = kLeaderHz + atan2f(cross, dot) * _rate / kTwoPi;
        // The filters delay the signal by five samples; the ring is written in step with the input,
        // and all timing is relative, so the delay cancels out
        _f[_n & (kRing - 1)] = hz;
        ++_n;
        _smooth += 0.08f * (hz - _smooth);

        // --- header: a long leader tone, then the start bit ---
        if (fabsf(_smooth - kLeaderHz) < 90) {
            ++_leader;
            _since_leader = 0;
            _start_run = 0;
        } else {
            // The tone needs a millisecond or two to fall from the leader to the start bit
            if (++_since_leader > (int)(4 * _per_ms) && _start_run == 0) {
                _leader = 0;
            }
            if (_smooth < 1350) {
                if (_start_run == 0) {
                    _leader_at_drop = _leader;
                    _leader = 0;
                }
                ++_start_run;
                // 25 ms of the start bit make it one; the 10 ms break inside the leader does not
                if (_start_run == (int)(25 * _per_ms) && _leader_at_drop >= (int)(200 * _per_ms) && _vis_at == 0) {
                    _vis_start = _n - (uint64_t)_start_run - 12;  // the smoothing lags by about a millisecond
                    _vis_at = _vis_start + (uint64_t)(300 * _per_ms) + 16;
                }
            } else if (_start_run > 0) {
                _start_run = 0;  // the low tone ended
            }
        }
        if (_vis_at != 0 && _n >= _vis_at) {
            _vis_at = 0;
            _start_run = 0;
            _leader_at_drop = 0;
            readVis();
        }

        // --- a picture without header: align on the next pulse ---
        if (_hunt != SstvMode::None) {
            const Timing &t = kTiming[(int)_hunt];
            if (hz < 1350) {
                ++_pulse;
            } else {
                int len = (int)(t.sync * _per_ms);
                if (_pulse > 0.6 * len && _pulse < 1.6 * len) {
                    // The pulse just ended; the line it belongs to began where the pulse did,
                    // or for Scottie two scans earlier - there the next line starts after the red scan
                    double pulse_start = (double)_n - _pulse - 1;
                    double first = isScottie(_hunt) ? pulse_start + (t.sync + t.porch + t.scan) * _per_ms : pulse_start;
                    beginPicture(_hunt, first);
                }
                _pulse = 0;
            }
        }

        // --- picture lines, as soon as the whole line and the pulse after it are in the ring ---
        if (_mode != SstvMode::None) {
            const Timing &t = kTiming[(int)_mode];
            double need = _line_start + (kInfo[(int)_mode].line_ms + t.sync + 8) * _per_ms;
            if ((double)_n >= need) {
                decodeLine();
            }
        }
    }
}

// --- encoder -----------------------------------------------------------------------

void SstvEncoder::begin(SstvMode mode, PixelFn fn, void *ctx, int sample_rate, float amplitude)
{
    _mode = mode;
    _fn = fn;
    _ctx = ctx;
    _rate = sample_rate;
    _amp = amplitude;
    _phase = 0;
    _step = 0;
    _line = 0;
    _pixel = 0;
    _hz = kLeaderHz;
    _until = 0;
    _clock = 0;
    _done = mode == SstvMode::None;
}

void SstvEncoder::tone(float hz, double ms)
{
    _hz = hz;
    _until += ms * _rate / 1000.0;
}

/// Sets up the next tone. The header has 13 steps; after it each call handles
/// one element of a line. Returns false once the picture is complete.
bool SstvEncoder::next()
{
    const SstvInfo &info = kInfo[(int)_mode];
    const Timing &t = kTiming[(int)_mode];
    if (_step < 14) {
        // Leader, break, leader, start bit, seven bits of the code, parity, stop bit
        int s = _step++;
        if (s == 0 || s == 2) {
            tone(kLeaderHz, 300);
        } else if (s == 1) {
            tone(kSyncHz, 10);
        } else if (s == 3 || s == 12) {
            tone(kSyncHz, 30);
        } else if (s <= 10) {
            tone((info.vis >> (s - 4)) & 1 ? 1100 : 1300, 30);
        } else if (s == 11) {
            int ones = 0;
            for (int k = 0; k < 7; ++k) {
                ones += (info.vis >> k) & 1;
            }
            tone(ones & 1 ? 1100 : 1300, 30);
        } else {
            // Scottie: one pulse before the first line; the others go straight on
            if (isScottie(_mode)) {
                tone(kSyncHz, t.sync);
            } else {
                return next();
            }
        }
        return true;
    }
    if (_line >= info.lines) {
        return false;
    }

    // The elements of a line, each a (kind, duration) pair: 's' pulse, 'p' porch at black,
    // digits scans, 'e'/'o' the separator of Robot 36
    static const char *const kMartin = "sp0g1g2g";
    static const char *const kScottie = "g0g1sp2";
    static const char *const kRobot = "sp3xq4";
    static const char *const kPdLine = "sp3567";
    const char *layout = isScottie(_mode) ? kScottie : (isPd(_mode) ? kPdLine : (_mode == SstvMode::Robot36 ? kRobot : kMartin));
    int element = _step - 14;
    char kind = layout[element];
    if (kind == '\0') {
        _step = 14;
        _line += isPd(_mode) ? 2 : 1;
        return next();
    }
    if (kind >= '0' && kind <= '7') {
        // A scan: one tone per pixel. 0 green, 1 blue, 2 red, 3 brightness, 4 colour of this
        // Robot line, 5 red difference, 6 blue difference, 7 brightness of the second PD line
        double ms = (kind == '4' ? t.scan / 2 : t.scan) / kSstvWidth;
        uint8_t rgb[3] = {0, 0, 0}, rgb2[3] = {0, 0, 0}, y, ry, by, y2, ry2, by2;
        _fn(_pixel, kind == '7' ? _line + 1 : _line, rgb, _ctx);
        fromRgb(rgb, &y, &ry, &by);
        uint8_t v = 0;
        switch (kind) {
        case '0': v = rgb[1]; break;
        case '1': v = rgb[2]; break;
        case '2': v = rgb[0]; break;
        case '3':
        case '7': v = y; break;
        case '4': v = (_line & 1) ? by : ry; break;
        default:
            // PD: the colour differences are the mean of the two lines
            _fn(_pixel, _line + 1, rgb2, _ctx);
            fromRgb(rgb2, &y2, &ry2, &by2);
            v = kind == '5' ? (uint8_t)((ry + ry2) / 2) : (uint8_t)((by + by2) / 2);
            break;
        }
        tone(toneOf(v), ms);
        if (++_pixel == kSstvWidth) {
            _pixel = 0;
            ++_step;
        }
        return true;
    }
    ++_step;
    switch (kind) {
    case 's': tone(kSyncHz, t.sync); break;
    case 'p': tone(kBlackHz, t.porch); break;
    case 'g': tone(kBlackHz, t.gap); break;
    case 'x': tone((_line & 1) ? kWhiteHz : kBlackHz, 4.5); break;
    case 'q': tone(kLeaderHz, 1.5); break;
    default: break;
    }
    return true;
}

size_t SstvEncoder::read(int16_t *out, size_t count)
{
    size_t written = 0;
    while (written < count && !_done) {
        if (_clock >= _until) {
            if (!next()) {
                _done = true;
                break;
            }
            continue;
        }
        out[written++] = (int16_t)lroundf(32767.0f * _amp * sinf(_phase));
        _phase += kTwoPi * _hz / _rate;
        if (_phase > kTwoPi) {
            _phase -= kTwoPi;
        }
        _clock += 1;
    }
    return written;
}

}  // namespace digi
