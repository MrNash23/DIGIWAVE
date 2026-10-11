#include "ui.h"

#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <SD.h>

#include <band_plan.h>
#include <ft8_adif.h>

#include "assets/assets.h"
#include "engine.h"
#include "logbook.h"
#include "settings.h"
#include "timebase.h"
#include "web.h"

// Input is read directly from the rotary encoder and the keyboard instead of
// through LVGL input devices: the screens have a fixed layout and a handful
// of shortcuts, which is simpler to handle here than through focus groups.

namespace ui {

const char kFaxDir[] = "/wefax";

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

enum class Screen { Home, Slot, Text, Fax, Log, Setup, Band };
Screen screen = Screen::Home;

void openHome();
void openMode(Mode m);
void openLog();
void openSetup();
void openBand();
void modeWithFrequency(Mode m, char *out, size_t size);
void powerDown(const char *reason);

// --- widgets shared by the mode screens ------------------------------------------

struct Header {
    lv_obj_t *time, *gps, *txrx, *state, *level, *slot;
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
/// GPS time, amber on the RTC alone, magenta without any time.
void drawClockIcon(lv_obj_t *img)
{
    switch (timebase::source()) {
    case timebase::Source::GpsPps: setIcon(img, &img_status_gps_fix, kColCq); break;
    case timebase::Source::Gps: setIcon(img, &img_status_gps_fix, kColText); break;
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

Header makeHeader(lv_obj_t *scr, bool with_slot)
{
    Header h{};
    h.time = makeLabel(scr, 4, 1, 70, &lv_font_montserrat_14, kColText);
    h.gps = makeIcon(scr, 78, 1, &img_status_gps_no_fix);
    h.txrx = makeIcon(scr, 100, 1, &img_status_rx);
    if (with_slot) {
        h.slot = makeBar(scr, 122, 5, 70, 8, kColAccent);
        h.state = makeLabel(scr, 200, 1, 208, &lv_font_montserrat_14, kColText);
    } else {
        h.state = makeLabel(scr, 124, 1, 284, &lv_font_montserrat_14, kColText);
    }
    h.level = makeBar(scr, 414, 5, 62, 8, kColCq);
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

/// Moves new spectrum rows into the given waterfall, or just discards them.
void drawWaterfall(Waterfall *wf)
{
    static uint8_t row[engine::kWaterfallBins];
    bool changed = false;
    while (engine::nextWaterfallRow(row)) {
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

        uint32_t stride = wf->buf->header.stride;
        memmove(wf->buf->data, wf->buf->data + stride, stride * (wf->height - 1));
        uint16_t *line = (uint16_t *)(wf->buf->data + stride * (wf->height - 1));
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
const lv_color_t kColOwnBg = lv_color_hex(0x2A2408);

void buildSlot()
{
    scr_slot = makeScreen();
    slot_header = makeHeader(scr_slot, true);
    for (int i = 0; i < kListRows; ++i) {
        int y = kListY + i * kRowHeight;
        row_bg[i] = makeBox(scr_slot, 0, y, kWidth, kRowHeight, kColBg);
        row_meta[i] = makeLabel(scr_slot, 4, y + 1, 124, &lv_font_montserrat_14, kColDim);
        row_msg[i] = makeLabel(scr_slot, 132, y + 1, 188, &lv_font_montserrat_14, kColText);
        row_info[i] = makeLabel(scr_slot, 324, y + 1, 152, &lv_font_montserrat_14, kColDim);
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
        if (e.country[0] != '\0') {
            lv_label_set_text_fmt(row_info[i], "%s%s(%s)", e.grid, e.grid[0] ? " " : "", e.country);
        } else {
            lv_label_set_text(row_info[i], e.grid);
        }
        lv_obj_set_style_text_color(row_msg[i], e.to_me ? kColToMe : (e.msg.is_cq ? kColCq : kColText), 0);
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
        lv_label_set_text_fmt(slot_header.state, "%s%s TX  %s", engine::modeName(st.mode), settings.cb_mode ? " CB" : "", st.tx_text);
        lv_obj_set_style_text_color(slot_header.state, kColTx, 0);
    } else {
        if (st.dx_call[0] != '\0' && strcmp(st.qso_state, "Idle") != 0) {
            lv_label_set_text_fmt(slot_header.state, "%s RX  %s %s", name, st.qso_state, st.dx_call);
        } else {
            lv_label_set_text_fmt(slot_header.state, "%s RX  %s", name, st.qso_state);
        }
        lv_obj_set_style_text_color(slot_header.state, kColText, 0);
    }
    lv_obj_set_x(tx_marker, waterfallX((float)settings.tx_hz));

    if (noticeActive()) {
        lv_label_set_text(slot_footer, notice);
        lv_obj_set_style_text_color(slot_footer, kColWarn, 0);
    } else {
        lv_label_set_text_fmt(slot_footer, "TX %s (T)  CQ (C)  Halt (H)  Log (L)  Band (B)  View (V): %s  Menu (M)  |  %d dec",
                              st.tx_enabled ? "ON" : "off", list_view == ListView::Qso ? "QSO" : "all", st.decoded);
        lv_obj_set_style_text_color(slot_footer, st.tx_enabled ? kColTx : kColDim, 0);
    }

    if (st.history_seq != history_seq) {
        reloadHistory(st.history_seq);
        drawList();
    }
}

void slotRotate(int dir)
{
    if (entry_count == 0) {
        return;
    }
    selected = constrain(selected + dir, 0, entry_count - 1);
    follow = selected == entry_count - 1;
    drawList();
}

void slotPress()
{
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

void slotKey(char c)
{
    switch (tolower((unsigned char)c)) {
    case 't': {
        bool on = !engine::status().tx_enabled;
        if (on && !stationConfigured()) {
            return;
        }
        if (on && !timebase::valid()) {
            showNotice("No time source - TX stays off");
            return;
        }
        engine::setTxEnabled(on);
        break;
    }
    case 'c':
        if (stationConfigured()) {
            engine::startCq();
            showNotice(engine::status().tx_enabled ? "Calling CQ" : "CQ armed  -  TX is off (T)");
        }
        break;
    case 'h':
    case 0x08:
        engine::halt();
        showNotice("Halted");
        break;
    case 's':
        openSetup();
        break;
    case 'l':
        openLog();
        break;
    case 'b':
        openBand();
        break;
    case 'v':
        view_by_hand = true;
        setListView(list_view == ListView::Qso ? ListView::All : ListView::Qso);
        break;
    case 'm':
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
constexpr int kTextWfY = 180;       // spectrum strip
constexpr int kTextWfHeight = 20;
constexpr int kMarkY = 171;         // band above it for the tuning marks

lv_obj_t *scr_text;
Header text_header;
Waterfall text_wf;
lv_obj_t *text_box, *text_label, *input_label, *text_footer, *marker_a, *marker_b, *marker_span, *marker_tone;

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
bool cw_wheel_sets_speed = false;  // otherwise the wheel moves the receive window

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
    text_box = makeBox(scr_text, 0, kTextY, kWidth, 128, kColBg);
    text_label = lv_label_create(text_box);
    lv_obj_set_width(text_label, kWidth - 8);
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(text_label, kColText, 0);
    lv_label_set_text(text_label, "");
    // The newest text stays visible: the label grows upwards out of the box
    lv_obj_align(text_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    input_label = makeLabel(scr_text, 4, 150, kWidth - 8, &lv_font_montserrat_14, kColCq);
    text_wf = makeWaterfall(scr_text, kTextWfY, kTextWfHeight);
    // The tuning marks sit above the spectrum, not in it
    buildTriangle();
    marker_span = makeBox(scr_text, kWfX, kMarkY, 10, 2, kColText);
    marker_a = makeIcon(scr_text, kWfX, kMarkY, &tri_image);
    marker_b = makeIcon(scr_text, kWfX, kMarkY, &tri_image);
    marker_tone = makeBox(scr_text, kWfX, kMarkY + 4, 3, 4, kColCq);
    text_footer = makeLabel(scr_text, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
}

void drawText()
{
    engine::Status st = engine::status();
    drawHeader(text_header, st);
    bool can_send = engine::canSendText(st.mode);

    const char *lock = st.locked ? "signal" : "no signal";
    char name[28];
    modeWithFrequency(st.mode, name, sizeof(name));
    switch (st.mode) {
    case Mode::CW:
        if (st.transmitting) {
            lv_label_set_text_fmt(text_header.state, "%s TX  %d WPM  %d Hz", name, settings.cw_wpm, settings.cw_tone);
        } else {
            lv_label_set_text_fmt(text_header.state, "%s RX  %d WPM  %d Hz  %s", name, st.wpm, st.tune_hz, lock);
        }
        if (cw_wheel_sets_speed) {
            lv_label_set_text_fmt(text_footer, "wheel: sending speed %d WPM   press: window   Enter: send, on an empty line: band   Backspace: menu",
                                  settings.cw_wpm);
        } else {
            lv_label_set_text_fmt(text_footer, "wheel: window %d Hz (signal between the marks)   press: speed   Enter: send, empty: band",
                                  settings.cw_tone);
        }
        break;
    case Mode::RTTY:
        lv_label_set_text_fmt(text_header.state, "%s %s  %s Bd  %d/%d%s  %s", name, st.transmitting ? "TX" : "RX",
                              settings.rtty_50_baud ? "50" : "45", settings.rtty_mark, settings.rtty_mark + settings.rtty_shift,
                              st.option ? " rev" : "", st.transmitting ? "" : lock);
        lv_label_set_text(text_footer, "Enter: send, on an empty line: band   wheel: tune   press: reverse   Backspace on empty line: menu");
        break;
    default:
        lv_label_set_text_fmt(text_header.state, "%s kHz  centre %d Hz%s  %s", name, settings.navtex_center, st.option ? " rev" : "",
                              st.locked ? "in step" : "searching");
        lv_label_set_text(text_footer, "wheel: tune   press: reverse   B: stations   Backspace: menu");
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
    lv_obj_set_height(text_box, can_send ? 128 : 148);
    if (can_send) {
        lv_obj_remove_flag(input_label, LV_OBJ_FLAG_HIDDEN);
        if (input[0] == '\0') {
            lv_label_set_text(input_label, "> _      digits and symbols: hold Space + key");
        } else {
            lv_label_set_text_fmt(input_label, "> %s_", input);
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
    engine::halt();
    settings.save();  // tuning and options changed on this screen
    openHome();
}

void textRotate(int dir)
{
    switch (engine::mode()) {
    case Mode::CW:
        if (cw_wheel_sets_speed) {
            settings.cw_wpm = constrain(settings.cw_wpm + dir, 5, 40);
        } else {
            // The window is also the tone we send on
            settings.cw_tone = constrain(settings.cw_tone + dir * 10, 400, 1200);
        }
        engine::applySettings();
        break;
    default:
        engine::tune(dir * 5);
        break;
    }
}

void textKey(char c)
{
    bool can_send = engine::canSendText(engine::mode());
    size_t n = strlen(input);
    if (c == 0x08) {
        if (engine::status().transmitting) {
            engine::halt();
        } else if (n > 0) {
            input[n - 1] = '\0';
        } else {
            leaveTextMode();
        }
    } else if (c == '\n' || c == '\r') {
        if (can_send && n > 0) {
            submitText(input);
            input[0] = '\0';
        } else if (!engine::status().transmitting) {
            // Letters are text to send here, so Enter on an empty line opens the frequencies
            openBand();
        }
    } else if (!can_send && tolower((unsigned char)c) == 'b') {
        openBand();
    } else if (can_send && isprint((unsigned char)c) && n + 1 < sizeof(input)) {
        input[n] = (char)toupper((unsigned char)c);
        input[n + 1] = '\0';
    }
}

// --- WEFAX screen ----------------------------------------------------------------------

constexpr int kFaxY = 20;
constexpr int kFaxHeight = 180;

lv_obj_t *scr_fax;
Header fax_header;
lv_obj_t *fax_canvas, *fax_footer;
lv_draw_buf_t *fax_buf;

File fax_file;
bool fax_saving = false;
bool fax_auto = true;       // start and stop saving with the station's tones
char fax_name[40] = "";
uint32_t fax_lines_saved = 0;
uint32_t fax_line_count = 0;

void putLe(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/// 8-bit greyscale BMP, rows stored top to bottom so that lines can simply be appended.
void writeBmpHeader(File &f, uint32_t lines)
{
    const uint32_t data_offset = 14 + 40 + 256 * 4;
    const uint32_t image_size = lines * engine::kFaxWidth;  // the width needs no row padding
    uint8_t h[54] = {0};
    h[0] = 'B';
    h[1] = 'M';
    putLe(h + 2, data_offset + image_size, 4);
    putLe(h + 10, data_offset, 4);
    putLe(h + 14, 40, 4);
    putLe(h + 18, engine::kFaxWidth, 4);
    putLe(h + 22, (uint32_t)(-(int32_t)lines), 4);  // negative height: top-down
    putLe(h + 26, 1, 2);
    putLe(h + 28, 8, 2);
    putLe(h + 34, image_size, 4);
    putLe(h + 46, 256, 4);
    f.seek(0);
    f.write(h, sizeof(h));
}

void faxStartSaving()
{
    if (fax_saving) {
        return;
    }
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
        fax_file = SD.open(path, FILE_WRITE);
        ok = (bool)fax_file;
        if (ok) {
            writeBmpHeader(fax_file, 0);
            uint8_t pal[4];
            for (int i = 0; i < 256; ++i) {
                pal[0] = pal[1] = pal[2] = (uint8_t)i;
                pal[3] = 0;
                fax_file.write(pal, 4);
            }
        }
    }
    instance.unlockSPI();
    fax_saving = ok;
    fax_lines_saved = 0;
    showNotice(ok ? "Saving picture to SD card" : "Cannot save: SD card?");
}

void faxStopSaving()
{
    if (!fax_saving) {
        return;
    }
    instance.lockSPI();
    writeBmpHeader(fax_file, fax_lines_saved);
    fax_file.close();
    instance.unlockSPI();
    fax_saving = false;
    char text[56];
    snprintf(text, sizeof(text), "Saved %s, %u lines", fax_name, (unsigned)fax_lines_saved);
    showNotice(text);
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
    fax_footer = makeLabel(scr_fax, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
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

    switch (engine::nextFaxEvent()) {
    case engine::FaxEvent::Start:
        showNotice("Start tone");
        if (fax_auto) {
            faxStopSaving();
            faxStartSaving();
        }
        break;
    case engine::FaxEvent::Phased:
        showNotice("Picture aligned");
        break;
    case engine::FaxEvent::Stop:
        if (fax_auto && fax_saving) {
            faxStopSaving();
        } else {
            showNotice("Stop tone");
        }
        break;
    default:
        break;
    }

    bool changed = false;
    while (engine::nextFaxLine(line)) {
        if (fax_saving) {
            instance.lockSPI();
            fax_file.write(line, sizeof(line));
            ++fax_lines_saved;
            if (fax_lines_saved % 40 == 0) {
                // Keep the file readable if power is lost during reception
                writeBmpHeader(fax_file, fax_lines_saved);
                fax_file.seek(fax_file.size());
                fax_file.flush();
            }
            instance.unlockSPI();
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
    if (fax_saving) {
        lv_label_set_text_fmt(fax_header.state, "%s kHz  saving, %u lines", name, (unsigned)fax_lines_saved);
        lv_obj_set_style_text_color(fax_header.state, kColCq, 0);
    } else {
        lv_label_set_text_fmt(fax_header.state, "%s kHz  120 LPM  %s", name, fax_auto ? "waiting for start" : "manual");
        lv_obj_set_style_text_color(fax_header.state, kColText, 0);
    }
    if (noticeActive()) {
        lv_label_set_text(fax_footer, notice);
        lv_obj_set_style_text_color(fax_footer, kColWarn, 0);
    } else {
        lv_label_set_text_fmt(fax_footer, "Enter: %s saving   A: auto %s   B: stations   wheel: move   press: align   Backspace: menu",
                              fax_saving ? "stop" : "start", fax_auto ? "on" : "off");
        lv_obj_set_style_text_color(fax_footer, kColDim, 0);
    }
}

void faxKey(char c)
{
    switch (tolower((unsigned char)c)) {
    case '\n':
    case '\r':
        fax_saving ? faxStopSaving() : faxStartSaving();
        break;
    case 'a':
        fax_auto = !fax_auto;
        break;
    case 'b':
        openBand();
        break;
    case 0x08:
        openHome();
        break;
    default:
        break;
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
    faxStopSaving();
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
    bool busy = received || st.transmitting || fax_saving || web::clients() > 0 ||
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

enum class Kind { Text, Number, Toggle, Action };
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
    {"Callsign", Kind::Text, settings.call, sizeof(settings.call), Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"FT8/FT4 operation", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.cb_mode, "CB (11 m)", "amateur radio"},
    {"Locator from", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.grid_auto, "GPS", "manual entry"},
    {"Locator", Kind::Text, settings.grid, 5, Chars::Call, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"FT8/FT4 TX frequency", Kind::Number, nullptr, 0, Chars::Call, &settings.tx_hz, 300, 2800, 50, " Hz", nullptr, "", ""},
    {"FT8/FT4 max repeats", Kind::Number, nullptr, 0, Chars::Call, &settings.max_retries, 1, 20, 1, "", nullptr, "", ""},
    {"CW sending speed", Kind::Number, nullptr, 0, Chars::Call, &settings.cw_wpm, 5, 40, 1, " WPM", nullptr, "", ""},
    {"CW frequency (RX window, TX)", Kind::Number, nullptr, 0, Chars::Call, &settings.cw_tone, 400, 1200, 10, " Hz", nullptr, "", ""},
    {"RTTY speed", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.rtty_50_baud, "50 baud", "45.45 baud"},
    {"RTTY shift", Kind::Number, nullptr, 0, Chars::Call, &settings.rtty_shift, 85, 850, 5, " Hz", nullptr, "", ""},
    {"TX level", Kind::Number, nullptr, 0, Chars::Call, &settings.tx_level, 5, 100, 5, " %", nullptr, "", ""},
    {"Volume", Kind::Number, nullptr, 0, Chars::Call, &settings.volume, 0, 100, 5, " %", nullptr, "", ""},
    {"Input gain", Kind::Number, nullptr, 0, Chars::Call, &settings.mic_gain_db, 0, 42, 3, " dB", nullptr, "", ""},
    {"Clock offset", Kind::Number, nullptr, 0, Chars::Call, &settings.time_offset_ds, -75, 75, 1, " x0.1 s", nullptr, "", ""},
    {"PTT output", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.ptt_active_high, "active high", "active low"},
    {"Wi-Fi access point", Kind::Toggle, nullptr, 0, Chars::Call, nullptr, 0, 0, 0, "", &settings.wifi_on, "on", "off"},
    {"Wi-Fi password", Kind::Text, settings.wifi_pass, sizeof(settings.wifi_pass), Chars::Any, nullptr, 0, 0, 0, "", nullptr, "", ""},
    {"Sleep when idle after", Kind::Number, nullptr, 0, Chars::Call, &settings.sleep_min, 0, 30, 1, " min (0 = never)", nullptr, "", ""},
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
    lv_label_set_text(hint, "wheel: select   press: edit / done   digits and symbols: hold Space + key   Backspace: leave");
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
    if (set_selected >= set_top + kSetRows) {
        set_top = set_selected - kSetRows + 1;
    }
    for (int i = 0; i < kSetRows; ++i) {
        int idx = set_top + i;
        const Field &f = fields[idx];
        bool active = idx == set_selected;
        lv_label_set_text(set_name[i], f.name);
        switch (f.kind) {
        case Kind::Text:
            lv_label_set_text_fmt(set_value[i], "%s%s%s", f.text, (active && editing) ? "_" : "", f.unit);
            break;
        case Kind::Number:
            lv_label_set_text_fmt(set_value[i], "%d%s", *f.number, f.unit);
            break;
        case Kind::Toggle:
            lv_label_set_text(set_value[i], *f.toggle ? f.on : f.off);
            break;
        case Kind::Action:
            lv_label_set_text(set_value[i], "press the wheel");
            break;
        }
        lv_obj_set_style_bg_color(set_bg[i], active ? (editing ? kColEdit : kColSel) : kColBg, 0);
    }
    lv_label_set_text_fmt(set_footer, "%d of %d", set_selected + 1, kFields);
}

void openSetup()
{
    set_selected = 0;
    set_top = 0;
    editing = false;
    screen = Screen::Setup;
    drawSetup();
    lv_screen_load(scr_setup);
}

void closeSetup()
{
    settings.save();
    engine::applySettings();
    extensionPortPtt(settings.ptt_active_high).set(false);
    web::apply();
    openHome();
}

void setupRotate(int dir)
{
    Field &f = fields[set_selected];
    if (!editing) {
        set_selected = constrain(set_selected + dir, 0, kFields - 1);
    } else if (f.kind == Kind::Number) {
        *f.number = constrain(*f.number + dir * f.step, f.min, f.max);
    } else if (f.kind == Kind::Toggle) {
        *f.toggle = !*f.toggle;
    }
    drawSetup();
}

void setupPress()
{
    if (fields[set_selected].kind == Kind::Action) {
            powerDown("Switched off");
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
    &img_tile_wefax,  &img_tile_logbook, &img_tile_setup, &img_tile_wifi,
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
    home_info = makeLabel(scr_home, 174, 10, 150, &lv_font_montserrat_12, kColDim);
    home_time = makeLabel(scr_home, 326, 9, 72, &lv_font_montserrat_14, kColText);
    home_gps = makeIcon(scr_home, 402, 9, &img_status_gps_no_fix);
    home_wifi = makeIcon(scr_home, 421, 9, &img_status_wifi_on);
    home_sd = makeIcon(scr_home, 440, 9, &img_status_sd_missing);
    home_battery = makeIcon(scr_home, 459, 9, &img_status_battery_3);

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
    static int battery = 100;
    if (last_slow == 0 || millis() - last_slow > 3000) {
        last_slow = millis();
        card = instance.io.digitalRead(EXPANDS_SD_DET) == LOW;  // the card pulls the detect line low
        instance.gauge.refresh();  // the readings are only fetched from the chip on request
        battery = (int)instance.gauge.getStateOfCharge();
    }

    const int cols = 5, pitch_x = 94, pitch_y = 92;
    lv_obj_set_pos(tile_tint, 9 + (tile_selected % cols) * pitch_x, 38 + (tile_selected / cols) * pitch_y);

    uint32_t secs = (uint32_t)(timebase::utcMs() / 1000);
    lv_label_set_text_fmt(home_time, "%02u:%02u:%02u", (unsigned)(secs / 3600 % 24), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
    drawClockIcon(home_gps);
    lv_obj_set_style_image_recolor(home_wifi, web::running() ? kColCq : kColFrame, 0);
    setIcon(home_sd, card ? &img_status_sd_present : &img_status_sd_missing, card ? kColText : kColWarn);
    const lv_image_dsc_t *const levels[4] = {&img_status_battery_0, &img_status_battery_1, &img_status_battery_2, &img_status_battery_3};
    setIcon(home_battery, levels[constrain((battery + 16) / 33, 0, 3)], battery < 15 ? kColTx : kColText);

    if (web::running()) {
        lv_label_set_text_fmt(home_info, "%s  %s", settings.wifi_pass, web::address() + 7);
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
    } else {
        screen = Screen::Fax;
        lv_screen_load(scr_fax);
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
lv_obj_t *log_title, *log_row[kLogRows];
int log_top = 0;

void buildLog()
{
    scr_log = makeScreen();
    log_title = makeLabel(scr_log, 4, 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    for (int i = 0; i < kLogRows; ++i) {
        log_row[i] = makeLabel(scr_log, 8, 22 + i * 20, kWidth - 16, &lv_font_montserrat_14, kColText);
    }
    lv_obj_t *hint = makeLabel(scr_log, 4, kFooterY + 2, kWidth - 8, &lv_font_montserrat_12, kColDim);
    lv_label_set_text_fmt(hint, "wheel: scroll   Backspace: menu   file on SD card: %s", logbook::kPath + 1);
}

void drawLog()
{
    int count = logbook::count();
    log_top = constrain(log_top, 0, max(0, count - kLogRows));
    if (count == 0) {
        lv_label_set_text(log_title, "Logbook  -  no QSOs yet");
    } else {
        lv_label_set_text_fmt(log_title, "Logbook  -  %d QSOs, newest first", logbook::total());
    }
    for (int i = 0; i < kLogRows; ++i) {
        int idx = count - 1 - (log_top + i);  // newest on top
        if (idx < 0) {
            lv_label_set_text(log_row[i], "");
            continue;
        }
        const logbook::Row &r = logbook::row(idx);
        lv_label_set_text_fmt(log_row[i], "%.4s-%.2s-%.2s  %.2s:%.2s   %s   %s   %s / %s   %s %s", r.date, r.date + 4, r.date + 6,
                              r.time, r.time + 2, r.call, r.grid[0] ? r.grid : "----", r.sent, r.rcvd, r.band, r.mode);
    }
}

void openLog()
{
    log_top = 0;
    screen = Screen::Log;
    drawLog();
    lv_screen_load(scr_log);
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
                                  p.remark, p.remark[0] ? ")" : "", p.hz == current ? "    - in use" : "");
        } else {
            lv_label_set_text_fmt(band_row[i], "Other:  %s%s kHz   (type the frequency)", band_other, idx == band_selected ? "_" : "");
        }
        lv_obj_set_style_bg_color(band_bg[i], idx == band_selected ? kColSel : kColBg, 0);
    }
    lv_label_set_text(band_footer, band_selected == band_entries
                                       ? "digits: hold Space + key   point for half kHz   press: take it   Backspace: delete / leave"
                                       : "wheel: select   press: take it   Backspace: leave");
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

    if (dir != 0) {
        switch (screen) {
        case Screen::Home: homeRotate(dir); break;
        case Screen::Slot: slotRotate(dir); break;
        case Screen::Text: textRotate(dir); break;
        case Screen::Fax: engine::faxShift(dir * 16); break;
        case Screen::Log:
            log_top += dir;
            drawLog();
            break;
        case Screen::Setup: setupRotate(dir); break;
        case Screen::Band:
            band_selected = constrain(band_selected + dir, 0, band_entries);
            drawBand();
            break;
        }
    }
    if (pressed) {
        switch (screen) {
        case Screen::Home: homePress(); break;
        case Screen::Slot: slotPress(); break;
        case Screen::Text:
            if (engine::mode() == Mode::CW) {
                cw_wheel_sets_speed = !cw_wheel_sets_speed;
            } else {
                engine::toggleOption();
            }
            break;
        case Screen::Fax:
            engine::faxRephase();
            showNotice("Waiting for the phasing signal");
            break;
        case Screen::Log: break;
        case Screen::Setup: setupPress(); break;
        case Screen::Band: closeBand(true); break;
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
        case Screen::Log:
            if (c == 0x08) {
                openHome();
            }
            break;
        case Screen::Setup: setupKey(c); break;
        case Screen::Band: bandKey(c); break;
        }
    }
}

}  // namespace

void selectMode(int mode)
{
    openMode((Mode)constrain(mode, 0, engine::kModes - 1));
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
    // The library registers LVGL input devices for encoder and keyboard; they would
    // consume the events this module reads itself.
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        lv_indev_enable(indev, false);
    }
    buildPalette();
    buildSlot();
    buildText();
    buildFax();
    buildSetup();
    buildBand();
    buildHome();
    buildLog();
    logbook::load();
    web::apply();
    tile_selected = (int)engine::mode();
    // Leave the boot picture up for a moment
    while (splash_since != 0 && millis() - splash_since < 1800) {
        delay(20);
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
        default: break;
        }
    }
    lv_timer_handler();
}

}  // namespace ui
