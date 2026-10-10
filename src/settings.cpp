#include "settings.h"

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
    dial_khz = p.getUInt("dial", dial_khz);
    tx_hz = p.getInt("txhz", tx_hz);
    tx_level = p.getInt("txlvl", tx_level);
    volume = p.getInt("vol", volume);
    mic_gain_db = p.getInt("mic", mic_gain_db);
    time_offset_ds = p.getInt("toff", time_offset_ds);
    max_retries = p.getInt("retry", max_retries);
    max_cq = p.getInt("maxcq", max_cq);
    ptt_active_high = p.getBool("ptthi", ptt_active_high);
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
    p.putUInt("dial", dial_khz);
    p.putInt("txhz", tx_hz);
    p.putInt("txlvl", tx_level);
    p.putInt("vol", volume);
    p.putInt("mic", mic_gain_db);
    p.putInt("toff", time_offset_ds);
    p.putInt("retry", max_retries);
    p.putInt("maxcq", max_cq);
    p.putBool("ptthi", ptt_active_high);
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
    p.putInt("mode", last_mode);
    p.end();
}
