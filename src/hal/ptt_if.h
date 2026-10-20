// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#pragma once

/// Transmit keying of the connected radio.
class Ptt {
public:
    virtual ~Ptt() = default;
    virtual void begin() = 0;
    virtual void set(bool transmit) = 0;
};

/// Switched output on pin 10 of the 12-pin extension port (XL9555 P11, NRF_CE in
/// the schematic). On the interface board it also turns the audio data pin around.
/// @param active_high level that keys the transmitter
Ptt &extensionPortPtt(bool active_high);
