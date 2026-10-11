#include "logbook.h"

#include <LilyGoLib.h>
#include <SD.h>

#include <ft8_adif.h>

#include "settings.h"

namespace logbook {

const char kPath[] = "/digiwave.adi";

namespace {

Row *rows = nullptr;
int row_count = 0;
int total_count = 0;

void addRow(const char *record)
{
    if (rows == nullptr) {
        rows = (Row *)ps_calloc(kMaxRows, sizeof(Row));
        if (rows == nullptr) {
            return;
        }
    }
    if (row_count == kMaxRows) {
        memmove(rows, rows + 1, sizeof(Row) * (kMaxRows - 1));
        --row_count;
    }
    Row &r = rows[row_count++];
    ft8::adifField(record, "qso_date", r.date, sizeof(r.date));
    ft8::adifField(record, "time_on", r.time, sizeof(r.time));
    ft8::adifField(record, "call", r.call, sizeof(r.call));
    ft8::adifField(record, "gridsquare", r.grid, sizeof(r.grid));
    ft8::adifField(record, "rst_sent", r.sent, sizeof(r.sent));
    ft8::adifField(record, "rst_rcvd", r.rcvd, sizeof(r.rcvd));
    ft8::adifField(record, "band", r.band, sizeof(r.band));
    // FT4 is logged as submode of MFSK
    if (!ft8::adifField(record, "submode", r.mode, sizeof(r.mode))) {
        ft8::adifField(record, "mode", r.mode, sizeof(r.mode));
    }
    ++total_count;
}

}  // namespace

bool load()
{
    row_count = 0;
    total_count = 0;
    // The card shares the SPI bus with the display
    instance.lockSPI();
    bool ok = SD.cardType() != CARD_NONE;
    if (ok) {
        File f = SD.open(kPath, FILE_READ);
        if (f) {
            // One record per line, as append() writes them
            static char line[400];
            while (f.available()) {
                size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
                line[n] = '\0';
                char call[14];
                if (ft8::adifField(line, "call", call, sizeof(call))) {
                    addRow(line);
                }
            }
            f.close();
        }
    }
    instance.unlockSPI();
    return ok;
}

bool append(const ft8::QsoRecord &rec, ft8::Protocol protocol)
{
    ft8::QsoConfig cfg;
    strncpy(cfg.my_call, settings.call, sizeof(cfg.my_call) - 1);
    strncpy(cfg.my_grid, settings.grid, sizeof(cfg.my_grid) - 1);
    char line[400];
    ft8::formatAdif(rec, cfg, settings.dialHz(protocol == ft8::Protocol::FT4 ? 1 : 0), line, sizeof(line), protocol);
    Serial.printf("[log] %s", line);
    addRow(line);

    instance.lockSPI();
    bool ok = false;
    if (SD.cardType() != CARD_NONE) {
        bool is_new = !SD.exists(kPath);
        File f = SD.open(kPath, FILE_APPEND);
        if (f) {
            if (is_new) {
                f.print(ft8::kAdifHeader);
            }
            ok = f.print(line) == strlen(line);
            f.close();
        }
    }
    instance.unlockSPI();
    return ok;
}

int total()
{
    return total_count;
}

int count()
{
    return row_count;
}

const Row &row(int index)
{
    return rows[index];
}

}  // namespace logbook
