#pragma once

#include <stdint.h>

#include "ft8_decoder.h"
#include "ft8_encoder.h"

namespace ft8 {

struct QsoRecord {
    char dx_call[14];
    char dx_grid[8];
    int rst_sent;
    int rst_rcvd;
    uint32_t time_on;   // UTC, seconds since 1970
    uint32_t time_off;
    float audio_freq;
};

struct QsoConfig {
    char my_call[14] = "";
    char my_grid[8] = "";
    int max_retries = 5;  // repeats of the same message before the QSO is abandoned
    int max_cq = 20;      // unanswered CQ calls before calling stops
    // CB callsigns do not fit the standard messages. The sequence of WSJT-CB is used:
    //   CQ ME   <DX> ME   DX -NN   DX R-NN   DX RR73   DX 73
    // The last four are free text and name only the station addressed.
    bool cb_mode = false;
};

/// Auto-sequencer for standard FT8 QSOs, following the WSJT-X message order:
///   Tx1 DX ME GRID   Tx2 DX ME -NN   Tx3 DX ME R-NN   Tx4 DX ME RR73   Tx5 DX ME 73   Tx6 CQ ME GRID
class Qso {
public:
    enum class State {
        Idle,
        CallingCq,    // sending Tx6
        Calling,      // sending Tx1 to a station
        SendReport,   // sending Tx2
        SendRReport,  // sending Tx3
        SendRR73,     // sending Tx4
        Send73,       // sending Tx5
    };

    using LogFn = void (*)(const QsoRecord &rec, void *ctx);

    void configure(const QsoConfig &cfg) { _cfg = cfg; }
    void onComplete(LogFn fn, void *ctx)
    {
        _log = fn;
        _log_ctx = ctx;
    }

    /// Start calling CQ in slots of the given parity (0 = even, 1 = odd).
    void startCq(int tx_parity);

    /// Call the station that sent the given message, heard in a slot of rx_parity.
    void answer(const Decoded &d, int rx_parity, uint32_t now);

    /// Stop transmitting and drop the current QSO.
    void halt();

    /// Process the messages decoded in one slot.
    void onDecoded(const Decoded *list, int count, int rx_parity, uint32_t now);

    /// Called at the start of a slot. Returns true and fills text (at least 36 bytes)
    /// if a message is to be transmitted in this slot; packing says how to encode it.
    bool nextTx(int parity, char *text, Packing *packing = nullptr);

    State state() const { return _state; }
    const char *stateName() const;
    const char *dxCall() const { return _rec.dx_call; }
    int txParity() const { return _tx_parity; }
    bool active() const { return _state != State::Idle; }

private:
    Packing buildMessage(char *text) const;
    void enter(State s);
    void finish(uint32_t now);
    void beginWith(const Decoded &d, int rx_parity, uint32_t now);

    QsoConfig _cfg;
    State _state = State::Idle;
    QsoRecord _rec{};
    char _done_call[14] = "";  // station of the QSO just completed
    bool _cq_mode = false;  // return to CQ after a QSO
    bool _logged = false;
    int _tx_parity = 0;
    int _repeats = 0;
    LogFn _log = nullptr;
    void *_log_ctx = nullptr;
};

/// Parses "-07", "+12", "R-07". Returns false if s is not a signal report.
bool parseReport(const char *s, int *report, bool *has_r);

/// Formats a report as "-07" / "+12", clamped to the range FT8 can carry.
void formatReport(int snr, char *out);

}  // namespace ft8
