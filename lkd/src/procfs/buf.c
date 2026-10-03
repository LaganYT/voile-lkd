/* buf.c: growable text buffer for generated files. */
#include <stdarg.h>

#include "procfs_priv.h"

void pb_need(struct pbuf *b, size_t more)
{
    if (b->oom) return;
    if (b->n + more + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 512;
    while (cap < b->n + more + 1) cap *= 2;
    if (cap > PF_TEXT_MAX) { b->oom = 1; return; }
    char *p = realloc(b->p, cap);
    if (!p) { b->oom = 1; return; }
    b->p = p;
    b->cap = cap;
}

void pb_write(struct pbuf *b, const void *s, size_t n)
{
    pb_need(b, n);
    if (b->oom) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void pb_putc(struct pbuf *b, char c) { pb_write(b, &c, 1); }
void pb_puts(struct pbuf *b, const char *s) { pb_write(b, s, strlen(s)); }

void pb_printf(struct pbuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w < sizeof tmp) { pb_write(b, tmp, (size_t)w); return; }
    pb_need(b, (size_t)w);
    if (b->oom) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)w + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)w;
}

void pb_udec_w(struct pbuf *b, const char *prefix, uint64_t v, int width)
{
    pb_puts(b, prefix);
    pb_printf(b, "%*llu", width, (unsigned long long)v);
}

/* lib/vsprintf.c bitmap_string: 32-bit chunks, most significant first, comma separated, each chunk
 * zero-padded to its width (the first one to the bits it holds). */
void pb_mask(struct pbuf *b, uint64_t mask, int nbits)
{
    if (nbits <= 0) return;
    int chunksz = nbits & 31;
    if (!chunksz) chunksz = 32;
    int first = 1;
    for (int i = ((nbits + 31) & ~31) - 32; i >= 0; i -= 32) {
        uint64_t cm = chunksz == 64 ? ~0ull : ((1ull << chunksz) - 1);
        uint64_t val = i < 64 ? (mask >> i) & cm : 0;
        if (!first) pb_putc(b, ',');
        pb_printf(b, "%0*llx", (chunksz + 3) / 4, (unsigned long long)val);
        first = 0;
        chunksz = 32;
    }
}

/* lib/vsprintf.c bitmap_list_string: "0-3,5". */
void pb_mask_list(struct pbuf *b, uint64_t mask, int nbits)
{
    int first = 1;
    for (int i = 0; i < nbits && i < 64; i++) {
        if (!(mask >> i & 1)) continue;
        int j = i;
        while (j + 1 < nbits && j + 1 < 64 && (mask >> (j + 1) & 1)) j++;
        if (!first) pb_putc(b, ',');
        if (j > i) pb_printf(b, "%d-%d", i, j);
        else pb_printf(b, "%d", i);
        first = 0;
        i = j;
    }
}

void pb_free(struct pbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}
