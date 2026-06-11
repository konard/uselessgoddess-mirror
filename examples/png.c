#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static void put_be32(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static bool write_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t size) {
    uint8_t header[8];
    put_be32(header, size);
    memcpy(header + 4, type, 4);

    uint32_t crc = crc32(0, (const Bytef *)type, 4);
    if (size)
        crc = crc32(crc, data, size);
    uint8_t crc_bytes[4];
    put_be32(crc_bytes, crc);

    return fwrite(header, 1, 8, file) == 8 && (size == 0 || fwrite(data, 1, size, file) == size) &&
           fwrite(crc_bytes, 1, 4, file) == 4;
}

bool png_write_rgba(const char *path, uint32_t width, uint32_t height, const uint8_t *rgba) {
    bool ok = false;
    FILE *file = NULL;
    uint8_t *raw = NULL;
    uint8_t *compressed = NULL;

    /* Each scanline is prefixed with filter type 0 (none). */
    size_t stride = (size_t)width * 4;
    size_t raw_size = (stride + 1) * height;
    raw = malloc(raw_size);
    if (!raw)
        goto done;
    for (uint32_t y = 0; y < height; y++) {
        raw[y * (stride + 1)] = 0;
        memcpy(raw + y * (stride + 1) + 1, rgba + y * stride, stride);
    }

    uLongf compressed_size = compressBound((uLong)raw_size);
    compressed = malloc(compressed_size);
    if (!compressed)
        goto done;
    if (compress2(compressed, &compressed_size, raw, (uLong)raw_size, 6) != Z_OK)
        goto done;

    file = fopen(path, "wb");
    if (!file)
        goto done;

    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (fwrite(signature, 1, 8, file) != 8)
        goto done;

    uint8_t ihdr[13];
    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* color type: RGBA */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    if (!write_chunk(file, "IHDR", ihdr, sizeof ihdr))
        goto done;
    if (!write_chunk(file, "IDAT", compressed, (uint32_t)compressed_size))
        goto done;
    if (!write_chunk(file, "IEND", NULL, 0))
        goto done;
    ok = true;

done:
    if (file && fclose(file) != 0)
        ok = false;
    free(raw);
    free(compressed);
    return ok;
}
