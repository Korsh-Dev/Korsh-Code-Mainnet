#ifndef BITCOIN_CHAINPARAMSSEEDS_H
#define BITCOIN_CHAINPARAMSSEEDS_H
#include <cstdint>
/**
 * List of fixed seed nodes for the korsh network
 * Each line contains a BIP155 serialized (networkID, addr, port) tuple.
 */
static const uint8_t chainparams_seed_main[] = {
    0x01,0x04,0xc3,0x1a,0xf4,0xd1,0x26,0x31, // 195.26.244.209:9777
    0x01,0x04,0xb9,0xfb,0x13,0xa1,0x26,0x31, // 185.251.19.161:9777
};

static const uint8_t chainparams_seed_test[] = {
};
#endif // BITCOIN_CHAINPARAMSSEEDS_H
