#pragma once

#include <ft8_decoder.h>
#include <ft8_qso.h>

/// ADIF logbook on the SD card, with the most recent QSOs kept in memory for display.
namespace logbook {

extern const char kPath[];  // file on the SD card

struct Row {
    char date[9];   // YYYYMMDD
    char time[7];   // HHMMSS
    char call[14];
    char grid[8];
    char sent[4];
    char rcvd[4];
    char band[6];
    char mode[6];
    int km;         // distance to the station, -1 if a locator is missing
};

/// Figures over all QSOs in the file.
struct Stats {
    int total;
    int today;             // on the current UTC day
    int bands;             // number of entries in band[]
    struct {
        char name[6];
        int count;
    } band[12];
    int ft8, ft4;
    int countries;
    int with_distance;
    long km_sum;
    int km_max;
    char km_max_call[14];
    char first_date[9], last_date[9];
};

constexpr int kMaxRows = 200;

/// Reads the existing log from the SD card. Returns false if there is no card.
bool load();

/// Appends a QSO to the file and to the list. Returns false if the card is missing or the write fails.
bool append(const ft8::QsoRecord &rec, ft8::Protocol protocol);

/// QSOs in the file (all of them, also those no longer in memory).
int total();

/// QSOs held in memory, oldest first.
int count();
const Row &row(int index);

/// Whether a station is in the logbook of this device: 0 no, 1 on another
/// band only, 2 on the given band (ADIF name such as "20m").
int worked(const char *call, const char *band);

/// today: the current UTC date as YYYYMMDD, for the count of the day.
const Stats &stats(const char *today);

}  // namespace logbook
