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

}  // namespace logbook
