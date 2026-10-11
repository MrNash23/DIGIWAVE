#pragma once

#include <stdint.h>

struct Settings {
    char call[14] = "";
    char grid[8] = "";
    bool grid_auto = true;      // take the locator from the GPS position
    bool cb_mode = false;       // FT8/FT4 with CB callsigns and the WSJT-CB message sequence
    // Dial frequency in Hz for the log; the radio is tuned by hand, so the user
    // tells which one it is. Kept per mode, and separately for CB operation.
    uint32_t dial_ft8_hz = 14074000;
    uint32_t dial_ft4_hz = 14080000;
    uint32_t dial_cb_ft8_hz = 27265000;
    uint32_t dial_cb_ft4_hz = 27265000;
    uint32_t dial_cw_hz = 7030000;
    uint32_t dial_rtty_hz = 14085000;
    uint32_t dial_navtex_hz = 518000;   // of the station; the radio is tuned lower by the audio centre
    uint32_t dial_wefax_hz = 7880000;   // of the station; the radio is tuned 1.9 kHz lower
    uint32_t dial_sstv_hz = 14230000;

    /// The frequency that applies to a mode (0..5, see engine::Mode) in the current kind of operation.
    uint32_t &dialHz(int mode)
    {
        switch (mode) {
        case 0: return cb_mode ? dial_cb_ft8_hz : dial_ft8_hz;
        case 1: return cb_mode ? dial_cb_ft4_hz : dial_ft4_hz;
        case 2: return dial_cw_hz;
        case 3: return dial_rtty_hz;
        case 4: return dial_navtex_hz;
        case 5: return dial_wefax_hz;
        default: return dial_sstv_hz;
        }
    }
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
    int sleep_min = 3;          // sleep after this time without input and reception, 0 = never
    int cq_tag = 0;             // addition to the CQ call, index into kCqTags
    int last_mode = 0;          // engine::Mode active at power-off

    void load();
    void save() const;
};

/// What can follow CQ; the first entry is the plain call.
constexpr const char *kCqTags[] = {"", "DX", "POTA", "SOTA", "QRP", "TEST", "EU", "NA"};
constexpr int kCqTagCount = sizeof(kCqTags) / sizeof(kCqTags[0]);

extern Settings settings;
