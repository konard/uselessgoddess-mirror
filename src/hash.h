#ifndef MIRROR_HASH_H
#define MIRROR_HASH_H

#include <stddef.h>
#include <stdint.h>

/* FNV-1a 64-bit hash, used to identify shader modules by their SPIR-V code. */
uint64_t mirror_fnv1a64(const void *data, size_t size);

#endif
