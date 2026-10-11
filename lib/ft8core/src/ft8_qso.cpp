#include "ft8_qso.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ft8 {

bool parseReport(const char *s, int *report, bool *has_r)
{
    bool r = false;
    if (*s == 'R') {
        r = true;
        ++s;
    }
    if ((s[0] != '+' && s[0] != '-') || !isdigit((unsigned char)s[1]) || !isdigit((unsigned char)s[2]) || s[3] != '\0') {
        return false;
    }
    *report = atoi(s);
    *has_r = r;
    return true;
}

void formatReport(int snr, char *out)
{
    if (snr < -30) {
        snr = -30;
    }
    if (snr > 30) {
        snr = 30;
    }
    snprintf(out, 4, "%c%02d", snr < 0 ? '-' : '+', abs(snr));
}

const char *Qso::stateName() const
{
    switch (_state) {
    case State::Idle: return "Idle";
    case State::CallingCq: return "CQ";
    case State::Calling: return "Call";
    case State::SendReport: return "Report";
    case State::SendRReport: return "R-Report";
    case State::SendRR73: return "RR73";
    case State::Send73: return "73";
    }
    return "";
}

void Qso::enter(State s)
{
    _state = s;
    _repeats = 0;
}

void Qso::startCq(int tx_parity)
{
    _cq_mode = true;
    _tx_parity = tx_parity & 1;
    memset(&_rec, 0, sizeof(_rec));
    enter(State::CallingCq);
}

void Qso::halt()
{
    _cq_mode = false;
    memset(&_rec, 0, sizeof(_rec));
    _done_call[0] = '\0';
    enter(State::Idle);
}

void Qso::beginWith(const Decoded &d, int rx_parity, uint32_t now)
{
    memset(&_rec, 0, sizeof(_rec));
    strncpy(_rec.dx_call, d.call_de, sizeof(_rec.dx_call) - 1);
    _rec.rst_sent = d.snr;
    _rec.time_on = now;
    _rec.audio_freq = d.freq;
    _logged = false;
    _tx_parity = (rx_parity + 1) & 1;
}

void Qso::answer(const Decoded &d, int rx_parity, uint32_t now)
{
    if (d.call_de[0] == '\0') {
        return;
    }
    _cq_mode = false;
    beginWith(d, rx_parity, now);
    if (d.is_cq) {
        strncpy(_rec.dx_grid, d.extra, sizeof(_rec.dx_grid) - 1);
    }
    enter(State::Calling);
}

void Qso::finish(uint32_t now)
{
    if (!_logged) {
        _logged = true;
        _rec.time_off = now;
        if (_log != nullptr) {
            _log(_rec, _log_ctx);
        }
    }
}

void Qso::onDecoded(const Decoded *list, int count, int rx_parity, uint32_t now)
{
    if (_cfg.my_call[0] == '\0' || (_state == State::Idle && _done_call[0] == '\0')) {
        return;
    }
    // Only the slots we do not transmit in can carry replies to us
    if (rx_parity == _tx_parity) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        const Decoded &d = list[i];
        if (strcmp(d.call_to, _cfg.my_call) != 0) {
            continue;
        }

        int report = 0;
        bool has_r = false;
        bool is_report = parseReport(d.extra, &report, &has_r);
        bool is_rr73 = strcmp(d.extra, "RR73") == 0 || strcmp(d.extra, "RRR") == 0;
        bool is_73 = strcmp(d.extra, "73") == 0;

        // In CB mode reports and sign-offs do not name their sender; they are taken
        // to come from the station we are in contact with, as WSJT-CB does.
        bool anonymous = _cfg.cb_mode && d.call_de[0] == '\0' && (is_report || is_rr73 || is_73);

        if ((_state == State::Idle || _state == State::CallingCq) && _done_call[0] != '\0' &&
            (strcmp(d.call_de, _done_call) == 0 || anonymous)) {
            // The station we just finished with: a repeated R-report means our RR73
            // was not copied, anything else needs no answer.
            if (is_report && has_r) {
                enter(State::SendRR73);
                return;
            }
            continue;
        }
        if (_state == State::Idle) {
            continue;
        }

        if (_state == State::CallingCq) {
            if (d.call_de[0] == '\0') {
                continue;  // nobody to answer
            }
            // First reply to our CQ wins
            beginWith(d, rx_parity, now);
        } else if (!anonymous && strcmp(d.call_de, _rec.dx_call) != 0) {
            continue;  // someone else calling us during a QSO
        }

        if (is_report && !has_r) {
            // They sent Tx2: answer with R+report
            _rec.rst_rcvd = report;
            if (_state != State::SendRReport) {
                _rec.rst_sent = d.snr;  // the report we are about to send
                enter(State::SendRReport);
            } else {
                _repeats = 0;
            }
        } else if (is_report && has_r) {
            // They sent Tx3: confirm with RR73, the QSO counts from here
            _rec.rst_rcvd = report;
            finish(now);
            enter(State::SendRR73);
        } else if (is_rr73) {
            // They sent Tx4: reply 73 once
            if (_state == State::SendRReport) {
                finish(now);
                enter(State::Send73);
            }
        } else if (is_73) {
            if (_state == State::SendRR73 || _state == State::SendRReport) {
                finish(now);
                if (_cq_mode) {
                    enter(State::CallingCq);
                } else {
                    enter(State::Idle);
                }
            }
        } else {
            // Grid or no extra field: they sent Tx1, answer with a report
            if (d.extra[0] != '\0') {
                strncpy(_rec.dx_grid, d.extra, sizeof(_rec.dx_grid) - 1);
            }
            if (_state == State::CallingCq || _state == State::Calling) {
                _rec.rst_sent = d.snr;
                enter(State::SendReport);
            }
        }
        return;
    }
}

Packing Qso::buildMessage(char *text) const
{
    char rpt[4];
    formatReport(_rec.rst_sent, rpt);
    const char *dx = _rec.dx_call;
    const char *me = _cfg.my_call;
    text[0] = '\0';

    if (_cfg.cb_mode) {
        switch (_state) {
        case State::CallingCq:
            snprintf(text, 36, "CQ %s", me);
            return Packing::NonStandard;
        case State::Calling:
            snprintf(text, 36, "<%s> %s", dx, me);
            return Packing::NonStandard;
        case State::SendReport:
            snprintf(text, 36, "%s %s", dx, rpt);
            return Packing::FreeText;
        case State::SendRReport:
            snprintf(text, 36, "%s R%s", dx, rpt);
            return Packing::FreeText;
        case State::SendRR73:
            snprintf(text, 36, "%s RR73", dx);
            return Packing::FreeText;
        case State::Send73:
            snprintf(text, 36, "%s 73", dx);
            return Packing::FreeText;
        case State::Idle:
            break;
        }
        return Packing::FreeText;
    }

    switch (_state) {
    case State::CallingCq:
        snprintf(text, 36, "CQ %s %s", me, _cfg.my_grid);
        break;
    case State::Calling:
        snprintf(text, 36, "%s %s %s", dx, me, _cfg.my_grid);
        break;
    case State::SendReport:
        snprintf(text, 36, "%s %s %s", dx, me, rpt);
        break;
    case State::SendRReport:
        snprintf(text, 36, "%s %s R%s", dx, me, rpt);
        break;
    case State::SendRR73:
        snprintf(text, 36, "%s %s RR73", dx, me);
        break;
    case State::Send73:
        snprintf(text, 36, "%s %s 73", dx, me);
        break;
    case State::Idle:
        break;
    }
    return Packing::Auto;
}

bool Qso::nextTx(int parity, char *text, Packing *packing)
{
    if (_state == State::Idle || (parity & 1) != _tx_parity || _cfg.my_call[0] == '\0') {
        return false;
    }

    int limit = (_state == State::CallingCq) ? _cfg.max_cq : _cfg.max_retries;
    if (_repeats >= limit) {
        // No progress: give up on this station, or stop calling CQ altogether
        if (_state != State::CallingCq && _cq_mode) {
            enter(State::CallingCq);
        } else {
            halt();
            return false;
        }
    }

    Packing how = buildMessage(text);
    if (packing != nullptr) {
        *packing = how;
    }
    ++_repeats;

    // RR73 and 73 close the QSO from our side and are sent once
    if (_state == State::SendRR73 || _state == State::Send73) {
        // If the RR73 is not copied the station repeats its R-report; onDecoded()
        // recognises it by _done_call and sends the RR73 again.
        strncpy(_done_call, _rec.dx_call, sizeof(_done_call) - 1);
        enter(_cq_mode ? State::CallingCq : State::Idle);
    }
    return true;
}

bool belongsToQso(const Decoded &msg, bool own, const char *my_call, const char *dx_call)
{
    if (own) {
        return true;
    }
    if (my_call[0] != '\0' && strcmp(msg.call_to, my_call) == 0) {
        return true;
    }
    return dx_call[0] != '\0' && (strcmp(msg.call_de, dx_call) == 0 || strcmp(msg.call_to, dx_call) == 0);
}

}  // namespace ft8
