#pragma once

#include <stdint.h>

struct Settings {
    char call[14] = "";
    char grid[8] = "";
    bool grid_auto = true;      // take the locator from the GPS position
    bool cb_mode = false;       // FT8/FT4 with CB callsigns and the WSJT-CB message sequence
    uint32_t dial_khz = 14074;  // for the log; the radio is tuned by hand
    int tx_hz = 1500;           // audio frequency of the lowest tone
    int tx_level = 50;          // percent of full scale
    int volume = 80;            // codec output volume, percent
    int mic_gain_db = 24;
    int time_offset_ds = 0;     // manual clock correction in 0.1 s
    int max_retries = 5;
    int max_cq = 20;
    bool ptt_active_high = true;
    bool wifi_on = false;       // access point with the web interface
    char wifi_pass[20] = "digiwave";

    int cw_wpm = 20;            // sending speed
    int cw_tone = 700;          // sending tone in Hz
    bool cw_tracking = true;    // receiver follows the tone it finds
    int rtty_mark = 2125;       // mark tone in Hz
    int rtty_shift = 170;
    bool rtty_50_baud = false;  // otherwise 45.45
    bool rtty_reverse = false;
    int navtex_center = 1000;
    bool navtex_reverse = false;
    int last_mode = 0;          // engine::Mode active at power-off

    void load();
    void save() const;
};

extern Settings settings;
