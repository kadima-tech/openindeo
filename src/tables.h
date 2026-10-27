/* Indeo 4/5 format constants (tables.c). */
#ifndef OPENINDEO_TABLES_H
#define OPENINDEO_TABLES_H

#include <stdint.h>

/* A Huffman codebook layout: row r holds 2^xbits[r] codes, each r one-bits, a zero bit (not
 * in the last row), then xbits[r] bits of index. */
typedef struct oi_huff_desc {
    int num_rows;
    uint8_t xbits[16];
} oi_huff_desc;

/* A run/value map: what each block symbol means. */
typedef struct oi_rvmap {
    uint8_t eob_sym, esc_sym;
    uint8_t runtab[256];
    int8_t valtab[256];
} oi_rvmap;

extern const oi_huff_desc oi_mb_huff_desc[8];
extern const oi_huff_desc oi_blk_huff_desc[8];
extern const oi_rvmap oi_rvmap_tabs[9];

extern const uint8_t oi_vertical_scan_8x8[64];
extern const uint8_t oi_horizontal_scan_8x8[64];
extern const uint8_t oi_direct_scan_4x4[16];
extern const uint8_t oi_alternate_scan_8x8[64];
extern const uint8_t oi_alternate_scan_4x4[16];
extern const uint8_t oi_vertical_scan_4x4[16];
extern const uint8_t oi_horizontal_scan_4x4[16];
extern const uint8_t oi_zigzag_scan_8x8[64];

extern const uint8_t oi_ivi5_pic_sizes[30];
extern const uint16_t oi_ivi5_base_quant_8x8_inter[5][64];
extern const uint16_t oi_ivi5_base_quant_8x8_intra[5][64];
extern const uint16_t oi_ivi5_base_quant_4x4_inter[16];
extern const uint16_t oi_ivi5_base_quant_4x4_intra[16];
extern const uint8_t oi_ivi5_scale_quant_8x8_inter[5][24];
extern const uint8_t oi_ivi5_scale_quant_8x8_intra[5][24];
extern const uint8_t oi_ivi5_scale_quant_4x4_inter[24];
extern const uint8_t oi_ivi5_scale_quant_4x4_intra[24];

extern const uint16_t oi_ivi4_pic_sizes[14];
extern const uint16_t oi_ivi4_quant_8x8_intra[9][64];
extern const uint16_t oi_ivi4_quant_8x8_inter[9][64];
extern const uint16_t oi_ivi4_quant_4x4_intra[5][16];
extern const uint16_t oi_ivi4_quant_4x4_inter[5][16];
extern const uint8_t oi_ivi4_quant_index_to_tab[22];

#endif
