// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <unity.h>

#include <cb_mode.h>
#include <cw_codec.h>
#include <band_plan.h>
#include <ft8_adif.h>
#include <ft8_country.h>
#include <ft8_decoder.h>
#include <ft8_encoder.h>
#include <ft8_hashtable.h>
#include <ft8_qso.h>
#include <navtex_decoder.h>
#include <rtty_codec.h>
#include <sstv.h>
#include <wefax_decoder.h>

extern "C" {
#include <common/wave.h>
}

using namespace ft8;

void setUp() {}
void tearDown() {}

// --- helpers ---------------------------------------------------------------

static std::vector<int16_t> loadWav(const char *path)
{
    static float signal[15 * 12000];
    int num_samples = 15 * 12000;
    int sample_rate = 0;
    int rc = load_wav(signal, &num_samples, &sample_rate, path);
    TEST_ASSERT_EQUAL_MESSAGE(0, rc, path);
    TEST_ASSERT_EQUAL(12000, sample_rate);
    std::vector<int16_t> pcm(num_samples);
    for (int i = 0; i < num_samples; ++i) {
        pcm[i] = (int16_t)(signal[i] * 32767.0f);
    }
    return pcm;
}

static std::vector<std::string> expectedMessages(const char *path)
{
    std::vector<std::string> out;
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *tilde = strchr(line, '~');
        if (!tilde) {
            continue;
        }
        std::string msg(tilde + 1);
        size_t a = msg.find_first_not_of(" ");
        if (a == std::string::npos) {
            continue;
        }
        // WSJT-X appends the country after a run of spaces
        size_t b = msg.find("  ", a);
        msg = msg.substr(a, b == std::string::npos ? b : b - a);
        msg.erase(msg.find_last_not_of(" \r\n") + 1);
        out.push_back(msg);
    }
    fclose(f);
    return out;
}

static std::vector<Decoded> decodePcm(const std::vector<int16_t> &pcm, Protocol protocol = Protocol::FT8)
{
    Decoder dec;
    TEST_ASSERT_TRUE(dec.begin(protocol));
    // Feed in odd-sized chunks, as an audio driver would
    for (size_t pos = 0; pos < pcm.size();) {
        size_t n = std::min<size_t>(777, pcm.size() - pos);
        dec.feed(pcm.data() + pos, n);
        pos += n;
    }
    std::vector<Decoded> out(50);
    out.resize(dec.decode(out.data(), 50));
    return out;
}

static std::vector<int16_t> synthSlot(const char *text, float f0, float start = 0.5f)
{
    Synth synth;
    TEST_ASSERT_TRUE_MESSAGE(synth.begin(text, f0), text);
    std::vector<int16_t> pcm(15 * 12000, 0);
    size_t pos = (size_t)(start * 12000);
    while (!synth.done()) {
        pos += synth.read(pcm.data() + pos, 500);
    }
    TEST_ASSERT_EQUAL((size_t)(start * 12000) + 79 * 1920, pos);
    return pcm;
}

// --- decoder ---------------------------------------------------------------

static void test_decode_wav_samples()
{
    const char *names[] = {"191111_110115", "191111_110130", "191111_110145",
                           "191111_110200", "191111_110215", "191111_110630"};
    int expected_total = 0, found_total = 0;
    for (const char *name : names) {
        std::string base = std::string("test/wav/") + name;
        auto expected = expectedMessages((base + ".txt").c_str());
        auto decoded = decodePcm(loadWav((base + ".wav").c_str()));
        int found = 0;
        for (const auto &e : expected) {
            for (const auto &d : decoded) {
                if (e == d.text) {
                    ++found;
                    break;
                }
            }
        }
        printf("%s: %d of %d WSJT-X decodes found, %d decoded in total\n", name, found, (int)expected.size(),
               (int)decoded.size());
        for (const auto &d : decoded) {
            printf("    %+3d %+4.1f %4.0f  %s\n", d.snr, d.dt, d.freq, d.text);
        }
        expected_total += (int)expected.size();
        found_total += found;
    }
    printf("total: %d of %d\n", found_total, expected_total);
    // ft8_lib does not reach WSJT-X sensitivity; require most of the reference decodes
    TEST_ASSERT_GREATER_OR_EQUAL(expected_total * 7 / 10, found_total);
}

static void test_parse_message()
{
    Decoded d{};
    strcpy(d.text, "CQ DX DL1ABC JO62");
    parseMessage(d);
    TEST_ASSERT_TRUE(d.is_cq);
    TEST_ASSERT_EQUAL_STRING("DL1ABC", d.call_de);
    TEST_ASSERT_EQUAL_STRING("JO62", d.extra);

    strcpy(d.text, "CQ DL1ABC");
    parseMessage(d);
    TEST_ASSERT_TRUE(d.is_cq);
    TEST_ASSERT_EQUAL_STRING("DL1ABC", d.call_de);
    TEST_ASSERT_EQUAL_STRING("", d.extra);

    strcpy(d.text, "<DL1ABC> K1XYZ/P R-07");
    parseMessage(d);
    TEST_ASSERT_FALSE(d.is_cq);
    TEST_ASSERT_EQUAL_STRING("DL1ABC", d.call_to);
    TEST_ASSERT_EQUAL_STRING("K1XYZ/P", d.call_de);
    TEST_ASSERT_EQUAL_STRING("R-07", d.extra);

    strcpy(d.text, "DL1ABC K1XYZ RR73");
    parseMessage(d);
    TEST_ASSERT_EQUAL_STRING("RR73", d.extra);
}

// --- encoder ---------------------------------------------------------------

static void test_roundtrip()
{
    struct Case {
        const char *text;
        float f0;
        float start;
    } cases[] = {
        {"CQ DL1ABC JO62", 1000.0f, 0.5f},
        {"CQ POTA DL1ABC JO62", 1200.0f, 0.5f},
        {"K1XYZ DL1ABC -07", 1537.5f, 0.5f},
        {"K1XYZ DL1ABC R+12", 2400.0f, 1.3f},
        {"K1XYZ DL1ABC RR73", 450.0f, 0.1f},
        {"K1XYZ DL1ABC 73", 2800.0f, 0.5f},
    };
    for (const auto &c : cases) {
        auto decoded = decodePcm(synthSlot(c.text, c.f0, c.start));
        TEST_ASSERT_EQUAL_MESSAGE(1, decoded.size(), c.text);
        TEST_ASSERT_EQUAL_STRING(c.text, decoded[0].text);
        TEST_ASSERT_FLOAT_WITHIN(4.0f, c.f0, decoded[0].freq);
        TEST_ASSERT_FLOAT_WITHIN(0.05f, c.start - 0.5f, decoded[0].dt);
    }
}

static void test_roundtrip_ft4()
{
    struct Case {
        const char *text;
        float f0;
        float start;
    } cases[] = {
        {"CQ DL1ABC JO62", 1000.0f, 0.5f},
        {"K1XYZ DL1ABC R-07", 1540.0f, 0.8f},
        {"K1XYZ DL1ABC RR73", 2500.0f, 0.3f},
    };
    const ProtocolInfo &pi = info(Protocol::FT4);
    for (const auto &c : cases) {
        Synth synth;
        TEST_ASSERT_TRUE(synth.begin(c.text, c.f0, 0.5f, Protocol::FT4));
        TEST_ASSERT_EQUAL(105 * 576, synth.totalSamples());
        std::vector<int16_t> pcm(pi.slot_ms * 12, 0);
        size_t pos = (size_t)(c.start * 12000);
        while (!synth.done()) {
            pos += synth.read(pcm.data() + pos, 500);
        }
        auto decoded = decodePcm(pcm, Protocol::FT4);
        TEST_ASSERT_EQUAL_MESSAGE(1, decoded.size(), c.text);
        TEST_ASSERT_EQUAL_STRING(c.text, decoded[0].text);
        TEST_ASSERT_FLOAT_WITHIN(11.0f, c.f0, decoded[0].freq);
        TEST_ASSERT_FLOAT_WITHIN(0.03f, c.start - 0.5f, decoded[0].dt);
    }
    // The same decoder object can switch back
    Decoder dec;
    TEST_ASSERT_TRUE(dec.begin(Protocol::FT4));
    TEST_ASSERT_TRUE(dec.begin(Protocol::FT8));
    TEST_ASSERT_EQUAL(93, dec.maxBlocks());  // 15 s of 0.16 s symbols
}

static void test_encoder_rejects_garbage()
{
    Synth synth;
    TEST_ASSERT_FALSE(synth.begin("THIS TEXT IS FAR TOO LONG FOR FT8", 1000.0f));
}

static void test_synth_level()
{
    Synth synth;
    TEST_ASSERT_TRUE(synth.begin("CQ DL1ABC JO62", 1000.0f, 0.5f));
    std::vector<int16_t> pcm(synth.totalSamples());
    TEST_ASSERT_EQUAL(pcm.size(), synth.read(pcm.data(), pcm.size()));
    TEST_ASSERT_EQUAL(0, synth.read(pcm.data(), 10));
    int peak = 0;
    for (int16_t s : pcm) {
        peak = std::max(peak, abs(s));
    }
    TEST_ASSERT_INT_WITHIN(200, 16384, peak);
    TEST_ASSERT_INT_WITHIN(50, 0, pcm.front());
    TEST_ASSERT_INT_WITHIN(50, 0, pcm.back());
}

// --- QSO sequencer ---------------------------------------------------------

static std::vector<QsoRecord> logged;
static void logFn(const QsoRecord &rec, void *) { logged.push_back(rec); }

static Decoded rx(const char *text, int snr = -5)
{
    Decoded d{};
    strcpy(d.text, text);
    d.snr = snr;
    d.freq = 1200;
    parseMessage(d);
    return d;
}

static Qso makeQso()
{
    Qso q;
    QsoConfig cfg;
    strcpy(cfg.my_call, "DL1ABC");
    strcpy(cfg.my_grid, "JO62");
    cfg.max_retries = 3;
    cfg.max_cq = 4;
    q.configure(cfg);
    q.onComplete(logFn, nullptr);
    logged.clear();
    return q;
}

static void expectTx(Qso &q, int parity, const char *text)
{
    char buf[36] = "";
    TEST_ASSERT_TRUE_MESSAGE(q.nextTx(parity, buf), text);
    TEST_ASSERT_EQUAL_STRING(text, buf);
}

static void expectNoTx(Qso &q, int parity)
{
    char buf[36] = "";
    TEST_ASSERT_FALSE_MESSAGE(q.nextTx(parity, buf), buf);
}

static void test_qso_cq_full_sequence()
{
    Qso q = makeQso();
    q.startCq(0);
    expectTx(q, 0, "CQ DL1ABC JO62");
    expectNoTx(q, 1);

    Decoded d = rx("DL1ABC K1XYZ FN42", -12);
    q.onDecoded(&d, 1, 1, 1000);
    expectTx(q, 0, "K1XYZ DL1ABC -12");

    d = rx("DL1ABC K1XYZ R-03", -10);
    q.onDecoded(&d, 1, 1, 1030);
    TEST_ASSERT_EQUAL(1, logged.size());
    expectTx(q, 0, "K1XYZ DL1ABC RR73");

    TEST_ASSERT_EQUAL_STRING("K1XYZ", logged[0].dx_call);
    TEST_ASSERT_EQUAL_STRING("FN42", logged[0].dx_grid);
    TEST_ASSERT_EQUAL(-3, logged[0].rst_rcvd);
    TEST_ASSERT_EQUAL(1000, logged[0].time_on);
    TEST_ASSERT_EQUAL(1030, logged[0].time_off);

    d = rx("DL1ABC K1XYZ 73");
    q.onDecoded(&d, 1, 1, 1060);
    // Back to calling CQ, and the QSO is logged only once
    expectTx(q, 0, "CQ DL1ABC JO62");
    TEST_ASSERT_EQUAL(1, logged.size());
}

static void test_qso_rr73_repeated_when_not_copied()
{
    Qso q = makeQso();
    q.startCq(1);
    Decoded d = rx("DL1ABC K1XYZ -08");  // caller skips the grid
    q.onDecoded(&d, 1, 0, 1000);
    expectTx(q, 1, "K1XYZ DL1ABC R-05");
    d = rx("DL1ABC K1XYZ R-08");  // unusual, but must not confuse the sequencer
    q.onDecoded(&d, 1, 0, 1030);
    expectTx(q, 1, "K1XYZ DL1ABC RR73");
    d = rx("DL1ABC K1XYZ R-08");  // our RR73 was lost
    q.onDecoded(&d, 1, 0, 1060);
    expectTx(q, 1, "K1XYZ DL1ABC RR73");
    TEST_ASSERT_EQUAL(1, logged.size());
    expectTx(q, 1, "CQ DL1ABC JO62");
}

static void test_qso_answer_cq()
{
    Qso q = makeQso();
    Decoded cq = rx("CQ K1XYZ FN42", -15);
    q.answer(cq, 0, 2000);
    expectNoTx(q, 0);
    expectTx(q, 1, "K1XYZ DL1ABC JO62");

    Decoded d = rx("DL1ABC K1XYZ -11", -14);
    q.onDecoded(&d, 1, 0, 2030);
    expectTx(q, 1, "K1XYZ DL1ABC R-14");

    d = rx("DL1ABC K1XYZ RR73");
    q.onDecoded(&d, 1, 0, 2060);
    TEST_ASSERT_EQUAL(1, logged.size());
    TEST_ASSERT_EQUAL(-11, logged[0].rst_rcvd);
    TEST_ASSERT_EQUAL(-14, logged[0].rst_sent);
    expectTx(q, 1, "K1XYZ DL1ABC 73");
    expectNoTx(q, 1);
    TEST_ASSERT_FALSE(q.active());
}

static void test_qso_ignores_other_stations_and_own_slot()
{
    Qso q = makeQso();
    Decoded cq = rx("CQ K1XYZ FN42");
    q.answer(cq, 0, 0);
    Decoded others[] = {rx("DL1ABC W9ZZZ -01"), rx("DL9QQQ K1XYZ -11"), rx("CQ F5AAA JN18")};
    q.onDecoded(others, 3, 0, 0);
    expectTx(q, 1, "K1XYZ DL1ABC JO62");
    // A matching message in our own transmit slot cannot be a reply
    Decoded d = rx("DL1ABC K1XYZ -11");
    q.onDecoded(&d, 1, 1, 0);
    expectTx(q, 1, "K1XYZ DL1ABC JO62");
}

static void test_qso_gives_up()
{
    Qso q = makeQso();
    Decoded cq = rx("CQ K1XYZ FN42");
    q.answer(cq, 0, 0);
    for (int i = 0; i < 3; ++i) {
        expectTx(q, 1, "K1XYZ DL1ABC JO62");
    }
    expectNoTx(q, 1);
    TEST_ASSERT_FALSE(q.active());

    q.startCq(0);
    for (int i = 0; i < 4; ++i) {
        expectTx(q, 0, "CQ DL1ABC JO62");
    }
    expectNoTx(q, 0);
    TEST_ASSERT_FALSE(q.active());
    TEST_ASSERT_EQUAL(0, logged.size());
}

static void test_qso_halt()
{
    Qso q = makeQso();
    q.startCq(0);
    q.halt();
    expectNoTx(q, 0);
}

static void test_reports()
{
    int r;
    bool has_r;
    TEST_ASSERT_TRUE(parseReport("-07", &r, &has_r));
    TEST_ASSERT_EQUAL(-7, r);
    TEST_ASSERT_FALSE(has_r);
    TEST_ASSERT_TRUE(parseReport("R+12", &r, &has_r));
    TEST_ASSERT_EQUAL(12, r);
    TEST_ASSERT_TRUE(has_r);
    TEST_ASSERT_FALSE(parseReport("RR73", &r, &has_r));
    TEST_ASSERT_FALSE(parseReport("JO62", &r, &has_r));
    TEST_ASSERT_FALSE(parseReport("73", &r, &has_r));
    TEST_ASSERT_FALSE(parseReport("", &r, &has_r));
    char buf[4];
    formatReport(-45, buf);
    TEST_ASSERT_EQUAL_STRING("-30", buf);
    formatReport(0, buf);
    TEST_ASSERT_EQUAL_STRING("+00", buf);
}

// --- ADIF ------------------------------------------------------------------

static void test_adif()
{
    QsoConfig cfg;
    strcpy(cfg.my_call, "DL1ABC");
    strcpy(cfg.my_grid, "JO62");
    QsoRecord rec{};
    strcpy(rec.dx_call, "K1XYZ");
    strcpy(rec.dx_grid, "FN42");
    rec.rst_sent = -12;
    rec.rst_rcvd = 3;
    rec.time_on = 1791627300;   // 2026-10-10 10:15:00 UTC
    rec.time_off = 1791627345;
    rec.audio_freq = 1500;
    char line[400];
    formatAdif(rec, cfg, 14080000, line, sizeof(line), Protocol::FT4);
    TEST_ASSERT_NOT_NULL(strstr(line, "<mode:4>MFSK <submode:3>FT4 "));
    // FT4 on 40 m sits on half a kilohertz
    formatAdif(rec, cfg, 7047500, line, sizeof(line), Protocol::FT4);
    TEST_ASSERT_NOT_NULL(strstr(line, "<band:3>40m <freq:8>7.049000 "));
    formatAdif(rec, cfg, 27265000, line, sizeof(line));
    TEST_ASSERT_NOT_NULL(strstr(line, "<band:3>11m <freq:9>27.266500 "));
    formatAdif(rec, cfg, 0, line, sizeof(line));
    TEST_ASSERT_NULL(strstr(line, "<freq"));
    size_t n = formatAdif(rec, cfg, 14074000, line, sizeof(line));
    TEST_ASSERT_EQUAL(strlen(line), n);
    TEST_ASSERT_EQUAL_STRING(
        "<call:5>K1XYZ <gridsquare:4>FN42 <mode:3>FT8 <rst_sent:3>-12 <rst_rcvd:3>+03 "
        "<qso_date:8>20261010 <time_on:6>101500 <qso_date_off:8>20261010 <time_off:6>101545 "
        "<band:3>20m <freq:9>14.075500 <distance:4>6043 <station_callsign:6>DL1ABC <my_gridsquare:4>JO62 <eor>\n",
        line);
    TEST_ASSERT_NULL(bandName(13000));
    TEST_ASSERT_EQUAL_STRING("40m", bandName(7074));
    TEST_ASSERT_EQUAL_STRING("11m", bandName(27265));
}

static void test_grid_from_position()
{
    char g[5];
    TEST_ASSERT_TRUE(gridFromLatLon(52.52, 13.40, g));  // Berlin
    TEST_ASSERT_EQUAL_STRING("JO62", g);
    TEST_ASSERT_TRUE(gridFromLatLon(48.14, 11.58, g));  // Munich
    TEST_ASSERT_EQUAL_STRING("JN58", g);
    TEST_ASSERT_TRUE(gridFromLatLon(41.71, -72.73, g));  // Newington CT, ARRL
    TEST_ASSERT_EQUAL_STRING("FN31", g);
    TEST_ASSERT_TRUE(gridFromLatLon(-33.87, 151.21, g));  // Sydney
    TEST_ASSERT_EQUAL_STRING("QF56", g);
    TEST_ASSERT_TRUE(gridFromLatLon(-90.0, -180.0, g));
    TEST_ASSERT_EQUAL_STRING("AA00", g);
    TEST_ASSERT_TRUE(gridFromLatLon(90.0, 180.0, g));
    TEST_ASSERT_EQUAL_STRING("RR99", g);
    TEST_ASSERT_FALSE(gridFromLatLon(91.0, 0.0, g));
}

static Decoded parsed(const char *text)
{
    Decoded d{};
    strncpy(d.text, text, sizeof(d.text) - 1);
    parseMessage(d);
    return d;
}

static void test_qso_window()
{
    // What we send ourselves is read like a received message
    Decoded cq = parsed("CQ DL1ABC JO62");
    TEST_ASSERT_TRUE(cq.is_cq);
    TEST_ASSERT_EQUAL_STRING("DL1ABC", cq.call_de);
    Decoded report = parsed("K1XYZ DL1ABC -05");
    TEST_ASSERT_EQUAL_STRING("K1XYZ", report.call_to);
    TEST_ASSERT_EQUAL_STRING("-05", report.extra);

    TEST_ASSERT_TRUE(belongsToQso(cq, true, "DL1ABC", ""));
    TEST_ASSERT_TRUE(belongsToQso(parsed("DL1ABC K1XYZ FN42"), false, "DL1ABC", ""));        // to us
    TEST_ASSERT_TRUE(belongsToQso(parsed("PA3XYZ K1XYZ RR73"), false, "DL1ABC", "K1XYZ"));  // from the partner
    TEST_ASSERT_TRUE(belongsToQso(parsed("K1XYZ G4ABC JO01"), false, "DL1ABC", "K1XYZ"));   // to the partner
    TEST_ASSERT_FALSE(belongsToQso(parsed("CQ G4ABC JO01"), false, "DL1ABC", "K1XYZ"));
    TEST_ASSERT_FALSE(belongsToQso(parsed("CQ G4ABC JO01"), false, "DL1ABC", ""));
    // No callsign set: nothing counts as addressed to us
    TEST_ASSERT_FALSE(belongsToQso(parsed("CQ G4ABC JO01"), false, "", ""));
}

static void test_distance_and_cq_addition()
{
    TEST_ASSERT_EQUAL(0, gridDistanceKm("JO62", "JO62"));
    // Berlin to the middle of FN42 (Boston) is a little over 6000 km
    TEST_ASSERT_INT_WITHIN(60, 6040, gridDistanceKm("JO62", "FN42"));
    TEST_ASSERT_INT_WITHIN(15, 111, gridDistanceKm("JO62", "JO63"));       // one degree of latitude
    TEST_ASSERT_INT_WITHIN(3, gridDistanceKm("JO62QM", "JO63QM"), 111);
    TEST_ASSERT_EQUAL(-1, gridDistanceKm("JO62", ""));
    TEST_ASSERT_EQUAL(-1, gridDistanceKm("RR73", "JO62"));

    // A CQ with an addition is still a CQ from the same station
    Decoded d = parsed("CQ POTA DL1ABC JO62");
    TEST_ASSERT_TRUE(d.is_cq);
    TEST_ASSERT_EQUAL_STRING("DL1ABC", d.call_de);
    TEST_ASSERT_EQUAL_STRING("JO62", d.extra);

    QsoConfig cfg;
    strcpy(cfg.my_call, "DL1ABC");
    strcpy(cfg.my_grid, "JO62");
    strcpy(cfg.cq_tag, "SOTA");
    Qso qso;
    qso.configure(cfg);
    qso.startCq(0);
    char text[36];
    Packing packing;
    TEST_ASSERT_TRUE(qso.nextTx(0, text, &packing));
    TEST_ASSERT_EQUAL_STRING("CQ SOTA DL1ABC JO62", text);
}

static void test_band_plan()
{
    TEST_ASSERT_EQUAL(12, bandPresetCount(BandUse::FT8, false));
    TEST_ASSERT_EQUAL(10, bandPresetCount(BandUse::FT4, false));
    TEST_ASSERT_EQUAL(1, bandPresetCount(BandUse::FT8, true));
    TEST_ASSERT_EQUAL_UINT32(27265000, bandPreset(BandUse::FT4, true, 0).hz);
    // CB operation only concerns FT8 and FT4
    TEST_ASSERT_EQUAL(bandPresetCount(BandUse::CW, false), bandPresetCount(BandUse::CW, true));

    // Every amateur frequency lies in the band it is named after
    for (BandUse use : {BandUse::FT8, BandUse::FT4, BandUse::CW, BandUse::RTTY, BandUse::SSTV}) {
        for (int i = 0; i < bandPresetCount(use, false); ++i) {
            const BandPreset &p = bandPreset(use, false, i);
            if (strstr(p.label, " m") == nullptr) {
                continue;  // a station, not a band
            }
            const char *name = bandName(p.hz / 1000);
            TEST_ASSERT_NOT_NULL_MESSAGE(name, p.label);
            char spaced[8];
            snprintf(spaced, sizeof(spaced), "%.*s m", (int)strlen(name) - 1, name);
            TEST_ASSERT_EQUAL_STRING(p.label, spaced);
        }
    }
    // Every list has entries, and none of them an empty frequency
    for (BandUse use : {BandUse::FT8, BandUse::FT4, BandUse::CW, BandUse::RTTY, BandUse::NAVTEX, BandUse::WEFAX, BandUse::SSTV}) {
        TEST_ASSERT_GREATER_THAN(0, bandPresetCount(use, false));
        for (int i = 0; i < bandPresetCount(use, false); ++i) {
            TEST_ASSERT_GREATER_THAN_UINT32(100000, bandPreset(use, false, i).hz);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(518000, bandPreset(BandUse::NAVTEX, false, 0).hz);

    char text[12];
    formatMhz(14074000, text);
    TEST_ASSERT_EQUAL_STRING("14.074", text);
    formatMhz(7047500, text);
    TEST_ASSERT_EQUAL_STRING("7.0475", text);
    formatMhz(144174000, text);
    TEST_ASSERT_EQUAL_STRING("144.174", text);
    formatKhz(518000, text);
    TEST_ASSERT_EQUAL_STRING("518", text);
    formatKhz(13882500, text);
    TEST_ASSERT_EQUAL_STRING("13882.5", text);
}

static void test_adif_field()
{
    const char *rec = "<call:5>K1XYZ <gridsquare:4>FN42 <MODE:3>FT8 <rst_sent:3>-12 <band:3>20m <eor>\n";
    char v[16];
    TEST_ASSERT_TRUE(adifField(rec, "call", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("K1XYZ", v);
    TEST_ASSERT_TRUE(adifField(rec, "mode", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("FT8", v);
    TEST_ASSERT_TRUE(adifField(rec, "rst_sent", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("-12", v);
    TEST_ASSERT_FALSE(adifField(rec, "rst", v, sizeof(v)));  // prefix of another field name
    TEST_ASSERT_EQUAL_STRING("", v);
    char tiny[3];
    TEST_ASSERT_TRUE(adifField(rec, "call", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("K1", tiny);
    TEST_ASSERT_TRUE(adifField("<call:9>AB", "call", v, sizeof(v)));  // truncated file
    TEST_ASSERT_EQUAL_STRING("AB", v);
}

static void test_country()
{
    struct Case {
        const char *call;
        const char *country;
    } cases[] = {
        {"DL1ABC", "Germany"},      {"DO7XYZ", "Germany"},     {"K1XYZ", "USA"},          {"W4FGA", "USA"},
        {"N2AB", "USA"},            {"AA7A", "USA"},           {"KH6XX", "Hawaii"},       {"KL7AA", "Alaska"},
        {"G4ABC", "England"},       {"GM0XYZ", "Scotland"},    {"GW4ABC", "Wales"},       {"GJ0KYZ", "Jersey"},
        {"M0ABC", "England"},       {"2E0ABC", "England"},     {"F8PKC", "France"},       {"TK4LS", "Corsica"},
        {"EA3UV", "Spain"},         {"EA8ABC", "Canary Is."},  {"I2ABC", "Italy"},        {"IK0YVV", "Italy"},
        {"IQ8DO", "Italy"},         {"IT9ABC", "Sicily"},      {"IS0ABC", "Sardinia"},    {"JA2GQT", "Japan"},
        {"JH1AJT", "Japan"},        {"7K1ABC", "Japan"},       {"LY5A", "Lithuania"},     {"PA3EPP", "Netherlands"},
        {"PE1BSA", "Netherlands"},  {"SP7XIF", "Poland"},      {"SQ8OHR", "Poland"},      {"SM7HZK", "Sweden"},
        {"SV1GN", "Greece"},        {"SV9ABC", "Crete"},       {"OH3NIV", "Finland"},     {"OH0ABC", "Aland Is."},
        {"ZS6S", "South Africa"},   {"TA6CQ", "Turkey"},       {"LZ1JZ", "Bulgaria"},     {"YC1MRF", "Indonesia"},
        {"VK2ABC", "Australia"},    {"ZL1ABC", "New Zealand"}, {"PY2ABC", "Brazil"},      {"LU1ABC", "Argentina"},
        {"VE3ABC", "Canada"},       {"BV2ABC", "Taiwan"},      {"BG7ABC", "China"},       {"HB9ABC", "Switzerland"},
        {"HB0ABC", "Liechtenstein"}, {"OE1ABC", "Austria"},    {"ON4ABC", "Belgium"},     {"OZ1ABC", "Denmark"},
        {"R6WA", "Eu. Russia"},     {"RK9AX", "As. Russia"},   {"RA0ANO", "As. Russia"},  {"UA6HI", "Eu. Russia"},
        {"R3KCW", "Eu. Russia"},    {"RL1B", "Eu. Russia"},    {"UA2FAA", "Kaliningrad"}, {"RY8CAA", "As. Russia"},
        {"UR5ABC", "Ukraine"},      {"UN7ABC", "Kazakhstan"},  {"EA8/DL1ABC", "Canary Is."}, {"DL1ABC/P", "Germany"},
        {"DL1ABC/OH0", "Aland Is."}, {"K1XYZ/7", "USA"},       {"dl1abc", "Germany"},     {"Q1ABC", ""},
        {"X", ""},                  {"", ""},
    };
    for (const auto &c : cases) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.country, countryForCall(c.call), c.call);
    }
}

// --- CB mode -----------------------------------------------------------------

static void test_cb_callsigns()
{
    // The table of the WSJT-CB documentation
    const char *valid[] = {"1A1", "1TT1", "1TT01", "1TT001", "1TT1000", "1AT1000", "11TT1", "111TT11", "111TT999",
                           "26AT715", "26AT016", "13AT123", "999ZZ999", "999ZZ/ZZ"};
    const char *invalid[] = {"26AT1000", "111TT1000", "99Z9999", "1TT10000", "1TT", "1AT", "AT1000", "AAA", "123",
                             "12A", "12ABC1", "ABC123", "1/AT100", "DL1ABC", "K1XYZ", "", "26AT/D", "26AT/DXX"};
    for (const char *c : valid) {
        TEST_ASSERT_TRUE_MESSAGE(isCbCallsign(c), c);
    }
    for (const char *c : invalid) {
        TEST_ASSERT_FALSE_MESSAGE(isCbCallsign(c), c);
    }
}

static void test_cb_countries()
{
    TEST_ASSERT_EQUAL_STRING("Italy", cbCountryForCall("1AT106"));
    TEST_ASSERT_EQUAL_STRING("Italy", cbCountryForCall("001AB123"));
    TEST_ASSERT_EQUAL_STRING("U.S.A.", cbCountryForCall("2AT001"));
    TEST_ASSERT_EQUAL_STRING("Germany", cbCountryForCall("13AT123"));
    TEST_ASSERT_EQUAL_STRING("France", cbCountryForCall("14FB005"));
    TEST_ASSERT_EQUAL_STRING("Netherlands", cbCountryForCall("19AT/DX"));
    TEST_ASSERT_EQUAL_STRING("England", cbCountryForCall("26AT016"));
    TEST_ASSERT_EQUAL_STRING("Spain", cbCountryForCall("30AT084"));
    TEST_ASSERT_EQUAL_STRING("Poland", cbCountryForCall("161AT001"));
    TEST_ASSERT_EQUAL_STRING("", cbCountryForCall("7AT001"));    // not assigned
    TEST_ASSERT_EQUAL_STRING("", cbCountryForCall("999ZZ999"));
    TEST_ASSERT_EQUAL_STRING("", cbCountryForCall("DL1ABC"));
    TEST_ASSERT_EQUAL(26, cbDivision("26AT016"));
    TEST_ASSERT_EQUAL(0, cbDivision("AT016"));
}

static std::string cbRoundtrip(const char *text, Packing packing)
{
    Synth synth;
    TEST_ASSERT_TRUE_MESSAGE(synth.begin(text, 1200.0f, 0.5f, Protocol::FT8, 12000, packing), text);
    std::vector<int16_t> pcm(15 * 12000, 0);
    size_t pos = 6000;
    while (!synth.done()) {
        pos += synth.read(pcm.data() + pos, 500);
    }
    auto decoded = decodePcm(pcm);
    TEST_ASSERT_EQUAL_MESSAGE(1, decoded.size(), text);
    return decoded[0].text;
}

static void test_cb_messages_on_the_air()
{
    hashClear();
    // CQ carries the callsign in clear, readable by anyone
    TEST_ASSERT_EQUAL_STRING("CQ 26AT016", cbRoundtrip("CQ 26AT016", Packing::NonStandard).c_str());
    TEST_ASSERT_EQUAL_STRING("CQ 999ZZ/ZZ", cbRoundtrip("CQ 999ZZ/ZZ", Packing::NonStandard).c_str());
    // The call to a station: its callsign as hash, ours in clear
    TEST_ASSERT_EQUAL_STRING("<26AT016> 13AT123", cbRoundtrip("<26AT016> 13AT123", Packing::NonStandard).c_str());
    // Report, roger and sign-off are free text
    TEST_ASSERT_EQUAL_STRING("13AT123 -07", cbRoundtrip("13AT123 -07", Packing::FreeText).c_str());
    TEST_ASSERT_EQUAL_STRING("26AT016 R+12", cbRoundtrip("26AT016 R+12", Packing::FreeText).c_str());
    TEST_ASSERT_EQUAL_STRING("13AT123 RR73", cbRoundtrip("13AT123 RR73", Packing::FreeText).c_str());
    TEST_ASSERT_EQUAL_STRING("999ZZ999 R-07", cbRoundtrip("999ZZ999 R-07", Packing::FreeText).c_str());

    // A station that never heard 26AT016 cannot resolve the hash ...
    hashClear();
    Synth synth;
    TEST_ASSERT_TRUE(synth.begin("<26AT016> 13AT123", 1200.0f, 0.5f, Protocol::FT8, 12000, Packing::NonStandard));
    std::vector<int16_t> pcm(15 * 12000, 0);
    size_t pos = 6000;
    while (!synth.done()) {
        pos += synth.read(pcm.data() + pos, 500);
    }
    hashClear();
    TEST_ASSERT_EQUAL_STRING("<...> 13AT123", decodePcm(pcm)[0].text);
    // ... but the station called can, once it has entered its own callsign
    hashClear();
    rememberCall("26AT016");
    auto decoded = decodePcm(pcm);
    TEST_ASSERT_EQUAL_STRING("<26AT016> 13AT123", decoded[0].text);
    TEST_ASSERT_EQUAL_STRING("26AT016", decoded[0].call_to);
    TEST_ASSERT_EQUAL_STRING("13AT123", decoded[0].call_de);

    // Too long for free text
    TEST_ASSERT_FALSE(synth.begin("999ZZ999 R-07 X", 1200.0f, 0.5f, Protocol::FT8, 12000, Packing::FreeText));
}

static void test_cb_parse()
{
    Decoded d{};
    strcpy(d.text, "26AT016 -07");
    parseMessage(d);
    TEST_ASSERT_EQUAL_STRING("26AT016", d.call_to);
    TEST_ASSERT_EQUAL_STRING("", d.call_de);
    TEST_ASSERT_EQUAL_STRING("-07", d.extra);
    strcpy(d.text, "26AT016 RR73");
    parseMessage(d);
    TEST_ASSERT_EQUAL_STRING("", d.call_de);
    TEST_ASSERT_EQUAL_STRING("RR73", d.extra);
    strcpy(d.text, "CQ 13AT123");
    parseMessage(d);
    TEST_ASSERT_TRUE(d.is_cq);
    TEST_ASSERT_EQUAL_STRING("13AT123", d.call_de);
    strcpy(d.text, "<...> 13AT123");
    parseMessage(d);
    TEST_ASSERT_EQUAL_STRING("...", d.call_to);
    TEST_ASSERT_EQUAL_STRING("13AT123", d.call_de);
    // Two amateur callsigns without anything else stay two callsigns
    strcpy(d.text, "DL1ABC K1XYZ");
    parseMessage(d);
    TEST_ASSERT_EQUAL_STRING("K1XYZ", d.call_de);
}

static Qso makeCbQso()
{
    Qso q;
    QsoConfig cfg;
    strcpy(cfg.my_call, "26AT016");
    cfg.max_retries = 3;
    cfg.max_cq = 4;
    cfg.cb_mode = true;
    q.configure(cfg);
    q.onComplete(logFn, nullptr);
    logged.clear();
    return q;
}

static void expectCbTx(Qso &q, int parity, const char *text, Packing packing)
{
    char buf[36] = "";
    Packing how = Packing::Auto;
    TEST_ASSERT_TRUE_MESSAGE(q.nextTx(parity, buf, &how), text);
    TEST_ASSERT_EQUAL_STRING(text, buf);
    TEST_ASSERT_EQUAL_MESSAGE((int)packing, (int)how, text);
}

static void test_cb_qso_after_cq()
{
    Qso q = makeCbQso();
    q.startCq(0);
    expectCbTx(q, 0, "CQ 26AT016", Packing::NonStandard);

    Decoded d = rx("<26AT016> 13AT123", -9);
    q.onDecoded(&d, 1, 1, 1000);
    expectCbTx(q, 0, "13AT123 -09", Packing::FreeText);

    d = rx("26AT016 R-04", -8);  // names only us; taken as coming from 13AT123
    q.onDecoded(&d, 1, 1, 1030);
    TEST_ASSERT_EQUAL(1, logged.size());
    TEST_ASSERT_EQUAL_STRING("13AT123", logged[0].dx_call);
    TEST_ASSERT_EQUAL(-9, logged[0].rst_sent);
    TEST_ASSERT_EQUAL(-4, logged[0].rst_rcvd);
    TEST_ASSERT_EQUAL_STRING("", logged[0].dx_grid);
    expectCbTx(q, 0, "13AT123 RR73", Packing::FreeText);

    // Our RR73 was not copied: the report comes again and is answered again, logged once
    d = rx("26AT016 R-04");
    q.onDecoded(&d, 1, 1, 1060);
    expectCbTx(q, 0, "13AT123 RR73", Packing::FreeText);
    d = rx("26AT016 73");
    q.onDecoded(&d, 1, 1, 1090);
    expectCbTx(q, 0, "CQ 26AT016", Packing::NonStandard);
    TEST_ASSERT_EQUAL(1, logged.size());
}

static void test_cb_qso_answering_cq()
{
    Qso q = makeCbQso();
    Decoded cq = rx("CQ 30AT084", -15);
    q.answer(cq, 0, 2000);
    expectCbTx(q, 1, "<30AT084> 26AT016", Packing::NonStandard);

    Decoded d = rx("26AT016 -11", -14);
    q.onDecoded(&d, 1, 0, 2030);
    expectCbTx(q, 1, "30AT084 R-14", Packing::FreeText);

    d = rx("26AT016 RR73");
    q.onDecoded(&d, 1, 0, 2060);
    TEST_ASSERT_EQUAL(1, logged.size());
    TEST_ASSERT_EQUAL(-11, logged[0].rst_rcvd);
    expectCbTx(q, 1, "30AT084 73", Packing::FreeText);
    TEST_ASSERT_FALSE(q.active());
}

static void test_cb_reports_need_cb_mode_and_a_contact()
{
    // Outside CB mode a report without sender is not attributed to anybody
    Qso ham = makeQso();
    Decoded cq = rx("CQ K1XYZ FN42");
    ham.answer(cq, 0, 0);
    Decoded d = rx("DL1ABC -11");
    ham.onDecoded(&d, 1, 0, 0);
    expectTx(ham, 1, "K1XYZ DL1ABC JO62");

    // In CB mode, while only calling CQ, such a report starts nothing
    Qso q = makeCbQso();
    q.startCq(0);
    d = rx("26AT016 -11");
    q.onDecoded(&d, 1, 1, 0);
    expectCbTx(q, 0, "CQ 26AT016", Packing::NonStandard);
    TEST_ASSERT_EQUAL(0, logged.size());
}

// --- CW ----------------------------------------------------------------------

static uint32_t rng_state = 12345;
static float noiseSample()
{
    // Sum of uniform values, close enough to Gaussian for these tests
    float acc = 0;
    for (int i = 0; i < 6; ++i) {
        rng_state = rng_state * 1664525u + 1013904223u;
        acc += (float)(rng_state >> 8) / 16777216.0f - 0.5f;
    }
    return acc * 1.414f;  // unit variance
}

static void addNoise(std::vector<int16_t> &pcm, float rms)
{
    for (auto &s : pcm) {
        float v = s + rms * noiseSample();
        s = (int16_t)std::max(-32768.0f, std::min(32767.0f, v));
    }
}

static std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \n");
    size_t b = s.find_last_not_of(" \n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::vector<int16_t> keyCw(const char *text, int wpm, float tone, float amp = 0.3f)
{
    digi::CwKeyer keyer;
    keyer.begin(wpm, tone, amp);
    std::vector<int16_t> pcm(6000, 0);  // half a second of silence first
    const char *p = text;
    while (*p || keyer.busy()) {
        if (*p) {
            p += keyer.send(p);
        }
        int16_t buf[400];
        size_t n = keyer.read(buf, 400);
        pcm.insert(pcm.end(), buf, buf + n);
        if (n == 0) {
            break;
        }
    }
    pcm.insert(pcm.end(), 12000, 0);  // lets the decoder close the last character
    return pcm;
}

static std::string decodeCw(const std::vector<int16_t> &pcm, int *wpm = nullptr, float *tone = nullptr, float centre = 700.0f)
{
    digi::CwDecoder dec;
    dec.begin();
    dec.setWindow(centre);
    std::string text;
    for (size_t pos = 0; pos < pcm.size();) {
        size_t n = std::min<size_t>(333, pcm.size() - pos);
        dec.feed(pcm.data() + pos, n);
        pos += n;
        char buf[32];
        size_t got;
        while ((got = dec.read(buf, sizeof(buf))) > 0) {
            text.append(buf, got);
        }
    }
    if (wpm) {
        *wpm = dec.wpm();
    }
    if (tone) {
        *tone = dec.tone();
    }
    return trim(text);
}

static void test_cw_roundtrip()
{
    const char *text = "CQ CQ DE DL1ABC DL1ABC K";
    // The window is centred on 'centre'; the signal may sit anywhere inside it
    struct Case {
        int wpm;
        float centre;
        float tone;
        float noise_rms;
    } cases[] = {
        {20, 700, 700, 0},    {12, 600, 600, 0},    {30, 800, 830, 0},    {40, 550, 510, 0},
        {8, 950, 950, 0},     {20, 700, 740, 1500}, {25, 640, 660, 2500}, {18, 700, 655, 6000},
    };
    for (const auto &c : cases) {
        auto pcm = keyCw(text, c.wpm, c.tone);
        if (c.noise_rms > 0) {
            addNoise(pcm, c.noise_rms);
        }
        int wpm = 0;
        float tone = 0;
        std::string got = decodeCw(pcm, &wpm, &tone, c.centre);
        char label[64];
        snprintf(label, sizeof(label), "%d WPM, %.0f Hz in window at %.0f, noise %.0f", c.wpm, c.tone, c.centre, c.noise_rms);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(text, got.c_str(), label);
        TEST_ASSERT_INT_WITHIN_MESSAGE(std::max(2, c.wpm / 8), c.wpm, wpm, label);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(15.0f, c.tone, tone, label);
    }
}

static std::vector<int16_t> mix(const std::vector<int16_t> &a, const std::vector<int16_t> &b)
{
    std::vector<int16_t> out(std::max(a.size(), b.size()), 0);
    for (size_t i = 0; i < out.size(); ++i) {
        int v = (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        out[i] = (int16_t)std::max(-32768, std::min(32767, v));
    }
    return out;
}

static void test_cw_window_ignores_other_signals()
{
    // A station outside the window produces no text, however strong
    TEST_ASSERT_EQUAL_STRING("", decodeCw(keyCw("CQ CQ DE K1XYZ", 20, 1000, 0.5f), nullptr, nullptr, 700).c_str());
    TEST_ASSERT_EQUAL_STRING("", decodeCw(keyCw("CQ CQ DE K1XYZ", 20, 450, 0.5f), nullptr, nullptr, 700).c_str());

    // The wanted station is read although a stronger one sends 250 Hz away at another speed
    const char *wanted = "CQ CQ DE DL1ABC DL1ABC K";
    auto pcm = mix(keyCw(wanted, 20, 710, 0.2f), keyCw("TEST TEST DE K1XYZ K1XYZ TEST TEST DE K1XYZ", 27, 960, 0.4f));
    float tone = 0;
    TEST_ASSERT_EQUAL_STRING(wanted, decodeCw(pcm, nullptr, &tone, 700).c_str());
    TEST_ASSERT_FLOAT_WITHIN(15.0f, 710.0f, tone);
}

static void test_cw_clicks_and_fixed_tone()
{
    // Broadband clicks, as from switching or static, are not keying
    std::vector<int16_t> clicks(12000 * 8, 0);
    for (size_t i = 3000; i < clicks.size(); i += 2700) {
        clicks[i] = 30000;
        clicks[i + 1] = -22000;
        clicks[i + 2] = 9000;
    }
    addNoise(clicks, 300);
    TEST_ASSERT_EQUAL_STRING("", decodeCw(clicks).c_str());

    // With tracking off the decoder stays on the centre and still reads a signal there
    digi::CwDecoder dec;
    dec.begin();
    dec.setWindow(650);
    dec.setTracking(false);
    auto pcm = keyCw("TEST DE DL1ABC", 20, 650);
    dec.feed(pcm.data(), pcm.size());
    char buf[64] = {0};
    dec.read(buf, sizeof(buf) - 1);
    TEST_ASSERT_EQUAL_STRING("TEST DE DL1ABC", trim(buf).c_str());
    TEST_ASSERT_EQUAL_FLOAT(650.0f, dec.tone());
}

static void test_cw_punctuation_and_digits()
{
    const char *text = "RST 599 5NN = NAME HANS ? QTH JO62/P + 73";
    TEST_ASSERT_EQUAL_STRING(text, decodeCw(keyCw(text, 22, 700)).c_str());
}

static void test_cw_silence_and_noise_give_no_text()
{
    std::vector<int16_t> pcm(12000 * 60, 0);
    TEST_ASSERT_EQUAL_STRING("", decodeCw(pcm).c_str());
    addNoise(pcm, 3000);
    // Noise may trigger a stray element now and then, but no stream of characters
    TEST_ASSERT_LESS_OR_EQUAL(3, decodeCw(pcm).size());
}

static void test_cw_keyer()
{
    TEST_ASSERT_EQUAL_STRING(".-", digi::morseFor('a'));
    TEST_ASSERT_NULL(digi::morseFor('#'));
    digi::CwKeyer keyer;
    keyer.begin(20, 700, 0.5f);
    TEST_ASSERT_FALSE(keyer.busy());
    TEST_ASSERT_EQUAL(1, keyer.send("#E#"));  // characters without code are dropped
    std::vector<int16_t> pcm(12000);
    size_t n = keyer.read(pcm.data(), pcm.size());
    // One dot of 60 ms, three dots of character gap, plus the falling edge
    TEST_ASSERT_INT_WITHIN(100, 4 * 720, n);
    TEST_ASSERT_FALSE(keyer.busy());
    int peak = 0;
    for (size_t i = 0; i < n; ++i) {
        peak = std::max(peak, abs(pcm[i]));
    }
    TEST_ASSERT_INT_WITHIN(300, 16384, peak);
    TEST_ASSERT_INT_WITHIN(60, 0, pcm[0]);
}

// --- RTTY --------------------------------------------------------------------

static std::vector<int16_t> sendRtty(const char *text, const digi::RttyConfig &cfg, float amp = 0.3f)
{
    digi::RttyEncoder enc;
    enc.begin(cfg, amp);
    std::vector<int16_t> pcm(3000, 0);
    const char *p = text;
    for (;;) {
        if (*p) {
            p += enc.send(p);
        }
        int16_t buf[500];
        size_t n = enc.read(buf, 500);
        pcm.insert(pcm.end(), buf, buf + n);
        if (n == 0 && !*p) {
            break;
        }
    }
    pcm.insert(pcm.end(), 6000, 0);
    return pcm;
}

static std::string receiveRtty(const std::vector<int16_t> &pcm, const digi::RttyConfig &cfg, int *quality = nullptr)
{
    digi::RttyDecoder dec;
    TEST_ASSERT_TRUE(dec.begin(cfg));
    std::string text;
    for (size_t pos = 0; pos < pcm.size();) {
        size_t n = std::min<size_t>(411, pcm.size() - pos);
        dec.feed(pcm.data() + pos, n);
        pos += n;
        char buf[32];
        size_t got;
        while ((got = dec.read(buf, sizeof(buf))) > 0) {
            text.append(buf, got);
        }
    }
    if (quality) {
        *quality = dec.quality();
    }
    return trim(text);
}

static void test_rtty_roundtrip()
{
    const char *text = "CQ CQ DE DL1ABC DL1ABC PSE K";
    digi::RttyConfig cfg;
    int quality = 0;
    TEST_ASSERT_EQUAL_STRING(text, receiveRtty(sendRtty(text, cfg), cfg, &quality).c_str());
    TEST_ASSERT_GREATER_THAN(90, quality);

    auto noisy = sendRtty(text, cfg);
    addNoise(noisy, 4000);  // about 5 dB signal to noise in the full audio bandwidth
    TEST_ASSERT_EQUAL_STRING(text, receiveRtty(noisy, cfg).c_str());

    // Other tones, as with a different receiver setting
    digi::RttyConfig low;
    low.mark_hz = 915;
    low.space_hz = 1085;
    TEST_ASSERT_EQUAL_STRING(text, receiveRtty(sendRtty(text, low), low).c_str());

    // 50 baud, used by commercial and weather stations
    digi::RttyConfig fifty;
    fifty.baud = 50;
    fifty.space_hz = fifty.mark_hz + 450;
    TEST_ASSERT_EQUAL_STRING(text, receiveRtty(sendRtty(text, fifty), fifty).c_str());
}

static void test_rtty_figures_and_lines()
{
    digi::RttyConfig cfg;
    const char *text = "UR RST 599 599 QTH BERLIN, JO62. WX: 12C\nNR 001/A 73";
    TEST_ASSERT_EQUAL_STRING(text, receiveRtty(sendRtty(text, cfg), cfg).c_str());
    // Lower case is sent as upper case, characters without a code are dropped
    TEST_ASSERT_EQUAL_STRING("TEST 1", receiveRtty(sendRtty("te{st} 1", cfg), cfg).c_str());
}

static void test_rtty_reverse()
{
    digi::RttyConfig tx, rx;
    rx.reverse = true;
    const char *text = "RYRYRY THE QUICK BROWN FOX";
    // Received on the wrong sideband the text is garbage ...
    TEST_ASSERT_TRUE(receiveRtty(sendRtty(text, tx), rx) != text);
    // ... and correct again when both sides agree
    tx.reverse = true;
    TEST_ASSERT_EQUAL_STRING(text, receiveRtty(sendRtty(text, tx), rx).c_str());
}

static void test_rtty_noise_gives_little_text()
{
    digi::RttyConfig cfg;
    std::vector<int16_t> pcm(12000 * 10, 0);
    TEST_ASSERT_EQUAL_STRING("", receiveRtty(pcm, cfg).c_str());
}

// --- NAVTEX ------------------------------------------------------------------

// Builds the stream of 7-bit codes of a SITOR-B broadcast: phasing, then the text
// with every character repeated five positions later. damage lists positions whose
// code is destroyed, as a burst of interference would.
static std::vector<uint8_t> sitorCodes(const char *text, const std::vector<int> &damage = {})
{
    using namespace digi::ccir476;
    std::vector<uint8_t> dx;
    const int phasing = 14;
    for (int i = 0; i < phasing; ++i) {
        dx.push_back(kRep);
    }
    bool figures = false;
    dx.push_back(kLtrs);
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') {
            dx.push_back(kCr);
            dx.push_back(kLf);
            continue;
        }
        bool fig = false;
        uint8_t code = encode(*p, &fig);
        if (code == 0) {
            continue;
        }
        if (code != kSpace && fig != figures) {
            dx.push_back(fig ? kFigs : kLtrs);
            figures = fig;
        }
        dx.push_back(code);
    }
    for (int i = 0; i < 6; ++i) {
        dx.push_back(kAlpha);  // idle, so that the last characters get their repeat
    }
    std::vector<uint8_t> stream;
    for (int k = 0; k < (int)dx.size(); ++k) {
        stream.push_back(dx[k]);
        stream.push_back(k - 2 >= phasing ? dx[k - 2] : kAlpha);
    }
    for (int pos : damage) {
        stream[pos] = 0x7F;  // not a valid code
    }
    return stream;
}

static std::vector<int16_t> sitorAudio(const std::vector<uint8_t> &codes, float center = 1000.0f, float amp = 0.3f)
{
    digi::FskMod mod;
    mod.begin(center + 85.0f, center - 85.0f, 100.0f, amp);
    std::vector<int16_t> pcm(4000, 0);
    int16_t buf[130];
    for (uint8_t code : codes) {
        for (int b = 0; b < 7; ++b) {
            size_t n = mod.emitBit((code >> b) & 1, buf);
            pcm.insert(pcm.end(), buf, buf + n);
        }
    }
    pcm.insert(pcm.end(), 6000, 0);
    return pcm;
}

static std::string receiveNavtex(const std::vector<int16_t> &pcm, const digi::NavtexConfig &cfg = {})
{
    digi::NavtexDecoder dec;
    TEST_ASSERT_TRUE(dec.begin(cfg));
    std::string text;
    for (size_t pos = 0; pos < pcm.size();) {
        size_t n = std::min<size_t>(397, pcm.size() - pos);
        dec.feed(pcm.data() + pos, n);
        pos += n;
        char buf[64];
        size_t got;
        while ((got = dec.read(buf, sizeof(buf))) > 0) {
            text.append(buf, got);
        }
    }
    return trim(text);
}

static const char kNavtexMessage[] = "ZCZC EA01\n101200 UTC OCT 26\nGALE WARNING NR 123\nGERMAN BIGHT: W 8, GUSTS 10.\nNNNN";

static void test_ccir476_table()
{
    using namespace digi::ccir476;
    // Every code has four of seven bits set, and no two characters share one
    bool seen[128] = {false};
    for (char c = 'A'; c <= 'Z'; ++c) {
        bool fig = true;
        uint8_t code = encode(c, &fig);
        TEST_ASSERT_TRUE(valid(code));
        TEST_ASSERT_FALSE(fig);
        TEST_ASSERT_FALSE(seen[code]);
        seen[code] = true;
        TEST_ASSERT_EQUAL(c, decode(code, false));
    }
    for (uint8_t code : {kAlpha, kBeta, kRep, kLtrs, kFigs, kCr, kLf, kSpace}) {
        TEST_ASSERT_TRUE(valid(code));
        TEST_ASSERT_FALSE(seen[code]);
        seen[code] = true;
    }
    bool fig = false;
    TEST_ASSERT_EQUAL(encode('E', &fig), encode('3', &fig));
    TEST_ASSERT_TRUE(fig);
    TEST_ASSERT_FALSE(valid(0x7F));
    TEST_ASSERT_FALSE(valid(0x00));
}

static void test_navtex_clean()
{
    TEST_ASSERT_EQUAL_STRING(kNavtexMessage, receiveNavtex(sitorAudio(sitorCodes(kNavtexMessage))).c_str());
    digi::NavtexConfig cfg;
    cfg.center_hz = 1700.0f;
    TEST_ASSERT_EQUAL_STRING(kNavtexMessage, receiveNavtex(sitorAudio(sitorCodes(kNavtexMessage), 1700.0f), cfg).c_str());
}

static void test_navtex_noise()
{
    auto pcm = sitorAudio(sitorCodes(kNavtexMessage));
    addNoise(pcm, 4000);
    TEST_ASSERT_EQUAL_STRING(kNavtexMessage, receiveNavtex(pcm).c_str());
}

static void test_navtex_repairs_from_repeat()
{
    // Destroy single transmissions well inside the message, in both kinds of position;
    // the other copy of each character must fill in.
    auto codes = sitorCodes(kNavtexMessage, {60, 61, 62, 63, 95, 120, 121});
    TEST_ASSERT_EQUAL_STRING(kNavtexMessage, receiveNavtex(sitorAudio(codes)).c_str());
}

static void test_navtex_marks_unrecoverable_characters()
{
    // Position 60 is a first transmission, 65 its repeat: that character is gone
    auto codes = sitorCodes("ZCZC EA01\nABCDEFGHIJKLMNOPQRSTUVWXYZ\nNNNN", {60, 65});
    std::string got = receiveNavtex(sitorAudio(codes));
    TEST_ASSERT_EQUAL(1, std::count(got.begin(), got.end(), '_'));
    TEST_ASSERT_EQUAL(strlen("ZCZC EA01\nABCDEFGHIJKLMNOPQRSTUVWXYZ\nNNNN"), got.size());
}

static void test_navtex_wrong_sideband_and_noise()
{
    auto pcm = sitorAudio(sitorCodes(kNavtexMessage));
    digi::NavtexConfig reversed;
    reversed.reverse = true;
    // Inverted tones turn every code into one with three "B" bits: nothing valid arrives
    TEST_ASSERT_EQUAL_STRING("", receiveNavtex(pcm, reversed).c_str());

    std::vector<int16_t> noise(12000 * 20, 0);
    addNoise(noise, 5000);
    TEST_ASSERT_EQUAL_STRING("", receiveNavtex(noise).c_str());
}

// --- WEFAX -------------------------------------------------------------------

// Frequency modulates a picture signal: value 0 = 1500 Hz (black), 1 = 2300 Hz (white)
struct FaxSender {
    std::vector<int16_t> pcm;
    double phi = 0;
    void sample(float value)
    {
        phi += 2 * M_PI * (1500.0 + 800.0 * value) / 12000.0;
        pcm.push_back((int16_t)(9000 * sin(phi)));
    }
    void tone(int hz, float seconds)
    {
        int n = (int)(seconds * 12000);
        for (int i = 0; i < n; ++i) {
            sample(((i * 2 * hz / 12000) & 1) ? 1.0f : 0.0f);
        }
    }
    // 6000 samples per line at 120 lines per minute
    void phasingLine(bool white_pulse)
    {
        for (int i = 0; i < 6000; ++i) {
            bool pulse = i < 150 || i >= 5850;  // 5 %, centred on the line boundary
            sample(pulse == white_pulse ? 1.0f : 0.0f);
        }
    }
    void gradientLine()
    {
        for (int i = 0; i < 6000; ++i) {
            sample(i / 6000.0f);
        }
    }
};

static std::vector<std::vector<uint8_t>> fax_lines;
static void faxLine(const uint8_t *px, int width, void *)
{
    fax_lines.emplace_back(px, px + width);
}

static void runFax(bool white_pulse, int lead_in_samples, float noise_rms)
{
    FaxSender tx;
    tx.pcm.assign(lead_in_samples, 0);  // the decoder's line clock starts at an arbitrary point
    tx.tone(300, 3.0f);
    for (int i = 0; i < 12; ++i) {
        tx.phasingLine(white_pulse);
    }
    for (int i = 0; i < 20; ++i) {
        tx.gradientLine();
    }
    tx.tone(450, 3.0f);
    if (noise_rms > 0) {
        addNoise(tx.pcm, noise_rms);
    }

    digi::WefaxDecoder dec;
    TEST_ASSERT_TRUE(dec.begin(904, 120));
    fax_lines.clear();
    dec.onLine(faxLine, nullptr);
    bool got_start = false, got_phased = false, got_stop = false;
    size_t phased_at_line = 0;
    for (size_t pos = 0; pos < tx.pcm.size();) {
        size_t n = std::min<size_t>(480, tx.pcm.size() - pos);
        dec.feed(tx.pcm.data() + pos, n);
        pos += n;
        switch (dec.takeEvent()) {
        case digi::WefaxDecoder::Event::Start: got_start = true; break;
        case digi::WefaxDecoder::Event::Phased:
            got_phased = true;
            phased_at_line = fax_lines.size();
            break;
        case digi::WefaxDecoder::Event::Stop: got_stop = true; break;
        default: break;
        }
    }
    TEST_ASSERT_TRUE(got_start);
    TEST_ASSERT_TRUE(got_phased);
    TEST_ASSERT_TRUE(got_stop);

    // Find gradient lines after the alignment: dark on the left, bright on the right, and linear
    int good = 0;
    for (size_t i = phased_at_line + 1; i < fax_lines.size(); ++i) {
        const auto &l = fax_lines[i];
        if (abs(l[226] - 64) <= 12 && abs(l[452] - 128) <= 12 && abs(l[678] - 191) <= 12 && l[40] < 30 && l[860] > 225) {
            ++good;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL(17, good);
}

// --- SSTV --------------------------------------------------------------------------

static void sstvPattern(int x, int y, uint8_t *rgb, void *)
{
    // Colour bars above, a grey ramp in the middle, a colour gradient below
    if (y % 96 < 40) {
        static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                           {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
        memcpy(rgb, bars[x / 40], 3);
    } else if (y % 96 < 64) {
        rgb[0] = rgb[1] = rgb[2] = (uint8_t)(x * 255 / 319);
    } else {
        rgb[0] = (uint8_t)(x * 255 / 319);
        rgb[1] = (uint8_t)(255 - x * 255 / 319);
        rgb[2] = 128;
    }
}

struct SstvResult {
    std::vector<uint8_t> image;  // lines x 320 x 3
    int lines = 0;
    bool started = false, ended = false;
    digi::SstvMode mode = digi::SstvMode::None;
};

static void sstvLine(int line, const uint8_t *rgb, void *ctx)
{
    SstvResult *r = (SstvResult *)ctx;
    if ((int)r->image.size() < (line + 1) * digi::kSstvWidth * 3) {
        r->image.resize((line + 1) * digi::kSstvWidth * 3);
    }
    memcpy(&r->image[line * digi::kSstvWidth * 3], rgb, digi::kSstvWidth * 3);
    r->lines = line + 1 > r->lines ? line + 1 : r->lines;
}

/// Sends the test picture and receives it. skip_header: the receiver is started by hand after the header.
static SstvResult sstvLoop(digi::SstvMode mode, bool skip_header = false, float noise = 0)
{
    digi::SstvEncoder enc;
    enc.begin(mode, sstvPattern, nullptr);
    digi::SstvDecoder dec;
    SstvResult r;
    TEST_ASSERT_TRUE(dec.begin());
    dec.onLine(sstvLine, &r);
    int16_t buf[480];
    uint32_t seed = 12345;
    size_t total = 0;
    bool hunting = false;
    for (int pad = 0; pad < 2; ++pad) {
        for (;;) {
            size_t n = pad ? sizeof(buf) / sizeof(buf[0]) : enc.read(buf, sizeof(buf) / sizeof(buf[0]));
            if (pad) {
                memset(buf, 0, sizeof(buf));
            }
            if (n == 0) {
                break;
            }
            for (size_t i = 0; i < n; ++i) {
                seed = seed * 1664525u + 1013904223u;
                buf[i] = (int16_t)(buf[i] + noise * ((int)(seed >> 16) - 32768));
            }
            total += n;
            // Past the header (about 0.9 s) the receiver is told the format
            if (skip_header && !hunting && total > 12000) {
                hunting = true;
                dec.start(mode);
            }
            if (!(skip_header && !hunting)) {
                dec.feed(buf, n);
            }
            switch (dec.takeEvent()) {
            case digi::SstvDecoder::Event::Start:
                r.started = true;
                r.mode = dec.mode();
                break;
            case digi::SstvDecoder::Event::End: r.ended = true; break;
            default: break;
            }
            if (pad && total > 0 && pad++ > 60) {
                break;  // a second and a half of silence after the picture
            }
        }
    }
    return r;
}

/// Mean difference to the test picture, leaving out the columns next to the edges of the bars.
static float sstvError(const SstvResult &r, int first_line = 0)
{
    double sum = 0;
    long n = 0;
    for (int y = first_line; y < r.lines; ++y) {
        for (int x = 4; x < digi::kSstvWidth - 4; ++x) {
            if (y % 96 < 40 && (x % 40 < 4 || x % 40 > 35)) {
                continue;
            }
            uint8_t want[3];
            sstvPattern(x, y, want, nullptr);
            for (int c = 0; c < 3; ++c) {
                sum += abs((int)r.image[(y * digi::kSstvWidth + x) * 3 + c] - (int)want[c]);
                ++n;
            }
        }
    }
    return n ? (float)(sum / n) : 999;
}

static void test_sstv_modes()
{
    for (digi::SstvMode mode : {digi::SstvMode::MartinM1, digi::SstvMode::MartinM2, digi::SstvMode::ScottieS1, digi::SstvMode::ScottieS2,
                                digi::SstvMode::Robot36, digi::SstvMode::PD120, digi::SstvMode::PD180}) {
        const char *name = digi::sstvInfo(mode).name;
        SstvResult r = sstvLoop(mode);
        TEST_ASSERT_TRUE_MESSAGE(r.started, name);
        TEST_ASSERT_EQUAL_MESSAGE((int)mode, (int)r.mode, name);
        TEST_ASSERT_TRUE_MESSAGE(r.ended, name);
        TEST_ASSERT_EQUAL_MESSAGE(digi::sstvInfo(mode).lines, r.lines, name);
        // Colour-difference modes halve the colour resolution, which costs a little on the gradient
        float limit = (mode == digi::SstvMode::Robot36 || mode == digi::SstvMode::PD120 || mode == digi::SstvMode::PD180) ? 9.0f : 5.0f;
        char text[64];
        snprintf(text, sizeof(text), "%s: mean error %.1f", name, sstvError(r));
        TEST_ASSERT_TRUE_MESSAGE(sstvError(r) < limit, text);
    }
}

static void test_sstv_without_header_and_with_noise()
{
    // Started by hand in the middle: the lines after the first few must be right
    SstvResult r = sstvLoop(digi::SstvMode::MartinM1, true);
    TEST_ASSERT_TRUE(r.started);
    TEST_ASSERT_TRUE(r.lines > 200);
    // The receiver cannot know which line of the picture it joined at; compare the pattern, which repeats every 96 lines
    SstvResult s = sstvLoop(digi::SstvMode::ScottieS1, true);
    TEST_ASSERT_TRUE(s.started);
    TEST_ASSERT_TRUE(s.lines > 200);

    // Noise 20 dB below the signal
    SstvResult n = sstvLoop(digi::SstvMode::MartinM1, false, 0.05f);
    TEST_ASSERT_TRUE(n.started);
    TEST_ASSERT_EQUAL(256, n.lines);
    char text[64];
    snprintf(text, sizeof(text), "with noise: mean error %.1f", sstvError(n));
    TEST_ASSERT_TRUE_MESSAGE(sstvError(n) < 14.0f, text);
}

static void test_wefax_picture_and_phasing()
{
    runFax(true, 1234, 0);
    runFax(false, 4321, 0);  // some stations send the phasing pulse in black
    runFax(true, 2900, 800);
}

static void test_wefax_manual_shift()
{
    FaxSender tx;
    for (int i = 0; i < 6; ++i) {
        tx.gradientLine();
    }
    digi::WefaxDecoder dec;
    TEST_ASSERT_TRUE(dec.begin(904, 120));
    fax_lines.clear();
    dec.onLine(faxLine, nullptr);
    dec.feed(tx.pcm.data(), 6000 * 3);
    TEST_ASSERT_INT_WITHIN(12, 128, fax_lines.back()[452]);
    dec.shift(226);  // a quarter line: the middle of the picture now shows what was at three quarters
    dec.feed(tx.pcm.data() + 6000 * 3, 6000 * 3);
    TEST_ASSERT_INT_WITHIN(12, 191, fax_lines.back()[452]);
    TEST_ASSERT_FALSE(dec.phased());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_message);
    RUN_TEST(test_reports);
    RUN_TEST(test_decode_wav_samples);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_roundtrip_ft4);
    RUN_TEST(test_encoder_rejects_garbage);
    RUN_TEST(test_synth_level);
    RUN_TEST(test_qso_cq_full_sequence);
    RUN_TEST(test_qso_rr73_repeated_when_not_copied);
    RUN_TEST(test_qso_answer_cq);
    RUN_TEST(test_qso_ignores_other_stations_and_own_slot);
    RUN_TEST(test_qso_gives_up);
    RUN_TEST(test_qso_halt);
    RUN_TEST(test_adif);
    RUN_TEST(test_band_plan);
    RUN_TEST(test_distance_and_cq_addition);
    RUN_TEST(test_qso_window);
    RUN_TEST(test_adif_field);
    RUN_TEST(test_grid_from_position);
    RUN_TEST(test_country);
    RUN_TEST(test_cb_callsigns);
    RUN_TEST(test_cb_countries);
    RUN_TEST(test_cb_parse);
    RUN_TEST(test_cb_messages_on_the_air);
    RUN_TEST(test_cb_qso_after_cq);
    RUN_TEST(test_cb_qso_answering_cq);
    RUN_TEST(test_cb_reports_need_cb_mode_and_a_contact);
    RUN_TEST(test_cw_keyer);
    RUN_TEST(test_cw_roundtrip);
    RUN_TEST(test_cw_window_ignores_other_signals);
    RUN_TEST(test_cw_clicks_and_fixed_tone);
    RUN_TEST(test_cw_punctuation_and_digits);
    RUN_TEST(test_cw_silence_and_noise_give_no_text);
    RUN_TEST(test_rtty_roundtrip);
    RUN_TEST(test_rtty_figures_and_lines);
    RUN_TEST(test_rtty_reverse);
    RUN_TEST(test_rtty_noise_gives_little_text);
    RUN_TEST(test_ccir476_table);
    RUN_TEST(test_navtex_clean);
    RUN_TEST(test_navtex_noise);
    RUN_TEST(test_navtex_repairs_from_repeat);
    RUN_TEST(test_navtex_marks_unrecoverable_characters);
    RUN_TEST(test_navtex_wrong_sideband_and_noise);
    RUN_TEST(test_sstv_modes);
    RUN_TEST(test_sstv_without_header_and_with_noise);
    RUN_TEST(test_wefax_picture_and_phasing);
    RUN_TEST(test_wefax_manual_shift);
    return UNITY_END();
}
