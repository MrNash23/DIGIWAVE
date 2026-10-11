// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
//
// Stand-ins for the NFC reader classes. The board library declares a reader
// object whether it is used or not; this firmware never uses NFC, and with
// these stand-ins the reader library of the chip maker, which comes under a
// licence of its own, is not part of the build. The file is included into
// every source by the build flags (see platformio.ini).
#pragma once

#ifdef __cplusplus

class SPIClass;

enum { ST_ERR_NONE = 0, ST_ERR_NOT_PRESENT = 1 };

class RfalRfST25R3916Class {
public:
    RfalRfST25R3916Class(SPIClass *, int, int) {}
};

class RfalNfcClass {
public:
    explicit RfalNfcClass(RfalRfST25R3916Class *) {}
    int rfalNfcInitialize() { return ST_ERR_NOT_PRESENT; }
    void rfalNfcWorker() {}
};

#endif
