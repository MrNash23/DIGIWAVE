// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

Settings settings;

namespace {
const char kNamespace[] = "ft8";  // kept from the first version so that stored settings survive
}

void Settings::load()
{
    Preferences p;
    if (!p.begin(kNamespace, true)) {
        return;  // nothing stored yet
    }
    p.getString("call", call, sizeof(call));
    p.getString("grid", grid, sizeof(grid));
    // Earlier versions kept one frequency in kHz
    dial_ft8_hz = p.getUInt("dialf8", p.isKey("dial") ? p.getUInt("dial", 14074) * 1000 : dial_ft8_hz);
    dial_ft4_hz = p.getUInt("dialf4", dial_ft4_hz);
    dial_cb_ft8_hz = p.getUInt("dialc8", dial_cb_ft8_hz);
    dial_cb_ft4_hz = p.getUInt("dialc4", dial_cb_ft4_hz);
    dial_cw_hz = p.getUInt("dialcw", dial_cw_hz);
    dial_rtty_hz = p.getUInt("dialry", dial_rtty_hz);
    dial_navtex_hz = p.getUInt("dialnx", dial_navtex_hz);
    dial_wefax_hz = p.getUInt("dialwf", dial_wefax_hz);
    dial_sstv_hz = p.getUInt("dialsv", dial_sstv_hz);
    tx_hz = p.getInt("txhz", tx_hz);
    tx_level = p.getInt("txlvl", tx_level);
    volume = p.getInt("vol", volume);
    mic_gain_db = p.getInt("mic", mic_gain_db);
    time_offset_ds = p.getInt("toff", time_offset_ds);
    max_retries = p.getInt("retry", max_retries);
    max_cq = p.getInt("maxcq", max_cq);
    ptt_active_high = p.getBool("ptthi", ptt_active_high);
    audio_board = p.getBool("audioif", audio_board);
    grid_auto = p.getBool("gridauto", grid_auto);
    cb_mode = p.getBool("cbmode", cb_mode);
    wifi_on = p.getBool("wifi", wifi_on);
    p.getString("wifipw", wifi_pass, sizeof(wifi_pass));
    cw_wpm = p.getInt("cwwpm", cw_wpm);
    cw_tone = p.getInt("cwtone", cw_tone);
    cw_tracking = p.getBool("cwtrack", cw_tracking);
    rtty_mark = p.getInt("rymark", rtty_mark);
    rtty_shift = p.getInt("ryshift", rtty_shift);
    rtty_50_baud = p.getBool("ry50", rtty_50_baud);
    rtty_reverse = p.getBool("ryrev", rtty_reverse);
    navtex_center = p.getInt("nxctr", navtex_center);
    navtex_reverse = p.getBool("nxrev", navtex_reverse);
    sleep_min = p.getInt("sleep", sleep_min);
    cq_tag = constrain(p.getInt("cqtag", cq_tag), 0, kCqTagCount - 1);
    last_mode = p.getInt("mode", last_mode);
    p.end();
}

void Settings::save() const
{
    Preferences p;
    if (!p.begin(kNamespace, false)) {
        return;
    }
    p.putString("call", call);
    p.putString("grid", grid);
    p.putUInt("dialf8", dial_ft8_hz);
    p.putUInt("dialf4", dial_ft4_hz);
    p.putUInt("dialc8", dial_cb_ft8_hz);
    p.putUInt("dialc4", dial_cb_ft4_hz);
    p.putUInt("dialcw", dial_cw_hz);
    p.putUInt("dialry", dial_rtty_hz);
    p.putUInt("dialnx", dial_navtex_hz);
    p.putUInt("dialwf", dial_wefax_hz);
    p.putUInt("dialsv", dial_sstv_hz);
    p.remove("dial");
    p.putInt("txhz", tx_hz);
    p.putInt("txlvl", tx_level);
    p.putInt("vol", volume);
    p.putInt("mic", mic_gain_db);
    p.putInt("toff", time_offset_ds);
    p.putInt("retry", max_retries);
    p.putInt("maxcq", max_cq);
    p.putBool("ptthi", ptt_active_high);
    p.putBool("audioif", audio_board);
    p.putBool("gridauto", grid_auto);
    p.putBool("cbmode", cb_mode);
    p.putBool("wifi", wifi_on);
    p.putString("wifipw", wifi_pass);
    p.putInt("cwwpm", cw_wpm);
    p.putInt("cwtone", cw_tone);
    p.putBool("cwtrack", cw_tracking);
    p.putInt("rymark", rtty_mark);
    p.putInt("ryshift", rtty_shift);
    p.putBool("ry50", rtty_50_baud);
    p.putBool("ryrev", rtty_reverse);
    p.putInt("nxctr", navtex_center);
    p.putBool("nxrev", navtex_reverse);
    p.putInt("sleep", sleep_min);
    p.putInt("cqtag", cq_tag);
    p.putInt("mode", last_mode);
    p.end();
}
