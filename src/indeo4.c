/* Indeo Video Interactive 4 ("IV41"): picture and band headers, macroblock info, buffers.
 *
 * Unlike Indeo 5, every picture carries its layout, and each band chooses its transform, scan
 * order and quantisation matrix. Frames can be bidirectional (predicted from the previous and
 * the next reference). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivi.h"

static int debug_enabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("OI_DEBUG") != NULL;
    return v;
}

/* Transforms by id: inverse, DC-only, and whether the DC is predicted (two-dimensional). The DCT
 * ids (7-9, 17) and 12 (none) are not used by the Intel encoder. */
static const struct {
    oi_inv_transform inv;
    oi_dc_transform dc;
    int is_2d;
} transforms[18] = {
    {oi_inverse_haar_8x8, oi_dc_haar_2d, 1},
    {oi_row_haar8, oi_dc_haar_2d, 0},
    {oi_col_haar8, oi_dc_haar_2d, 0},
    {oi_put_pixels_8x8, oi_put_dc_pixel_8x8, 1},
    {oi_inverse_slant_8x8, oi_dc_slant_2d, 1},
    {oi_row_slant8, oi_dc_row_slant, 1},
    {oi_col_slant8, oi_dc_col_slant, 1},
    {NULL, NULL, 0},
    {NULL, NULL, 0},
    {NULL, NULL, 0},
    {oi_inverse_haar_4x4, oi_dc_haar_2d, 1},
    {oi_inverse_slant_4x4, oi_dc_slant_2d, 1},
    {NULL, NULL, 0},
    {oi_row_haar4, oi_dc_haar_2d, 0},
    {oi_col_haar4, oi_dc_haar_2d, 0},
    {oi_row_slant4, oi_dc_row_slant, 0},
    {oi_col_slant4, oi_dc_col_slant, 0},
    {NULL, NULL, 0},
};

/* Scan orders by index: 0-4 for 8x8 blocks, 5-9 for 4x4. */
static const uint8_t *scan_tab(int i) {
    switch (i) {
    case 0: return oi_zigzag_scan_8x8;
    case 1: return oi_alternate_scan_8x8;
    case 2: return oi_horizontal_scan_8x8;
    case 3: return oi_vertical_scan_8x8;
    case 4: return oi_zigzag_scan_8x8;
    case 5: return oi_direct_scan_4x4;
    case 6: return oi_alternate_scan_4x4;
    case 7: return oi_vertical_scan_4x4;
    case 8: return oi_horizontal_scan_4x4;
    case 9: return oi_direct_scan_4x4;
    default: return oi_horizontal_scan_8x8;
    }
}

/* A plane's band count: 1, or 4 (a wavelet level whose four quarters are each one band). */
static int plane_subdivision(oi_bits *gb) {
    switch (oi_get_bits(gb, 2)) {
    case 3: return 1;
    case 2:
        for (int i = 0; i < 4; i++)
            if (oi_get_bits(gb, 2) != 3) return 0;
        return 4;
    default: return 0;
    }
}

static int scale_tile_size(int def, int factor) { return factor == 15 ? def : (factor + 1) << 5; }

int oi_ivi4_is_nonnull(const oi_decoder *d) { return d->frame_type < OI_FRAME4_NULL_FIRST; }

int oi_ivi4_decode_pic_hdr(oi_decoder *d) {
    oi_bits *gb = &d->gb;
    if (oi_get_bits(gb, 18) != 0x3fff8) return OI_ERR_INVALID;  /* picture start code */
    d->prev_frame_type = d->frame_type;
    d->frame_type = (int)oi_get_bits(gb, 3);
    if (d->frame_type == 7) return OI_ERR_INVALID;
    d->has_transp = (int)oi_get_bit(gb);
    if (oi_get_bit(gb)) return OI_ERR_INVALID;  /* sync bit */
    d->data_size = oi_get_bit(gb) ? (int)oi_get_bits(gb, 24) : 0;
    if (debug_enabled())
        fprintf(stderr, "[iv41] frame type %d transp %d data size %d\n", d->frame_type, d->has_transp, d->data_size);
    if (d->frame_type >= OI_FRAME4_NULL_FIRST) return OI_OK;  /* null frames carry nothing else */
    if (oi_get_bit(gb)) oi_skip_bits(gb, 32);  /* lock word of a protected clip */
    oi_pic_conf cfg;
    memset(&cfg, 0, sizeof(cfg));
    int size_index = (int)oi_get_bits(gb, 3);
    if (size_index == 7) {
        cfg.pic_height = (int)oi_get_bits(gb, 16);
        cfg.pic_width = (int)oi_get_bits(gb, 16);
    } else {
        cfg.pic_height = oi_ivi4_pic_sizes[size_index * 2 + 1];
        cfg.pic_width = oi_ivi4_pic_sizes[size_index * 2];
    }
    d->uses_tiling = (int)oi_get_bit(gb);
    if (d->uses_tiling) {
        cfg.tile_height = scale_tile_size(cfg.pic_height, (int)oi_get_bits(gb, 4));
        cfg.tile_width = scale_tile_size(cfg.pic_width, (int)oi_get_bits(gb, 4));
    } else {
        cfg.tile_height = cfg.pic_height;
        cfg.tile_width = cfg.pic_width;
    }
    if (oi_get_bits(gb, 2)) return OI_ERR_UNSUPPORTED;  /* only YVU9 (4:1:0) */
    if (cfg.pic_width <= 0 || cfg.pic_height <= 0) return OI_ERR_INVALID;
    cfg.chroma_height = (cfg.pic_height + 3) >> 2;
    cfg.chroma_width = (cfg.pic_width + 3) >> 2;
    cfg.luma_bands = plane_subdivision(gb);
    cfg.chroma_bands = cfg.luma_bands ? plane_subdivision(gb) : 0;
    if (!cfg.luma_bands || !cfg.chroma_bands) return OI_ERR_INVALID;
    d->is_scalable = cfg.luma_bands != 1 || cfg.chroma_bands != 1;
    if (d->is_scalable) return OI_ERR_UNSUPPORTED;  /* wavelet bands: not yet */
    if (debug_enabled())
        fprintf(stderr, "[iv41] %dx%d tiles %dx%d bands %d/%d\n", cfg.pic_width, cfg.pic_height, cfg.tile_width, cfg.tile_height,
                cfg.luma_bands, cfg.chroma_bands);
    if (!d->configured || memcmp(&cfg, &d->pic_conf, sizeof(cfg)) != 0) {
        int r = oi_init_planes(d, &cfg, 1);
        if (r != OI_OK) {
            d->configured = 0;
            return r;
        }
        d->pic_conf = cfg;
        d->configured = 1;
        for (int p = 0; p <= 2; p++)
            for (int i = 0; i < (!p ? cfg.luma_bands : cfg.chroma_bands); i++) {
                d->planes[p].bands[i].mb_size = !p ? (!d->is_scalable ? 16 : 8) : 4;
                d->planes[p].bands[i].blk_size = !p ? 8 : 4;
            }
        r = oi_init_tiles(d, cfg.tile_width, cfg.tile_height);
        if (r != OI_OK) return r;
    }
    d->frame_num = oi_get_bit(gb) ? (int)oi_get_bits(gb, 20) : 0;
    if (oi_get_bit(gb)) oi_skip_bits(gb, 8);  /* decoding time estimate */
    int r = oi_dec_huff_desc(gb, (int)oi_get_bit(gb), 0, &d->mb_vlc);
    if (r == OI_OK) r = oi_dec_huff_desc(gb, (int)oi_get_bit(gb), 1, &d->blk_vlc4);
    if (r != OI_OK) return r;
    d->rvmap_sel4 = oi_get_bit(gb) ? (int)oi_get_bits(gb, 3) : 8;
    d->in_imf = (int)oi_get_bit(gb);
    d->in_q = (int)oi_get_bit(gb);
    d->pic_glob_quant = (int)oi_get_bits(gb, 5);
    if (oi_get_bit(gb)) oi_skip_bits(gb, 3);       /* not interpreted */
    d->checksum = oi_get_bit(gb) ? (int)oi_get_bits(gb, 16) : 0;
    while (oi_get_bit(gb) && !gb->overread) oi_skip_bits(gb, 8);  /* header extension */
    if (oi_get_bit(gb)) return OI_ERR_UNSUPPORTED;  /* "bad blocks" */
    oi_align(gb);
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

int oi_ivi4_decode_band_hdr(oi_decoder *d, oi_band *band) {
    oi_bits *gb = &d->gb;
    int plane = (int)oi_get_bits(gb, 2);
    int band_num = (int)oi_get_bits(gb, 4);
    if (band->plane != plane || band->band_num != band_num) return OI_ERR_INVALID;
    band->is_empty = (int)oi_get_bit(gb);
    if (!band->is_empty) {
        int old_blk_size = band->blk_size;
        if (oi_get_bit(gb)) oi_skip_bits(gb, 16);  /* header size */
        band->is_halfpel = (int)oi_get_bits(gb, 2);
        if (band->is_halfpel >= 2) return OI_ERR_UNSUPPORTED;  /* quarter-pel */
        if (!band->is_halfpel) d->uses_fullpel = 1;
        band->checksum_present = (int)oi_get_bit(gb);
        if (band->checksum_present) band->checksum = (int)oi_get_bits(gb, 16);
        int indx = (int)oi_get_bits(gb, 2);
        if (indx == 3) return OI_ERR_INVALID;
        band->mb_size = 16 >> indx;
        band->blk_size = 8 >> (indx >> 1);
        band->inherit_mv = (int)oi_get_bit(gb);
        band->inherit_qdelta = (int)oi_get_bit(gb);
        band->glob_quant = (int)oi_get_bits(gb, 5);
        if (!oi_get_bit(gb) || d->frame_type == OI_FRAME_INTRA) {
            int tid = (int)oi_get_bits(gb, 5);
            if (tid >= 18 || !transforms[tid].inv) return OI_ERR_UNSUPPORTED;
            if (tid < 10 && band->blk_size < 8) return OI_ERR_INVALID;
            if (tid <= 2 || tid == 10 || tid == 13 || tid == 14) return OI_ERR_UNSUPPORTED;  /* Haar: not yet */
            band->inv_transform = transforms[tid].inv;
            band->dc_transform = transforms[tid].dc;
            band->is_2d_trans = transforms[tid].is_2d;
            band->transform_size = tid < 10 ? 8 : 4;
            if (band->blk_size != band->transform_size) return OI_ERR_INVALID;
            int scan = (int)oi_get_bits(gb, 4);
            if (scan == 15) return OI_ERR_UNSUPPORTED;  /* custom scan */
            if (scan > 4 && scan < 10) {
                if (band->blk_size != 4) return OI_ERR_INVALID;
            } else if (band->blk_size != 8) {
                return OI_ERR_INVALID;
            }
            band->scan = scan_tab(scan);
            band->scan_size = band->blk_size * band->blk_size;
            int quant_mat = (int)oi_get_bits(gb, 5);
            if (quant_mat == 31) return OI_ERR_UNSUPPORTED;  /* custom matrix */
            if (quant_mat >= 22) return OI_ERR_INVALID;
            band->quant_mat = quant_mat;
            if (debug_enabled())
                fprintf(stderr, "[iv41]   band %d.%d mb %d blk %d halfpel %d transform %d scan %d quant %d\n", plane, band_num,
                        band->mb_size, band->blk_size, band->is_halfpel, tid, scan, quant_mat);
        } else if (old_blk_size != band->blk_size) {
            return OI_ERR_INVALID;
        }
        if (oi_ivi4_quant_index_to_tab[band->quant_mat] > 4 && band->blk_size == 4) return OI_ERR_INVALID;
        if (band->transform_size == 8 && band->blk_size < 8) return OI_ERR_INVALID;
        if (!oi_get_bit(gb))
            band->blk_vlc = d->blk_vlc4;
        else if (oi_dec_huff_desc(gb, 1, 1, &band->blk_vlc) != OI_OK)
            return OI_ERR_INVALID;
        band->rvmap_sel = oi_get_bit(gb) ? (int)oi_get_bits(gb, 3) : 8;
        band->num_corr = 0;
        if (oi_get_bit(gb)) {
            band->num_corr = (int)oi_get_bits(gb, 8);
            if (band->num_corr > 61) return OI_ERR_INVALID;
            for (int i = 0; i < band->num_corr * 2; i++) band->corr[i] = (uint8_t)oi_get_bits(gb, 8);
        }
    }
    int t = oi_ivi4_quant_index_to_tab[band->quant_mat];
    if (band->blk_size == 8) {
        band->intra_base = oi_ivi4_quant_8x8_intra[t];
        band->inter_base = oi_ivi4_quant_8x8_inter[t];
    } else {
        band->intra_base = oi_ivi4_quant_4x4_intra[t];
        band->inter_base = oi_ivi4_quant_4x4_inter[t];
    }
    band->intra_scale = band->inter_scale = NULL;  /* Indeo 4 quantises directly */
    oi_align(gb);
    if (!band->is_empty && !band->scan) return OI_ERR_INVALID;
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

static int scale_mv(int mv, int scale) { return (mv + (mv > 0) + (scale - 1)) >> scale; }

/* As Indeo 5, with a two-bit type in bidirectional frames (2 backward, 3 both: a second vector
 * delta, the backward vector being the negated sum). */
int oi_ivi4_decode_mb_info(oi_decoder *d, oi_band *band, oi_tile *tile) {
    oi_bits *gb = &d->gb;
    oi_mb *mb = tile->mbs, *ref_mb = tile->ref_mbs;
    int mb_size = band->mb_size;
    int blks = mb_size != band->blk_size ? 4 : 1;
    int type_bits = d->frame_type == OI_FRAME4_BIDIR ? 2 : 1;
    int mv_scale = (d->planes[0].bands[0].mb_size >> 3) - (mb_size >> 3);
    int mv_x = 0, mv_y = 0;
    int first_band = !band->plane && !band->band_num;
    if (((tile->width + mb_size - 1) / mb_size) * ((tile->height + mb_size - 1) / mb_size) != tile->num_mbs) return OI_ERR_INVALID;
    int offs = tile->ypos * band->pitch + tile->xpos;
    for (int y = tile->ypos; y < tile->ypos + tile->height; y += mb_size) {
        int mb_offset = offs;
        for (int x = tile->xpos; x < tile->xpos + tile->width; x += mb_size) {
            mb->xpos = x;
            mb->ypos = y;
            mb->buf_offs = mb_offset;
            mb->b_mv_x = mb->b_mv_y = 0;
            if (oi_get_bit(gb)) {
                if (d->frame_type == OI_FRAME_INTRA) return OI_ERR_INVALID;
                mb->type = 1;
                mb->cbp = 0;
                mb->q_delta = 0;
                if (first_band && d->in_q) {
                    int v = oi_get_vlc(gb, &d->mb_vlc);
                    if (v < 0) return OI_ERR_INVALID;
                    mb->q_delta = oi_to_signed(v);
                }
                mb->mv_x = mb->mv_y = 0;
                if (band->inherit_mv && ref_mb) {
                    mb->mv_x = mv_scale ? scale_mv(ref_mb->mv_x, mv_scale) : ref_mb->mv_x;
                    mb->mv_y = mv_scale ? scale_mv(ref_mb->mv_y, mv_scale) : ref_mb->mv_y;
                }
            } else {
                if (band->inherit_mv) {
                    if (!ref_mb) return OI_ERR_INVALID;
                    mb->type = ref_mb->type;
                } else if (d->frame_type == OI_FRAME_INTRA || d->frame_type == OI_FRAME4_INTRA1) {
                    mb->type = 0;
                } else {
                    mb->type = (int)oi_get_bits(gb, type_bits);
                }
                mb->cbp = (int)oi_get_bits(gb, blks);
                mb->q_delta = 0;
                if (band->inherit_qdelta) {
                    if (ref_mb) mb->q_delta = ref_mb->q_delta;
                } else if (mb->cbp || (first_band && d->in_q)) {
                    int v = oi_get_vlc(gb, &d->mb_vlc);
                    if (v < 0) return OI_ERR_INVALID;
                    mb->q_delta = oi_to_signed(v);
                }
                if (!mb->type) {
                    mb->mv_x = mb->mv_y = 0;
                } else {
                    if (band->inherit_mv) {
                        if (ref_mb) {
                            mb->mv_x = mv_scale ? scale_mv(ref_mb->mv_x, mv_scale) : ref_mb->mv_x;
                            mb->mv_y = mv_scale ? scale_mv(ref_mb->mv_y, mv_scale) : ref_mb->mv_y;
                        }
                    } else {
                        int dy = oi_get_vlc(gb, &d->mb_vlc), dx = oi_get_vlc(gb, &d->mb_vlc);
                        if (dy < 0 || dx < 0) return OI_ERR_INVALID;
                        mv_y += oi_to_signed(dy);
                        mv_x += oi_to_signed(dx);
                        mb->mv_x = mv_x;
                        mb->mv_y = mv_y;
                        if (mb->type == 3) {
                            dy = oi_get_vlc(gb, &d->mb_vlc);
                            dx = oi_get_vlc(gb, &d->mb_vlc);
                            if (dy < 0 || dx < 0) return OI_ERR_INVALID;
                            mv_y += oi_to_signed(dy);
                            mv_x += oi_to_signed(dx);
                            mb->b_mv_x = -mv_x;
                            mb->b_mv_y = -mv_y;
                        }
                    }
                    if (mb->type == 2) {
                        mb->b_mv_x = -mb->mv_x;
                        mb->b_mv_y = -mb->mv_y;
                        mb->mv_x = mb->mv_y = 0;
                    }
                }
            }
            mb++;
            if (ref_mb) ref_mb++;
            mb_offset += mb_size;
        }
        offs += mb_size * band->pitch;
    }
    oi_align(gb);
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

/* Reference frames (intra, intra1, inter) swap the decode and reference buffers; after a
 * reference frame a bidirectional one decodes into the spare buffer, predicting from the
 * previous reference (backward) and the one before it. */
void oi_ivi4_switch_buffers(oi_decoder *d) {
    int is_prev_ref = d->prev_frame_type == OI_FRAME_INTRA || d->prev_frame_type == OI_FRAME4_INTRA1 || d->prev_frame_type == OI_FRAME4_INTER;
    int is_ref = d->frame_type == OI_FRAME_INTRA || d->frame_type == OI_FRAME4_INTRA1 || d->frame_type == OI_FRAME4_INTER;
    if (is_prev_ref && is_ref) {
        int t = d->dst_buf;
        d->dst_buf = d->ref_buf;
        d->ref_buf = t;
    } else if (is_prev_ref) {
        int t = d->ref_buf;
        d->ref_buf = d->b_ref_buf;
        d->b_ref_buf = t;
        t = d->dst_buf;
        d->dst_buf = d->ref_buf;
        d->ref_buf = t;
    }
}

/* --- Haar transforms and recomposition: not yet written (the streams that need them are
 * rejected as unsupported before these are reached) ---------------------------------------- */

void oi_inverse_haar_8x8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)in; (void)out; (void)pitch; (void)flags;
}
void oi_inverse_haar_4x4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)in; (void)out; (void)pitch; (void)flags;
}
void oi_row_haar8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)in; (void)out; (void)pitch; (void)flags;
}
void oi_col_haar8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)in; (void)out; (void)pitch; (void)flags;
}
void oi_row_haar4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)in; (void)out; (void)pitch; (void)flags;
}
void oi_col_haar4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)in; (void)out; (void)pitch; (void)flags;
}
void oi_dc_haar_2d(const int32_t *in, int16_t *out, int pitch, int blk_size) {
    (void)in; (void)out; (void)pitch; (void)blk_size;
}
void oi_recompose_haar(const oi_plane *plane, uint8_t *dst, int dst_pitch) {
    (void)plane; (void)dst; (void)dst_pitch;
}
void oi_recompose53(const oi_plane *plane, uint8_t *dst, int dst_pitch) {
    (void)plane; (void)dst; (void)dst_pitch;
}
