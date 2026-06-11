#ifndef MIRROR_UMAP_H
#define MIRROR_UMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Open-addressing hash map from uint64_t keys to uint64_t values.
 * Used to remember the SPIR-V hash of every live VkShaderModule, so the
 * number of entries can grow into the thousands for real games.
 */
struct mirror_umap {
    uint64_t *keys;
    uint64_t *values;
    uint8_t *states; /* 0 = empty, 1 = occupied, 2 = tombstone */
    size_t capacity; /* always zero or a power of two */
    size_t occupied; /* live entries */
    size_t filled;   /* live entries + tombstones */
};

void mirror_umap_init(struct mirror_umap *map);
void mirror_umap_free(struct mirror_umap *map);

/* Inserts or updates a key. Returns false on allocation failure. */
bool mirror_umap_put(struct mirror_umap *map, uint64_t key, uint64_t value);

/* Returns true and stores the value if the key is present. */
bool mirror_umap_get(const struct mirror_umap *map, uint64_t key, uint64_t *value);

void mirror_umap_remove(struct mirror_umap *map, uint64_t key);

#endif
