#pragma once

/// Transmit keying of the connected radio.
class Ptt {
public:
    virtual ~Ptt() = default;
    virtual void begin() = 0;
    virtual void set(bool transmit) = 0;
};

/// Switched output on pin 5 of the 12-pin extension port (XL9555 IO9).
/// @param active_high level that keys the transmitter
Ptt &extensionPortPtt(bool active_high);
