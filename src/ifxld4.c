/**
 * @file ifxld4.c
 * @brief Susie I/F adapter for xLD images.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "spibase.h"
#include "xld4decode.h"

#define IFKTY_VERSION "0.10"

const int NumInfo = 6;
const LPCSTR PluginInfo[] = { "00IN", "XLD4 / QLD to DIB filter ver." IFKTY_VERSION " (C) Ru^3",
    "*.q4", "XLD4", "*.gra", "QLD" };

/* Read from the current SPI position, preserving file-offset and memory input
 * semantics. The decoder borrows this buffer; ownership stays in the adapter. */
static int read_all(SPI_FILE *file, uint8_t **out, size_t *size)
{
    uint8_t *buffer = NULL;
    size_t used = 0, capacity = 0;
    int err = SPI_ERROR_SUCCESS;
    *out = NULL; *size = 0;
    for (;;) {
        size_t count, needed;
        if (file->mcount <= 0) SpiFillBuf(file);
        if (SpiIsError(file)) { err = SPI_ERROR_FILE_READ; break; }
        if (file->mcount <= 0) break;
        count = (size_t)file->mcount;
        if (count > SIZE_MAX - used) { err = SPI_ERROR_ALLOCATE_MEMORY; break; }
        needed = used + count;
        if (needed > capacity) {
            uint8_t *grown;
            size_t next = capacity <= SIZE_MAX / 2 ? capacity * 2 : SIZE_MAX;
            if (next < needed) next = needed;
            grown = realloc(buffer, next);
            if (!grown) { err = SPI_ERROR_ALLOCATE_MEMORY; break; }
            buffer = grown; capacity = next;
        }
        memcpy(buffer + used, file->mptr, count);
        used = needed; file->mptr += count; file->mcount = 0;
    }
    if (err) free(buffer);
    else { *out = buffer; *size = used; }
    return err;
}

static int decode_error(int err)
{
    switch (err) {
    case XLD4_OK: return SPI_ERROR_SUCCESS;
    case XLD4_ERR_FORMAT: return SPI_ERROR_UNKNOWN_FORMAT;
    case XLD4_ERR_MEMORY: return SPI_ERROR_ALLOCATE_MEMORY;
    case XLD4_ERR_CANCELLED: return SPI_ERROR_CANCEL_EXPAND;
    default: return SPI_ERROR_BROKEN_DATA;
    }
}

typedef struct {
    SPIPROC callback;
    LONG_PTR data;
} ProgressContext;

static int progress_adapter(int current, int total, void *user)
{
    ProgressContext *context = user;
    return context->callback(current, total, context->data);
}

int IsSupportedFormat(LPBYTE buf, DWORD n, LPCSTR filename)
{
    (void)filename;
    return xld4_probe(buf, n);
}

int GetImageInfo(SPI_FILE *file, PictureInfo *info)
{
    uint8_t *data;
    size_t size;
    Xld4Info decoded;
    int err;
    if (!info) return SPI_ERROR_INTERNAL;
    err = read_all(file, &data, &size);
    if (err) return err;
    err = decode_error(xld4_get_info(data, size, &decoded));
    free(data);
    if (!err) SpiSetPictureInfo(info, decoded.width, decoded.height,
                                decoded.bits_per_pixel, 0, 0, 0, 0, NULL);
    return err;
}

int GetImage(SPI_FILE *file, HANDLE *info, HANDLE *image, SPIPROC cb, LONG_PTR user)
{
    uint8_t *data;
    size_t size;
    Xld4Image decoded;
    ProgressContext context;
    LPBYTE bits;
    LPBITMAPINFO bmi;
    DWORD stride;
    unsigned x, y;
    int err;
    if (!info || !image) return SPI_ERROR_INTERNAL;
    *info = NULL; *image = NULL;
    err = read_all(file, &data, &size);
    if (err) return err;
    context.callback = cb; context.data = user;
    err = decode_error(xld4_decode(data, size, &decoded,
                                   cb ? progress_adapter : NULL, &context));
    free(data);
    if (err) return err;
    /* Both source depths are returned as a Windows-supported 4-bit DIB. */
    err = SpiInitBitmap(info, &bmi, image, &bits, &stride,
                         decoded.info.width, decoded.info.height, 4, 16, 0, 0);
    if (!err) {
        for (x = 0; x < decoded.info.palette_entries; x++) {
            bmi->bmiColors[x].rgbRed = decoded.palette[x][0];
            bmi->bmiColors[x].rgbGreen = decoded.palette[x][1];
            bmi->bmiColors[x].rgbBlue = decoded.palette[x][2];
        }
        for (y = 0; y < decoded.info.height; y++) {
            const uint8_t *row = decoded.pixels + y * decoded.info.width;
            for (x = 0; x < decoded.info.width; x += 2)
                bits[(decoded.info.height - 1 - y) * stride + x / 2] =
                    (row[x] << 4) | row[x + 1];
        }
        SpiUnlockBuffer(info); SpiUnlockBuffer(image);
    }
    xld4_free(&decoded);
    return err;
}
