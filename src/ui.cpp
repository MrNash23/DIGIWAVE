// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "ui.h"

#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <SD.h>

#include <algorithm>

#include <band_plan.h>
#include <ft8_adif.h>

#include "assets/assets.h"
#include "engine.h"
#include "hal/audio_if.h"
#include "logbook.h"
#include "settings.h"
#include "timebase.h"
#include "web.h"
#include "schedule.h"

// Input is read directly from the rotary encoder and the keyboard instead of
// through LVGL input devices: the screens have a fixed layout and a handful
// of shortcuts, which is simpler to handle here than through focus groups.

namespace ui {

const char kFaxDir[] = "/wefax";
const char kSstvDir[] = "/sstv";

namespace {

using engine::Mode;

constexpr int kWidth = 480;
constexpr int kRowHeight = 19;
constexpr int kFooterY = 202;
constexpr int kWfX = 16;

const lv_color_t kColBg = lv_color_hex(0x030513);
const lv_color_t kColText = lv_color_hex(0xD0D0D0);
const lv_color_t kColDim = lv_color_hex(0x808080);
const lv_color_t kColCq = lv_color_hex(0x00F5F8);     // cyan of the artwork
const lv_color_t kColToMe = lv_color_hex(0xFF1ED7);   // magenta of the artwork
const lv_color_t kColSel = lv_color_hex(0x202D7A);
const lv_color_t kColEdit = lv_color_hex(0x6A1560);
const lv_color_t kColTx = lv_color_hex(0xFF1ED7);
const lv_color_t kColWarn = lv_color_hex(0xE0A020);
const lv_color_t kColAccent = lv_color_hex(0x3766FF);
const lv_color_t kColFrame = lv_color_hex(0x202D7A);

enum class Screen { Home, Slot, Text, Fax, Sstv, Log, Setup, About, Band, FaxList, FaxView, Schedule, HomeWifi };
Screen screen = Screen::Home;

void openHome();
void openMode(Mode m);
void openLog();
void openSetup();
bool sub_from_app = false;  // a page opened from a mode returns to it, not to the menu
void openBand();
void openAbout();
void openHomeWifi();
void openFaxList();
void openOverlay();
void drawNoticeBar();
void clearText();
void backFromSub();
void openSchedule();
bool inApp();
void modeWithFrequency(Mode m, char *out, size_t size);
void powerDown(const char *reason);

// --- widgets shared by the mode screens ------------------------------------------

struct Header {
    lv_obj_t *time, *gps, *txrx, *state, *level, *slot, *battery;
};

struct Waterfall {
    lv_obj_t *canvas = nullptr;
    lv_draw_buf_t *buf = nullptr;
    int height = 0;
};

uint16_t palette[256];
float wf_floor = 60;

lv_obj_t *makeLabel(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *makeBox(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *makeBar(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, kColFrame, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    return b;
}

/// White status icon, coloured through its alpha channel.
lv_obj_t *makeIcon(lv_obj_t *parent, int x, int y, const lv_image_dsc_t *src)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_style_image_recolor(img, kColText, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

void setIcon(lv_obj_t *img, const lv_image_dsc_t *src, lv_color_t color)
{
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, color, 0);
}

/// GPS icon for the state of the clock: cyan with pulse-per-second, white with
/// GPS time, white without fix with the time of a phone, amber on the RTC alone,
/// magenta without any time.
void drawClockIcon(lv_obj_t *img)
{
    switch (timebase::source()) {
    case timebase::Source::GpsPps: setIcon(img, &img_status_gps_fix, kColCq); break;
    case timebase::Source::Gps: setIcon(img, &img_status_gps_fix, kColText); break;
    case timebase::Source::Phone: setIcon(img, &img_status_gps_no_fix, kColText); break;
    case timebase::Source::Rtc: setIcon(img, &img_status_gps_no_fix, kColWarn); break;
    case timebase::Source::None: setIcon(img, &img_status_gps_no_fix, kColTx); break;
    }
}

lv_obj_t *makeScreen()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, kColBg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

// --- battery ----------------------------------------------------------------------------

// Resting voltage of a LiPo cell against its charge, from 100 % down in steps of
// 5 %. The curve is flat in the middle and steep at both ends, so a straight line
// between 3.3 and 4.2 V would be far off.
const uint16_t kLipoMv[21] = {4200, 4150, 4110, 4080, 4020, 3980, 3950, 3910, 3870, 3850, 3840,
                              3820, 3800, 3790, 3770, 3750, 3730, 3710, 3690, 3610, 3270};
constexpr int kCellMilliohm = 150;  // internal resistance, cell and wiring

int lipoPercent(int mv)
{
    if (mv >= kLipoMv[0]) {
        return 100;
    }
    for (int i = 1; i < 21; ++i) {
        if (mv >= kLipoMv[i]) {
            // Straight between the two points of the curve
            int hi = kLipoMv[i - 1], lo = kLipoMv[i];
            return (20 - i) * 5 + 5 * (mv - lo) / max(1, hi - lo);
        }
    }
    return 0;
}

struct Battery {
    int percent = -1;      // -1 until the first reading
    bool charging = false; // USB power present
    int mv = 0, ma = 0, gauge = 0, curve = 0;
};
Battery battery;

/// Reads the gauge every few seconds. The gauge keeps its own model of the cell;
/// it is taken when it agrees with the voltage curve, which needs no learning.
const Battery &readBattery()
{
    static uint32_t last = 0;
    if (battery.percent >= 0 && millis() - last < 5000) {
        return battery;
    }
    last = millis();
    instance.gauge.refresh();  // the readings are only fetched from the chip on request
    battery.mv = instance.gauge.getVoltage();
    battery.ma = instance.gauge.getCurrent();  // negative while discharging
    battery.gauge = instance.gauge.getStateOfCharge();
    battery.charging = instance.ppm.isVbusIn();
    // The voltage under load is lower than at rest by current times resistance
    int rest_mv = battery.mv - battery.ma * kCellMilliohm / 1000;
    battery.curve = lipoPercent(rest_mv);
    int value = (battery.gauge > 0 && battery.gauge <= 100 && abs(battery.gauge - battery.curve) <= 20) ? battery.gauge : battery.curve;
    if (battery.mv < 2500) {
        value = battery.gauge;  // no plausible voltage: the gauge is all there is
    }
    // Smoothed, so that a burst of transmitting does not make it jump
    battery.percent = battery.percent < 0 ? value : (battery.percent * 3 + value + 2) / 4;
    return battery;
}

lv_color_t batteryColor(const Battery &b)
{
    if (b.charging) {
        return kColCq;
    }
    return b.percent < 15 ? kColTx : (b.percent < 35 ? kColWarn : kColText);
}

void drawBattery(lv_obj_t *label)
{
    const Battery &b = readBattery();
    lv_label_set_text_fmt(label, "%d%%", constrain(b.percent, 0, 100));
    lv_obj_set_style_text_color(label, batteryColor(b), 0);
}

Header makeHeader(lv_obj_t *scr, bool with_slot)
{
    Header h{};
    h.time = makeLabel(scr, 4, 1, 70, &lv_font_montserrat_14, kColText);
    h.gps = makeIcon(scr, 78, 1, &img_status_gps_no_fix);
    h.txrx = makeIcon(scr, 100, 1, &img_status_rx);
    if (with_slot) {
        h.slot = makeBar(scr, 122, 5, 40, 8, kColAccent);
        h.state = makeLabel(scr, 168, 1, 242, &lv_font_montserrat_14, kColText);
    } else {
        h.state = makeLabel(scr, 124, 1, 284, &lv_font_montserrat_14, kColText);
    }
    // Input level, then the battery in the corner
    h.level = makeBar(scr, 414, 5, 30, 8, kColCq);
    h.battery = makeLabel(scr, 446, 2, 32, &lv_font_montserrat_12, kColText);
    lv_obj_set_style_text_align(h.battery, LV_TEXT_ALIGN_RIGHT, 0);
    return h;
}

void drawHeader(const Header &h, const engine::Status &st)
{
    uint32_t secs = (uint32_t)(timebase::utcMs() / 1000);
    lv_label_set_text_fmt(h.time, "%02u:%02u:%02u", (unsigned)(secs / 3600 % 24), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
    drawClockIcon(h.gps);
    setIcon(h.txrx, st.transmitting ? &img_status_tx : &img_status_rx, st.transmitting ? kColTx : kColCq);
    lv_bar_set_value(h.level, st.level, LV_ANIM_OFF);
    // Magenta from -3 dBFS: the input is close to clipping
    lv_obj_set_style_bg_color(h.level, st.level > 95 ? kColTx : kColCq, LV_PART_INDICATOR);
    drawBattery(h.battery);
}

Waterfall makeWaterfall(lv_obj_t *scr, int y, int height)
{
    Waterfall wf;
    wf.height = height;
    wf.canvas = lv_canvas_create(scr);
    wf.buf = lv_draw_buf_create(engine::kWaterfallBins, height, LV_COLOR_FORMAT_RGB565, 0);
    if (wf.buf != nullptr) {
        lv_canvas_set_draw_buf(wf.canvas, wf.buf);
        lv_canvas_fill_bg(wf.canvas, kColBg, LV_OPA_COVER);
    }
    lv_obj_set_pos(wf.canvas, kWfX, y);
    return wf;
}

int waterfallX(float hz)
{
    return kWfX + (int)((hz - engine::kWaterfallMinHz) / engine::kWaterfallBinHz);
}

// The spectrum averaged over about half a minute, both slots of a pair, for
// finding a free place to transmit on
float wf_avg[engine::kWaterfallBins];
int wf_avg_rows = 0;
constexpr int kAvgRows = 176;  // a row every 2048 samples at 12 kHz: 30 s

void averageSpectrum(const uint8_t *row)
{
    // A plain mean until the time is full, then a running one
    float a = 1.0f / (float)(min(wf_avg_rows, kAvgRows - 1) + 1);
    for (int b = 0; b < engine::kWaterfallBins; ++b) {
        wf_avg[b] += a * ((float)row[b] - wf_avg[b]);
    }
    if (wf_avg_rows < kAvgRows) {
        ++wf_avg_rows;
    }
}

/// Lowest tone of the quietest place for a signal of the given width, near the
/// present TX frequency when several are equally quiet. -1 before half the time is heard.
int findFreeSpot(float width_hz)
{
    if (wf_avg_rows < kAvgRows / 2) {
        return -1;
    }
    auto bin = [](float hz) {
        return constrain((int)((hz - engine::kWaterfallMinHz) / engine::kWaterfallBinHz), 0, engine::kWaterfallBins - 1);
    };
    // The noise where every receiver passes audio: the quietest fifth of 500-2000 Hz.
    // Beyond the edges of the receiver filter it is far quieter, but nobody would hear us there.
    static float sorted[engine::kWaterfallBins];
    int n = 0;
    for (int b = bin(500); b <= bin(2000); ++b) {
        sorted[n++] = wf_avg[b];
    }
    std::nth_element(sorted, sorted + n / 5, sorted + n);
    const float noise = sorted[n / 5];
    const float guard = 25.0f;  // space to the neighbours on both sides

    int best = -1;
    float best_score = 0;
    for (int hz = 300; hz + width_hz <= 2800; hz += 10) {
        int lo = bin(hz - guard), hi = bin(hz + width_hz + guard);
        float sum = 0;
        bool dead = false;
        for (int b = lo; b <= hi; ++b) {
            sum += wf_avg[b];
            dead = dead || wf_avg[b] < noise - 12;  // 6 dB under the noise: outside the filter
        }
        if (dead) {
            continue;
        }
        // In 0.5 dB steps; 1000 Hz away from the present frequency weighs 1 dB
        float score = sum / (float)(hi - lo + 1) + (float)abs(hz - settings.tx_hz) * 0.002f;
        if (best < 0 || score < best_score) {
            best = hz;
            best_score = score;
        }
    }
    return best;
}

/// Moves new spectrum rows into the given waterfall, or just discards them.
void drawWaterfall(Waterfall *wf)
{
    static uint8_t row[engine::kWaterfallBins];
    bool changed = false;
    while (engine::nextWaterfallRow(row)) {
        averageSpectrum(row);
        if (wf == nullptr || wf->buf == nullptr) {
            continue;
        }
        // The colour scale starts at the noise in the part of the spectrum that carries
        // audio. A receiver with a narrow filter leaves most of the display empty, so
        // the average over everything would sit far below that noise; the level that
        // 85 % of the values stay under does not.
        uint16_t hist[256] = {0};
        for (uint8_t v : row) {
            ++hist[v];
        }
        int level = 255, above = 0;
        while (level > 0 && above + hist[level] <= engine::kWaterfallBins * 15 / 100) {
            above += hist[level--];
        }
        wf_floor = 0.9f * wf_floor + 0.1f * level;

        // The newest row is the top one and the picture runs downwards, as in WSJT-X.
        // The marks for tuning sit above the strip and so point at what is heard right now.
        uint32_t stride = wf->buf->header.stride;
        memmove(wf->buf->data + stride, wf->buf->data, stride * (wf->height - 1));
        uint16_t *line = (uint16_t *)wf->buf->data;
        for (int x = 0; x < engine::kWaterfallBins; ++x) {
            int idx = (int)((row[x] - wf_floor + 6) * 6);  // red at about 18 dB above the noise
            line[x] = palette[constrain(idx, 0, 255)];
        }
        changed = true;
    }
    if (changed) {
        lv_obj_invalidate(wf->canvas);
    }
}

void buildPalette()
{
    // black - blue - cyan - yellow - red
    for (int i = 0; i < 256; ++i) {
        uint8_t r, g, b;
        if (i < 64) {
            r = 0, g = 0, b = i * 3;
        } else if (i < 128) {
            r = 0, g = (i - 64) * 4, b = 192;
        } else if (i < 192) {
            r = (i - 128) * 4, g = 255, b = 192 - (i - 128) * 3;
        } else {
            r = 255, g = 255 - (i - 192) * 4, b = 0;
        }
        palette[i] = lv_color_to_u16(lv_color_make(r, g, b));
    }
}

char notice[56] = "";
uint32_t notice_until = 0;

void showNotice(const char *text)
{
    strncpy(notice, text, sizeof(notice) - 1);
    notice_until = millis() + 4000;
}

bool noticeActive()
{
    return millis() < notice_until;
}

bool stationConfigured()
{
    if (settings.cb_mode) {
        // CB messages carry no locator
        if (settings.call[0] == '\0') {
            showNotice("Set your callsign first (Setup)");
            return false;
        }
        return true;
    }
    if (settings.call[0] == '\0' || strlen(settings.grid) < 4) {
        showNotice("Set callsign and locator first (Setup)");
        return false;
    }
    return true;
}

// --- FT8 / FT4 screen ----------------------------------------------------------------

constexpr int kListRows = 7;
constexpr int kListY = 20;
constexpr int kSlotWfY = 156;

lv_obj_t *scr_slot;
Header slot_header;
Waterfall slot_wf;
lv_obj_t *tx_marker, *slot_footer;
lv_obj_t *row_bg[kListRows], *row_meta[kListRows], *row_msg[kListRows], *row_info[kListRows];

engine::Entry entries[engine::kHistory];
int entry_count = 0;
int selected = -1;
bool follow = true;
uint32_t history_seq = 0;

// The list shows everything heard, or only the own contact. It turns to the
// contact by itself while one is going on; V switches by hand.
enum class ListView { All, Qso };
ListView list_view = ListView::All;
bool view_by_hand = false;
bool contact_active = false;
uint32_t contact_ended = 0;
const lv_color_t kColOwn = lv_color_hex(0xFFD23C);     // our own transmissions
const lv_color_t kColWorked = lv_color_hex(0x5C5C5C);  // in the logbook on this band
const lv_color_t kColWorkedElsewhere = lv_color_hex(0x7FA0B8);  // in the logbook on another band
const lv_color_t kColOwnBg = lv_color_hex(0x2A2408);

// F: the wheel moves the TX frequency until a press or F; Back restores it
bool tx_tuning = false;
int tx_hz_before = 0;

void setTxHz(int hz);
void startTxTuning();
void endTxTuning(bool keep);
void freeSpot();
void toggleSlot();

/// Bandwidth of a signal: 8 tones 6.25 Hz apart in FT8, 4 tones 20.8 Hz apart in FT4
float signalWidthHz(Mode m)
{
    return m == Mode::FT4 ? 83.3f : 50.0f;
}

/// The slots of a parity; -1 is the next slot, as a CQ call starts without a choice
const char *slotName(int parity, bool brief)
{
    if (parity < 0) {
        return brief ? "next" : "next free";
    }
    if (brief) {
        return parity ? "2nd" : "1st";
    }
    return parity ? "2nd (odd)" : "1st (even)";
}

void buildSlot()
{
    scr_slot = makeScreen();
    slot_header = makeHeader(scr_slot, true);
    for (int i = 0; i < kListRows; ++i) {
        int y = kListY + i * kRowHeight;
        row_bg[i] = makeBox(scr_slot, 0, y, kWidth, kRowHeight, kColBg);
        row_meta[i] = makeLabel(scr_slot, 4, y + 1, 124, &lv_font_montserrat_14, kColDim);
        row_msg[i] = makeLabel(scr_slot, 132, y + 1, 164, &lv_font_montserrat_14, kColText);
        row_info[i] = makeLabel(scr_slot, 298, y + 1, 178, &lv_font_montserrat_14, kColDim);
        lv_obj_set_style_text_align(row_info[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    slot_wf = makeWaterfall(scr_slot, kSlotWfY, 44);
    tx_marker = makeBox(scr_slot, kWfX, kSlotWfY, 8, 3, kColTx);
    slot_footer = makeLabel(scr_slot, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
}

void reloadHistory(uint32_t seq)
{
    // Keep the selection on the same message while new ones arrive
    engine::Entry previous{};
    bool had_selection = !follow && selected >= 0 && selected < entry_count;
    if (had_selection) {
        previous = entries[selected];
    }
    entry_count = list_view == ListView::Qso ? engine::qsoHistory(entries, engine::kHistory) : engine::history(entries, engine::kHistory);
    history_seq = seq;
    if (had_selection) {
        selected = -1;
        for (int i = 0; i < entry_count; ++i) {
            if (entries[i].slot_utc == previous.slot_utc && strcmp(entries[i].msg.text, previous.msg.text) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (!had_selection || selected < 0) {
        follow = true;
        selected = entry_count - 1;
    }
}

void drawList()
{
    int top = selected - (kListRows - 1);
    if (top > entry_count - kListRows) {
        top = entry_count - kListRows;
    }
    if (top < 0) {
        top = 0;
    }
    for (int i = 0; i < kListRows; ++i) {
        int idx = top + i;
        if (idx >= entry_count) {
            lv_label_set_text(row_meta[i], "");
            // An empty contact view says so instead of looking broken
            lv_label_set_text(row_msg[i], i == 0 && entry_count == 0 && list_view == ListView::Qso ? "No contact yet" : "");
            lv_obj_set_style_text_color(row_msg[i], kColDim, 0);
            lv_label_set_text(row_info[i], "");
            lv_obj_set_style_bg_color(row_bg[i], kColBg, 0);
            continue;
        }
        const engine::Entry &e = entries[idx];
        uint32_t t = e.slot_utc;
        if (e.own) {
            lv_label_set_text_fmt(row_meta[i], "%02u%02u%02u   TX  %4d", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60),
                                  (unsigned)(t % 60), (int)e.msg.freq);
            lv_label_set_text(row_msg[i], e.msg.text);
            lv_label_set_text(row_info[i], "");
            lv_obj_set_style_text_color(row_msg[i], kColOwn, 0);
            lv_obj_set_style_bg_color(row_bg[i], idx == selected ? kColSel : kColOwnBg, 0);
            continue;
        }
        lv_label_set_text_fmt(row_meta[i], "%02u%02u%02u  %+03d  %4d", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60),
                              (unsigned)(t % 60), e.msg.snr, (int)e.msg.freq);
        lv_label_set_text(row_msg[i], e.msg.text);
        // Distance from the own locator and country. The locator itself is left out:
        // where a station sends it, it stands in the message already.
        char where[20] = "";
        int km = ft8::gridDistanceKm(settings.grid, e.grid);
        if (km >= 0) {
            snprintf(where, sizeof(where), "%d km", km);
        }
        if (e.country[0] != '\0') {
            lv_label_set_text_fmt(row_info[i], "%s%s%.15s", where, where[0] ? "  " : "", e.country);
        } else {
            lv_label_set_text(row_info[i], where);
        }
        // Stations already in the logbook step back: grey on this band, muted when only worked on another
        lv_color_t color = e.msg.is_cq ? kColCq : kColText;
        if (e.to_me) {
            color = kColToMe;
        } else if (e.msg.call_de[0] != '\0') {
            const char *band = ft8::bandName(settings.dialHz((int)engine::mode()) / 1000);
            int worked = logbook::worked(e.msg.call_de, band ? band : "");
            if (worked == 2) {
                color = kColWorked;
            } else if (worked == 1) {
                color = kColWorkedElsewhere;
            }
        }
        lv_obj_set_style_text_color(row_msg[i], color, 0);
        lv_obj_set_style_bg_color(row_bg[i], idx == selected ? kColSel : kColBg, 0);
    }
}

void drawList();

void setListView(ListView view)
{
    list_view = view;
    follow = true;
    selected = -1;
    reloadHistory(history_seq);
    drawList();
}

void drawSlot()
{
    engine::Status st = engine::status();
    drawHeader(slot_header, st);

    // Calling or working a station turns the list to the contact; some time after its end it turns back
    bool active = st.transmitting || strcmp(st.qso_state, "Idle") != 0;
    if (active != contact_active) {
        contact_active = active;
        view_by_hand = false;
        if (active) {
            if (list_view != ListView::Qso) {
                setListView(ListView::Qso);
            }
        } else {
            contact_ended = millis();
        }
    }
    if (!active && !view_by_hand && list_view == ListView::Qso && millis() - contact_ended > 30000) {
        setListView(ListView::All);
    }
    int slot_ms = ft8::info(st.mode == Mode::FT4 ? ft8::Protocol::FT4 : ft8::Protocol::FT8).slot_ms;
    lv_bar_set_value(slot_header.slot, (int)(timebase::utcMs() % slot_ms) * 100 / slot_ms, LV_ANIM_OFF);

    // Mode and the frequency the logbook will record
    char name[28];
    modeWithFrequency(st.mode, name, sizeof(name));
    if (st.transmitting) {
        // The message needs the room, so the frequency is left out while sending
        lv_label_set_text_fmt(slot_header.state, "%s%s TX %s", engine::modeName(st.mode), settings.cb_mode ? " CB" : "", st.tx_text);
        lv_obj_set_style_text_color(slot_header.state, kColTx, 0);
    } else {
        if (st.dx_call[0] != '\0' && strcmp(st.qso_state, "Idle") != 0) {
            lv_label_set_text_fmt(slot_header.state, "%s RX %s %s", name, st.qso_state, st.dx_call);
        } else {
            lv_label_set_text_fmt(slot_header.state, "%s RX %s", name, st.qso_state);
        }
        lv_obj_set_style_text_color(slot_header.state, kColText, 0);
    }
    // The marker covers the signal; while tuning it runs down the waterfall
    lv_obj_set_x(tx_marker, waterfallX((float)settings.tx_hz));
    lv_obj_set_width(tx_marker, max(2, (int)(signalWidthHz(st.mode) / engine::kWaterfallBinHz + 0.5f)));
    lv_obj_set_height(tx_marker, tx_tuning ? 44 : 3);
    lv_obj_set_style_bg_opa(tx_marker, tx_tuning ? LV_OPA_50 : LV_OPA_COVER, 0);

    if (noticeActive()) {
        lv_label_set_text(slot_footer, notice);
        lv_obj_set_style_text_color(slot_footer, kColWarn, 0);
    } else if (tx_tuning) {
        lv_label_set_text_fmt(slot_footer, "TX %d Hz   wheel: move  G: free spot  E: slot %s   press: OK  Back: undo",
                              settings.tx_hz, slotName(engine::txParity(), true));
        lv_obj_set_style_text_color(slot_footer, kColCq, 0);
    } else {
        // T shows the state of the transmitter; V tells which list a press brings up
        lv_label_set_text_fmt(slot_footer, "Enter: call  T: TX %s  C: CQ  H: halt  V: %s  B: band  |  %d dec",
                              st.tx_enabled ? "ON" : "off", list_view == ListView::Qso ? "show all" : "show QSO", st.decoded);
        lv_obj_set_style_text_color(slot_footer, st.tx_enabled ? kColTx : kColDim, 0);
    }

    if (st.history_seq != history_seq) {
        reloadHistory(st.history_seq);
        drawList();
    }
}

void slotRotate(int dir)
{
    if (tx_tuning) {
        setTxHz(settings.tx_hz - dir * 50);  // up raises it, as in the setup
        return;
    }
    if (entry_count == 0) {
        return;
    }
    selected = constrain(selected + dir, 0, entry_count - 1);
    follow = selected == entry_count - 1;
    drawList();
}

void slotPress()
{
    if (tx_tuning) {
        endTxTuning(true);
        return;
    }
    if (selected < 0 || selected >= entry_count || !stationConfigured()) {
        return;
    }
    const engine::Entry &e = entries[selected];
    if (e.own || e.msg.call_de[0] == '\0' || strcmp(e.msg.call_de, settings.call) == 0) {
        showNotice("No station to answer in this message");
        return;
    }
    // Answering a station is a decision to transmit: TX is switched on with it
    if (!timebase::valid()) {
        showNotice("No time source - cannot transmit");
        return;
    }
    engine::answer(e);
    engine::setTxEnabled(true);
    char text[56];
    snprintf(text, sizeof(text), "Calling %s  -  TX is on, H stops", e.msg.call_de);
    showNotice(text);
}

void toggleTx()
{
    bool on = !engine::status().tx_enabled;
    if (on && !stationConfigured()) {
        return;
    }
    if (on && !timebase::valid()) {
        showNotice("No time source - TX stays off");
        return;
    }
    engine::setTxEnabled(on);
}

/// The addition to CQ in use; none in CB operation, whose call has no room for it.
const char *cqTag()
{
    return settings.cb_mode ? "" : kCqTags[constrain(settings.cq_tag, 0, kCqTagCount - 1)];
}

void callCq()
{
    if (stationConfigured()) {
        engine::startCq();
        char text[56];
        snprintf(text, sizeof(text), "%s CQ%s%s%s", engine::status().tx_enabled ? "Calling" : "Armed:", cqTag()[0] ? " " : "", cqTag(),
                 engine::status().tx_enabled ? "" : "  -  TX is off (T)");
        showNotice(text);
    }
}

void nextCqTag()
{
    settings.cq_tag = (settings.cq_tag + 1) % kCqTagCount;
    settings.save();
    engine::applySettings();
}

void haltNow()
{
    engine::halt();
    showNotice("Halted");
}

void setTxHz(int hz)
{
    settings.tx_hz = constrain(hz, 300, 2800);
    engine::applySettings();
}

void startTxTuning()
{
    tx_tuning = true;
    tx_hz_before = settings.tx_hz;
}

void endTxTuning(bool keep)
{
    if (!tx_tuning) {
        return;
    }
    tx_tuning = false;
    if (keep) {
        settings.save();
    } else {
        setTxHz(tx_hz_before);
    }
}

void freeSpot()
{
    int hz = findFreeSpot(signalWidthHz(engine::mode()));
    if (hz < 0) {
        showNotice("Listening first - try again in a few seconds");
        return;
    }
    setTxHz(hz);
    if (!tx_tuning) {
        settings.save();
    }
    char text[56];
    snprintf(text, sizeof(text), "TX %d Hz - the quietest spot", hz);
    showNotice(text);
}

void toggleSlot()
{
    int parity = engine::toggleTxParity();
    if (parity < 0) {
        showNotice("In a QSO the slot follows the other station");
        return;
    }
    char text[56];
    snprintf(text, sizeof(text), "TX in the %s slots", slotName(parity, false));
    showNotice(text);
}

void toggleView()
{
    view_by_hand = true;
    setListView(list_view == ListView::Qso ? ListView::All : ListView::Qso);
}

void slotKey(char c)
{
    char k = (char)tolower((unsigned char)c);
    if (tx_tuning) {
        if (k == 0x08) {
            endTxTuning(false);
            return;
        }
        if (k == 'f') {
            endTxTuning(true);
            return;
        }
        if (k != 'g' && k != 'e' && k != '\n' && k != '\r') {
            endTxTuning(true);  // any other key keeps the new frequency and does its job
        }
    }
    switch (k) {
    case 'f': startTxTuning(); break;
    case 'g': freeSpot(); break;
    case 'e': toggleSlot(); break;
    case 't': toggleTx(); break;
    case 'c': callCq(); break;
    case 'h': haltNow(); break;
    case 's': openSetup(); break;
    case 'l': openLog(); break;
    case 'b': openBand(); break;
    case 'v': toggleView(); break;
    case 'm':
    case 0x08:
        // Back leaves to the menu; a contact goes on in the background until H
        openHome();
        break;
    case '\n':
    case '\r':
        slotPress();
        break;
    default:
        break;
    }
}

// --- CW, RTTY, NAVTEX screen ---------------------------------------------------------

constexpr int kTextY = 20;
constexpr int kTextWfY = 182;       // spectrum strip, down to the bottom of the screen
constexpr int kTextWfHeight = 40;
constexpr int kMarkY = 173;         // band above it for the tuning marks

lv_obj_t *scr_text;
Header text_header;
Waterfall text_wf;
lv_obj_t *text_box, *text_label, *input_label, *marker_a, *marker_b, *marker_span, *marker_tone;

// Triangle pointing down, 9 x 7 pixels, as an alpha mask
constexpr int kTriW = 9, kTriH = 7;
uint8_t tri_pixels[kTriW * kTriH];
lv_image_dsc_t tri_image;

void buildTriangle()
{
    for (int y = 0; y < kTriH; ++y) {
        for (int x = 0; x < kTriW; ++x) {
            // Full width in the two top rows, then one pixel less on each side per row
            int inset = y < 2 ? 0 : y - 2;
            tri_pixels[y * kTriW + x] = (x >= inset && x < kTriW - inset && inset <= kTriW / 2) ? 255 : 0;
        }
    }
    tri_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    tri_image.header.cf = LV_COLOR_FORMAT_A8;
    tri_image.header.w = kTriW;
    tri_image.header.h = kTriH;
    tri_image.header.stride = kTriW;
    tri_image.data = tri_pixels;
    tri_image.data_size = sizeof(tri_pixels);
}

char rx_text[1500] = "";
size_t rx_len = 0;
bool rx_dirty = false;
char input[80] = "";

void appendText(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (rx_len + 1 >= sizeof(rx_text)) {
            // Drop the oldest third
            size_t cut = sizeof(rx_text) / 3;
            memmove(rx_text, rx_text + cut, rx_len - cut);
            rx_len -= cut;
        }
        rx_text[rx_len++] = s[i];
    }
    rx_text[rx_len] = '\0';
    rx_dirty = true;
}

void buildText()
{
    scr_text = makeScreen();
    text_header = makeHeader(scr_text, false);
    text_box = makeBox(scr_text, 0, kTextY, kWidth, 130, kColBg);
    text_label = lv_label_create(text_box);
    lv_obj_set_width(text_label, kWidth - 8);
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(text_label, kColText, 0);
    lv_label_set_text(text_label, "");
    // The newest text stays visible: the label grows upwards out of the box
    lv_obj_align(text_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    input_label = makeLabel(scr_text, 4, 152, kWidth - 8, &lv_font_montserrat_14, kColCq);
    text_wf = makeWaterfall(scr_text, kTextWfY, kTextWfHeight);
    // The tuning marks sit above the spectrum, not in it
    buildTriangle();
    marker_span = makeBox(scr_text, kWfX, kMarkY, 10, 2, kColText);
    marker_a = makeIcon(scr_text, kWfX, kMarkY, &tri_image);
    marker_b = makeIcon(scr_text, kWfX, kMarkY, &tri_image);
    marker_tone = makeBox(scr_text, kWfX, kMarkY + 4, 3, 4, kColCq);
}

void drawText()
{
    engine::Status st = engine::status();
    drawHeader(text_header, st);
    bool can_send = engine::canSendText(st.mode);

    const char *lock = st.locked ? "signal" : "-";
    char name[28];
    modeWithFrequency(st.mode, name, sizeof(name));
    switch (st.mode) {
    case Mode::CW:
        if (st.transmitting) {
            lv_label_set_text_fmt(text_header.state, "%s TX %d WPM %d Hz", name, settings.cw_wpm, settings.cw_tone);
        } else {
            lv_label_set_text_fmt(text_header.state, "%s RX %d WPM %d Hz %s", name, st.wpm, st.tune_hz, lock);
        }
        break;
    case Mode::RTTY:
        lv_label_set_text_fmt(text_header.state, "%s %s %sBd %d/%d%s %s", name, st.transmitting ? "TX" : "RX",
                              settings.rtty_50_baud ? "50" : "45", settings.rtty_mark, settings.rtty_mark + settings.rtty_shift,
                              st.option ? " rev" : "", st.transmitting ? "" : lock);
        break;
    default:
        lv_label_set_text_fmt(text_header.state, "%s kHz %d Hz%s %s", name, settings.navtex_center, st.option ? " rev" : "",
                              st.locked ? "in step" : "searching");
        break;
    }
    lv_obj_set_style_text_color(text_header.state, st.transmitting ? kColTx : kColText, 0);

    // Tone markers on the spectrum
    float a = 0, b = 0;
    if (st.mode == Mode::CW) {
        // The window the signal has to be tuned into
        a = settings.cw_tone - engine::kCwHalfWidthHz;
        b = settings.cw_tone + engine::kCwHalfWidthHz;
    } else if (st.mode == Mode::RTTY) {
        a = (float)settings.rtty_mark;
        b = (float)(settings.rtty_mark + settings.rtty_shift);
    } else {
        a = settings.navtex_center - 85.0f;
        b = settings.navtex_center + 85.0f;
    }
    // Each triangle points at one frequency with its tip
    const int left = kWfX, right = kWfX + engine::kWaterfallBins - 1;
    int xa = constrain(waterfallX(a), left, right);
    int xb = constrain(waterfallX(b), left, right);
    lv_obj_set_x(marker_a, xa - kTriW / 2);
    lv_obj_set_x(marker_b, xb - kTriW / 2);
    if (st.mode == Mode::CW) {
        // The bar between them is the receive window
        lv_obj_remove_flag(marker_span, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(marker_span, xa);
        lv_obj_set_width(marker_span, xb - xa + 1);
    } else {
        lv_obj_add_flag(marker_span, LV_OBJ_FLAG_HIDDEN);
    }

    // In CW a small mark shows where in the window the decoder hears the tone
    if (st.mode == Mode::CW && st.locked) {
        lv_obj_remove_flag(marker_tone, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(marker_tone, constrain(waterfallX((float)st.tune_hz), xa, xb) - 1);
    } else {
        lv_obj_add_flag(marker_tone, LV_OBJ_FLAG_HIDDEN);
    }

    // NAVTEX only receives: the input line makes room for text
    lv_obj_set_height(text_box, can_send ? 130 : 150);
    if (can_send) {
        lv_obj_remove_flag(input_label, LV_OBJ_FLAG_HIDDEN);
        if (input[0] == '\0') {
            lv_label_set_text(input_label, "> _     Enter sends   Space + key: digits, symbols");
        } else {
            // A long line shows its end, where the typing happens
            size_t n = strlen(input);
            lv_label_set_text_fmt(input_label, "> %s%s_", n > 54 ? "..." : "", n > 54 ? input + n - 52 : input);
        }
    } else {
        lv_obj_add_flag(input_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (rx_dirty) {
        rx_dirty = false;
        // The label only needs what fits on the screen
        size_t from = rx_len > 500 ? rx_len - 500 : 0;
        lv_label_set_text(text_label, rx_text + from);
        lv_obj_align(text_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    }
}

void leaveTextMode()
{
    settings.save();  // tuning and options changed on this screen
    openHome();
}

void textRotate(int dir)
{
    switch (engine::mode()) {
    case Mode::CW:
        // The wheel moves the receive window, which is also the tone we send on
        settings.cw_tone = constrain(settings.cw_tone - dir * 10, 400, 1200);
        engine::applySettings();
        break;
    default:
        engine::tune(-dir * 5);
        break;
    }
}

void textKey(char c)
{
    bool can_send = engine::canSendText(engine::mode());
    size_t n = strlen(input);
    if (c == 0x08) {
        // Back takes typed text away first, then leaves to the menu
        if (n > 0) {
            input[n - 1] = '\0';
        } else {
            leaveTextMode();
        }
    } else if (c == '\n' || c == '\r') {
        if (can_send && n > 0) {
            submitText(input);
            input[0] = '\0';
        }
    } else if (!can_send && tolower((unsigned char)c) == 'b') {
        openBand();
    } else if (!can_send && tolower((unsigned char)c) == 'c') {
        clearText();
    } else if (can_send && isprint((unsigned char)c) && n + 1 < sizeof(input)) {
        input[n] = (char)toupper((unsigned char)c);
        input[n + 1] = '\0';
    }
}

// --- WEFAX screen ----------------------------------------------------------------------

constexpr int kFaxY = 20;
constexpr int kFaxHeight = 202;
constexpr uint32_t kBmpDataOffset = 14 + 40 + 256 * 4;

lv_obj_t *scr_fax;
Header fax_header;
lv_obj_t *fax_canvas;
lv_draw_buf_t *fax_buf;

// Every received line is kept in memory, so that a picture can be saved as a
// whole at any time - also the part that arrived before anyone decided to
// keep it. kFaxKeep lines are half an hour at 120 lines per minute.
constexpr uint32_t kFaxKeep = 3600;
uint8_t *fax_ring = nullptr;     // kFaxKeep lines of kFaxWidth grey values, in PSRAM
uint32_t fax_total = 0;          // lines received since the mode was entered
uint32_t fax_begin = 0;          // where the current picture starts
uint32_t fax_saved_to = 0;       // lines up to here are in a file already
bool fax_auto = true;            // save a picture when the station's stop tone ends it
bool fax_hold = false;           // reception paused: new lines are not taken
char fax_name[40] = "";
uint32_t fax_line_count = 0;

void putLe(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/// 8-bit greyscale BMP, rows stored top to bottom.
void writeBmpHeader(File &f, uint32_t lines)
{
    const uint32_t image_size = lines * engine::kFaxWidth;  // the width needs no row padding
    uint8_t h[54] = {0};
    h[0] = 'B';
    h[1] = 'M';
    putLe(h + 2, kBmpDataOffset + image_size, 4);
    putLe(h + 10, kBmpDataOffset, 4);
    putLe(h + 14, 40, 4);
    putLe(h + 18, engine::kFaxWidth, 4);
    putLe(h + 22, (uint32_t)(-(int32_t)lines), 4);  // negative height: top-down
    putLe(h + 26, 1, 2);
    putLe(h + 28, 8, 2);
    putLe(h + 34, image_size, 4);
    putLe(h + 46, 256, 4);
    f.write(h, sizeof(h));
    uint8_t pal[4];
    for (int i = 0; i < 256; ++i) {
        pal[0] = pal[1] = pal[2] = (uint8_t)i;
        pal[3] = 0;
        f.write(pal, 4);
    }
}

/// Lines of the current picture that are still in memory.
uint32_t faxPictureLines()
{
    return min(fax_total - fax_begin, kFaxKeep);
}

/// Writes the current picture, from its start to the line received last, to
/// a new file. The whole picture is written in one go, so the file is complete
/// or not there at all. Returns false if nothing was saved.
bool saveFaxPicture()
{
    uint32_t lines = faxPictureLines();
    if (fax_ring == nullptr || lines < 20) {
        showNotice("No picture to save yet");
        return false;
    }
    uint32_t first = fax_total - lines;
    time_t secs = (time_t)(timebase::utcMs() / 1000);
    struct tm t;
    gmtime_r(&secs, &t);
    snprintf(fax_name, sizeof(fax_name), "WF_%04d%02d%02d_%02d%02d%02d.bmp", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", kFaxDir, fax_name);

    // The card shares the SPI bus with the display
    instance.lockSPI();
    bool ok = SD.cardType() != CARD_NONE;
    if (ok) {
        SD.mkdir(kFaxDir);
        File f = SD.open(path, FILE_WRITE);
        ok = (bool)f;
        if (ok) {
            writeBmpHeader(f, lines);
            // The lines lie in a ring; up to 32 of them go to the card at a time
            for (uint32_t i = 0; i < lines && ok;) {
                uint32_t slot = (first + i) % kFaxKeep;
                uint32_t n = min(min(lines - i, kFaxKeep - slot), (uint32_t)32);
                size_t bytes = (size_t)n * engine::kFaxWidth;
                ok = f.write(fax_ring + (size_t)slot * engine::kFaxWidth, bytes) == bytes;
                i += n;
            }
            f.close();
            if (!ok) {
                SD.remove(path);
            }
        }
    }
    instance.unlockSPI();
    char text[56];
    if (ok) {
        fax_saved_to = fax_total;
        snprintf(text, sizeof(text), "Saved %u lines as %s", (unsigned)lines, fax_name);
    } else {
        snprintf(text, sizeof(text), "NOT saved - SD card?");
    }
    showNotice(text);
    return ok;
}

/// Saves the picture if saving is automatic and it has lines no file holds yet.
void saveFaxIfUnsaved()
{
    if (fax_auto && fax_total > fax_saved_to && faxPictureLines() >= 100 && fax_total - max(fax_begin, fax_saved_to) >= 100) {
        saveFaxPicture();
    }
}

void buildFax()
{
    scr_fax = makeScreen();
    fax_header = makeHeader(scr_fax, false);
    fax_canvas = lv_canvas_create(scr_fax);
    fax_buf = lv_draw_buf_create(kWidth, kFaxHeight, LV_COLOR_FORMAT_RGB565, 0);
    if (fax_buf != nullptr) {
        lv_canvas_set_draw_buf(fax_canvas, fax_buf);
        lv_canvas_fill_bg(fax_canvas, kColBg, LV_OPA_COVER);
    }
    lv_obj_set_pos(fax_canvas, 0, kFaxY);
}

/// Takes picture lines from the receiver: saves them and adds them to the display.
void processFax()
{
    static uint8_t line[engine::kFaxWidth];
    static uint16_t grey565[256];
    static bool grey_ready = false;
    if (!grey_ready) {
        grey_ready = true;
        for (int i = 0; i < 256; ++i) {
            grey565[i] = lv_color_to_u16(lv_color_make(i, i, i));
        }
    }

    // With automatic saving the station's tones divide the reception into
    // pictures. Without it they are only reported, and the picture is whatever
    // has arrived since the user last started a new one.
    switch (engine::nextFaxEvent()) {
    case engine::FaxEvent::Start:
        if (fax_auto) {
            saveFaxIfUnsaved();
            fax_begin = fax_total;
            fax_saved_to = fax_total;
        }
        showNotice("Start tone - a new picture begins");
        break;
    case engine::FaxEvent::Phased:
        showNotice("Picture aligned");
        break;
    case engine::FaxEvent::Stop:
        if (fax_auto) {
            saveFaxIfUnsaved();
            fax_begin = fax_total;
            fax_saved_to = fax_total;
        } else {
            showNotice("Stop tone - Enter saves the picture");
        }
        break;
    default:
        break;
    }

    bool changed = false;
    while (engine::nextFaxLine(line)) {
        if (fax_hold) {
            continue;  // paused: the picture stays as it is
        }
        if (fax_ring == nullptr) {
            fax_ring = (uint8_t *)ps_malloc((size_t)kFaxKeep * engine::kFaxWidth);
        }
        if (fax_ring != nullptr) {
            memcpy(fax_ring + (size_t)(fax_total % kFaxKeep) * engine::kFaxWidth, line, sizeof(line));
            ++fax_total;
        }
        // The display is about half as wide as the picture, so every second line keeps the proportions
        if ((fax_line_count++ & 1) == 0 && fax_buf != nullptr) {
            uint32_t stride = fax_buf->header.stride;
            memmove(fax_buf->data, fax_buf->data + stride, stride * (kFaxHeight - 1));
            uint16_t *out = (uint16_t *)(fax_buf->data + stride * (kFaxHeight - 1));
            for (int x = 0; x < kWidth; ++x) {
                int a = x * engine::kFaxWidth / kWidth;
                out[x] = grey565[(line[a] + line[a + 1]) / 2];
            }
            changed = true;
        }
    }
    if (changed && screen == Screen::Fax) {
        lv_obj_invalidate(fax_canvas);
    }
}

void drawFax()
{
    engine::Status st = engine::status();
    drawHeader(fax_header, st);
    char name[28];
    modeWithFrequency(Mode::WEFAX, name, sizeof(name));
    // How much of the picture is in memory, and whether a file holds it already
    uint32_t lines = faxPictureLines();
    bool unsaved = fax_total > fax_saved_to;
    lv_label_set_text_fmt(fax_header.state, "%s kHz %u lines %s", name, (unsigned)lines,
                          fax_hold ? "PAUSED" : (lines >= 20 && unsaved ? (fax_auto ? "auto save" : "not saved") : ""));
    lv_obj_set_style_text_color(fax_header.state, fax_hold ? kColWarn : kColText, 0);
}

void saveFaxNow()
{
    showNotice("Saving the picture ...");
    drawNoticeBar();
    lv_timer_handler();
    saveFaxPicture();
}

void toggleFaxAuto()
{
    fax_auto = !fax_auto;
    showNotice(fax_auto ? "A picture is saved when its stop tone ends it" : "Pictures are only saved with Enter");
}

void toggleFaxHold()
{
    fax_hold = !fax_hold;
    showNotice(fax_hold ? "Paused - the picture stays; Enter saves it, H goes on" : "Receiving again");
}

/// Throws away what was received and starts afresh: empty screen, empty
/// picture, reception running. What is saved on the card stays.
void clearFaxPicture()
{
    fax_begin = fax_total;
    fax_saved_to = fax_total;
    fax_hold = false;
    fax_line_count = 0;
    if (fax_buf != nullptr) {
        lv_canvas_fill_bg(fax_canvas, kColBg, LV_OPA_COVER);
    }
    showNotice("Cleared - a new picture starts here");
}

void alignFax()
{
    engine::faxRephase();
    showNotice("Waiting for the phasing signal");
}

void faxKey(char c)
{
    switch (tolower((unsigned char)c)) {
    case '\n':
    case '\r':
        saveFaxNow();
        break;
    case 'a': toggleFaxAuto(); break;
    case 'h': toggleFaxHold(); break;
    case 'c':
    case 'n': clearFaxPicture(); break;
    case 'b': openBand(); break;
    case 'p': openFaxList(); break;
    case 't': openSchedule(); break;
    case 0x08:
        openHome();
        break;
    default:
        break;
    }
}

// --- SSTV screen ---------------------------------------------------------------------------

constexpr int kSstvViewX = (kWidth - engine::kSstvWidth) / 2;
lv_obj_t *scr_sstv;
Header sstv_header;
lv_obj_t *sstv_canvas, *sstv_left, *sstv_right;
lv_draw_buf_t *sstv_buf = nullptr;
uint16_t *sstv_img = nullptr;   // the whole picture, kSstvWidth x kSstvMaxLines, in PSRAM
int sstv_have = 0;              // lines of the current picture received so far
int sstv_top = 0;               // first line shown
bool sstv_follow = true;        // the view moves with the line being received
bool sstv_auto = true;          // a finished picture is saved by itself
bool sstv_unsaved = false;
bool sstv_dirty = true;
int sstv_manual = 0;            // format used when a picture is started by hand
char sstv_format[12] = "";
char sstv_name[40] = "";
bool sstv_ended = false;

void buildSstv()
{
    scr_sstv = makeScreen();
    sstv_header = makeHeader(scr_sstv, false);
    sstv_canvas = lv_canvas_create(scr_sstv);
    sstv_buf = lv_draw_buf_create(engine::kSstvWidth, kFaxHeight, LV_COLOR_FORMAT_RGB565, 0);
    if (sstv_buf != nullptr) {
        lv_canvas_set_draw_buf(sstv_canvas, sstv_buf);
        lv_canvas_fill_bg(sstv_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    }
    lv_obj_set_pos(sstv_canvas, kSstvViewX, kFaxY);
    // The picture is narrower than the screen; the margins tell what is going on
    sstv_left = makeLabel(scr_sstv, 3, kFaxY + 4, kSstvViewX - 6, &lv_font_montserrat_12, kColText);
    lv_label_set_long_mode(sstv_left, LV_LABEL_LONG_WRAP);
    sstv_right = makeLabel(scr_sstv, kSstvViewX + engine::kSstvWidth + 3, kFaxY + 4, kSstvViewX - 6, &lv_font_montserrat_12, kColDim);
    lv_label_set_long_mode(sstv_right, LV_LABEL_LONG_WRAP);
    lv_label_set_text(sstv_right, "Enter:\nsave\n\nwheel:\nscroll\n\npress:\nfunctions");
}

/// Writes the lines received so far as a 24-bit BMP. Returns false if nothing was saved.
bool saveSstvPicture()
{
    if (sstv_img == nullptr || sstv_have < 16) {
        showNotice("No picture to save yet");
        return false;
    }
    time_t secs = (time_t)(timebase::utcMs() / 1000);
    struct tm t;
    gmtime_r(&secs, &t);
    snprintf(sstv_name, sizeof(sstv_name), "SS_%04d%02d%02d_%02d%02d%02d.bmp", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", kSstvDir, sstv_name);
    const uint32_t row_bytes = engine::kSstvWidth * 3;  // a multiple of four, as the format wants
    const uint32_t image_size = row_bytes * (uint32_t)sstv_have;

    instance.lockSPI();
    bool ok = SD.cardType() != CARD_NONE;
    if (ok) {
        SD.mkdir(kSstvDir);
        File f = SD.open(path, FILE_WRITE);
        ok = (bool)f;
        if (ok) {
            uint8_t h[54] = {0};
            h[0] = 'B';
            h[1] = 'M';
            putLe(h + 2, 54 + image_size, 4);
            putLe(h + 10, 54, 4);
            putLe(h + 14, 40, 4);
            putLe(h + 18, engine::kSstvWidth, 4);
            putLe(h + 22, (uint32_t)sstv_have, 4);  // positive: rows from the bottom up, the form every program reads
            putLe(h + 26, 1, 2);
            putLe(h + 28, 24, 2);
            putLe(h + 34, image_size, 4);
            ok = f.write(h, sizeof(h)) == sizeof(h);
            static uint8_t row[engine::kSstvWidth * 3];
            for (int y = sstv_have - 1; y >= 0 && ok; --y) {
                const uint16_t *src = sstv_img + (size_t)y * engine::kSstvWidth;
                for (int x = 0; x < engine::kSstvWidth; ++x) {
                    uint16_t c = src[x];
                    row[3 * x] = (uint8_t)((c & 0x1F) * 255 / 31);
                    row[3 * x + 1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
                    row[3 * x + 2] = (uint8_t)((c >> 11) * 255 / 31);
                }
                ok = f.write(row, sizeof(row)) == sizeof(row);
            }
            f.close();
            if (!ok) {
                SD.remove(path);
            }
        }
    }
    instance.unlockSPI();
    char text[56];
    if (ok) {
        sstv_unsaved = false;
        snprintf(text, sizeof(text), "Saved %d lines as %s", sstv_have, sstv_name);
    } else {
        snprintf(text, sizeof(text), "NOT saved - SD card?");
    }
    showNotice(text);
    return ok;
}

void saveSstvIfUnsaved()
{
    if (sstv_auto && sstv_unsaved && sstv_have >= 40) {
        saveSstvPicture();
    }
}

void clearSstvPicture()
{
    if (sstv_img != nullptr) {
        memset(sstv_img, 0, (size_t)engine::kSstvWidth * engine::kSstvMaxLines * sizeof(uint16_t));
    }
    sstv_have = 0;
    sstv_top = 0;
    sstv_follow = true;
    sstv_unsaved = false;
    sstv_dirty = true;
}

/// Takes picture lines from the receiver, whichever screen is shown.
void processSstv()
{
    switch (engine::nextSstvEvent()) {
    case engine::SstvEvent::Start: {
        saveSstvIfUnsaved();
        clearSstvPicture();
        engine::SstvState state = engine::sstvState();
        strncpy(sstv_format, state.format, sizeof(sstv_format) - 1);
        char text[56];
        snprintf(text, sizeof(text), "A picture begins: %s", sstv_format);
        showNotice(text);
        break;
    }
    case engine::SstvEvent::End:
        sstv_ended = true;  // handled below, once the last lines have been taken
        break;
    default:
        break;
    }

    static engine::SstvLine line;
    while (engine::nextSstvLine(&line)) {
        if (sstv_img == nullptr) {
            sstv_img = (uint16_t *)ps_calloc((size_t)engine::kSstvWidth * engine::kSstvMaxLines, sizeof(uint16_t));
        }
        if (sstv_img == nullptr || line.line < 0 || line.line >= engine::kSstvMaxLines) {
            continue;
        }
        memcpy(sstv_img + (size_t)line.line * engine::kSstvWidth, line.pixels, sizeof(line.pixels));
        sstv_have = max(sstv_have, line.line + 1);
        sstv_unsaved = true;
        sstv_dirty = true;
        if (sstv_follow) {
            sstv_top = max(0, sstv_have - kFaxHeight);
        }
    }

    if (sstv_ended) {
        sstv_ended = false;
        // Lines the receiver did not stand behind are dropped
        int good = engine::sstvState().good_lines;
        if (good < sstv_have) {
            if (sstv_img != nullptr) {
                memset(sstv_img + (size_t)good * engine::kSstvWidth, 0, (size_t)(sstv_have - good) * engine::kSstvWidth * sizeof(uint16_t));
            }
            sstv_have = good;
            sstv_top = max(0, sstv_have - kFaxHeight);
            sstv_dirty = true;
        }
        if (sstv_auto) {
            saveSstvIfUnsaved();
        } else {
            showNotice("Picture complete - Enter saves it");
        }
    }
}

void drawSstv()
{
    engine::Status st = engine::status();
    drawHeader(sstv_header, st);
    engine::SstvState state = engine::sstvState();
    char name[28];
    modeWithFrequency(Mode::SSTV, name, sizeof(name));
    if (state.receiving) {
        lv_label_set_text_fmt(sstv_header.state, "%s %s %d/%d", name, state.format, state.line, state.lines);
    } else {
        lv_label_set_text_fmt(sstv_header.state, "%s waiting for a picture", name);
    }
    lv_obj_set_style_text_color(sstv_header.state, state.receiving ? kColCq : kColText, 0);
    lv_label_set_text_fmt(sstv_left, "%s\n%d lines\n\n%s\n\n%s", sstv_have ? sstv_format : "no picture", sstv_have,
                          sstv_have == 0 ? "" : (sstv_unsaved ? (sstv_auto ? "saved when complete" : "not saved") : "saved"),
                          sstv_auto ? "auto save on" : "auto save off");

    if (sstv_dirty && sstv_buf != nullptr) {
        sstv_dirty = false;
        uint32_t stride = sstv_buf->header.stride;
        for (int row = 0; row < kFaxHeight; ++row) {
            uint16_t *out = (uint16_t *)(sstv_buf->data + stride * row);
            int y = sstv_top + row;
            if (sstv_img != nullptr && y < sstv_have) {
                memcpy(out, sstv_img + (size_t)y * engine::kSstvWidth, engine::kSstvWidth * sizeof(uint16_t));
            } else {
                memset(out, 0, engine::kSstvWidth * sizeof(uint16_t));
            }
        }
        lv_obj_invalidate(sstv_canvas);
    }
}

void sstvRotate(int dir)
{
    int most = max(0, sstv_have - kFaxHeight);
    sstv_top = constrain(sstv_top + dir * 30, 0, most);
    sstv_follow = sstv_top == most;
    sstv_dirty = true;
}

void saveSstvNow()
{
    showNotice("Saving the picture ...");
    drawNoticeBar();
    lv_timer_handler();
    saveSstvPicture();
}

void toggleSstvAuto()
{
    sstv_auto = !sstv_auto;
    showNotice(sstv_auto ? "A picture is saved when it is complete" : "Pictures are only saved with Enter");
}

void nextSstvFormat()
{
    sstv_manual = (sstv_manual + 1) % engine::kSstvFormats;
}

/// For a picture whose beginning was missed: the format has to be told, the receiver finds the lines itself.
void startSstvByHand()
{
    saveSstvIfUnsaved();
    clearSstvPicture();
    engine::sstvStart(sstv_manual);
    char text[56];
    snprintf(text, sizeof(text), "Waiting for a line of %s", engine::sstvFormatName(sstv_manual));
    showNotice(text);
}

void stopSstv()
{
    engine::sstvStop();
    showNotice("Stopped");
}

void clearSstvNow()
{
    engine::sstvStop();
    clearSstvPicture();
    showNotice("Cleared");
}

void sstvKey(char c)
{
    switch (tolower((unsigned char)c)) {
    case '\n':
    case '\r': saveSstvNow(); break;
    case 'a': toggleSstvAuto(); break;
    case 'b': openBand(); break;
    case 'c': clearSstvNow(); break;
    case 'm': startSstvByHand(); break;
    case 'p': openFaxList(); break;
    case 'x': stopSstv(); break;
    case 0x08: openHome(); break;
    default: break;
    }
}

// --- power saving ------------------------------------------------------------------------

uint32_t last_input = 0;    // millis() of the last key or wheel event
uint32_t last_busy = 0;     // ... of the last reception or transmission
bool display_off = false;

void setDisplayOff(bool off)
{
    if (off == display_off) {
        return;
    }
    display_off = off;
    instance.setBrightness(off ? 0 : DEVICE_MAX_BRIGHTNESS_LEVEL);
}

/// Ends everything in an orderly way and puts the device into deep sleep. It
/// restarts when the wheel or the BOOT button is pressed.
void powerDown(const char *reason)
{
    setDisplayOff(false);
    lv_obj_t *scr = makeScreen();
    lv_obj_t *label = makeLabel(scr, 0, 84, kWidth, &lv_font_montserrat_14, kColText);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(label, "%s\n\nPress the wheel to switch on again", reason);
    lv_screen_load(scr);
    lv_timer_handler();
    Serial.printf("[dw] %s\n", reason);

    engine::stop();
    saveFaxIfUnsaved();
    saveSstvIfUnsaved();
    saveSstvIfUnsaved();
    web::shutdown();
    settings.save();
    delay(1200);
    instance.sleep((WakeupSource_t)(WAKEUP_SRC_ROTARY_BUTTON | WAKEUP_SRC_BOOT_BUTTON));
}

/// Display off after the idle time without input; deep sleep once nothing has
/// been received or sent for that time either.
void pollPower(const engine::Status &st, bool received)
{
    uint32_t now = millis();
    // WEFAX and NAVTEX are listened to for a long time without touching the
    // device, and a picture arriving is not "received" in the sense of a
    // decode: these modes keep the device awake, only the display goes off.
    bool listening = st.mode == Mode::WEFAX || st.mode == Mode::NAVTEX || st.mode == Mode::SSTV;
    bool busy = received || listening || st.transmitting || web::inUse() ||
                (engine::isSlotMode(st.mode) && strcmp(st.qso_state, "Idle") != 0);
    if (busy) {
        last_busy = now;
    }
    if (settings.sleep_min <= 0) {
        setDisplayOff(false);
        return;
    }
    uint32_t limit = (uint32_t)settings.sleep_min * 60000u;
    if (now - last_input < limit) {
        return;
    }
    setDisplayOff(true);
    if (now - last_busy >= limit) {
        powerDown("Sleeping, nothing received");
    }
}

// --- setup screen ------------------------------------------------------------------------

enum class Kind { Text, Number, Toggle, Action, Header };  // Header: a section title, not selectable

bool audio_board_at_start = false;  // the setting the audio path was chosen with
enum class Chars { Call, Digits, Any };

struct Field {
    const char *name;
    Kind kind;
    char *text;       // Kind::Text
    size_t text_size;
    Chars chars;
    int *number;      // Kind::Number
    int min, max, step;
    const char *unit;
    bool *toggle;     // Kind::Toggle
    const char *on, *off;
};

Field fields[] = {
    {"STATION", Kind::Header, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Callsign", Kind::Text, settings.call, sizeof(settings.call), Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Locator from", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.grid_auto, "GPS", "manual entry"},
    {"Locator", Kind::Text, settings.grid, 5, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"FT8/FT4 operation", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.cb_mode, "CB (11 m)", "amateur radio"},
    {"AUDIO AND RADIO", Kind::Header, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Audio (restarts)", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.audio_board, "interface board", "built-in"},
    {"Input gain", Kind::Number, nullptr, 0, Chars::Call, &settings.mic_gain_db, 0, 42, 3, " dB", nullptr, "", ""},
    {"Volume", Kind::Number, nullptr, 0, Chars::Call, &settings.volume, 0, 100, 1, " %", nullptr, "", ""},
    {"TX level", Kind::Number, nullptr, 0, Chars::Call, &settings.tx_level, 5, 100, 5, " %", nullptr, "", ""},
    {"PTT output", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.ptt_active_high, "active high", "active low"},
    {"FT8 / FT4", Kind::Header, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"FT8/FT4 TX frequency", Kind::Number, nullptr, 0, Chars::Call, &settings.tx_hz, 300, 2800, 50, " Hz", nullptr, "", ""},
    {"FT8/FT4 max repeats", Kind::Number, nullptr, 0, Chars::Call, &settings.max_retries, 1, 20, 1, "", nullptr, "", ""},
    {"Voice announcements", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.voice, "on", "off"},
    {"Voice volume", Kind::Number, nullptr, 0, Chars::Call, &settings.voice_volume, 0, 100, 5, " %", nullptr, "", ""},
    {"CW", Kind::Header, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"CW sending speed", Kind::Number, nullptr, 0, Chars::Call, &settings.cw_wpm, 5, 40, 1, " WPM", nullptr, "", ""},
    {"CW tone (RX window, TX)", Kind::Number, nullptr, 0, Chars::Call, &settings.cw_tone, 400, 1200, 10, " Hz", nullptr, "", ""},
    {"RTTY", Kind::Header, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"RTTY speed", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.rtty_50_baud, "50 baud", "45.45 baud"},
    {"RTTY shift", Kind::Number, nullptr, 0, Chars::Call, &settings.rtty_shift, 85, 850, 5, " Hz", nullptr, "", ""},
    {"DEVICE", Kind::Header, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Clock offset", Kind::Number, nullptr, 0, Chars::Call, &settings.time_offset_ds, -75, 75, 1, " x0.1 s", nullptr, "", ""},
    {"Wi-Fi access point", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.wifi_on, "on", "off"},
    {"Wi-Fi password", Kind::Text, settings.wifi_pass, sizeof(settings.wifi_pass), Chars::Any, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Home Wi-Fi", Kind::Action, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Sleep when idle after", Kind::Number, nullptr, 0, Chars::Call, &settings.sleep_min, 0, 30, 1, " min (0 = never)", nullptr, "", ""},
    {"About DIGIWAVE", Kind::Action, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Switch off", Kind::Action, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
};
constexpr int kFields = sizeof(fields) / sizeof(fields[0]);
constexpr int kSetRows = 9;

lv_obj_t *scr_setup;
lv_obj_t *set_bg[kSetRows], *set_name[kSetRows], *set_value[kSetRows], *set_footer;
int set_selected = 0;
int set_top = 0;
bool editing = false;

void buildSetup()
{
    scr_setup = makeScreen();
    lv_obj_t *hint = makeLabel(scr_setup, 4, 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    lv_label_set_text(hint, "wheel: select  press: edit  Space + key: digits, symbols  Backspace: back");
    for (int i = 0; i < kSetRows; ++i) {
        int y = 20 + i * 20;
        set_bg[i] = makeBox(scr_setup, 0, y, kWidth, 20, kColBg);
        set_name[i] = makeLabel(scr_setup, 8, y + 2, 200, &lv_font_montserrat_14, kColText);
        set_value[i] = makeLabel(scr_setup, 220, y + 2, 250, &lv_font_montserrat_14, kColText);
    }
    set_footer = makeLabel(scr_setup, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
}

void drawSetup()
{
    if (set_selected < set_top) {
        set_top = set_selected;
    }
    // The title of the section stays in view above its first entry
    if (set_selected > 0 && fields[set_selected - 1].kind == Kind::Header && set_selected - 1 < set_top) {
        set_top = set_selected - 1;
    }
    if (set_selected >= set_top + kSetRows) {
        set_top = set_selected - kSetRows + 1;
    }
    for (int i = 0; i < kSetRows; ++i) {
        int idx = set_top + i;
        const Field &f = fields[idx];
        bool active = idx == set_selected;
        lv_label_set_text(set_name[i], f.name);
        lv_obj_set_style_text_color(set_name[i], f.kind == Kind::Header ? kColCq : kColText, 0);
        switch (f.kind) {
        case Kind::Header:
            lv_label_set_text(set_value[i], "");
            break;
        case Kind::Text:
            lv_label_set_text_fmt(set_value[i], "%s%s%s", f.text, (active && editing) ? "_" : "", f.unit);
            break;
        case Kind::Number:
            lv_label_set_text_fmt(set_value[i], "%d%s", *f.number, f.unit);
            break;
        case Kind::Toggle:
            if (f.toggle == &settings.audio_board && settings.audio_board && !audio_on_board && audio_board_at_start) {
                lv_label_set_text(set_value[i], "interface board - not found");
            } else {
                lv_label_set_text(set_value[i], *f.toggle ? f.on : f.off);
            }
            break;
        case Kind::Action:
            if (strcmp(f.name, "Home Wi-Fi") == 0) {
                lv_label_set_text(set_value[i], settings.home_ssid[0] ? settings.home_ssid : "set up with the phone");
            } else {
                lv_label_set_text(set_value[i], "press the wheel");
            }
            break;
        }
        lv_obj_set_style_bg_color(set_bg[i], active ? (editing ? kColEdit : kColSel) : kColBg, 0);
    }
    int number = 0, total = 0;
    for (int i = 0; i < kFields; ++i) {
        if (fields[i].kind != Kind::Header) {
            ++total;
            if (i <= set_selected) {
                ++number;
            }
        }
    }
    lv_label_set_text_fmt(set_footer, "%d of %d", number, total);
}

// --- licence and credits ---------------------------------------------------------------

// What the firmware is, what it may be used for, and whose work is in it.
// THIRD-PARTY-NOTICES.md in the source has the same with versions.
const char kAboutText[] =
    "DIGIWAVE - digital radio modes on the LilyGo T-LoRa Pager\n"
    "Copyright (C) 2026 Andres Leroi\n"
    "github.com/MrNash23/DIGIWAVE\n\n"
    "Free software under the GNU General Public License, version 3 or later. You may use, "
    "study, share and change it; whoever passes it on has to pass on these freedoms and "
    "the source. There is NO WARRANTY, to the extent permitted by law.\n\n"
    "It builds on the work of others:\n"
    "FT8 and FT4 - protocols by Joe Taylor K1JT and Steve Franke K9AN\n"
    "ft8_lib - Karlis Goba (MIT)\n"
    "11 m division list - from WSJT-CB, derived from WSJT-X (GPL 3)\n"
    "Voice announcements - spoken by Piper (GPL 3) with the voice ljspeech, from the "
    "LJ Speech dataset (public domain)\n"
    "LilyGoLib - Shenzhen Xinyuan Electronic Technology (MIT)\n"
    "LVGL - LVGL Kft (MIT)\n"
    "Montserrat typeface - The Montserrat Project Authors (SIL Open Font License 1.1)\n"
    "RadioLib - Jan Gromes (MIT)\n"
    "SensorLib, XPowersLib - Lewis He (MIT); sensor interface by Bosch Sensortec (BSD)\n"
    "TinyGPSPlus - Mikal Hart (LGPL 2.1)\n"
    "Adafruit TCA8418, BusIO - Adafruit Industries (BSD, MIT)\n"
    "Arduino core for ESP32 - Espressif Systems and contributors (LGPL 2.1)\n"
    "ESP-IDF with FreeRTOS, lwIP, mbedTLS, NimBLE - Espressif Systems and the authors of "
    "these components (Apache 2.0, MIT, BSD)\n\n"
    "The source, with which every part can be replaced, is at the address above.";

lv_obj_t *scr_about, *about_box;

void buildAbout()
{
    scr_about = makeScreen();
    lv_obj_t *title = makeLabel(scr_about, 4, 2, kWidth - 8, &lv_font_montserrat_14, kColCq);
    lv_label_set_text(title, "About DIGIWAVE");
    // The text is longer than the screen and scrolls with the wheel
    about_box = makeBox(scr_about, 0, 22, kWidth, 178, kColBg);
    lv_obj_add_flag(about_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(about_box, LV_DIR_VER);
    lv_obj_t *text = lv_label_create(about_box);
    lv_obj_set_pos(text, 8, 0);
    lv_obj_set_width(text, kWidth - 24);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(text, kColText, 0);
    lv_label_set_text_static(text, kAboutText);
    lv_obj_t *hint = makeLabel(scr_about, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    lv_label_set_text(hint, "wheel: scroll   Backspace: back");
}

void openAbout()
{
    screen = Screen::About;
    lv_obj_scroll_to_y(about_box, 0, LV_ANIM_OFF);
    lv_screen_load(scr_about);
}

void aboutRotate(int dir)
{
    lv_obj_update_layout(about_box);
    int32_t most = max((int32_t)0, lv_obj_get_height(lv_obj_get_child(about_box, 0)) - lv_obj_get_height(about_box));
    lv_obj_scroll_to_y(about_box, constrain(lv_obj_get_scroll_y(about_box) + dir * 40, (int32_t)0, most), LV_ANIM_OFF);
}

void openSetup()
{
    sub_from_app = inApp();
    set_selected = 1;  // the first entry below the first section title
    set_top = 0;
    editing = false;
    screen = Screen::Setup;
    drawSetup();
    lv_screen_load(scr_setup);
}

void closeSetup()
{
    settings.save();
    if (settings.audio_board != audio_board_at_start) {
        // The audio path is chosen at the start
        lv_obj_t *scr = makeScreen();
        lv_obj_t *label = makeLabel(scr, 0, 100, kWidth, &lv_font_montserrat_14, kColText);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(label, "Restarting with the new audio path");
        lv_screen_load(scr);
        lv_timer_handler();
        engine::stop();
        saveFaxIfUnsaved();
        saveSstvIfUnsaved();
        web::shutdown();
        delay(800);
        esp_restart();
    }
    engine::applySettings();
    extensionPortPtt(settings.ptt_active_high).set(false);
    web::apply();
    backFromSub();
}

void setupRotate(int dir)
{
    Field &f = fields[set_selected];
    if (!editing) {
        // Section titles are passed over
        int next = set_selected + dir;
        while (next >= 0 && next < kFields && fields[next].kind == Kind::Header) {
            next += dir;
        }
        if (next >= 0 && next < kFields) {
            set_selected = next;
        }
    } else if (f.kind == Kind::Number) {
        // Turning up, which moves a list up, raises the number
        *f.number = constrain(*f.number - dir * f.step, f.min, f.max);
    } else if (f.kind == Kind::Toggle) {
        *f.toggle = !*f.toggle;
    }
    drawSetup();
}

void setupPress()
{
    if (fields[set_selected].kind == Kind::Action) {
        if (strncmp(fields[set_selected].name, "About", 5) == 0) {
            openAbout();
        } else if (strcmp(fields[set_selected].name, "Home Wi-Fi") == 0) {
            openHomeWifi();
        } else {
            powerDown("Switched off");
        }
        return;
    }
    editing = !editing;
    drawSetup();
}

void setupKey(char c)
{
    Field &f = fields[set_selected];
    if (c == 0x08) {
        if (!editing) {
            closeSetup();
            return;
        }
        if (f.kind == Kind::Text) {
            size_t n = strlen(f.text);
            if (n > 0) {
                f.text[n - 1] = '\0';
            }
        }
    } else if (c == '\n' || c == '\r') {
        setupPress();
        return;
    } else if (editing && f.kind == Kind::Text) {
        bool ok = false;
        switch (f.chars) {
        case Chars::Call:
            c = (char)toupper((unsigned char)c);
            ok = isalnum((unsigned char)c) || c == '/';
            break;
        case Chars::Digits:
            ok = isdigit((unsigned char)c);
            break;
        case Chars::Any:
            ok = isgraph((unsigned char)c);
            break;
        }
        size_t n = strlen(f.text);
        if (ok && n + 1 < f.text_size) {
            f.text[n] = c;
            f.text[n + 1] = '\0';
        }
    }
    drawSetup();
}

// --- home screen -----------------------------------------------------------------------

// The artwork of each tile carries its frame and its name
const lv_image_dsc_t *const kTileIcons[] = {
    &img_tile_ft8,    &img_tile_ft4,   &img_tile_cw,      &img_tile_rtty,  &img_tile_navtex,
    &img_tile_wefax,  &img_tile_sstv,  &img_tile_logbook, &img_tile_setup, &img_tile_wifi,
};
constexpr int kTiles = sizeof(kTileIcons) / sizeof(kTileIcons[0]);
constexpr int kTileLog = engine::kModes, kTileSetup = kTileLog + 1, kTileWifi = kTileLog + 2;
constexpr int kTileSize = 86;  // TILE in tools/make_assets.py

lv_obj_t *scr_home;
lv_obj_t *tile_tint;
lv_obj_t *home_time, *home_info, *home_gps, *home_wifi, *home_sd, *home_battery;
int tile_selected = 0;

void buildHome()
{
    scr_home = makeScreen();
    lv_obj_t *mark = lv_image_create(scr_home);
    lv_image_set_src(mark, &img_wordmark);
    lv_obj_set_pos(mark, 6, 3);
    home_info = makeLabel(scr_home, 174, 10, 136, &lv_font_montserrat_12, kColDim);
    home_time = makeLabel(scr_home, 312, 9, 70, &lv_font_montserrat_14, kColText);
    home_gps = makeIcon(scr_home, 386, 9, &img_status_gps_no_fix);
    home_wifi = makeIcon(scr_home, 405, 9, &img_status_wifi_on);
    home_sd = makeIcon(scr_home, 424, 9, &img_status_sd_missing);
    // The charge as a number, as in the modes
    home_battery = makeLabel(scr_home, 444, 10, 34, &lv_font_montserrat_12, kColText);
    lv_obj_set_style_text_align(home_battery, LV_TEXT_ALIGN_RIGHT, 0);

    const int cols = 5, pitch_x = 94, pitch_y = 92;
    for (int i = 0; i < kTiles; ++i) {
        int x = 9 + (i % cols) * pitch_x;
        int y = 38 + (i / cols) * pitch_y;
        lv_obj_t *icon = lv_image_create(scr_home);
        lv_image_set_src(icon, kTileIcons[i]);
        lv_obj_set_pos(icon, x, y);
    }
    // The selected tile is coloured: a veil in the magenta of the artwork lies over it
    tile_tint = makeBox(scr_home, 0, 0, kTileSize, kTileSize, kColToMe);
    lv_obj_set_style_bg_opa(tile_tint, LV_OPA_40, 0);
    lv_obj_set_style_radius(tile_tint, 5, 0);
}

void drawHome()
{
    static uint32_t last_slow = 0;
    static bool card = false;
    if (last_slow == 0 || millis() - last_slow > 3000) {
        last_slow = millis();
        card = instance.io.digitalRead(EXPANDS_SD_DET) == LOW;  // the card pulls the detect line low
    }


    const int cols = 5, pitch_x = 94, pitch_y = 92;
    lv_obj_set_pos(tile_tint, 9 + (tile_selected % cols) * pitch_x, 38 + (tile_selected / cols) * pitch_y);

    uint32_t secs = (uint32_t)(timebase::utcMs() / 1000);
    lv_label_set_text_fmt(home_time, "%02u:%02u:%02u", (unsigned)(secs / 3600 % 24), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
    drawClockIcon(home_gps);
    lv_obj_set_style_image_recolor(home_wifi, web::running() ? kColCq : kColFrame, 0);
    setIcon(home_sd, card ? &img_status_sd_present : &img_status_sd_missing, card ? kColText : kColWarn);
    drawBattery(home_battery);

    if (noticeActive()) {
        lv_label_set_text(home_info, notice);
    } else if (web::running()) {
        if (web::homeAddress()[0] != '\0') {
            lv_label_set_text_fmt(home_info, "%s  %s  home %s", settings.wifi_pass, web::address() + 7, web::homeAddress());
        } else {
            lv_label_set_text_fmt(home_info, "%s  %s", settings.wifi_pass, web::address() + 7);
        }
    } else if (settings.call[0] == '\0') {
        lv_label_set_text(home_info, "Enter callsign in Setup");
    } else {
        lv_label_set_text_fmt(home_info, "%s  %s", settings.call, settings.grid);
    }
}

void openHome()
{
    screen = Screen::Home;
    drawHome();
    lv_screen_load(scr_home);
}

void openMode(Mode m)
{
    if (engine::mode() != m) {
        if (engine::mode() == Mode::WEFAX) {
            saveFaxIfUnsaved();
            fax_total = fax_begin = fax_saved_to = 0;
            fax_hold = false;
        }
        if (engine::mode() == Mode::SSTV) {
            saveSstvIfUnsaved();
            clearSstvPicture();
        }
        engine::setMode(m);
        settings.last_mode = (int)m;
        settings.save();
        rx_len = 0;
        rx_text[0] = '\0';
        rx_dirty = true;
        input[0] = '\0';
        follow = true;
        selected = -1;
        list_view = ListView::All;
        view_by_hand = false;
        contact_active = false;
    }
    tile_selected = (int)m;
    if (engine::isSlotMode(m)) {
        screen = Screen::Slot;
        lv_screen_load(scr_slot);
    } else if (engine::isTextMode(m)) {
        screen = Screen::Text;
        rx_dirty = true;
        lv_screen_load(scr_text);
        if (!noticeActive()) {
            showNotice("Press the wheel for functions");
        }
    } else if (m == Mode::SSTV) {
        screen = Screen::Sstv;
        sstv_dirty = true;
        lv_screen_load(scr_sstv);
        if (!noticeActive()) {
            showNotice("Pictures start by themselves   wheel press: functions");
        }
    } else {
        screen = Screen::Fax;
        lv_screen_load(scr_fax);
        if (!noticeActive()) {
            showNotice("Enter: save  H: pause  C: clear  press: functions");
        }
    }
}

void homeRotate(int dir)
{
    tile_selected = constrain(tile_selected + dir, 0, kTiles - 1);
    drawHome();
}

void homePress()
{
    if (tile_selected < engine::kModes) {
        openMode((Mode)tile_selected);
    } else if (tile_selected == kTileLog) {
        openLog();
    } else if (tile_selected == kTileSetup) {
        openSetup();
    } else {
        settings.wifi_on = !settings.wifi_on;
        settings.save();
        web::apply();
        drawHome();
    }
}

// --- logbook screen --------------------------------------------------------------------

constexpr int kLogRows = 9;
lv_obj_t *scr_log;
lv_obj_t *log_title, *log_row[kLogRows], *log_hint;
lv_obj_t *log_qr_box, *log_qr, *log_qr_text;
int log_qr_page = 0;  // 0: hidden, 1: joining the Wi-Fi, 2: downloading the file
int log_top = 0;
bool log_all = false;  // with the QSOs of the station at home, which a logging program puts on the card

void buildLog()
{
    scr_log = makeScreen();
    log_title = makeLabel(scr_log, 4, 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    for (int i = 0; i < kLogRows; ++i) {
        log_row[i] = makeLabel(scr_log, 8, 22 + i * 20, kWidth - 16, &lv_font_montserrat_14, kColText);
    }
    log_hint = makeLabel(scr_log, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);

    // A phone reads the codes: the first joins the access point, the second fetches the file
    log_qr_box = makeBox(scr_log, 0, 0, kWidth, kFooterY, kColBg);
    log_qr = lv_qrcode_create(log_qr_box);
    lv_qrcode_set_size(log_qr, 176);
    lv_qrcode_set_dark_color(log_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(log_qr, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_color(log_qr, lv_color_hex(0xFFFFFF), 0);  // the quiet zone around the code
    lv_obj_set_style_border_width(log_qr, 6, 0);
    lv_obj_set_pos(log_qr, 12, 8);
    log_qr_text = makeLabel(log_qr_box, 214, 12, kWidth - 222, &lv_font_montserrat_14, kColText);
    lv_label_set_long_mode(log_qr_text, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(log_qr_box, LV_OBJ_FLAG_HIDDEN);
}

/// Text for a Wi-Fi QR code; the characters with a meaning in it are escaped.
void appendQrEscaped(char *out, size_t size, const char *text)
{
    size_t n = strlen(out);
    for (; *text && n + 2 < size; ++text) {
        if (strchr("\\;,:\"", *text)) {
            out[n++] = '\\';
        }
        out[n++] = *text;
    }
    out[n] = '\0';
}

void drawLogQr()
{
    if (log_qr_page == 0) {
        lv_obj_add_flag(log_qr_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char data[96];
    if (log_qr_page == 1) {
        snprintf(data, sizeof(data), "WIFI:T:WPA;S:");
        appendQrEscaped(data, sizeof(data), web::kSsid);
        strncat(data, ";P:", sizeof(data) - strlen(data) - 1);
        appendQrEscaped(data, sizeof(data), web::password());
        strncat(data, ";;", sizeof(data) - strlen(data) - 1);
        lv_label_set_text_fmt(log_qr_text, "1  Join the Wi-Fi\n\nScan with the phone camera.\n\nNetwork: %s\nPassword: %s\n\nQ: next code", web::kSsid,
                              web::password());
    } else {
        snprintf(data, sizeof(data), "%s/log.adi", web::address());
        lv_label_set_text_fmt(log_qr_text, "2  Download the logbook\n\nScan once the phone is in the Wi-Fi.\n\n%s\n\nQ or Backspace: close", data);
    }
    lv_qrcode_update(log_qr, data, strlen(data));
    lv_obj_remove_flag(log_qr_box, LV_OBJ_FLAG_HIDDEN);
}

// --- home Wi-Fi -------------------------------------------------------------------------

// The home network is set up from the phone, without typing on the pager: the
// first code joins the access point, the second opens the page that lists the
// networks in reach, where the password is pasted
lv_obj_t *scr_homewifi, *hw_qr, *hw_text, *hw_state;
int hw_page = 0;  // 0: join the access point, 1: open the setup page

void buildHomeWifi()
{
    scr_homewifi = makeScreen();
    hw_qr = lv_qrcode_create(scr_homewifi);
    lv_qrcode_set_size(hw_qr, 176);
    lv_qrcode_set_dark_color(hw_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(hw_qr, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_color(hw_qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(hw_qr, 6, 0);
    lv_obj_set_pos(hw_qr, 12, 8);
    hw_text = makeLabel(scr_homewifi, 214, 8, kWidth - 222, &lv_font_montserrat_14, kColText);
    lv_label_set_long_mode(hw_text, LV_LABEL_LONG_WRAP);
    hw_state = makeLabel(scr_homewifi, 214, 140, kWidth - 222, &lv_font_montserrat_14, kColText);
    lv_label_set_long_mode(hw_state, LV_LABEL_LONG_WRAP);
    lv_obj_t *hint = makeLabel(scr_homewifi, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    lv_label_set_text(hint, "Q or press: other code   Backspace: back");
}

void drawHomeWifiQr()
{
    char data[96];
    if (hw_page == 0) {
        snprintf(data, sizeof(data), "WIFI:T:WPA;S:");
        appendQrEscaped(data, sizeof(data), web::kSsid);
        strncat(data, ";P:", sizeof(data) - strlen(data) - 1);
        appendQrEscaped(data, sizeof(data), web::password());
        strncat(data, ";;", sizeof(data) - strlen(data) - 1);
        lv_label_set_text(hw_text, "HOME WI-FI\n1  Scan with the phone camera\n    to join the pager.\n\nThen Q for the second code.");
    } else {
        snprintf(data, sizeof(data), "%s/#home", web::address());
        lv_label_set_text(hw_text, "HOME WI-FI\n2  Scan to open the setup page.\n    Choose the network there and\n    paste its password.");
    }
    lv_qrcode_update(hw_qr, data, strlen(data));
}

/// The state of the home network, redrawn while the screen is open.
void drawHomeWifi()
{
    const char *st = web::homeState();
    lv_color_t col = kColText;
    if (!web::running()) {
        lv_label_set_text(hw_state, "Wi-Fi is off");
        col = kColWarn;
    } else if (st[0] == '\0') {
        lv_label_set_text(hw_state, "No home network yet");
    } else if (strcmp(st, "joined") == 0) {
        lv_label_set_text_fmt(hw_state, "Joined %s\n%s  digiwave.local", settings.home_ssid, web::homeAddress());
        col = kColCq;
    } else if (strcmp(st, "joining") == 0) {
        lv_label_set_text_fmt(hw_state, "Joining %s ...", settings.home_ssid);
    } else if (strcmp(st, "notfound") == 0) {
        lv_label_set_text_fmt(hw_state, "%s not in reach", settings.home_ssid);
        col = kColWarn;
    } else {
        lv_label_set_text_fmt(hw_state, "Could not join %s - password wrong?", settings.home_ssid);
        col = kColWarn;
    }
    lv_obj_set_style_text_color(hw_state, col, 0);
}

void openHomeWifi()
{
    // The codes are of no use without the access point
    if (!web::running()) {
        settings.wifi_on = true;
        settings.save();
        web::apply();
        if (!web::running()) {
            showNotice("Wi-Fi did not start");
            return;
        }
    }
    hw_page = 0;
    screen = Screen::HomeWifi;
    drawHomeWifiQr();
    drawHomeWifi();
    lv_screen_load(scr_homewifi);
}

void closeHomeWifi()
{
    screen = Screen::Setup;
    drawSetup();
    lv_screen_load(scr_setup);
}

void homeWifiKey(char c)
{
    if (c == 0x08) {
        closeHomeWifi();
    } else if (c == 'q' || c == 'Q' || c == '\n' || c == '\r') {
        hw_page = 1 - hw_page;
        drawHomeWifiQr();
    }
}

void drawLog();

/// Switches the access point on or off from the logbook, as in the setup.
void setLogWifi(bool on)
{
    settings.wifi_on = on;
    settings.save();
    web::apply();
    if (on && !web::running()) {
        showNotice("Wi-Fi did not start");
    }
}

void logKey(char c)
{
    if (c == 0x08) {
        if (log_qr_page != 0) {
            log_qr_page = 0;
            drawLogQr();
        } else {
            backFromSub();
        }
        return;
    }
    switch (c) {
    case 'w':
    case 'W':
        log_qr_page = 0;
        drawLogQr();
        setLogWifi(!web::running());
        break;
    case 'a':
    case 'A':
        log_all = !log_all;
        log_top = 0;
        break;
    case 'q':
    case 'Q':
        if (log_qr_page == 0 && !web::running()) {
            setLogWifi(true);  // the codes are of no use without the access point
        }
        if (!web::running()) {
            return;
        }
        log_qr_page = (log_qr_page + 1) % 3;
        drawLogQr();
        break;
    default:
        return;
    }
    drawLog();
}

bool log_stats = false;  // the page with the figures instead of the list

void drawLogStats()
{
    time_t secs = (time_t)(timebase::utcMs() / 1000);
    struct tm t;
    gmtime_r(&secs, &t);
    char today[9];
    snprintf(today, sizeof(today), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    const logbook::Stats &st = logbook::stats(today);
    lv_label_set_text(log_title, "Logbook  -  figures over all QSOs of this device      press: list");
    char line[kLogRows][96];
    for (auto &l : line) {
        l[0] = '\0';
    }
    snprintf(line[0], sizeof(line[0]), "QSOs: %d      today (UTC): %d", st.total, st.today);
    if (st.total > 0) {
        snprintf(line[1], sizeof(line[1]), "From %.4s-%.2s-%.2s to %.4s-%.2s-%.2s", st.first_date, st.first_date + 4, st.first_date + 6,
                 st.last_date, st.last_date + 4, st.last_date + 6);
        snprintf(line[2], sizeof(line[2]), "FT8: %d      FT4: %d", st.ft8, st.ft4);
        snprintf(line[3], sizeof(line[3]), "Countries: %d", st.countries);
        if (st.with_distance > 0) {
            snprintf(line[4], sizeof(line[4]), "Farthest: %s, %d km      average: %ld km", st.km_max_call, st.km_max, st.km_sum / st.with_distance);
        }
        // The bands, as many as fit into two lines
        int n = 0;
        for (int i = 0; i < st.bands; ++i) {
            char *out = line[6 + min(i / 5, 2)];  // five bands to a line
            size_t used = strlen(out);
            snprintf(out + used, sizeof(line[0]) - used, "%s%s: %d", used ? "    " : "", st.band[i].name, st.band[i].count);
            ++n;
        }
        if (n > 0) {
            snprintf(line[5], sizeof(line[5]), "By band:");
        }
    }
    for (int i = 0; i < kLogRows; ++i) {
        lv_label_set_text(log_row[i], line[i]);
    }
}

void drawLogHint()
{
    if (web::running()) {
        lv_label_set_text_fmt(log_hint, "A: with station  Q: QR code  W: Wi-Fi off  Backspace: back  |  %s/log.adi", web::address() + 7);
        lv_obj_set_style_text_color(log_hint, kColCq, 0);
    } else {
        lv_label_set_text(log_hint, "wheel: scroll  press: figures  A: with station  W: Wi-Fi  Q: QR code  Backspace: back");
        lv_obj_set_style_text_color(log_hint, kColDim, 0);
    }
}

void drawLog()
{
    drawLogHint();
    if (log_stats) {
        drawLogStats();
        return;
    }
    int count = log_all ? logbook::countAll() : logbook::count();
    log_top = constrain(log_top, 0, max(0, count - kLogRows));
    if (count == 0) {
        lv_label_set_text(log_title, log_all ? "Logbook with station  -  no QSOs yet" : "Logbook  -  no QSOs yet      A: with station");
    } else if (log_all) {
        lv_label_set_text_fmt(log_title, "With station  -  %d here, %d at home (grey)      A: this device", logbook::total(),
                              logbook::stationTotal());
    } else {
        lv_label_set_text_fmt(log_title, "Logbook  -  %d QSOs, newest first      press: figures", logbook::total());
    }
    for (int i = 0; i < kLogRows; ++i) {
        int idx = count - 1 - (log_top + i);  // newest on top
        if (idx < 0) {
            lv_label_set_text(log_row[i], "");
            continue;
        }
        const logbook::Row &r = log_all ? logbook::rowAll(idx) : logbook::row(idx);
        lv_obj_set_style_text_color(log_row[i], r.station ? kColDim : kColText, 0);
        char km[12] = "";
        if (r.km >= 0) {
            snprintf(km, sizeof(km), "  %dkm", r.km);
        }
        // Kept short so that a row fits: two-digit year, no spaces around the reports
        lv_label_set_text_fmt(log_row[i], "%.2s-%.2s-%.2s %.2s:%.2s  %s  %s  %s/%s  %s %s%s", r.date + 2, r.date + 4, r.date + 6,
                              r.time, r.time + 2, r.call, r.grid[0] ? r.grid : "----", r.sent, r.rcvd, r.band, r.mode, km);
    }
}

void openLog()
{
    sub_from_app = inApp();
    log_stats = false;
    log_top = 0;
    log_qr_page = 0;
    drawLogQr();
    screen = Screen::Log;
    drawLog();
    lv_screen_load(scr_log);
}

// --- broadcast schedule ---------------------------------------------------------------------

// The broadcasts of the weather station of the mode in use, by their time, with
// the next one marked. The plans are built in or come from the SD card, where
// the phone page saves them.
constexpr int kSchedRows = 9;
lv_obj_t *scr_sched, *sched_title, *sched_bg[kSchedRows], *sched_row[kSchedRows], *sched_footer;
schedule::Plan sched_plan = schedule::Plan::Wefax;
int sched_top = 0;
bool sched_follow = true;  // the list keeps the next broadcast in view until the wheel is turned

int utcMinuteOfDay()
{
    return (int)((timebase::utcMs() / 60000) % 1440);
}

/// The plan for a mode; false for modes without one.
bool planFor(Mode m, schedule::Plan *p)
{
    if (m == Mode::WEFAX) {
        *p = schedule::Plan::Wefax;
        return true;
    }
    if (m == Mode::RTTY) {
        *p = schedule::Plan::Rtty;
        return true;
    }
    return false;
}

void buildSchedule()
{
    scr_sched = makeScreen();
    sched_title = makeLabel(scr_sched, 4, 2, kWidth - 8, &lv_font_montserrat_14, kColCq);
    for (int i = 0; i < kSchedRows; ++i) {
        int y = 22 + i * 20;
        sched_bg[i] = makeBox(scr_sched, 0, y, kWidth, 20, kColBg);
        sched_row[i] = makeLabel(scr_sched, 8, y + 2, kWidth - 16, &lv_font_montserrat_14, kColText);
    }
    sched_footer = makeLabel(scr_sched, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
}

/// "in 7 min" or "in 2 h 05".
void formatWait(int minutes, char *out, size_t size)
{
    if (minutes < 60) {
        snprintf(out, size, "in %d min", minutes);
    } else {
        snprintf(out, size, "in %d h %02d", minutes / 60, minutes % 60);
    }
}

/// The next broadcast for the menu, for example "21:00 in 55 min".
void nextBroadcastText(schedule::Plan p, char *out, size_t size)
{
    int now = utcMinuteOfDay();
    int nx = schedule::next(p, now);
    if (nx < 0) {
        snprintf(out, size, "no plan");
        return;
    }
    const schedule::Entry &e = schedule::entry(p, nx);
    char wait[16];
    formatWait((e.minute - now + 1440) % 1440, wait, sizeof(wait));
    snprintf(out, size, "%02d:%02d %s", e.minute / 60, e.minute % 60, wait);
}

void drawSchedule()
{
    const schedule::Plan p = sched_plan;
    int now = utcMinuteOfDay();
    int n = schedule::count(p);
    int nx = schedule::next(p, now);
    lv_label_set_text_fmt(sched_title, "%s  %s  %s", p == schedule::Plan::Wefax ? "WEFAX" : "RTTY", schedule::station(p), schedule::frequencies(p));
    if (sched_follow) {
        sched_top = nx - 1;  // the broadcast before stays in view, it may still be running
    }
    sched_top = constrain(sched_top, 0, max(0, n - kSchedRows));
    // The one before the next started last; it runs until the next begins, at most for a while
    int last = nx > 0 ? nx - 1 : n - 1;
    bool on_air = n > 0 && (now - schedule::entry(p, last).minute + 1440) % 1440 < schedule::longestMinutes(p);
    for (int i = 0; i < kSchedRows; ++i) {
        int idx = sched_top + i;
        if (idx >= n) {
            lv_label_set_text(sched_row[i], "");
            lv_obj_set_style_bg_color(sched_bg[i], kColBg, 0);
            continue;
        }
        const schedule::Entry &e = schedule::entry(p, idx);
        // Broadcasts earlier today are over; after the last one of the day all come tomorrow
        bool past = e.minute <= now && !(nx == 0 && e.minute > now);
        char when[16] = "";
        lv_color_t color = kColText;
        if (idx == last && on_air) {
            snprintf(when, sizeof(when), "on air");
            color = kColCq;
        } else if (past) {
            color = kColDim;
        } else {
            formatWait((e.minute - now + 1440) % 1440, when, sizeof(when));
        }
        lv_label_set_text_fmt(sched_row[i], "%02d:%02d   %-12s %s", e.minute / 60, e.minute % 60, when, e.text);
        lv_obj_set_style_text_color(sched_row[i], idx == nx ? kColText : color, 0);
        lv_obj_set_style_bg_color(sched_bg[i], idx == nx ? kColSel : kColBg, 0);
    }
    lv_label_set_text_fmt(sched_footer, "now %02d:%02d UTC   wheel: scroll  Backspace: back   |  %s", now / 60, now % 60,
                          schedule::fromCard(p) ? "plan from the SD card" : "built-in plan, change it on the Wi-Fi page");
}

void openSchedule()
{
    if (!planFor(engine::mode(), &sched_plan)) {
        return;
    }
    sub_from_app = inApp();
    schedule::load(sched_plan);  // a plan saved on the phone page in the meantime
    sched_follow = true;
    screen = Screen::Schedule;
    drawSchedule();
    lv_screen_load(scr_sched);
}

// --- frequency of the radio ---------------------------------------------------------------

// The radio is tuned by hand; here the user tells which frequency it is on. For
// FT8 and FT4 it goes into the logbook, in the other modes it is shown, and for
// the broadcast modes the list is a reminder of where the stations are.
constexpr int kBandRows = 9;
constexpr int kMaxBandEntries = 16;
lv_obj_t *scr_band, *band_title, *band_bg[kBandRows], *band_row[kBandRows], *band_footer;
const ft8::BandPreset *band_preset[kMaxBandEntries];
int band_entries = 0;   // presets for the mode in use; one more row follows for another frequency
int band_selected = 0;
char band_other[10] = "";  // kHz as typed, for example "14074" or "7047.5"

/// Stations are listed by their frequency; the radio in USB is tuned lower by
/// the audio frequency their signal is to appear at. 0 for the other modes.
int bandTuneOffsetHz(Mode m)
{
    return m == Mode::NAVTEX ? settings.navtex_center : (m == Mode::WEFAX ? 1900 : 0);
}

/// Mode with frequency, for the headers: "CW 7.030", "NAVTEX 518".
void modeWithFrequency(Mode m, char *out, size_t size)
{
    char f[12];
    if (m == Mode::NAVTEX || m == Mode::WEFAX) {
        ft8::formatKhz(settings.dialHz((int)m), f);
    } else {
        ft8::formatMhz(settings.dialHz((int)m), f);
    }
    snprintf(out, size, "%s%s %s", engine::modeName(m), settings.cb_mode && engine::isSlotMode(m) ? " CB" : "", f);
}

void buildBand()
{
    scr_band = makeScreen();
    band_title = makeLabel(scr_band, 4, 2, kWidth - 8, &lv_font_montserrat_14, kColCq);
    for (int i = 0; i < kBandRows; ++i) {
        int y = 22 + i * 20;
        band_bg[i] = makeBox(scr_band, 0, y, kWidth, 20, kColBg);
        band_row[i] = makeLabel(scr_band, 8, y + 2, kWidth - 16, &lv_font_montserrat_14, kColText);
    }
    band_footer = makeLabel(scr_band, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
}

void drawBand()
{
    Mode m = engine::mode();
    uint32_t current = settings.dialHz((int)m);
    int offset = bandTuneOffsetHz(m);
    lv_label_set_text_fmt(band_title, "%s  -  %s%s", offset ? "Station (tune the radio as shown, USB)" : "Frequency of the radio",
                          engine::modeName(m), settings.cb_mode && engine::isSlotMode(m) ? " CB" : "");
    int rows = band_entries + 1;
    int top = constrain(band_selected - kBandRows / 2, 0, max(0, rows - kBandRows));
    for (int i = 0; i < kBandRows; ++i) {
        int idx = top + i;
        if (idx >= rows) {
            lv_label_set_text(band_row[i], "");
            lv_obj_set_style_bg_color(band_bg[i], kColBg, 0);
            continue;
        }
        if (idx < band_entries) {
            const ft8::BandPreset &p = *band_preset[idx];
            char f[12], tune[28] = "";
            if (offset != 0) {
                // Stations in kHz, with the dial setting that brings them to the right audio frequency
                char dial[12];
                ft8::formatKhz(p.hz, f);
                ft8::formatKhz(p.hz - (uint32_t)offset, dial);
                snprintf(tune, sizeof(tune), "  ->  dial %s", dial);
            } else {
                ft8::formatMhz(p.hz, f);
            }
            lv_label_set_text_fmt(band_row[i], "%-13s %s %s%s%s%s%s%s", p.label, f, offset ? "kHz" : "MHz", tune, p.remark[0] ? "  (" : "",
                                  p.remark, p.remark[0] ? ")" : "", p.hz == current ? "   < now" : "");
        } else {
            lv_label_set_text_fmt(band_row[i], "Other:  %s%s kHz   (type the frequency)", band_other, idx == band_selected ? "_" : "");
        }
        lv_obj_set_style_bg_color(band_bg[i], idx == band_selected ? kColSel : kColBg, 0);
    }
    lv_label_set_text(band_footer, band_selected == band_entries
                                       ? "Space + key: digits  point: half kHz  press: take it  Backspace: back"
                                       : "wheel: select   press: take it   Backspace: back");
}

void openBand()
{
    Mode m = engine::mode();
    ft8::BandUse use = (ft8::BandUse)(int)m;
    bool cb = settings.cb_mode && engine::isSlotMode(m);
    uint32_t current = settings.dialHz((int)m);
    band_entries = 0;
    band_selected = -1;
    for (int i = 0; i < ft8::bandPresetCount(use, cb) && band_entries < kMaxBandEntries; ++i) {
        const ft8::BandPreset &p = ft8::bandPreset(use, cb, i);
        if (p.hz == current) {
            band_selected = band_entries;
        }
        band_preset[band_entries++] = &p;
    }
    // A frequency that is none of the usual ones is shown in the row for typing
    band_other[0] = '\0';
    if (band_selected < 0) {
        band_selected = band_entries;
        ft8::formatKhz(current, band_other);
    }
    screen = Screen::Band;
    drawBand();
    lv_screen_load(scr_band);
}

void closeBand(bool take)
{
    if (take) {
        uint32_t hz = band_selected < band_entries ? band_preset[band_selected]->hz : (uint32_t)(atof(band_other) * 1000 + 0.5);
        if (hz < 100000 || hz > 450000000u) {
            showNotice("Not a frequency in kHz - nothing changed");
        } else {
            settings.dialHz((int)engine::mode()) = hz;
            if (engine::mode() == Mode::RTTY && band_selected < band_entries) {
                // The weather service and amateurs use different speeds and shifts; the choice sets them
                bool dwd = strncmp(band_preset[band_selected]->label, "DWD", 3) == 0;
                settings.rtty_50_baud = dwd;
                settings.rtty_shift = dwd ? 450 : 170;
                engine::applySettings();
                showNotice(dwd ? "Set for DWD weather: 50 Bd, shift 450 Hz" : "Set for amateur RTTY: 45.45 Bd, shift 170 Hz");
            }
            settings.save();
        }
    }
    openMode(engine::mode());
}

void bandKey(char c)
{
    size_t n = strlen(band_other);
    bool typing = band_selected == band_entries;
    if (c == '\n' || c == '\r') {
        closeBand(true);
        return;
    }
    if (c == 0x08) {
        if (typing && n > 0) {
            band_other[n - 1] = '\0';
        } else {
            closeBand(false);
            return;
        }
    } else if (typing && n + 1 < sizeof(band_other) && (isdigit((unsigned char)c) || ((c == '.' || c == ',') && strchr(band_other, '.') == nullptr))) {
        band_other[n] = c == ',' ? '.' : c;
        band_other[n + 1] = '\0';
    }
    drawBand();
}

// --- saved WEFAX pictures -----------------------------------------------------------------

constexpr int kMaxFaxFiles = 60;
constexpr int kFaxListRows = 9;
struct FaxFile {
    char name[32];
    uint32_t lines;
};
FaxFile *fax_files = nullptr;
int fax_file_count = 0;
int fax_file_selected = 0;
lv_obj_t *scr_faxlist, *faxlist_title, *faxlist_bg[kFaxListRows], *faxlist_row[kFaxListRows], *faxlist_footer;
lv_obj_t *scr_faxview, *faxview_title, *faxview_canvas;
lv_draw_buf_t *faxview_buf = nullptr;
uint32_t faxview_top = 0;     // first line of the picture on the screen

void buildFaxPages()
{
    scr_faxlist = makeScreen();
    faxlist_title = makeLabel(scr_faxlist, 4, 2, kWidth - 8, &lv_font_montserrat_14, kColCq);
    for (int i = 0; i < kFaxListRows; ++i) {
        int y = 22 + i * 20;
        faxlist_bg[i] = makeBox(scr_faxlist, 0, y, kWidth, 20, kColBg);
        faxlist_row[i] = makeLabel(scr_faxlist, 8, y + 2, kWidth - 16, &lv_font_montserrat_14, kColText);
    }
    faxlist_footer = makeLabel(scr_faxlist, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    lv_label_set_text(faxlist_footer, "wheel: select   press: look at it   Backspace: back");

    scr_faxview = makeScreen();
    faxview_title = makeLabel(scr_faxview, 4, 2, kWidth - 8, &lv_font_montserrat_14, kColCq);
    faxview_canvas = lv_canvas_create(scr_faxview);
    lv_obj_set_pos(faxview_canvas, 0, kFaxY);
}

bool pics_sstv = false;  // the pages show the pictures of SSTV instead of those of WEFAX

const char *picsDir()
{
    return pics_sstv ? kSstvDir : kFaxDir;
}

void scanFaxFiles()
{
    if (fax_files == nullptr) {
        fax_files = (FaxFile *)ps_calloc(kMaxFaxFiles, sizeof(FaxFile));
    }
    fax_file_count = 0;
    if (fax_files == nullptr) {
        return;
    }
    instance.lockSPI();
    File dir = SD.cardType() != CARD_NONE ? SD.open(picsDir()) : File();
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f && fax_file_count < kMaxFaxFiles; f = dir.openNextFile()) {
            const char *name = f.name();
            size_t n = strlen(name);
            if (!f.isDirectory() && name[0] != '.' && n > 4 && n < sizeof(fax_files[0].name) && strcasecmp(name + n - 4, ".bmp") == 0) {
                FaxFile &entry = fax_files[fax_file_count++];
                strcpy(entry.name, name);
                uint32_t size = (uint32_t)f.size();
                entry.lines = pics_sstv ? (size > 54 ? (size - 54) / (engine::kSstvWidth * 3) : 0)
                                        : (size > kBmpDataOffset ? (size - kBmpDataOffset) / engine::kFaxWidth : 0);
            }
            f.close();
        }
    }
    instance.unlockSPI();
    // The names carry date and time, so this puts the newest first
    qsort(fax_files, fax_file_count, sizeof(FaxFile), [](const void *a, const void *b) { return strcmp(((const FaxFile *)b)->name, ((const FaxFile *)a)->name); });
}

/// "WF_20261011_143210.bmp" as "2026-10-11 14:32:10"; other names stay as they are.
void faxTitle(const char *name, char *out, size_t size)
{
    if (strlen(name) >= 18 && name[2] == '_' && name[11] == '_') {
        snprintf(out, size, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s UTC", name + 3, name + 7, name + 9, name + 12, name + 14, name + 16);
    } else {
        snprintf(out, size, "%s", name);
    }
}

void drawFaxList()
{
    lv_label_set_text_fmt(faxlist_title, "Saved %s pictures (%d)", pics_sstv ? "SSTV" : "WEFAX", fax_file_count);
    int top = constrain(fax_file_selected - kFaxListRows / 2, 0, max(0, fax_file_count - kFaxListRows));
    for (int i = 0; i < kFaxListRows; ++i) {
        int idx = top + i;
        if (idx >= fax_file_count) {
            lv_label_set_text(faxlist_row[i], i == 0 && fax_file_count == 0 ? "None yet - Enter saves the picture being received" : "");
            lv_obj_set_style_bg_color(faxlist_bg[i], kColBg, 0);
            continue;
        }
        char title[32];
        faxTitle(fax_files[idx].name, title, sizeof(title));
        lv_label_set_text_fmt(faxlist_row[i], "%s     %u lines", title, (unsigned)fax_files[idx].lines);
        lv_obj_set_style_bg_color(faxlist_bg[i], idx == fax_file_selected ? kColSel : kColBg, 0);
    }
}

void openFaxList()
{
    pics_sstv = engine::mode() == Mode::SSTV;
    scanFaxFiles();
    fax_file_selected = constrain(fax_file_selected, 0, max(0, fax_file_count - 1));
    screen = Screen::FaxList;
    drawFaxList();
    lv_screen_load(scr_faxlist);
}

/// Draws the part of the saved picture that begins at faxview_top: two lines
/// and two columns of it make one pixel, as in the live view.
void drawFaxView()
{
    if (faxview_buf == nullptr) {
        faxview_buf = lv_draw_buf_create(kWidth, kFaxHeight, LV_COLOR_FORMAT_RGB565, 0);
        if (faxview_buf == nullptr) {
            return;
        }
        lv_canvas_set_draw_buf(faxview_canvas, faxview_buf);
    }
    const FaxFile &file = fax_files[fax_file_selected];
    static uint8_t pair[2 * engine::kFaxWidth];
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", picsDir(), file.name);
    uint32_t stride = faxview_buf->header.stride;
    instance.lockSPI();
    File f = SD.open(path, FILE_READ);
    for (int row = 0; pics_sstv && row < kFaxHeight; ++row) {
        // A colour picture, 320 pixels wide in the middle of the screen, its rows stored from the bottom up
        uint16_t *out = (uint16_t *)(faxview_buf->data + stride * row);
        static uint8_t bgr[engine::kSstvWidth * 3];
        uint32_t line = faxview_top + (uint32_t)row;
        bool have = f && line < file.lines && f.seek(54 + (file.lines - 1 - line) * sizeof(bgr)) && f.read(bgr, sizeof(bgr)) == sizeof(bgr);
        for (int x = 0; x < kWidth; ++x) {
            int px = x - kSstvViewX;
            out[x] = have && px >= 0 && px < engine::kSstvWidth
                         ? lv_color_to_u16(lv_color_make(bgr[3 * px + 2], bgr[3 * px + 1], bgr[3 * px]))
                         : lv_color_to_u16(kColBg);
        }
    }
    for (int row = 0; !pics_sstv && row < kFaxHeight; ++row) {
        uint16_t *out = (uint16_t *)(faxview_buf->data + stride * row);
        uint32_t line = faxview_top + 2 * (uint32_t)row;
        bool have = f && line + 1 < file.lines && f.seek(kBmpDataOffset + line * engine::kFaxWidth) && f.read(pair, sizeof(pair)) == sizeof(pair);
        for (int x = 0; x < kWidth; ++x) {
            if (!have) {
                out[x] = lv_color_to_u16(kColBg);
                continue;
            }
            int a = x * engine::kFaxWidth / kWidth;
            int v = (pair[a] + pair[a + 1] + pair[engine::kFaxWidth + a] + pair[engine::kFaxWidth + a + 1]) / 4;
            out[x] = lv_color_to_u16(lv_color_make(v, v, v));
        }
    }
    if (f) {
        f.close();
    }
    instance.unlockSPI();
    lv_obj_invalidate(faxview_canvas);

    char title[32];
    faxTitle(file.name, title, sizeof(title));
    lv_label_set_text_fmt(faxview_title, "%s   line %u of %u", title, (unsigned)faxview_top, (unsigned)file.lines);
}

void openFaxView()
{
    if (fax_file_count == 0) {
        return;
    }
    faxview_top = 0;
    screen = Screen::FaxView;
    lv_screen_load(scr_faxview);
    drawFaxView();
    showNotice("wheel: scroll   press: functions   Backspace: back");
}

void faxViewScroll(int dir)
{
    const FaxFile &file = fax_files[fax_file_selected];
    // WEFAX shows every second line, SSTV every line
    int32_t most = (int32_t)file.lines - (pics_sstv ? 1 : 2) * kFaxHeight;
    faxview_top = (uint32_t)constrain((int32_t)faxview_top + dir * (pics_sstv ? 30 : 60), (int32_t)0, max((int32_t)0, most));
    drawFaxView();
}

void deleteFaxPicture()
{
    const FaxFile &file = fax_files[fax_file_selected];
    // Choosing the entry in the list of functions is the decision; there is no second question
    char path[64], text[56];
    snprintf(path, sizeof(path), "%s/%s", picsDir(), file.name);
    snprintf(text, sizeof(text), "Deleted %s", file.name);
    instance.lockSPI();
    bool ok = SD.remove(path);
    instance.unlockSPI();
    openFaxList();
    showNotice(ok ? text : "Could not delete the picture");
}

// --- functions of a mode, opened with a press on the wheel -------------------------------

// Each mode lists what can be done in it; the letters on the right are the
// keys that do the same without opening the list (where letters are not text to send).
struct Act {
    char label[44];
    const char *key;
    void (*run)();        // nullptr for a value
    int *value;           // a number changed with the wheel after choosing the row
    int min, max, step;
    bool stay;            // the list stays open: the row shows a state that just changed
};
constexpr int kMaxActs = 14;
constexpr int kOverlayRows = 9;
constexpr int kOverlayWidth = 330;
Act acts[kMaxActs];
int act_count = 0;
int act_selected = 0;
bool overlay_open = false;
bool act_editing = false;
lv_obj_t *ov_box, *ov_bg[kOverlayRows], *ov_name[kOverlayRows], *ov_key[kOverlayRows];
lv_obj_t *notice_bar;

void addAct(const char *key, void (*run)(), bool stay, const char *fmt, ...)
{
    if (act_count == kMaxActs) {
        return;
    }
    Act &a = acts[act_count++];
    a = Act{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(a.label, sizeof(a.label), fmt, args);
    va_end(args);
    a.key = key;
    a.run = run;
    a.stay = stay;
}

void addValue(int *value, int min, int max, int step, const char *fmt, ...)
{
    if (act_count == kMaxActs) {
        return;
    }
    Act &a = acts[act_count++];
    a = Act{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(a.label, sizeof(a.label), fmt, args);
    va_end(args);
    a.key = "";
    a.value = value;
    a.min = min;
    a.max = max;
    a.step = step;
    a.stay = true;
}

bool inApp()
{
    return screen == Screen::Slot || screen == Screen::Text || screen == Screen::Fax || screen == Screen::Sstv || screen == Screen::FaxList ||
           screen == Screen::FaxView;
}

/// Where Back leads from a page that was opened out of a mode.
void backFromSub()
{
    if (sub_from_app) {
        openMode(engine::mode());
    } else {
        openHome();
    }
}

void clearText()
{
    rx_len = 0;
    rx_text[0] = '\0';
    rx_dirty = true;
    showNotice("Cleared");
}

void stopSending()
{
    engine::halt();
    showNotice("Sending stopped");
}

void toggleOptionAct()
{
    engine::toggleOption();
}

void backToFaxList()
{
    openFaxList();
}

/// The list for the screen shown. Built anew every time, so that the rows show the current state.
void fillActs()
{
    act_count = 0;
    engine::Status st = engine::status();
    if (screen == Screen::FaxView) {
        addAct("", deleteFaxPicture, false, "Delete this picture");
        addAct("Back", backToFaxList, false, "Back to the list");
        return;
    }
    switch (st.mode) {
    case Mode::FT8:
    case Mode::FT4:
        addAct("Enter", slotPress, false, "Call the selected station");
        addAct("T", toggleTx, true, "Transmitter: %s", st.tx_enabled ? "ON" : "off");
        addAct("C", callCq, false, "Call CQ%s%s", cqTag()[0] ? " " : "", cqTag());
        if (!settings.cb_mode) {
            addAct("", nextCqTag, true, "Addition to CQ: %s", cqTag()[0] ? cqTag() : "none");
        }
        addAct("H", haltNow, false, "Halt: stop, drop the contact");
        addAct("V", toggleView, true, "List shows: %s", list_view == ListView::Qso ? "the contact" : "everything heard");
        addAct("F", startTxTuning, false, "TX frequency: %d Hz, set with the wheel", settings.tx_hz);
        addAct("G", freeSpot, true, "TX frequency to the quietest spot");
        addAct("E", toggleSlot, true, "TX slot: %s", slotName(engine::txParity(), false));
        break;
    case Mode::CW:
        if (st.transmitting) {
            addAct("", stopSending, false, "Stop sending");
        }
        addAct("", clearText, false, "Clear the received text");
        addValue(&settings.cw_wpm, 5, 40, 1, "Sending speed: %d WPM", settings.cw_wpm);
        addAct("", toggleOptionAct, true, "Follow the tone: %s", st.option ? "on" : "off");
        break;
    case Mode::RTTY:
        if (st.transmitting) {
            addAct("", stopSending, false, "Stop sending");
        }
        addAct("", clearText, false, "Clear the received text");
        addAct("", toggleOptionAct, true, "Tones: %s", st.option ? "reversed" : "normal");
        {
            char next[24];
            nextBroadcastText(schedule::Plan::Rtty, next, sizeof(next));
            addAct("", openSchedule, false, "DWD schedule: next %s", next);  // letters are text to send here
        }
        break;
    case Mode::NAVTEX:
        addAct("C", clearText, false, "Clear the received text");
        addAct("", toggleOptionAct, true, "Tones: %s", st.option ? "reversed" : "normal");
        break;
    case Mode::WEFAX:
        addAct("Enter", saveFaxNow, false, "Save now (%u lines)", (unsigned)faxPictureLines());
        addAct("H", toggleFaxHold, false, fax_hold ? "Go on receiving" : "Pause (the picture stays)");
        addAct("A", toggleFaxAuto, true, "Save at the stop tone: %s", fax_auto ? "on" : "off");
        addAct("C", clearFaxPicture, false, "Clear, start a new picture");
        addAct("", alignFax, false, "Align the picture again");
        addAct("P", openFaxList, false, "Saved pictures");
        {
            char next[24];
            nextBroadcastText(schedule::Plan::Wefax, next, sizeof(next));
            addAct("T", openSchedule, false, "Schedule: next %s", next);
        }
        break;
    case Mode::SSTV:
        addAct("Enter", saveSstvNow, false, "Save now (%d lines)", sstv_have);
        addAct("A", toggleSstvAuto, true, "Save finished pictures: %s", sstv_auto ? "on" : "off");
        if (engine::sstvState().receiving) {
            addAct("X", stopSstv, false, "Stop receiving this picture");
        }
        addAct("", nextSstvFormat, true, "Format by hand: %s", engine::sstvFormatName(sstv_manual));
        addAct("M", startSstvByHand, false, "Receive now as %s", engine::sstvFormatName(sstv_manual));
        addAct("C", clearSstvNow, false, "Clear the picture");
        addAct("P", openFaxList, false, "Saved pictures");
        break;
    }
    bool letters = !engine::canSendText(st.mode);  // in CW and RTTY letters are text to send
    addAct(letters ? "B" : "", openBand, false, st.mode == Mode::NAVTEX || st.mode == Mode::WEFAX ? "Stations, frequencies" : "Band and frequency");
    addAct(engine::isSlotMode(st.mode) ? "L" : "", openLog, false, "Logbook");
    addAct(engine::isSlotMode(st.mode) ? "S" : "", openSetup, false, "Setup");
    addAct("Back", openHome, false, "Main menu");
}

void buildOverlay()
{
    // On the top layer, so that it lies over whichever screen is shown
    ov_box = makeBox(lv_layer_top(), kWidth - kOverlayWidth - 2, 21, kOverlayWidth, kOverlayRows * 20 + 4, lv_color_hex(0x0A0F2E));
    lv_obj_set_style_border_width(ov_box, 1, 0);
    lv_obj_set_style_border_color(ov_box, kColCq, 0);
    for (int i = 0; i < kOverlayRows; ++i) {
        ov_bg[i] = makeBox(ov_box, 1, 1 + i * 20, kOverlayWidth - 4, 20, lv_color_hex(0x0A0F2E));
        ov_name[i] = makeLabel(ov_box, 8, 3 + i * 20, kOverlayWidth - 70, &lv_font_montserrat_14, kColText);
        ov_key[i] = makeLabel(ov_box, kOverlayWidth - 60, 3 + i * 20, 50, &lv_font_montserrat_14, kColCq);
        lv_obj_set_style_text_align(ov_key[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_add_flag(ov_box, LV_OBJ_FLAG_HIDDEN);

    // Messages on screens without a line at the bottom
    notice_bar = makeLabel(lv_layer_top(), 0, kFooterY, kWidth, &lv_font_montserrat_14, kColWarn);
    lv_obj_set_style_bg_color(notice_bar, lv_color_hex(0x0A0F2E), 0);
    lv_obj_set_style_bg_opa(notice_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(notice_bar, 6, 0);
    lv_obj_set_style_pad_ver(notice_bar, 2, 0);
    lv_obj_add_flag(notice_bar, LV_OBJ_FLAG_HIDDEN);
}

void drawOverlay()
{
    fillActs();
    act_selected = constrain(act_selected, 0, act_count - 1);
    int rows = min(act_count, kOverlayRows);
    lv_obj_set_height(ov_box, rows * 20 + 4);
    int top = constrain(act_selected - kOverlayRows / 2, 0, max(0, act_count - kOverlayRows));
    for (int i = 0; i < kOverlayRows; ++i) {
        int idx = top + i;
        if (idx >= act_count) {
            lv_obj_add_flag(ov_bg[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(ov_name[i], "");
            lv_label_set_text(ov_key[i], "");
            continue;
        }
        lv_obj_remove_flag(ov_bg[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ov_name[i], acts[idx].label);
        lv_label_set_text(ov_key[i], idx == act_selected && act_editing ? "wheel" : acts[idx].key);
        lv_obj_set_style_bg_color(ov_bg[i], idx == act_selected ? (act_editing ? kColEdit : kColSel) : lv_color_hex(0x0A0F2E), 0);
    }
}

void closeOverlay()
{
    overlay_open = false;
    act_editing = false;
    lv_obj_add_flag(ov_box, LV_OBJ_FLAG_HIDDEN);
}

void openOverlay()
{
    overlay_open = true;
    act_editing = false;
    act_selected = 0;
    lv_obj_remove_flag(ov_box, LV_OBJ_FLAG_HIDDEN);
    drawOverlay();
}

void overlayRotate(int dir)
{
    Act &a = acts[act_selected];
    if (act_editing && a.value != nullptr) {
        *a.value = constrain(*a.value - dir * a.step, a.min, a.max);  // up raises it, as in the setup
        engine::applySettings();
    } else {
        act_selected = constrain(act_selected + dir, 0, act_count - 1);
    }
    drawOverlay();
}

void overlayChoose()
{
    Act a = acts[act_selected];
    if (a.value != nullptr) {
        // A press starts and ends changing the number with the wheel
        act_editing = !act_editing;
        if (!act_editing) {
            settings.save();
        }
        drawOverlay();
        return;
    }
    if (!a.stay) {
        closeOverlay();
    }
    a.run();
    if (a.stay && overlay_open) {
        drawOverlay();
    }
}

void overlayKey(char c)
{
    if (c == 0x08) {
        if (act_editing) {
            act_editing = false;
            settings.save();
            drawOverlay();
        } else {
            closeOverlay();
        }
    } else if (c == '\n' || c == '\r') {
        overlayChoose();
    }
}

/// The message strip of the screens that have no line at the bottom.
void drawNoticeBar()
{
    bool wanted = noticeActive() && (screen == Screen::Text || screen == Screen::Fax || screen == Screen::Sstv || screen == Screen::FaxView ||
                                     screen == Screen::FaxList);
    if (wanted) {
        lv_label_set_text(notice_bar, notice);
        lv_obj_remove_flag(notice_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(notice_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- input and housekeeping ------------------------------------------------------------

void updateGridFromGps()
{
    static uint32_t last_check = 0;
    if (!settings.grid_auto || millis() - last_check < 5000) {
        return;
    }
    last_check = millis();
    GPS &gps = instance.gps;
    if (!gps.location.isValid() || gps.location.age() > 5000) {
        return;
    }
    char grid[5];
    if (ft8::gridFromLatLon(gps.location.lat(), gps.location.lng(), grid) && strcmp(grid, settings.grid) != 0) {
        strcpy(settings.grid, grid);
        settings.save();
        engine::applySettings();
        char text[56];
        snprintf(text, sizeof(text), "Locator set from GPS: %s", grid);
        showNotice(text);
    }
}

void pollInput()
{
    static bool was_pressed = false;
    RotaryMsg_t msg = instance.getRotary();  // waits up to 50 ms for an event
    int dir = msg.dir == ROTARY_DIR_UP ? 1 : (msg.dir == ROTARY_DIR_DOWN ? -1 : 0);
    bool pressed = msg.centerBtnPressed && !was_pressed;
    was_pressed = msg.centerBtnPressed;

    // The first key or wheel event with the display off only switches it on again
    if (dir != 0 || pressed) {
        last_input = millis();
        if (display_off) {
            setDisplayOff(false);
            dir = 0;
            pressed = false;
        }
    }

    // The list of functions takes all input while it is open
    if (overlay_open) {
        if (dir != 0) {
            overlayRotate(dir);
        }
        if (pressed) {
            overlayChoose();
        }
        char key = 0;
        while (overlay_open && instance.getKeyChar(&key) == KB_PRESSED) {
            last_input = millis();
            overlayKey(key);
        }
        return;
    }

    if (dir != 0) {
        switch (screen) {
        case Screen::Home: homeRotate(dir); break;
        case Screen::Slot: slotRotate(dir); break;
        case Screen::Text: textRotate(dir); break;
        case Screen::Fax: engine::faxShift(dir * 16); break;
        case Screen::Sstv: sstvRotate(dir); break;
        case Screen::Log:
            log_top += dir;
            drawLog();
            break;
        case Screen::Setup: setupRotate(dir); break;
        case Screen::About: aboutRotate(dir); break;
        case Screen::Band:
            band_selected = constrain(band_selected + dir, 0, band_entries);
            drawBand();
            break;
        case Screen::FaxList:
            fax_file_selected = constrain(fax_file_selected + dir, 0, max(0, fax_file_count - 1));
            drawFaxList();
            break;
        case Screen::FaxView: faxViewScroll(dir); break;
        case Screen::Schedule:
            sched_follow = false;
            sched_top += dir;
            drawSchedule();
            break;
        }
    }
    if (pressed) {
        switch (screen) {
        case Screen::Home: homePress(); break;
        // In the modes the press opens their functions; while tuning TX it ends that
        case Screen::Slot:
            if (tx_tuning) {
                endTxTuning(true);
            } else {
                openOverlay();
            }
            break;
        case Screen::Text:
        case Screen::Fax:
        case Screen::Sstv:
        case Screen::FaxView: openOverlay(); break;
        case Screen::Log:
            log_stats = !log_stats;
            drawLog();
            break;
        case Screen::Setup: setupPress(); break;
        case Screen::HomeWifi:
            hw_page = 1 - hw_page;
            drawHomeWifiQr();
            break;
        case Screen::About: break;
        case Screen::Band: closeBand(true); break;
        case Screen::FaxList: openFaxView(); break;
        case Screen::Schedule:
            sched_follow = true;  // back to the next chart
            drawSchedule();
            break;
        }
    }

    char c = 0;
    while (instance.getKeyChar(&c) == KB_PRESSED) {
        last_input = millis();
        if (display_off) {
            setDisplayOff(false);
            continue;
        }
        switch (screen) {
        case Screen::Home:
            if (c == '\n' || c == '\r') {
                homePress();
            }
            break;
        case Screen::Slot: slotKey(c); break;
        case Screen::Text: textKey(c); break;
        case Screen::Fax: faxKey(c); break;
        case Screen::Sstv: sstvKey(c); break;
        case Screen::Log: logKey(c); break;
        case Screen::Setup: setupKey(c); break;
        case Screen::HomeWifi: homeWifiKey(c); break;
        case Screen::About:
            if (c == 0x08) {
                // Back to the setup, on the entry it was opened from
                screen = Screen::Setup;
                drawSetup();
                lv_screen_load(scr_setup);
            }
            break;
        case Screen::Band: bandKey(c); break;
        case Screen::FaxList:
            if (c == 0x08) {
                openMode(engine::mode());
            } else if (c == '\n' || c == '\r') {
                openFaxView();
            }
            break;
        case Screen::FaxView:
            if (c == 0x08) {
                openFaxList();
            }
            break;
        case Screen::Schedule:
            if (c == 0x08) {
                backFromSub();
            }
            break;
        }
        if (overlay_open) {
            break;  // a key just opened the list; the rest belongs to it
        }
    }
}

}  // namespace

void selectMode(int mode)
{
    openMode((Mode)constrain(mode, 0, engine::kModes - 1));
}

void printBattery()
{
    const Battery &b = readBattery();
    Serial.printf("[bat] %d %% shown; gauge %d %%, curve %d %% at %d mV, %d mA%s\n", b.percent, b.gauge, b.curve, b.mv, b.ma,
                  b.charging ? ", on USB" : "");
}

unsigned saveFaxNowForTest()
{
    return saveFaxPicture() ? (unsigned)faxPictureLines() : 0;
}

const char *recentText()
{
    return rx_len > 600 ? rx_text + (rx_len - 600) : rx_text;
}

bool submitText(const char *text)
{
    if (!engine::canSendText(engine::mode()) || text[0] == '\0') {
        return false;
    }
    char upper[80];
    size_t n = 0;
    for (; text[n] && n + 1 < sizeof(upper); ++n) {
        upper[n] = (char)toupper((unsigned char)text[n]);
    }
    upper[n] = '\0';
    engine::sendText(upper);
    engine::sendText(" ");
    // Sent text appears in the window on a line of its own
    appendText("\n> ", 3);
    appendText(upper, n);
    appendText("\n", 1);
    return true;
}

namespace {
uint32_t splash_since = 0;
}

void splash()
{
    lv_obj_t *scr = makeScreen();
    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &img_splash);
    lv_obj_set_pos(img, 0, 0);
    lv_screen_load(scr);
    lv_timer_handler();
    splash_since = millis();
}

void begin()
{
    audio_board_at_start = settings.audio_board;
    schedule::load(schedule::Plan::Wefax);
    schedule::load(schedule::Plan::Rtty);
    // The library registers LVGL input devices for encoder and keyboard; they would
    // consume the events this module reads itself.
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        lv_indev_enable(indev, false);
    }
    buildPalette();
    buildSlot();
    buildText();
    buildFax();
    buildSstv();
    buildSetup();
    buildAbout();
    buildHomeWifi();
    buildBand();
    buildFaxPages();
    buildOverlay();
    buildHome();
    buildLog();
    buildSchedule();
    logbook::load();
    web::apply();
    tile_selected = (int)engine::mode();
    // Leave the boot picture up for a moment
    while (splash_since != 0 && millis() - splash_since < 1800) {
        delay(20);
    }
    // Why the device started, so that waking from sleep can be told from a restart after a fault
    const char *why = nullptr;
    switch (esp_reset_reason()) {
    case ESP_RST_DEEPSLEEP: why = "Woke up from sleep"; break;
    case ESP_RST_PANIC: why = "Restart after a fault"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: why = "Restart: watchdog"; break;
    case ESP_RST_BROWNOUT: why = "Restart: low voltage"; break;
    default: break;
    }
    Serial.printf("[dw] reset reason %d%s%s\n", (int)esp_reset_reason(), why ? ": " : "", why ? why : "");
    if (why != nullptr) {
        showNotice(why);
        notice_until = millis() + 15000;
    }
    openHome();
}

void loop()
{
    static uint32_t last_draw = 0;

    pollInput();

    engine::LogItem item;
    while (engine::nextLogItem(&item)) {
        char text[56];
        snprintf(text, sizeof(text), "QSO with %s %s", item.rec.dx_call,
                 logbook::append(item.rec, item.protocol) ? "logged" : "NOT saved (SD card?)");
        showNotice(text);
    }

    // Received text and picture lines are collected whichever screen is shown
    static uint32_t seen_seq = 0;
    engine::Status st = engine::status();
    bool received = st.history_seq != seen_seq;
    seen_seq = st.history_seq;
    char buf[64];
    size_t n;
    while ((n = engine::readText(buf, sizeof(buf))) > 0) {
        appendText(buf, n);
        received = true;
    }
    processFax();
    processSstv();
    pollPower(st, received);

    updateGridFromGps();
    web::loop();

    // Nothing is drawn while the display is off; the waterfall rows are still fetched
    drawWaterfall(display_off ? nullptr : (screen == Screen::Slot ? &slot_wf : (screen == Screen::Text ? &text_wf : nullptr)));
    if (!display_off && millis() - last_draw >= 200) {
        last_draw = millis();
        switch (screen) {
        case Screen::Home: drawHome(); break;
        case Screen::Slot: drawSlot(); break;
        case Screen::Text: drawText(); break;
        case Screen::Fax: drawFax(); break;
        case Screen::Sstv: drawSstv(); break;
        case Screen::Schedule: drawSchedule(); break;
        case Screen::HomeWifi: drawHomeWifi(); break;
        default: break;
        }
        drawNoticeBar();
        if (overlay_open) {
            drawOverlay();  // states shown in it may have changed
        }
    }
    lv_timer_handler();
}

}  // namespace ui
