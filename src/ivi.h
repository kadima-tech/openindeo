/* The decoder's state, shared by Indeo 4 and 5: pictures are three planes (Y, V, U), each one or
 * more wavelet bands; a band is split into tiles, tiles into macroblocks, macroblocks into one
 * or four transform blocks. Band buffers hold signed samples (output adds 128). */
#ifndef OPENINDEO_IVI_H
#define OPENINDEO_IVI_H

#include <stdint.h>

#include "bits.h"
#include "openindeo.h"
#include "tables.h"

enum {
    OI_FRAME_INTRA = 0,
    OI_FRAME_INTER = 1,
    OI_FRAME_INTER_SCAL = 2,  /* Indeo 5: references the last scalable frame */
    OI_FRAME_INTER_NOREF = 3, /* not kept as a reference */
    OI_FRAME_NULL = 4,        /* nothing changes */
    /* Indeo 4 */
    OI_FRAME4_INTRA1 = 1,
    OI_FRAME4_INTER = 2,
    OI_FRAME4_BIDIR = 3,
    OI_FRAME4_INTER_NOREF = 4,
    OI_FRAME4_NULL_FIRST = 5,
    OI_FRAME4_NULL_LAST = 6
};

typedef struct oi_mb {
    int xpos, ypos;
    int buf_offs;
    int type;     /* 0 intra, 1 inter, Indeo 4: 2 backward, 3 bidirectional */
    int cbp;      /* coded blocks, block 0 in bit 0 */
    int q_delta;
    int mv_x, mv_y;      /* in the band's units (half pixels when halfpel) */
    int b_mv_x, b_mv_y;  /* Indeo 4 backward vector */
} oi_mb;

typedef struct oi_tile {
    int xpos, ypos, width, height;
    int mb_size;
    int is_empty;
    int data_size;
    int num_mbs;
    oi_mb *mbs;
    oi_mb *ref_mbs;  /* the first luma band's tile, for inherited vectors and quant */
} oi_tile;

struct oi_band;
typedef void (*oi_inv_transform)(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
typedef void (*oi_dc_transform)(const int32_t *in, int16_t *out, int pitch, int blk_size);

typedef struct oi_band {
    int plane, band_num;
    int width, height;
    int aheight;
    int pitch;
    int16_t *bufs[4];
    int16_t *buf, *ref_buf, *b_ref_buf;
    int mb_size, blk_size;
    int is_halfpel;
    int inherit_mv, inherit_qdelta, qdelta_present;
    int quant_mat;
    int glob_quant;
    const uint8_t *scan;
    int is_2d_trans;
    int transform_size;
    oi_inv_transform inv_transform;
    oi_dc_transform dc_transform;
    const uint16_t *intra_base, *inter_base;
    const uint8_t *intra_scale, *inter_scale;
    oi_vlc blk_vlc;
    int num_corr;
    uint8_t corr[61 * 2];
    int rvmap_sel;
    int is_empty;
    int data_size;
    int checksum_present, checksum;
    int num_tiles;
    oi_tile *tiles;
    /* Indeo 4 */
    int is_transparent;
    int scan_size;
} oi_band;

typedef struct oi_plane {
    int width, height;
    int num_bands;
    oi_band bands[4];
} oi_plane;

typedef struct oi_pic_conf {
    int pic_width, pic_height;
    int chroma_width, chroma_height;
    int tile_width, tile_height;
    int luma_bands, chroma_bands;
} oi_pic_conf;

struct oi_decoder {
    int indeo4;
    oi_bits gb;
    const uint8_t *frame_data;
    size_t frame_size;

    oi_pic_conf pic_conf;
    int configured;
    oi_plane planes[3];
    oi_rvmap rvmap[9];  /* per-band corrections are applied to these copies and undone */

    int frame_type, prev_frame_type;
    int frame_num;
    uint32_t frame_flags;
    int pic_hdr_size;
    int checksum;
    oi_vlc mb_vlc;
    int gop_flags;
    int gop_hdr_size;
    int lock_word;
    int is_scalable;
    int gop_invalid;
    int has_transp;
    int is_halfpel;  /* Indeo 4 picture-level */

    int buf_switch, dst_buf, ref_buf, ref2_buf, b_ref_buf;
    int inter_scal;
    int buf_valid[4];
    int have_output;

    /* Indeo 4 */
    int uses_tiling, uses_haar, uses_fullpel;
    int data_size;
    oi_vlc blk_vlc4;  /* the picture's block codebook, for bands without their own */
    int rvmap_sel4;
    int in_imf, in_q;
    int pic_glob_quant;

    /* output */
    uint8_t *out_y, *out_u, *out_v, *out_a;
    int out_w, out_h, out_cw, out_ch;
    int out_is_key;
};

/* ivi.c */
int oi_init_planes(oi_decoder *d, const oi_pic_conf *cfg, int indeo4);
int oi_init_tiles(oi_decoder *d, int tile_width, int tile_height);
void oi_free_planes(oi_decoder *d);
int oi_dec_huff_desc(oi_bits *b, int desc_coded, int blk, oi_vlc *out);
void oi_skip_hdr_extension(oi_bits *b);
int oi_decode_frame(oi_decoder *d, const uint8_t *data, size_t size);

/* indeo5.c / indeo4.c */
int oi_ivi5_decode_pic_hdr(oi_decoder *d);
int oi_ivi5_decode_band_hdr(oi_decoder *d, oi_band *band);
int oi_ivi5_decode_mb_info(oi_decoder *d, oi_band *band, oi_tile *tile);
void oi_ivi5_switch_buffers(oi_decoder *d);
int oi_ivi4_decode_pic_hdr(oi_decoder *d);
int oi_ivi4_decode_band_hdr(oi_decoder *d, oi_band *band);
int oi_ivi4_decode_mb_info(oi_decoder *d, oi_band *band, oi_tile *tile);
void oi_ivi4_switch_buffers(oi_decoder *d);
int oi_ivi4_is_nonnull(const oi_decoder *d);

/* transforms.c */
void oi_inverse_slant_8x8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_inverse_slant_4x4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_row_slant8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_col_slant8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_row_slant4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_col_slant4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_inverse_haar_8x8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_inverse_haar_4x4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_row_haar8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_col_haar8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_row_haar4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_col_haar4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_put_pixels_8x8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_put_pixels_4x4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags);
void oi_dc_slant_2d(const int32_t *in, int16_t *out, int pitch, int blk_size);
void oi_dc_row_slant(const int32_t *in, int16_t *out, int pitch, int blk_size);
void oi_dc_col_slant(const int32_t *in, int16_t *out, int pitch, int blk_size);
void oi_dc_haar_2d(const int32_t *in, int16_t *out, int pitch, int blk_size);
void oi_put_dc_pixel_8x8(const int32_t *in, int16_t *out, int pitch, int blk_size);
void oi_put_dc_pixel_4x4(const int32_t *in, int16_t *out, int pitch, int blk_size);
void oi_recompose53(const oi_plane *plane, uint8_t *dst, int dst_pitch);
void oi_recompose_haar(const oi_plane *plane, uint8_t *dst, int dst_pitch);

#endif
