#include "umap.h"

#include <stdlib.h>

#define UMAP_EMPTY     0u
#define UMAP_OCCUPIED  1u
#define UMAP_TOMBSTONE 2u
#define UMAP_MIN_CAP   16u

static size_t slot_hash(uint64_t key) {
    /* splitmix64 finalizer: cheap and well distributed for handle values. */
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9u;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebu;
    key ^= key >> 31;
    return (size_t)key;
}

void mirror_umap_init(struct mirror_umap *map) {
    *map = (struct mirror_umap){0};
}

void mirror_umap_free(struct mirror_umap *map) {
    free(map->keys);
    free(map->values);
    free(map->states);
    *map = (struct mirror_umap){0};
}

static bool umap_rehash(struct mirror_umap *map, size_t capacity) {
    uint64_t *keys = malloc(capacity * sizeof *keys);
    uint64_t *values = malloc(capacity * sizeof *values);
    uint8_t *states = calloc(capacity, sizeof *states);
    if (!keys || !values || !states) {
        free(keys);
        free(values);
        free(states);
        return false;
    }
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->states[i] != UMAP_OCCUPIED)
            continue;
        size_t slot = slot_hash(map->keys[i]) & (capacity - 1);
        while (states[slot] == UMAP_OCCUPIED)
            slot = (slot + 1) & (capacity - 1);
        keys[slot] = map->keys[i];
        values[slot] = map->values[i];
        states[slot] = UMAP_OCCUPIED;
    }
    free(map->keys);
    free(map->values);
    free(map->states);
    map->keys = keys;
    map->values = values;
    map->states = states;
    map->capacity = capacity;
    map->filled = map->occupied;
    return true;
}

bool mirror_umap_put(struct mirror_umap *map, uint64_t key, uint64_t value) {
    /* Keep load factor (including tombstones) below 3/4. */
    if (map->capacity == 0 || (map->filled + 1) * 4 > map->capacity * 3) {
        size_t capacity = map->capacity ? map->capacity * 2 : UMAP_MIN_CAP;
        if (!umap_rehash(map, capacity))
            return false;
    }
    size_t slot = slot_hash(key) & (map->capacity - 1);
    size_t insert_at = SIZE_MAX;
    while (map->states[slot] != UMAP_EMPTY) {
        if (map->states[slot] == UMAP_OCCUPIED && map->keys[slot] == key) {
            map->values[slot] = value;
            return true;
        }
        if (map->states[slot] == UMAP_TOMBSTONE && insert_at == SIZE_MAX)
            insert_at = slot;
        slot = (slot + 1) & (map->capacity - 1);
    }
    if (insert_at == SIZE_MAX) {
        insert_at = slot;
        map->filled++;
    }
    map->keys[insert_at] = key;
    map->values[insert_at] = value;
    map->states[insert_at] = UMAP_OCCUPIED;
    map->occupied++;
    return true;
}

bool mirror_umap_get(const struct mirror_umap *map, uint64_t key, uint64_t *value) {
    if (map->capacity == 0)
        return false;
    size_t slot = slot_hash(key) & (map->capacity - 1);
    while (map->states[slot] != UMAP_EMPTY) {
        if (map->states[slot] == UMAP_OCCUPIED && map->keys[slot] == key) {
            if (value)
                *value = map->values[slot];
            return true;
        }
        slot = (slot + 1) & (map->capacity - 1);
    }
    return false;
}

void mirror_umap_remove(struct mirror_umap *map, uint64_t key) {
    if (map->capacity == 0)
        return;
    size_t slot = slot_hash(key) & (map->capacity - 1);
    while (map->states[slot] != UMAP_EMPTY) {
        if (map->states[slot] == UMAP_OCCUPIED && map->keys[slot] == key) {
            map->states[slot] = UMAP_TOMBSTONE;
            map->occupied--;
            return;
        }
        slot = (slot + 1) & (map->capacity - 1);
    }
}
