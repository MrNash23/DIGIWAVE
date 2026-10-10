#include "ft8_hashtable.h"

#include <string.h>

// Adapted from demo/decode_ft8.c of ft8_lib.

namespace ft8 {

namespace {

constexpr int kSize = 256;

struct Entry {
    char callsign[12];
    uint32_t hash;  // 8 MSBs: age in slots, 22 LSBs: hash value
};

Entry table[kSize];

void put(const char *callsign, uint32_t hash, uint8_t age)
{
    uint16_t hash10 = (hash >> 12) & 0x3FFu;
    int idx = (hash10 * 23) % kSize;
    for (int probes = 0; probes < kSize && table[idx].callsign[0] != '\0'; ++probes) {
        if (((table[idx].hash & 0x3FFFFFu) == hash) && (0 == strcmp(table[idx].callsign, callsign))) {
            table[idx].hash = hash | ((uint32_t)age << 24);
            return;
        }
        idx = (idx + 1) % kSize;
    }
    if (table[idx].callsign[0] != '\0') {
        return;  // table full
    }
    strncpy(table[idx].callsign, callsign, 11);
    table[idx].callsign[11] = '\0';
    table[idx].hash = hash | ((uint32_t)age << 24);
}

void add(const char *callsign, uint32_t hash)
{
    put(callsign, hash & 0x3FFFFFu, 0);
}

bool lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign)
{
    uint8_t shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 : (hash_type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    uint16_t hash10 = (hash >> (12 - shift)) & 0x3FFu;
    int idx = (hash10 * 23) % kSize;
    for (int probes = 0; probes < kSize && table[idx].callsign[0] != '\0'; ++probes) {
        if (((table[idx].hash & 0x3FFFFFu) >> shift) == hash) {
            strcpy(callsign, table[idx].callsign);
            return true;
        }
        idx = (idx + 1) % kSize;
    }
    callsign[0] = '\0';
    return false;
}

ftx_callsign_hash_interface_t iface = {lookup, add};

}  // namespace

ftx_callsign_hash_interface_t *hashInterface()
{
    return &iface;
}

void hashCleanup(uint8_t max_age)
{
    // Removing entries in place would break the probe chains, so the table is rebuilt.
    static Entry old[kSize];
    memcpy(old, table, sizeof(table));
    memset(table, 0, sizeof(table));
    for (int i = 0; i < kSize; ++i) {
        if (old[i].callsign[0] == '\0') {
            continue;
        }
        uint8_t age = (uint8_t)(old[i].hash >> 24);
        if (age < max_age) {
            put(old[i].callsign, old[i].hash & 0x3FFFFFu, age + 1);
        }
    }
}

void hashClear()
{
    memset(table, 0, sizeof(table));
}

void rememberCall(const char *call)
{
    // Packing a CQ of that station computes its hash and stores it through the interface
    ftx_message_t msg;
    if (call[0] != '\0') {
        ftx_message_encode_nonstd(&msg, &iface, "CQ", call, "");
    }
}

}  // namespace ft8
