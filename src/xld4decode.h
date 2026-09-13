/** Portable XLD4 / QLD decoder. Requires only the C standard library. */
#ifndef XLD4DECODE_H
#define XLD4DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XLD4_OK = 0,
    XLD4_ERR_FORMAT,
    XLD4_ERR_DATA,
    XLD4_ERR_MEMORY,
    XLD4_ERR_CANCELLED
};

typedef enum Xld4Format {
    XLD4_FORMAT_Q4 = 1,
    XLD4_FORMAT_QLD = 2
} Xld4Format;

typedef struct Xld4Info {
    uint32_t width, height;
    uint8_t bits_per_pixel;    /* Source depth: 4 (Q4) or 3 (QLD). */
    uint16_t palette_entries;  /* 16 (Q4) or 8 (QLD). */
    Xld4Format format;
} Xld4Info;

typedef struct Xld4Image {
    Xld4Info info;
    uint8_t palette[16][3];    /* RGB; unused entries are zero. */
    uint8_t *pixels;           /* Top-down indices, one byte per pixel. */
} Xld4Image;

/* Return nonzero to cancel. user is passed through unchanged. The final
 * notification has current == total; total may differ between notifications. */
typedef int (*Xld4Progress)(int current, int total, void *user);

/* Signature check only; accepts a leading buffer, returns nonzero on match. */
int xld4_probe(const uint8_t *data, size_t size);

/* Parse a complete header without decompressing pixels. The buffer may contain
 * the complete file. On failure, info is cleared. NULL output is an error. */
int xld4_get_info(const uint8_t *data, size_t size, Xld4Info *info);

/* Decode a complete file in memory. Input is borrowed only for this call.
 * image must not own an existing allocation. On success, release it with
 * xld4_free(); on failure, it is cleared and no allocation is retained.
 * progress may be NULL. All state is local to the call (reentrant). */
int xld4_decode(const uint8_t *data, size_t size, Xld4Image *image,
                Xld4Progress progress, void *user);

/* Release pixels and clear the structure. NULL and repeated calls are safe. */
void xld4_free(Xld4Image *image);

#ifdef __cplusplus
}
#endif
#endif
