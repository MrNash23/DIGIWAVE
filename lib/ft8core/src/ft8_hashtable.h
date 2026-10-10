#pragma once

extern "C" {
#include <ft8/message.h>
}

namespace ft8 {

/// Callsign hash table shared by decoder and encoder, so that hashed calls
/// (<...>) heard earlier can be resolved.
ftx_callsign_hash_interface_t *hashInterface();

/// Ages all entries by one slot and drops the ones not heard for max_age slots.
void hashCleanup(uint8_t max_age);

void hashClear();

/// Enters a callsign into the table, as if it had been heard. Used for the own
/// call, so that messages addressing it by its hash can be read.
void rememberCall(const char *call);

}  // namespace ft8
