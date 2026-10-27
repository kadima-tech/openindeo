/* Bit reading for Indeo streams: bits come least significant first from each byte, and
 * multi-bit fields put the first bit read in their lowest bit. Huffman codes are read a bit
 * at a time against their codebook layout. */
#ifndef OPENINDEO_BITS_H
#define OPENINDEO_BITS_H

#include <stddef.h>
#include <stdint.h>

#include "tables.h"

typedef struct oi_bits {
    const uint8_t *buf;
    size_t size_bits;
    size_t pos;
    int overread;  /* set once a read went past the end (reads then return zeros) */
} oi_bits;

static inline void oi_bits_init(oi_bits *b, const uint8_t *buf, size_t size) {
    b->buf = buf;
    b->size_bits = size * 8;
    b->pos = 0;
    b->overread = 0;
}

static inline unsigned oi_get_bit(oi_bits *b) {
    if (b->pos >= b->size_bits) {
        b->overread = 1;
        return 0;
    }
    unsigned v = (b->buf[b->pos >> 3] >> (b->pos & 7)) & 1;
    b->pos++;
    return v;
}

static inline uint32_t oi_get_bits(oi_bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)oi_get_bit(b) << i;
    return v;
}

static inline void oi_skip_bits(oi_bits *b, size_t n) {
    b->pos += n;
    if (b->pos > b->size_bits) {
        b->pos = b->size_bits;
        b->overread = 1;
    }
}

static inline void oi_align(oi_bits *b) {
    b->pos = (b->pos + 7) & ~(size_t)7;
    if (b->pos > b->size_bits) b->pos = b->size_bits;
}

/* A codebook: its layout and the first symbol of each row. */
typedef struct oi_vlc {
    oi_huff_desc desc;
    int base[17];
} oi_vlc;

static inline void oi_vlc_init(oi_vlc *v, const oi_huff_desc *d) {
    v->desc = *d;
    v->base[0] = 0;
    for (int r = 0; r < d->num_rows; r++) v->base[r + 1] = v->base[r] + (1 << d->xbits[r]);
}

/* The next symbol: the row is the count of leading one-bits (the last row has no closing zero),
 * then the row's index bits, first bit most significant. Codebooks hold at most 256 symbols;
 * a code past that returns -1. */
static inline int oi_get_vlc(oi_bits *b, const oi_vlc *v) {
    int rows = v->desc.num_rows;
    int row = 0;
    while (row < rows - 1 && oi_get_bit(b)) row++;
    int n = v->desc.xbits[row];
    if (rows == 1 && n == 0) {  /* a lone zero-length code is sent as one bit */
        oi_get_bit(b);
        return 0;
    }
    int j = 0;
    for (int k = 0; k < n; k++) j = (j << 1) | (int)oi_get_bit(b);
    int sym = v->base[row] + j;
    return sym < 256 ? sym : -1;
}

/* IVI_TOSIGNED: 0, 1, -1, 2, -2, ... */
static inline int oi_to_signed(int v) {
    return (v & 1) ? (v >> 1) + 1 : -(v >> 1);
}

#endif
