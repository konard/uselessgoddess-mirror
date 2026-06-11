#ifndef MIRROR_PNG_H
#define MIRROR_PNG_H

#include <stdbool.h>
#include <stdint.h>

/* Writes an RGBA8 image to a PNG file. Returns false on I/O or zlib error. */
bool png_write_rgba(const char *path, uint32_t width, uint32_t height, const uint8_t *rgba);

#endif
