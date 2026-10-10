#include "engine.h"

#include <Arduino.h>

#include <cb_mode.h>
#include <cw_codec.h>
#include <ft8_country.h>
#include <ft8_encoder.h>
#include <ft8_hashtable.h>
#include <navtex_decoder.h>
#include <rtty_codec.h>
#include <wefax_decoder.h>

extern "C" {
#include <fft/kiss_fftr.h>
}

#include "settings.h"
#include "timebase.h"

namespace engine {

namespace {

constexpr int kChunk = 480;       // 40 ms of audio per read
constexpr int kTxStartMs = 500;   // FT8 and FT4 signals begin half a second into the slot
constexpr int kMaxPerSlot = 30;
constexpr int kFftSize = 2048;

AudioIo *audio = nullptr;
Ptt *ptt = nullptr;

SemaphoreHandle_t lock;
QueueHandle_t wf_queue, log_queue, rx_text_queue, tx_text_queue, fax_line_queue, fax_event_queue;

ft8::Decoder ft_decoder;
ft8::Synth ft_synth;
ft8::Qso qso;
digi::CwDecoder cw_rx;
digi::CwKeyer cw_tx;
digi::RttyDecoder rtty_rx;
digi::RttyEncoder rtty_tx;
digi::NavtexDecoder navtex_rx;
digi::WefaxDecoder fax_rx;

// Guarded by lock
Entry hist[kHistory];
int hist_count = 0;
Status st{};

// Copies of the settings the task needs, so that it never reads them while the UI edits
struct Config {
    char my_call[14] = "";
    bool cb_mode = false;
    int tx_hz = 1500;
    float tx_level = 0.5f;
    int cw_wpm = 20;
    int cw_tone = 700;
    bool cw_tracking = true;
    digi::RttyConfig rtty;
    digi::NavtexConfig navtex;
} cfg;

volatile bool halt_request = false;

int16_t *capture_buf = nullptr;
volatile size_t capture_want = 0, capture_have = 0;
volatile bool reconfigure = false;
volatile bool stop_request = false;
volatile bool stopped = false;
volatile Mode requested_mode = Mode::FT8;
volatile int fax_shift_request = 0;
volatile bool fax_rephase_request = false;

struct Guard {
    Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(lock); }
};

ft8::Protocol protocolOf(Mode m)
{
    return m == Mode::FT4 ? ft8::Protocol::FT4 : ft8::Protocol::FT8;
}

// --- spectrum for the waterfall ----------------------------------------------------

struct Spectrum {
    kiss_fftr_cfg fft = nullptr;
    float *window = nullptr;
    float *time = nullptr;
    kiss_fft_cpx *freq = nullptr;
    int16_t *samples = nullptr;
    int fill = 0;

    bool begin()
    {
        fft = kiss_fftr_alloc(kFftSize, 0, nullptr, nullptr);
        window = (float *)malloc(kFftSize * sizeof(float));
        time = (float *)malloc(kFftSize * sizeof(float));
        freq = (kiss_fft_cpx *)malloc((kFftSize / 2 + 1) * sizeof(kiss_fft_cpx));
        samples = (int16_t *)malloc(kFftSize * sizeof(int16_t));
        if (!fft || !window || !time || !freq || !samples) {
            return false;
        }
        for (int i = 0; i < kFftSize; ++i) {
            float s = sinf(PI * i / kFftSize);
            window[i] = 2.0f / kFftSize * s * s / 32768.0f;
        }
        return true;
    }

    void feed(const int16_t *pcm, size_t count)
    {
        if (!fft) {
            return;
        }
        while (count > 0) {
            size_t n = min(count, (size_t)(kFftSize - fill));
            memcpy(samples + fill, pcm, n * sizeof(int16_t));
            fill += n;
            pcm += n;
            count -= n;
            if (fill == kFftSize) {
                fill = 0;
                row();
            }
        }
    }

    void row()
    {
        static uint8_t out[kWaterfallBins];
        for (int i = 0; i < kFftSize; ++i) {
            time[i] = window[i] * samples[i];
        }
        kiss_fftr(fft, time, freq);
        const int first = 34;  // 199 Hz
        for (int b = 0; b < kWaterfallBins; ++b) {
            const kiss_fft_cpx &c = freq[first + b];
            float db = 10.0f * log10f(1e-12f + c.r * c.r + c.i * c.i);
            int scaled = (int)(2 * db + 240);  // 0.5 dB steps, 0 = -120 dB
            out[b] = (uint8_t)constrain(scaled, 0, 255);
        }
        xQueueSend(wf_queue, out, 0);
    }
} spectrum;

// --- helpers -----------------------------------------------------------------------

void discardInput()
{
    // Drops what the codec captured while we were not reading
    static int16_t scratch[kChunk];
    for (int i = 0; i < 6; ++i) {
        audio->read(scratch, kChunk);
    }
}

bool readChunk(int16_t *pcm)
{
    if (!audio->read(pcm, kChunk)) {
        static uint32_t last_report = 0;
        if (millis() - last_report > 5000) {
            last_report = millis();
            Serial.println("[dw] audio read failed");
        }
        delay(100);
        return false;
    }
    int peak = 0;
    for (int i = 0; i < kChunk; ++i) {
        int a = abs(pcm[i]);
        if (a > peak) {
            peak = a;
        }
    }
    spectrum.feed(pcm, kChunk);
    if (capture_have < capture_want) {
        size_t n = min((size_t)kChunk, capture_want - capture_have);
        memcpy(capture_buf + capture_have, pcm, n * sizeof(int16_t));
        capture_have = capture_have + n;
    }
    // Logarithmic: 0 at -60 dB below full scale, 100 at full scale
    int level = peak > 0 ? (int)((20.0f * log10f(peak / 32768.0f) + 60.0f) * 100.0f / 60.0f) : 0;
    Guard g;
    st.level = constrain(level, 0, 100);
    return true;
}

void pttOn()
{
    ptt->set(true);
    audio->setOutputEnabled(true);
    Guard g;
    st.transmitting = true;
}

void pttOff()
{
    // Let the tail play out before unkeying
    static int16_t silence[kChunk] = {0};
    for (int i = 0; i < 4; ++i) {
        audio->write(silence, kChunk);
    }
    audio->setOutputEnabled(false);
    ptt->set(false);
    {
        Guard g;
        st.transmitting = false;
    }
    discardInput();
}

// --- FT8 / FT4 -----------------------------------------------------------------------

void onQsoComplete(const ft8::QsoRecord &rec, void *ctx)
{
    LogItem item{rec, *(ft8::Protocol *)ctx};
    xQueueSend(log_queue, &item, 0);
}

// Locators heard so far, so that later messages of a station can show it too
struct KnownGrid {
    char call[14];
    char grid[5];
};
constexpr int kKnownGrids = 96;
KnownGrid known[kKnownGrids];
int known_next = 0;

bool looksLikeGrid(const char *s)
{
    return strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' && isdigit((unsigned char)s[2]) &&
           isdigit((unsigned char)s[3]) && strcmp(s, "RR73") != 0;
}

KnownGrid *findGrid(const char *call)
{
    for (KnownGrid &k : known) {
        if (k.call[0] != '\0' && strcmp(k.call, call) == 0) {
            return &k;
        }
    }
    return nullptr;
}

void rememberGrid(const char *call, const char *grid)
{
    KnownGrid *k = findGrid(call);
    if (k == nullptr) {
        k = &known[known_next];
        known_next = (known_next + 1) % kKnownGrids;
        strncpy(k->call, call, sizeof(k->call) - 1);
        k->call[sizeof(k->call) - 1] = '\0';
    }
    strcpy(k->grid, grid);
}

void pushHistory(const ft8::Decoded *list, int count, uint32_t slot_utc, int parity)
{
    Guard g;
    for (int i = 0; i < count; ++i) {
        if (hist_count == kHistory) {
            memmove(hist, hist + 1, sizeof(Entry) * (kHistory - 1));
            --hist_count;
        }
        Entry &e = hist[hist_count++];
        e.msg = list[i];
        e.slot_utc = slot_utc;
        e.parity = parity;
        e.to_me = cfg.my_call[0] != '\0' && strcmp(list[i].call_to, cfg.my_call) == 0;
        e.grid[0] = '\0';
        e.country = "";
        if (e.msg.call_de[0] != '\0') {
            if (looksLikeGrid(e.msg.extra)) {
                rememberGrid(e.msg.call_de, e.msg.extra);
            }
            if (const KnownGrid *k = findGrid(e.msg.call_de)) {
                strcpy(e.grid, k->grid);
            }
            // CB callsigns begin with their division number; the amateur prefix table does not apply
            e.country = (cfg.cb_mode && ft8::isCbCallsign(e.msg.call_de)) ? ft8::cbCountryForCall(e.msg.call_de)
                                                                          : ft8::countryForCall(e.msg.call_de);
        }
    }
    st.decoded = count;
    ++st.history_seq;
}

void refreshQsoStatus()
{
    strncpy(st.qso_state, qso.stateName(), sizeof(st.qso_state) - 1);
    strncpy(st.dx_call, qso.dxCall(), sizeof(st.dx_call) - 1);
}

void transmitSlot(const char *text, ft8::Protocol protocol, ft8::Packing packing)
{
    static int16_t buf[kChunk];
    int hz;
    float level;
    {
        Guard g;
        hz = cfg.tx_hz;
        level = cfg.tx_level;
    }
    if (!ft_synth.begin(text, (float)hz, level, protocol, ft8::kSampleRate, packing)) {
        Serial.printf("[dw] cannot encode \"%s\"\n", text);
        return;
    }
    {
        Guard g;
        strncpy(st.tx_text, text, sizeof(st.tx_text) - 1);
    }
    halt_request = false;
    pttOn();

    // Start earlier by the output latency; silence keeps the output running until then
    const int slot_ms = ft8::info(protocol).slot_ms;
    int start_ms = kTxStartMs - audio->outputLatencyMs();
    memset(buf, 0, sizeof(buf));
    while ((int)(timebase::utcMs() % slot_ms) < start_ms - 40) {
        audio->write(buf, kChunk);
    }
    int wait = start_ms - (int)(timebase::utcMs() % slot_ms);
    if (wait > 0) {
        delay(wait);
    }
    while (!ft_synth.done() && !halt_request) {
        size_t n = ft_synth.read(buf, kChunk);
        audio->write(buf, n);
    }
    pttOff();
}

struct SlotState {
    int64_t slot = -1;
    bool decoded = false;
} slot_state;

void stepSlotMode(Mode m, int16_t *pcm)
{
    static ft8::Decoded decoded[kMaxPerSlot];
    static ft8::Protocol log_protocol;

    const ft8::Protocol protocol = protocolOf(m);
    const ft8::ProtocolInfo &pi = ft8::info(protocol);
    // Signals end tx_ms after their start; a little margin for late stations
    const int decode_at = kTxStartMs + pi.tx_ms + 260;
    // Starting later than this, the far end will not decode us
    const int latest_tx = protocol == ft8::Protocol::FT4 ? 1000 : 2500;

    int64_t now = timebase::utcMs();
    int64_t this_slot = now / pi.slot_ms;
    int pos = (int)(now % pi.slot_ms);

    if (this_slot != slot_state.slot) {
        slot_state.slot = this_slot;
        slot_state.decoded = false;
        ft_decoder.reset();

        char text[36];
        ft8::Packing packing = ft8::Packing::Auto;
        bool send = false;
        {
            Guard g;
            send = st.tx_enabled && timebase::valid() && pos < latest_tx && qso.nextTx((int)(this_slot & 1), text, &packing);
            refreshQsoStatus();
        }
        if (send) {
            transmitSlot(text, protocol, packing);
            slot_state.decoded = true;  // nothing useful was received in this slot
            return;
        }
    }

    if (!readChunk(pcm)) {
        return;
    }
    ft_decoder.feed(pcm, kChunk);

    if (!slot_state.decoded && pos >= decode_at) {
        slot_state.decoded = true;
        uint32_t t0 = millis();
        {
            // Stations with a non-standard callsign are called by a hash of it;
            // our own must be in the table for such calls to be readable.
            Guard g;
            ft8::rememberCall(cfg.my_call);
        }
        int n = ft_decoder.decode(decoded, kMaxPerSlot);
        uint32_t took = millis() - t0;
        uint32_t slot_utc = (uint32_t)(slot_state.slot * pi.slot_ms / 1000);
        int parity = (int)(slot_state.slot & 1);
        pushHistory(decoded, n, slot_utc, parity);
        {
            Guard g;
            st.decode_ms = (int)took;
            log_protocol = protocol;
            qso.onComplete(onQsoComplete, &log_protocol);
            qso.onDecoded(decoded, n, parity, (uint32_t)(timebase::utcMs() / 1000));
            refreshQsoStatus();
        }
        Serial.printf("[dw] %s slot %02u:%02u:%02u  %d decoded in %u ms\n", pi.name, (unsigned)(slot_utc / 3600 % 24),
                      (unsigned)(slot_utc / 60 % 60), (unsigned)(slot_utc % 60), n, (unsigned)took);
        for (int i = 0; i < n; ++i) {
            Serial.printf("[dw]   %+3d %+4.1f %4.0f  %s\n", decoded[i].snr, decoded[i].dt, decoded[i].freq, decoded[i].text);
        }
        discardInput();
    }
}

// --- CW, RTTY, NAVTEX ------------------------------------------------------------------

void pushText(const char *text, size_t n)
{
    if (n > 0) {
        Serial.write((const uint8_t *)text, n);  // received text also goes to the serial log
    }
    for (size_t i = 0; i < n; ++i) {
        xQueueSend(rx_text_queue, &text[i], 0);
    }
}

void transmitText(Mode m)
{
    static int16_t buf[kChunk];
    Config c;
    {
        Guard g;
        c = cfg;
    }
    if (m == Mode::CW) {
        cw_tx.begin(c.cw_wpm, (float)c.cw_tone, c.tx_level);
        cw_tx.clear();
    } else {
        rtty_tx.begin(c.rtty, c.tx_level);
    }
    halt_request = false;
    pttOn();

    for (;;) {
        // Hand typed characters to the encoder as long as it has room
        char ch;
        while (xQueuePeek(tx_text_queue, &ch, 0) == pdTRUE) {
            char s[2] = {ch, '\0'};
            bool sendable = m != Mode::CW || ch == ' ' || digi::morseFor(ch) != nullptr;
            size_t accepted = !sendable ? 1 : (m == Mode::CW ? cw_tx.send(s) : rtty_tx.send(s));
            if (accepted == 0) {
                break;
            }
            xQueueReceive(tx_text_queue, &ch, 0);
        }
        size_t n = m == Mode::CW ? cw_tx.read(buf, kChunk) : rtty_tx.read(buf, kChunk);
        if (n == 0) {
            break;
        }
        audio->write(buf, n);
        if (halt_request) {
            xQueueReset(tx_text_queue);
            break;
        }
    }
    pttOff();
}

void stepTextMode(Mode m, int16_t *pcm)
{
    if (!readChunk(pcm)) {
        return;
    }
    char text[64];
    size_t n = 0;
    {
        Config c;
        {
            Guard g;
            c = cfg;
        }
        switch (m) {
        case Mode::CW: {
            cw_rx.feed(pcm, kChunk);
            n = cw_rx.read(text, sizeof(text));
            Guard g;
            st.tune_hz = (int)(cw_rx.tone() + 0.5f);
            st.wpm = cw_rx.wpm();
            st.locked = cw_rx.signal();
            st.option = c.cw_tracking;
            break;
        }
        case Mode::RTTY: {
            rtty_rx.feed(pcm, kChunk);
            n = rtty_rx.read(text, sizeof(text));
            Guard g;
            st.tune_hz = (int)c.rtty.mark_hz;
            st.wpm = 0;
            st.locked = rtty_rx.quality() > 60;
            st.option = c.rtty.reverse;
            break;
        }
        default: {
            navtex_rx.feed(pcm, kChunk);
            n = navtex_rx.read(text, sizeof(text));
            Guard g;
            st.tune_hz = (int)c.navtex.center_hz;
            st.wpm = 0;
            st.locked = navtex_rx.locked();
            st.option = c.navtex.reverse;
            break;
        }
        }
    }
    pushText(text, n);

    if (canSendText(m) && uxQueueMessagesWaiting(tx_text_queue) > 0) {
        transmitText(m);
    }
}

// --- WEFAX ---------------------------------------------------------------------------

void onFaxLine(const uint8_t *pixels, int width, void *)
{
    (void)width;
    xQueueSend(fax_line_queue, pixels, 0);
}

void stepFax(int16_t *pcm)
{
    if (!readChunk(pcm)) {
        return;
    }
    int shift = fax_shift_request;
    if (shift != 0) {
        fax_shift_request = 0;
        fax_rx.shift(shift);
    }
    if (fax_rephase_request) {
        fax_rephase_request = false;
        fax_rx.rearmPhasing();
    }
    fax_rx.feed(pcm, kChunk);
    FaxEvent event = FaxEvent::None;
    switch (fax_rx.takeEvent()) {
    case digi::WefaxDecoder::Event::Start: event = FaxEvent::Start; break;
    case digi::WefaxDecoder::Event::Phased: event = FaxEvent::Phased; break;
    case digi::WefaxDecoder::Event::Stop: event = FaxEvent::Stop; break;
    default: break;
    }
    if (event != FaxEvent::None) {
        xQueueSend(fax_event_queue, &event, 0);
    }
}

// --- task ----------------------------------------------------------------------------

void enterMode(Mode m, bool mode_changed)
{
    Config c;
    {
        Guard g;
        c = cfg;
    }
    switch (m) {
    case Mode::FT8:
    case Mode::FT4:
        if (mode_changed) {
            if (!ft_decoder.begin(protocolOf(m))) {
                Serial.println("[dw] decoder: out of memory");
            }
            slot_state = SlotState();
            Guard g;
            qso.halt();
            hist_count = 0;
            ++st.history_seq;
            st.decoded = 0;
            refreshQsoStatus();
        }
        break;
    case Mode::CW:
        if (mode_changed) {
            cw_rx.begin();
        }
        if (mode_changed || cw_rx.centre() != (float)c.cw_tone) {
            cw_rx.setWindow((float)c.cw_tone, kCwHalfWidthHz);
        }
        cw_rx.setTracking(c.cw_tracking);
        break;
    case Mode::RTTY:
        rtty_rx.begin(c.rtty);
        break;
    case Mode::NAVTEX:
        navtex_rx.begin(c.navtex);
        break;
    case Mode::WEFAX:
        if (mode_changed) {
            fax_rx.begin(kFaxWidth, 120);
            fax_rx.onLine(onFaxLine, nullptr);
        }
        break;
    }
    if (mode_changed) {
        xQueueReset(rx_text_queue);
        xQueueReset(tx_text_queue);
        xQueueReset(fax_line_queue);
        Guard g;
        st.mode = m;
        st.locked = false;
        st.wpm = 0;
        Serial.printf("[dw] mode %s\n", modeName(m));
    }
}

void task(void *)
{
    static int16_t pcm[kChunk];
    Mode current = requested_mode;
    enterMode(current, true);

    for (;;) {
        if (stop_request) {
            ptt->set(false);
            audio->setOutputEnabled(false);
            stopped = true;
            vTaskDelete(nullptr);
        }
        Mode want = requested_mode;
        if (want != current) {
            current = want;
            reconfigure = false;
            enterMode(current, true);
        } else if (reconfigure) {
            reconfigure = false;
            enterMode(current, false);
        }

        if (isSlotMode(current)) {
            stepSlotMode(current, pcm);
        } else if (isTextMode(current)) {
            stepTextMode(current, pcm);
        } else {
            stepFax(pcm);
        }
    }
}

}  // namespace

const char *modeName(Mode m)
{
    static const char *const names[kModes] = {"FT8", "FT4", "CW", "RTTY", "NAVTEX", "WEFAX"};
    return names[(int)m];
}

bool isSlotMode(Mode m)
{
    return m == Mode::FT8 || m == Mode::FT4;
}

bool isTextMode(Mode m)
{
    return m == Mode::CW || m == Mode::RTTY || m == Mode::NAVTEX;
}

bool canSendText(Mode m)
{
    return m == Mode::CW || m == Mode::RTTY;
}

void begin(AudioIo &a, Ptt &p)
{
    audio = &a;
    ptt = &p;
    lock = xSemaphoreCreateMutex();
    wf_queue = xQueueCreate(8, kWaterfallBins);
    log_queue = xQueueCreate(4, sizeof(LogItem));
    rx_text_queue = xQueueCreate(512, sizeof(char));
    tx_text_queue = xQueueCreate(256, sizeof(char));
    fax_line_queue = xQueueCreate(6, kFaxWidth);
    fax_event_queue = xQueueCreate(4, sizeof(FaxEvent));
    if (!spectrum.begin()) {
        Serial.println("[dw] spectrum: out of memory");
    }
    requested_mode = (Mode)constrain(settings.last_mode, 0, kModes - 1);
    applySettings();
    reconfigure = false;
    // monitor_process() of ft8_lib keeps about 30 kB of FFT buffers on the stack
    xTaskCreatePinnedToCore(task, "digiwave", 48 * 1024, nullptr, 5, nullptr, 0);
}

void applySettings()
{
    Guard g;
    ft8::QsoConfig qc;
    strncpy(qc.my_call, settings.call, sizeof(qc.my_call) - 1);
    strncpy(qc.my_grid, settings.grid, sizeof(qc.my_grid) - 1);
    qc.max_retries = settings.max_retries;
    qc.max_cq = settings.max_cq;
    qc.cb_mode = settings.cb_mode;
    qso.configure(qc);
    cfg.cb_mode = settings.cb_mode;

    strncpy(cfg.my_call, settings.call, sizeof(cfg.my_call) - 1);
    cfg.tx_hz = settings.tx_hz;
    cfg.tx_level = settings.tx_level / 100.0f;
    cfg.cw_wpm = settings.cw_wpm;
    cfg.cw_tone = settings.cw_tone;
    cfg.cw_tracking = true;  // inside the narrow window the decoder always follows the tone
    cfg.rtty.baud = settings.rtty_50_baud ? 50.0f : 45.45f;
    cfg.rtty.mark_hz = (float)settings.rtty_mark;
    cfg.rtty.space_hz = (float)(settings.rtty_mark + settings.rtty_shift);
    cfg.rtty.reverse = settings.rtty_reverse;
    cfg.navtex.center_hz = (float)settings.navtex_center;
    cfg.navtex.reverse = settings.navtex_reverse;

    audio->setInputGain((float)settings.mic_gain_db);
    audio->setOutputVolume(settings.volume);
    timebase::setOffsetMs(settings.time_offset_ds * 100);
    reconfigure = true;
}

void setMode(Mode m)
{
    halt();
    requested_mode = m;
}

Mode mode()
{
    return requested_mode;
}

Status status()
{
    Guard g;
    return st;
}

bool nextWaterfallRow(uint8_t *row)
{
    return xQueueReceive(wf_queue, row, 0) == pdTRUE;
}

void halt()
{
    Guard g;
    qso.halt();
    halt_request = true;
    xQueueReset(tx_text_queue);
    refreshQsoStatus();
}

void stop()
{
    halt();
    stop_request = true;
    for (int i = 0; i < 300 && !stopped; ++i) {
        delay(10);
    }
    ptt->set(false);
}

void capture(size_t samples)
{
    capture_want = 0;
    capture_have = 0;
    free(capture_buf);
    capture_buf = (int16_t *)ps_malloc(samples * sizeof(int16_t));
    if (capture_buf != nullptr) {
        capture_want = samples;
    }
}

const int16_t *captured(size_t *samples)
{
    if (capture_want == 0 || capture_have < capture_want) {
        return nullptr;
    }
    *samples = capture_want;
    return capture_buf;
}

int history(Entry *out, int max)
{
    Guard g;
    int n = min(max, hist_count);
    memcpy(out, hist + (hist_count - n), sizeof(Entry) * n);
    return n;
}

void setTxEnabled(bool on)
{
    Guard g;
    st.tx_enabled = on;
    if (!on) {
        halt_request = true;
    }
}

void startCq()
{
    Guard g;
    // The next slot to begin is ours
    int slot_ms = ft8::info(protocolOf(requested_mode)).slot_ms;
    int next_parity = (int)((timebase::utcMs() / slot_ms + 1) & 1);
    qso.startCq(next_parity);
    refreshQsoStatus();
}

void answer(const Entry &e)
{
    Guard g;
    qso.answer(e.msg, e.parity, (uint32_t)(timebase::utcMs() / 1000));
    refreshQsoStatus();
}

bool nextLogItem(LogItem *item)
{
    return xQueueReceive(log_queue, item, 0) == pdTRUE;
}

size_t readText(char *out, size_t max)
{
    size_t n = 0;
    while (n < max && xQueueReceive(rx_text_queue, &out[n], 0) == pdTRUE) {
        ++n;
    }
    return n;
}

size_t sendText(const char *text)
{
    if (!canSendText(requested_mode)) {
        return 0;
    }
    size_t n = 0;
    for (; text[n]; ++n) {
        if (xQueueSend(tx_text_queue, &text[n], 0) != pdTRUE) {
            break;
        }
    }
    return n;
}

void tune(int delta_hz)
{
    switch (requested_mode) {
    case Mode::RTTY:
        settings.rtty_mark = constrain(settings.rtty_mark + delta_hz, 300, 2700);
        break;
    case Mode::NAVTEX:
        settings.navtex_center = constrain(settings.navtex_center + delta_hz, 400, 2600);
        break;
    default:
        return;
    }
    applySettings();
}

void toggleOption()
{
    switch (requested_mode) {
    case Mode::CW:
        settings.cw_tracking = !settings.cw_tracking;
        break;
    case Mode::RTTY:
        settings.rtty_reverse = !settings.rtty_reverse;
        break;
    case Mode::NAVTEX:
        settings.navtex_reverse = !settings.navtex_reverse;
        break;
    default:
        return;
    }
    applySettings();
}

bool nextFaxLine(uint8_t *out)
{
    return xQueueReceive(fax_line_queue, out, 0) == pdTRUE;
}

FaxEvent nextFaxEvent()
{
    FaxEvent e = FaxEvent::None;
    xQueueReceive(fax_event_queue, &e, 0);
    return e;
}

void faxShift(int pixels)
{
    fax_shift_request = fax_shift_request + pixels;
}

void faxRephase()
{
    fax_rephase_request = true;
}

}  // namespace engine
