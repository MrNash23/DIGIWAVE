// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "ptt_if.h"

#include <LilyGoLib.h>

namespace {

constexpr uint8_t kPttPin = 9;  // XL9555 P11, pin 10 of the 12-pin port

class ExtensionPortPtt : public Ptt {
public:
    bool active_high = true;

    void begin() override
    {
        // The output register starts at 1; the level is set before the pin becomes an output
        set(false);
        instance.io.pinMode(kPttPin, OUTPUT);
        set(false);
    }

    void set(bool transmit) override { instance.io.digitalWrite(kPttPin, transmit == active_high ? HIGH : LOW); }
};

ExtensionPortPtt ptt;

}  // namespace

Ptt &extensionPortPtt(bool active_high)
{
    ptt.active_high = active_high;
    return ptt;
}
