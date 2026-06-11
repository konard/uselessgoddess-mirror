#include <stdio.h>

#include "../src/umap.h"

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void) {
    struct mirror_umap map;
    mirror_umap_init(&map);
    uint64_t value;

    CHECK(!mirror_umap_get(&map, 42, &value));
    mirror_umap_remove(&map, 42); /* removing from an empty map is a no-op */

    /* Insert enough entries to force several rehashes. */
    for (uint64_t key = 0; key < 10000; key++)
        CHECK(mirror_umap_put(&map, key * 0x10001, key));
    CHECK(map.occupied == 10000);
    for (uint64_t key = 0; key < 10000; key++)
        CHECK(mirror_umap_get(&map, key * 0x10001, &value) && value == key);

    /* Update in place. */
    CHECK(mirror_umap_put(&map, 0x10001, 777));
    CHECK(mirror_umap_get(&map, 0x10001, &value) && value == 777);
    CHECK(map.occupied == 10000);

    /* Remove half, the rest must survive. */
    for (uint64_t key = 0; key < 10000; key += 2)
        mirror_umap_remove(&map, key * 0x10001);
    CHECK(map.occupied == 5000);
    for (uint64_t key = 0; key < 10000; key++)
        CHECK(mirror_umap_get(&map, key * 0x10001, NULL) == (key % 2 == 1));

    /* Reinsert over tombstones. */
    for (uint64_t key = 0; key < 10000; key += 2)
        CHECK(mirror_umap_put(&map, key * 0x10001, key + 1));
    CHECK(map.occupied == 10000);
    CHECK(mirror_umap_get(&map, 0, &value) && value == 1);

    mirror_umap_free(&map);
    CHECK(map.capacity == 0 && map.occupied == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("unit_umap: all checks passed\n");
    return 0;
}
