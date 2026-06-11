#include "hash.h"

uint64_t mirror_fnv1a64(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint64_t hash = 0xcbf29ce484222325u;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 0x00000100000001b3u;
    }
    return hash;
}
