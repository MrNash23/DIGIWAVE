#include "ft8_decoder.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft8_hashtable.h"

extern "C" {
#include <ft8/constants.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
}

namespace ft8 {

namespace {

constexpr int kMinScore = 10;
// These limits trade a little sensitivity for speed: measured on the 31 reference
// recordings of ft8_lib, 100 candidates with 10 iterations and without retrying
// next to a decoded signal find 342 messages instead of 354 at a fifth of the
// work. On the ESP32-S3 the full search took up to 3.3 s and ran into the next slot.
constexpr int kMaxCandidates = 100;
constexpr int kLdpcIterations = 10;
constexpr int kNearby = 3;  // in half bins and half symbols
constexpr int kTimeOsr = 2;
constexpr int kFreqOsr = 2;
constexpr int kMaxDecoded = 50;

bool isGrid(const char *s)
{
    return strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' && isdigit((unsigned char)s[2]) &&
           isdigit((unsigned char)s[3]) && strcmp(s, "RR73") != 0;
}

/// Report, roger or sign-off, as opposed to a callsign.
bool isExchange(const char *s)
{
    if (strcmp(s, "RR73") == 0 || strcmp(s, "RRR") == 0 || strcmp(s, "73") == 0) {
        return true;
    }
    if (*s == 'R') {
        ++s;
    }
    return (s[0] == '+' || s[0] == '-') && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) && s[3] == '\0';
}

void copyCall(char *dst, size_t dst_size, const char *src)
{
    // Hashed calls arrive as <CALL>; the brackets are not part of the callsign
    size_t n = 0;
    for (; *src && n + 1 < dst_size; ++src) {
        if (*src != '<' && *src != '>') {
            dst[n++] = *src;
        }
    }
    dst[n] = '\0';
}

}  // namespace

const ProtocolInfo &info(Protocol p)
{
    static const ProtocolInfo ft8 = {"FT8", 15000, 12640, FT8_NN, FT8_SYMBOL_PERIOD};
    static const ProtocolInfo ft4 = {"FT4", 7500, 5040, FT4_NN, FT4_SYMBOL_PERIOD};
    return p == Protocol::FT4 ? ft4 : ft8;
}

void parseMessage(Decoded &d)
{
    d.call_to[0] = d.call_de[0] = d.extra[0] = '\0';
    d.is_cq = false;

    char buf[sizeof(d.text)];
    strncpy(buf, d.text, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    char *tok[5];
    int n = 0;
    for (char *t = strtok(buf, " "); t != nullptr && n < 5; t = strtok(nullptr, " ")) {
        tok[n++] = t;
    }
    if (n < 2) {
        return;
    }

    if (strcmp(tok[0], "CQ") == 0) {
        // CQ [modifier] CALL [GRID]
        d.is_cq = true;
        strcpy(d.call_to, "CQ");
        int last = n - 1;
        if (isGrid(tok[last]) && last >= 2) {
            strncpy(d.extra, tok[last], sizeof(d.extra) - 1);
            d.extra[sizeof(d.extra) - 1] = '\0';
            --last;
        }
        copyCall(d.call_de, sizeof(d.call_de), tok[last]);
        return;
    }

    if (n > 3) {
        return;  // free text or a message type the QSO logic does not handle
    }
    copyCall(d.call_to, sizeof(d.call_to), tok[0]);
    if (n == 2 && isExchange(tok[1])) {
        // "CALL -07", "CALL RR73": the short form used between two CB callsigns,
        // which names only the station addressed
        strncpy(d.extra, tok[1], sizeof(d.extra) - 1);
        d.extra[sizeof(d.extra) - 1] = '\0';
        return;
    }
    copyCall(d.call_de, sizeof(d.call_de), tok[1]);
    if (n == 3) {
        strncpy(d.extra, tok[2], sizeof(d.extra) - 1);
        d.extra[sizeof(d.extra) - 1] = '\0';
    }
}

Decoder::~Decoder()
{
    if (_ready) {
        monitor_free(&_mon);
    }
    free(_block);
}

bool Decoder::begin(Protocol protocol, int sample_rate, float f_min, float f_max)
{
    if (_ready) {
        monitor_free(&_mon);
        _ready = false;
    }
    free(_block);
    _block = nullptr;
    _protocol = protocol;

    monitor_config_t cfg{};
    cfg.f_min = f_min;
    cfg.f_max = f_max;
    cfg.sample_rate = sample_rate;
    cfg.time_osr = kTimeOsr;
    cfg.freq_osr = kFreqOsr;
    cfg.protocol = protocol == Protocol::FT4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    monitor_init(&_mon, &cfg);
    _block = (float *)malloc(_mon.block_size * sizeof(float));
    _ready = _mon.wf.mag != nullptr && _mon.window != nullptr && _mon.last_frame != nullptr && _mon.fft_work != nullptr &&
             _block != nullptr;
    _fill = 0;
    return _ready;
}

void Decoder::reset()
{
    monitor_reset(&_mon);
    _fill = 0;
}

void Decoder::feed(const int16_t *samples, size_t count)
{
    if (!_ready) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        int s = samples[i];
        int a = s < 0 ? -s : s;
        if (a > _peak) {
            _peak = a;
        }
        _block[_fill++] = s / 32768.0f;
        if (_fill == _mon.block_size) {
            monitor_process(&_mon, _block);
            _fill = 0;
        }
    }
}

int Decoder::takePeak()
{
    int p = _peak;
    _peak = 0;
    return p;
}

float Decoder::noiseFloorDb() const
{
    // Median of the magnitudes over the whole slot, taken from a histogram
    const ftx_waterfall_t &wf = _mon.wf;
    uint32_t hist[256] = {0};
    size_t total = (size_t)wf.num_blocks * wf.block_stride;
    if (total == 0) {
        return -120.0f;
    }
    for (size_t i = 0; i < total; ++i) {
        ++hist[wf.mag[i]];
    }
    size_t acc = 0;
    int median = 0;
    for (; median < 256; ++median) {
        acc += hist[median];
        if (acc >= total / 2) {
            break;
        }
    }
    return median * 0.5f - 120.0f;
}

int Decoder::estimateSnr(const ftx_candidate_t &cand, const ftx_message_t &msg, float noise_db) const
{
    const ftx_waterfall_t &wf = _mon.wf;
    uint8_t tones[FT4_NN];  // the longer of the two
    int num_symbols = info(_protocol).symbols;
    if (_protocol == Protocol::FT4) {
        ft4_encode(msg.payload, tones);
    } else {
        ft8_encode(msg.payload, tones);
    }

    float sum = 0.0f;
    int used = 0;
    for (int sym = 0; sym < num_symbols; ++sym) {
        int block = cand.time_offset + sym;
        int bin = cand.freq_offset + tones[sym];
        if (block < 0 || block >= wf.num_blocks || bin < 0 || bin >= wf.num_bins) {
            continue;
        }
        size_t idx = (size_t)block * wf.block_stride + ((size_t)cand.time_sub * wf.freq_osr + cand.freq_sub) * wf.num_bins + bin;
        sum += powf(10.0f, (wf.mag[idx] * 0.5f - 120.0f) / 10.0f);
        ++used;
    }
    if (used == 0) {
        return -24;
    }
    float sig_db = 10.0f * log10f(sum / used);
    // Signal and noise are measured in one bin of the tone spacing; refer the noise to 2500 Hz
    float snr = sig_db - noise_db - 10.0f * log10f(2500.0f * _mon.symbol_period);
    if (snr < -24.0f) {
        snr = -24.0f;
    }
    if (snr > 30.0f) {
        snr = 30.0f;
    }
    return (int)lroundf(snr);
}

int Decoder::decode(Decoded *out, int max_out)
{
    if (!_ready) {
        return 0;
    }
    const ftx_waterfall_t *wf = &_mon.wf;

    // Kept off the stack: the decode task is not the only user of this much memory
    static ftx_candidate_t candidates[kMaxCandidates];
    static ftx_message_t decoded[kMaxDecoded];
    static bool used[kMaxDecoded];
    memset(used, 0, sizeof(used));

    int num_candidates = ftx_find_candidates(wf, kMaxCandidates, candidates, kMinScore);
    float noise_db = noiseFloorDb();

    // Strongest first, so that the copies a strong signal leaves at neighbouring
    // offsets can be recognised and skipped
    qsort(candidates, num_candidates, sizeof(candidates[0]), [](const void *a, const void *b) {
        return (int)((const ftx_candidate_t *)b)->score - (int)((const ftx_candidate_t *)a)->score;
    });
    static int16_t hit_time[kMaxCandidates], hit_freq[kMaxCandidates];
    int num_hits = 0;

    int num_out = 0;
    int num_decoded = 0;
    for (int i = 0; i < num_candidates && num_out < max_out && num_decoded < kMaxDecoded; ++i) {
        const ftx_candidate_t &cand = candidates[i];
        int16_t t = (int16_t)(cand.time_offset * wf->time_osr + cand.time_sub);
        int16_t f = (int16_t)(cand.freq_offset * wf->freq_osr + cand.freq_sub);
        bool nearby = false;
        for (int h = 0; h < num_hits; ++h) {
            if (abs(t - hit_time[h]) <= kNearby && abs(f - hit_freq[h]) <= kNearby) {
                nearby = true;
                break;
            }
        }
        if (nearby) {
            continue;
        }
        ftx_message_t message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(wf, &cand, kLdpcIterations, &message, &status)) {
            continue;
        }
        hit_time[num_hits] = t;
        hit_freq[num_hits] = f;
        ++num_hits;

        // Duplicate check: the same message is usually found at several time/frequency offsets
        int idx = message.hash % kMaxDecoded;
        bool duplicate = false;
        while (used[idx]) {
            if (decoded[idx].hash == message.hash && 0 == memcmp(decoded[idx].payload, message.payload, sizeof(message.payload))) {
                duplicate = true;
                break;
            }
            idx = (idx + 1) % kMaxDecoded;
        }
        if (duplicate) {
            continue;
        }
        decoded[idx] = message;
        used[idx] = true;
        ++num_decoded;

        Decoded &d = out[num_out];
        ftx_message_offsets_t offsets;
        if (ftx_message_decode(&message, hashInterface(), d.text, &offsets) != FTX_MESSAGE_RC_OK) {
            continue;
        }
        d.freq = (_mon.min_bin + cand.freq_offset + (float)cand.freq_sub / wf->freq_osr) / _mon.symbol_period;
        // The analysis window spans two symbols, so a block matches the symbol that
        // started one symbol period before the block was completed.
        d.dt = (cand.time_offset + (float)cand.time_sub / wf->time_osr - 1.0f) * _mon.symbol_period - kNominalStart;
        d.snr = estimateSnr(cand, message, noise_db);
        parseMessage(d);
        ++num_out;
    }
    hashCleanup(40);
    return num_out;
}

}  // namespace ft8
