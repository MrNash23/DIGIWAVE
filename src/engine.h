#pragma once

#include <stdint.h>

#include <ft8_decoder.h>
#include <ft8_qso.h>

#include "hal/audio_if.h"
#include "hal/ptt_if.h"

/// Receive/transmit task for all modes. One mode is active at a time and owns
/// the audio path. Everything here may be called from the UI thread.
namespace engine {

enum class Mode { FT8, FT4, CW, RTTY, NAVTEX, WEFAX };
constexpr int kModes = 6;

const char *modeName(Mode m);
bool isSlotMode(Mode m);   // FT8, FT4
bool isTextMode(Mode m);   // CW, RTTY, NAVTEX
bool canSendText(Mode m);  // CW, RTTY

void begin(AudioIo &audio, Ptt &ptt);

/// Re-reads callsign, levels, speeds and tones from the settings.
void applySettings();

void setMode(Mode m);
Mode mode();

// --- common ---------------------------------------------------------------------

constexpr int kWaterfallBins = 448;
constexpr float kWaterfallMinHz = 199.2f;   // FFT bin 34 of 2048 at 12 kHz
constexpr float kWaterfallBinHz = 5.859375f;

struct Status {
    Mode mode;
    bool transmitting;
    int level;              // input peak of the last block: 0 = -60 dBFS, 100 = full scale

    // FT8 / FT4
    bool tx_enabled;
    char tx_text[36];       // message on the air or sent last
    char qso_state[12];
    char dx_call[14];
    int decode_ms;          // duration of the last decode
    int decoded;            // messages in the last slot
    uint32_t history_seq;   // increments whenever the history changes

    // CW, RTTY, NAVTEX
    int tune_hz;            // CW: tone followed inside the window, RTTY: mark tone, NAVTEX: centre
    int wpm;                // CW receive speed
    bool locked;            // signal present / receiver in step
    bool option;            // CW: tone tracking, RTTY and NAVTEX: reversed tones
};

Status status();

/// Fetches the next waterfall row (kWaterfallBins values, 0.5 dB per step). Returns false if none is waiting.
bool nextWaterfallRow(uint8_t *row);

/// Stops transmitting; in FT8/FT4 also drops the current QSO.
void halt();

/// Ends the task and releases the audio path and the PTT line, before the
/// device goes to sleep. There is no way back other than a restart.
void stop();

/// Diagnostics: records the next samples of received audio (12 kHz). captured()
/// returns them once the recording is complete, nullptr before.
void capture(size_t samples);
const int16_t *captured(size_t *samples);

// --- FT8 / FT4 --------------------------------------------------------------------

constexpr int kHistory = 40;

struct Entry {
    ft8::Decoded msg;
    uint32_t slot_utc;    // start of the slot it was heard in, seconds since 1970
    int parity;           // of that slot, 0 or 1
    bool to_me;
    char grid[5];         // locator of the sending station, from this or an earlier message
    const char *country;  // of the sending station, empty if unknown
};

/// Copies the decode history, oldest first. Returns the number of entries.
int history(Entry *out, int max);

void setTxEnabled(bool on);
void startCq();
void answer(const Entry &e);

struct LogItem {
    ft8::QsoRecord rec;
    ft8::Protocol protocol;
};

/// Fetches a completed QSO for logging. Returns false if none is waiting.
bool nextLogItem(LogItem *item);

// --- CW, RTTY, NAVTEX ---------------------------------------------------------------

/// Fetches received characters. Returns the number written to out.
size_t readText(char *out, size_t max);

/// Queues text for transmission (CW, RTTY). Returns the number of characters accepted.
size_t sendText(const char *text);

/// RTTY: moves the mark tone, NAVTEX: the centre frequency.
void tune(int delta_hz);

/// CW: tone tracking on/off, RTTY and NAVTEX: swaps the two tones.
void toggleOption();

// --- WEFAX ------------------------------------------------------------------------

/// CW is received within this distance of the CW frequency in the settings.
constexpr float kCwHalfWidthHz = 50.0f;

constexpr int kFaxWidth = 904;

enum class FaxEvent { None, Start, Phased, Stop };

/// Fetches the next picture line (kFaxWidth grey values). Returns false if none is waiting.
bool nextFaxLine(uint8_t *out);
FaxEvent nextFaxEvent();

/// Moves the picture horizontally by the given number of pixels.
void faxShift(int pixels);

/// Lets the receiver align itself to the next phasing signal again.
void faxRephase();

}  // namespace engine
