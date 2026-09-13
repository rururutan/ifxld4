/* XLD4 / QLD decoder. Format algorithms documented by the reference
 * q4toppm.c (Tetsuya INOUE / FUJI SYSTEM, 1992) and
 * xgload/load_qld.c (ICHIDA Toshihiko, 1991,1992).
 * All decode state is per invocation; all input/output accesses are bounded. */
#include "xld4decode.h"
#include <stdlib.h>
#include <string.h>

#define PIXELS (640 * 400)
#define LIMIT 65536
typedef struct {
    uint8_t packed[LIMIT], symbols[LIMIT], stack[LIMIT], suffix[LIMIT];
    unsigned codes[LIMIT], prefix[LIMIT];
} Q4Work;

static unsigned word(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
typedef struct {
    const uint8_t *data;
    size_t remaining;
} Reader;

static int read_bytes(Reader *f, void *dst, unsigned n)
{
    if (n > f->remaining) return 0;
    if (n && dst) memcpy(dst, f->data, n);
    if (n) f->data += n;
    f->remaining -= n;
    return 1;
}
static int kind(const uint8_t *h, size_t n)
{
    if (n >= 22 && !memcmp(h + 11, "MAJYO", 5) &&
        (h[2] == 2 || (h[1] <= 1 && h[3] <= 1))) return 1;
    if (n >= 16 && !memcmp(h + 13, "WED", 3) && h[1] >= 2) return 2;
    return 0;
}
static int header(Reader *f, uint8_t *h, int *type)
{
    if (!read_bytes(f, h, 22)) return XLD4_ERR_DATA;
    *type = kind(h, 22);
    if (!*type) return XLD4_ERR_FORMAT;
    if (*type == 2) {
        if (!read_bytes(f, h + 22, 10) || !read_bytes(f, NULL, h[1] * 16 - 32))
            return XLD4_ERR_DATA;
        if ((h[10] & 15) == 2 &&
            (h[2] >= h[3] || h[3] > 80 || h[4] >= h[5] || h[5] > 200))
            return XLD4_ERR_DATA;
    }
    return 0;
}
/* MSB-first variable width codes: 0=end, 1=increase width, others=code+2.
 * The last dictionary code is lookahead and is not emitted (reference format). */
static int unpack(Q4Work *w, unsigned bytes, unsigned *out)
{
    unsigned bit = 0, width = 3, count = 0, v, j, cur = 17, prev, next, used = 0;
    while (bit < bytes * 8) {
        v = 0;
        for (j = 0; j < width; j++, bit++)
            v = (v << 1) | (bit < bytes * 8 ? ((w->packed[bit / 8] >> (7 - bit % 8)) & 1) : 0);
        if (!v) break;
        if (v == 1) { if (++width > 16) return 0; }
        else { if (count == LIMIT) return 0; w->codes[count++] = v - 2; }
    }
    if (count < 2 || w->codes[0] > 16) return 0;
    prev = w->codes[0];
    for (j = 1; j < count; j++) {
        unsigned depth = 0, first, k;
        next = w->codes[j];
        if (next > cur) break;
        v = prev;
        while (v >= 17) {
            if (v >= cur || depth == LIMIT) return 0;
            w->stack[depth++] = w->suffix[v]; v = w->prefix[v];
        }
        w->stack[depth++] = (uint8_t)v;
        if (depth > LIMIT - used) return 0;
        for (k = depth; k; k--) w->symbols[used++] = w->stack[k - 1];
        v = next == cur ? prev : next;
        while (v >= 17) { if (v >= cur) return 0; v = w->prefix[v]; }
        first = v;
        if (cur >= LIMIT) return 0;
        w->prefix[cur] = prev; w->suffix[cur] = (uint8_t)first;
        cur++; prev = next;
    }
    *out = used;
    return used != 0;
}
static int runlength(Q4Work *w, unsigned n, uint8_t *out, unsigned total, int palette)
{
    static const uint8_t map[16] = {0,2,4,6,1,3,5,7,8,10,12,14,9,11,13,15};
    unsigned i = 0, pos = 0, rep = 0, color = 0, count, ch;
    while (i < n) {
        color = w->symbols[i++]; count = 1;
        if (color == 16) {
            if (i >= n) return 0;
            ch = w->symbols[i++];
            if (!ch) {
                if (n - i < 2) return 0;
                rep = w->symbols[i++]; ch = w->symbols[i++];
            } else if (palette) rep = 0;
            /* A final run may omit its zero low count byte. */
            color = rep; count = ch * 17 + (i < n ? w->symbols[i++] : 0);
        }
        if (color > 15) return 0;
        color = palette ? color : map[color];
        /* Palette padding is ignored; image runs may overhang the dot count. */
        if (count > total - pos) count = total - pos;
        memset(out + pos, color, count); pos += count;
    }
    if (palette) return pos == total;
    /* The supplied Windows decoder leaves short block tails black. */
    memset(out + pos, 0, total - pos);
    return 1;
}
static int q4(Reader *f, uint8_t *h, uint8_t *pixels, uint8_t pal[16][3], Xld4Progress cb, void *data)
{
    Q4Work *w = calloc(1, sizeof(*w));
    uint8_t colors[96], block[6]; unsigned n, len, dots, pos = 0, i;
    int err = XLD4_ERR_DATA;
    if (!w) return XLD4_ERR_MEMORY;
    len = word(h + 16);
    if (!read_bytes(f, w->packed, len) || !unpack(w, len, &n) ||
        !runlength(w, n, colors, 96, 1)) goto done;
    for (i = 0; i < 16; i++) {
        pal[i][0] = colors[i * 6 + 1] * 17;
        pal[i][1] = colors[i * 6 + 3] * 17;
        pal[i][2] = colors[i * 6 + 5] * 17;
    }
    n = ((h[4] & 2) != 0) + ((h[4] & 8) != 0);
    for (i = 0; i < n; i++)
        if (!read_bytes(f, block, 6) || !read_bytes(f, NULL, word(block))) goto done;
    while (pos < PIXELS) {
        if (cb && cb((int)pos, PIXELS, data)) { err = XLD4_ERR_CANCELLED; goto done; }
        if (!read_bytes(f, block, 6)) goto done;
        len = word(block); dots = word(block + 4) * 2;
        if (!dots || dots > PIXELS - pos || !read_bytes(f, w->packed, len) ||
            !unpack(w, len, &n) || !runlength(w, n, pixels + pos, dots, 0)) goto done;
        pos += dots;
    }
    err = 0;
done:
    free(w); return err;
}

static int delta(Reader *f, uint8_t *row, const uint8_t *base, const uint8_t *esc)
{
    uint8_t buf[255], size; unsigned i = 1, pos, c, d, count;
    memcpy(row, base, 160);
    if (!read_bytes(f, &size, 1) || !read_bytes(f, buf, size)) return 0;
    if (!size) return 1;
    pos = buf[0];
    if (pos > 160) return 0;
    while (i < size) {
        c = buf[i++]; count = 1;
        if (c == esc[0] || c == esc[1] || c == esc[2]) {
            if (i >= size) return 0;
            d = buf[i++];
            if (d != c) {
                if (c == esc[0]) { c = 0; count = d; }
                else if (c == esc[1]) { c = 255; count = d; }
                else { if (i >= size) return 0; c = d; count = buf[i++]; }
            }
        }
        if (count > 160 - pos) return 0;
        while (count--) row[pos++] ^= (uint8_t)c;
    }
    return 1;
}
static int qld(Reader *f, uint8_t *h, uint8_t *pixels, uint8_t pal[16][3], Xld4Progress cb, void *data)
{
    uint8_t rows[3][160] = {0}, prev[3][160] = {0}, mode;
    unsigned x1 = 0, x2 = 80, y1 = 0, y2 = 200, y, p, x, bit;
    if ((h[10] & 15) == 2) { x1 = h[2]; x2 = h[3]; y1 = h[4]; y2 = h[5]; }
    for (p = 0; p < 8; p++) {
        pal[p][2] = (p & 1) ? 255 : 0;
        pal[p][0] = (p & 2) ? 255 : 0;
        pal[p][1] = (p & 4) ? 255 : 0;
    }
    /* Match the palette used by the supplied IFQLD.SPI. */
    pal[3][1] = 170; pal[6][2] = 170;
    for (y = y1; y < y2; y++) {
        if (cb && cb((int)y, 200, data)) return XLD4_ERR_CANCELLED;
        if (!read_bytes(f, &mode, 1)) return XLD4_ERR_DATA;
        if (mode) {
            if (!delta(f, rows[0], prev[0], h + 7) ||
                !delta(f, rows[1], ((mode >> 2) & 3) == 1 ? prev[1] : rows[0], h + 7) ||
                !delta(f, rows[2], ((mode >> 4) & 3) == 1 ? prev[2] :
                    ((mode >> 4) & 3) == 2 ? rows[0] : rows[1], h + 7)) return XLD4_ERR_DATA;
        }
        memcpy(prev, rows, sizeof(rows));
        for (p = 0; p < 3; p++) for (x = 0; x < x2 - x1; x++) {
            uint8_t a = rows[p][2*x], b = rows[p][2*x+1];
            unsigned top = (a & 240) | (b >> 4), bottom = ((a & 15) << 4) | (b & 15);
            for (bit = 0; bit < 8; bit++) {
                pixels[(y*2)*640+(x+x1)*8+bit] |= ((top >> (7-bit)) & 1) << p;
                pixels[(y*2+1)*640+(x+x1)*8+bit] |= ((bottom >> (7-bit)) & 1) << p;
            }
        }
    }
    return 0;
}

static void set_info(Xld4Info *info, int type)
{
    info->width = 640;
    info->height = 400;
    info->bits_per_pixel = type == 1 ? 4 : 3;
    info->palette_entries = type == 1 ? 16 : 8;
    info->format = (Xld4Format)type;
}

int xld4_probe(const uint8_t *data, size_t size)
{
    return data && kind(data, size) != 0;
}

int xld4_get_info(const uint8_t *data, size_t size, Xld4Info *info)
{
    Reader reader;
    uint8_t h[32];
    int type, err;
    if (!info) return XLD4_ERR_DATA;
    memset(info, 0, sizeof(*info));
    if (!data) return XLD4_ERR_DATA;
    reader.data = data; reader.remaining = size;
    err = header(&reader, h, &type);
    if (!err) set_info(info, type);
    return err;
}

int xld4_decode(const uint8_t *data, size_t size, Xld4Image *image,
                Xld4Progress progress, void *user)
{
    Reader reader;
    uint8_t h[32];
    int type, err;
    if (!image) return XLD4_ERR_DATA;
    memset(image, 0, sizeof(*image));
    if (!data) return XLD4_ERR_DATA;
    reader.data = data; reader.remaining = size;
    err = header(&reader, h, &type);
    if (err) return err;
    image->pixels = calloc(PIXELS, 1);
    if (!image->pixels) return XLD4_ERR_MEMORY;
    err = type == 1 ? q4(&reader, h, image->pixels, image->palette, progress, user)
                    : qld(&reader, h, image->pixels, image->palette, progress, user);
    if (!err && progress && progress(1, 1, user)) err = XLD4_ERR_CANCELLED;
    if (err) xld4_free(image);
    else set_info(&image->info, type);
    return err;
}

void xld4_free(Xld4Image *image)
{
    if (image) {
        free(image->pixels);
        memset(image, 0, sizeof(*image));
    }
}
