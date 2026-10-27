/* Indeo Video Interactive 5 ("IV50"): picture, GOP and band headers, and macroblock info. */
#include <string.h>

#include "ivi.h"

/* GOP header, sent with every intra frame: picture size, tiling, the band layout (each band's
 * macroblock and block size and its transform), and the dequantisation tables. */
static int decode_gop_header(oi_decoder *d) {
    oi_bits *gb = &d->gb;
    oi_pic_conf cfg;
    memset(&cfg, 0, sizeof(cfg));
    d->gop_flags = (int)oi_get_bits(gb, 8);
    d->gop_hdr_size = (d->gop_flags & 1) ? (int)oi_get_bits(gb, 16) : 0;
    if (d->gop_flags & 0x20) d->lock_word = (int)oi_get_bits(gb, 32);  /* protected stream */
    int tile_size = (d->gop_flags & 0x40) ? 64 << oi_get_bits(gb, 2) : 0;
    if (tile_size > 256) return OI_ERR_INVALID;
    /* wavelet levels: luma 0..3 x 3 + 1 bands, chroma 0..1 x 3 + 1 */
    cfg.luma_bands = (int)oi_get_bits(gb, 2) * 3 + 1;
    cfg.chroma_bands = (int)oi_get_bit(gb) * 3 + 1;
    d->is_scalable = cfg.luma_bands != 1 || cfg.chroma_bands != 1;
    if (d->is_scalable) return OI_ERR_UNSUPPORTED;  /* wavelet bands (and scalable inter frames): not yet */
    int size_index = (int)oi_get_bits(gb, 4);
    if (size_index == 15) {
        cfg.pic_height = (int)oi_get_bits(gb, 13);
        cfg.pic_width = (int)oi_get_bits(gb, 13);
    } else {
        cfg.pic_height = oi_ivi5_pic_sizes[size_index * 2 + 1] << 2;
        cfg.pic_width = oi_ivi5_pic_sizes[size_index * 2] << 2;
    }
    if (d->gop_flags & 2) return OI_ERR_UNSUPPORTED;  /* YV12 layout */
    if (cfg.pic_width <= 0 || cfg.pic_height <= 0) return OI_ERR_INVALID;
    cfg.chroma_height = (cfg.pic_height + 3) >> 2;
    cfg.chroma_width = (cfg.pic_width + 3) >> 2;
    if (!tile_size) {
        cfg.tile_height = cfg.pic_height;
        cfg.tile_width = cfg.pic_width;
    } else {
        cfg.tile_height = cfg.tile_width = tile_size;
    }
    int changed = !d->configured || memcmp(&cfg, &d->pic_conf, sizeof(cfg)) != 0;
    if (changed) {
        int r = oi_init_planes(d, &cfg, 0);
        if (r != OI_OK) {
            d->configured = 0;
            return r;
        }
        d->pic_conf = cfg;
        d->configured = 1;
    }
    int blk_size_changed = changed;
    for (int p = 0; p <= 1; p++) {
        int nb = !p ? cfg.luma_bands : cfg.chroma_bands;
        for (int i = 0; i < nb; i++) {
            oi_band *band = &d->planes[p].bands[i];
            band->is_halfpel = (int)oi_get_bit(gb);
            int mb_flag = (int)oi_get_bit(gb);
            int blk_size = 8 >> oi_get_bit(gb);
            int mb_size = blk_size << !mb_flag;
            if (p == 0 && blk_size == 4) return OI_ERR_UNSUPPORTED;  /* 4x4 luma blocks */
            if (mb_size != band->mb_size || blk_size != band->blk_size) {
                band->mb_size = mb_size;
                band->blk_size = blk_size;
                blk_size_changed = 1;
            }
            if (oi_get_bit(gb)) return OI_ERR_UNSUPPORTED;  /* extended transform info */
            /* transform and scan by plane and band */
            switch ((p << 2) + i) {
            case 0:
                band->inv_transform = oi_inverse_slant_8x8;
                band->dc_transform = oi_dc_slant_2d;
                band->scan = oi_zigzag_scan_8x8;
                band->transform_size = 8;
                break;
            case 1:
                band->inv_transform = oi_row_slant8;
                band->dc_transform = oi_dc_row_slant;
                band->scan = oi_vertical_scan_8x8;
                band->transform_size = 8;
                break;
            case 2:
                band->inv_transform = oi_col_slant8;
                band->dc_transform = oi_dc_col_slant;
                band->scan = oi_horizontal_scan_8x8;
                band->transform_size = 8;
                break;
            case 3:
                band->inv_transform = oi_put_pixels_8x8;
                band->dc_transform = oi_put_dc_pixel_8x8;
                band->scan = oi_horizontal_scan_8x8;
                band->transform_size = 8;
                break;
            case 4:
                band->inv_transform = oi_inverse_slant_4x4;
                band->dc_transform = oi_dc_slant_2d;
                band->scan = oi_direct_scan_4x4;
                band->transform_size = 4;
                break;
            }
            band->scan_size = band->transform_size * band->transform_size;
            band->is_2d_trans = band->inv_transform == oi_inverse_slant_8x8 || band->inv_transform == oi_inverse_slant_4x4;
            if (band->transform_size != band->blk_size) return OI_ERR_INVALID;
            /* dequantisation: luma by band (the single band of a one-band plane uses table 0),
             * chroma its own */
            int quant_mat = !p ? (cfg.luma_bands > 1 ? i + 1 : 0) : 5;
            if (band->blk_size == 8) {
                if (quant_mat >= 5) return OI_ERR_INVALID;
                band->intra_base = oi_ivi5_base_quant_8x8_intra[quant_mat];
                band->inter_base = oi_ivi5_base_quant_8x8_inter[quant_mat];
                band->intra_scale = oi_ivi5_scale_quant_8x8_intra[quant_mat];
                band->inter_scale = oi_ivi5_scale_quant_8x8_inter[quant_mat];
            } else {
                band->intra_base = oi_ivi5_base_quant_4x4_intra;
                band->inter_base = oi_ivi5_base_quant_4x4_inter;
                band->intra_scale = oi_ivi5_scale_quant_4x4_intra;
                band->inter_scale = oi_ivi5_scale_quant_4x4_inter;
            }
            if (oi_get_bits(gb, 2)) return OI_ERR_INVALID;  /* end marker */
        }
    }
    /* the second chroma plane is set up like the first */
    for (int i = 0; i < cfg.chroma_bands; i++) {
        oi_band *b1 = &d->planes[2].bands[i];
        const oi_band *b2 = &d->planes[1].bands[i];
        b1->is_halfpel = b2->is_halfpel;
        b1->mb_size = b2->mb_size;
        b1->blk_size = b2->blk_size;
        b1->inv_transform = b2->inv_transform;
        b1->dc_transform = b2->dc_transform;
        b1->scan = b2->scan;
        b1->scan_size = b2->scan_size;
        b1->transform_size = b2->transform_size;
        b1->is_2d_trans = b2->is_2d_trans;
        b1->intra_base = b2->intra_base;
        b1->inter_base = b2->inter_base;
        b1->intra_scale = b2->intra_scale;
        b1->inter_scale = b2->inter_scale;
    }
    if (blk_size_changed) {
        int r = oi_init_tiles(d, cfg.tile_width, cfg.tile_height);
        if (r != OI_OK) return r;
    }
    if (d->gop_flags & 8) {
        if (oi_get_bits(gb, 3)) return OI_ERR_INVALID;  /* alignment bits */
        if (oi_get_bit(gb)) oi_skip_bits(gb, 24);      /* transparency fill colour */
    }
    oi_align(gb);
    oi_skip_bits(gb, 23);  /* not interpreted */
    if (oi_get_bit(gb)) {  /* GOP extension */
        uint32_t w;
        do {
            w = oi_get_bits(gb, 16);
        } while ((w & 0x8000) && !gb->overread);
    }
    oi_align(gb);
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

int oi_ivi5_decode_pic_hdr(oi_decoder *d) {
    oi_bits *gb = &d->gb;
    if (oi_get_bits(gb, 5) != 0x1f) return OI_ERR_INVALID;  /* picture start code */
    d->prev_frame_type = d->frame_type;
    d->frame_type = (int)oi_get_bits(gb, 3);
    if (d->frame_type >= 5) return OI_ERR_INVALID;
    d->frame_num = (int)oi_get_bits(gb, 8);
    if (d->frame_type == OI_FRAME_INTRA) {
        d->gop_invalid = 1;
        int r = decode_gop_header(d);
        if (r != OI_OK) return r;
        d->gop_invalid = 0;
    }
    if (d->frame_type == OI_FRAME_INTER_SCAL && !d->is_scalable) return OI_ERR_INVALID;
    if (d->frame_type != OI_FRAME_NULL) {
        d->frame_flags = oi_get_bits(gb, 8);
        d->pic_hdr_size = (d->frame_flags & 1) ? (int)oi_get_bits(gb, 24) : 0;
        d->checksum = (d->frame_flags & 0x10) ? (int)oi_get_bits(gb, 16) : 0;
        if (d->frame_flags & 0x20) oi_skip_hdr_extension(gb);
        int r = oi_dec_huff_desc(gb, d->frame_flags & 0x40, 0, &d->mb_vlc);
        if (r != OI_OK) return r;
        oi_skip_bits(gb, 3);  /* not interpreted */
    }
    oi_align(gb);
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

int oi_ivi5_decode_band_hdr(oi_decoder *d, oi_band *band) {
    oi_bits *gb = &d->gb;
    int flags = (int)oi_get_bits(gb, 8);
    band->is_empty = flags & 1;
    if (band->is_empty) return OI_OK;
    band->data_size = (d->frame_flags & 0x80) ? (int)oi_get_bits(gb, 24) : 0;
    band->inherit_mv = flags & 2;
    band->inherit_qdelta = flags & 8;
    band->qdelta_present = flags & 4;
    if (!band->qdelta_present) band->inherit_qdelta = 1;
    /* run/value map corrections: pairs of symbols to swap */
    band->num_corr = 0;
    if (flags & 0x10) {
        band->num_corr = (int)oi_get_bits(gb, 8);
        if (band->num_corr > 61) return OI_ERR_INVALID;
        for (int i = 0; i < band->num_corr * 2; i++) band->corr[i] = (uint8_t)oi_get_bits(gb, 8);
    }
    band->rvmap_sel = (flags & 0x40) ? (int)oi_get_bits(gb, 3) : 8;
    int r = oi_dec_huff_desc(gb, flags & 0x80, 1, &band->blk_vlc);
    if (r != OI_OK) return r;
    band->checksum_present = (int)oi_get_bit(gb);
    if (band->checksum_present) band->checksum = (int)oi_get_bits(gb, 16);
    band->glob_quant = (int)oi_get_bits(gb, 5);
    if (flags & 0x20) {  /* band extension */
        oi_align(gb);
        oi_skip_hdr_extension(gb);
    }
    oi_align(gb);
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

/* Per macroblock: an empty flag; else the type (inter frames, unless inherited), the coded block
 * pattern, a quant delta and motion vector deltas (summed along the tile), or values inherited
 * from the first luma band, vectors scaled to this band. */
int oi_ivi5_decode_mb_info(oi_decoder *d, oi_band *band, oi_tile *tile) {
    oi_bits *gb = &d->gb;
    oi_mb *mb = tile->mbs, *ref_mb = tile->ref_mbs;
    if (!ref_mb && ((band->qdelta_present && band->inherit_qdelta) || band->inherit_mv)) return OI_ERR_INVALID;
    int mb_size = band->mb_size;
    if (tile->num_mbs != ((tile->height + mb_size - 1) / mb_size) * ((tile->width + mb_size - 1) / mb_size)) return OI_ERR_INVALID;
    int mv_scale = (d->planes[0].bands[0].mb_size >> 3) - (band->mb_size >> 3);
    int mv_x = 0, mv_y = 0;
    int offs = tile->ypos * band->pitch + tile->xpos;
    int first_band = !band->plane && !band->band_num;
    for (int y = tile->ypos; y < tile->ypos + tile->height; y += mb_size) {
        int mb_offset = offs;
        for (int x = tile->xpos; x < tile->xpos + tile->width; x += mb_size) {
            mb->xpos = x;
            mb->ypos = y;
            mb->buf_offs = mb_offset;
            if (oi_get_bit(gb)) {
                if (d->frame_type == OI_FRAME_INTRA) return OI_ERR_INVALID;  /* empty mb in an intra picture */
                mb->type = 1;
                mb->cbp = 0;
                mb->q_delta = 0;
                if (first_band && (d->frame_flags & 8)) {
                    int v = oi_get_vlc(gb, &d->mb_vlc);
                    if (v < 0) return OI_ERR_INVALID;
                    mb->q_delta = oi_to_signed(v);
                }
                mb->mv_x = mb->mv_y = 0;
                if (band->inherit_mv && ref_mb) {
                    mb->mv_x = mv_scale ? (ref_mb->mv_x + (ref_mb->mv_x > 0) + (mv_scale - 1)) >> mv_scale : ref_mb->mv_x;
                    mb->mv_y = mv_scale ? (ref_mb->mv_y + (ref_mb->mv_y > 0) + (mv_scale - 1)) >> mv_scale : ref_mb->mv_y;
                }
            } else {
                if (band->inherit_mv && ref_mb)
                    mb->type = ref_mb->type;
                else if (d->frame_type == OI_FRAME_INTRA)
                    mb->type = 0;
                else
                    mb->type = (int)oi_get_bit(gb);
                int blks = band->mb_size != band->blk_size ? 4 : 1;
                mb->cbp = (int)oi_get_bits(gb, blks);
                mb->q_delta = 0;
                if (band->qdelta_present) {
                    if (band->inherit_qdelta) {
                        if (ref_mb) mb->q_delta = ref_mb->q_delta;
                    } else if (mb->cbp || (first_band && (d->frame_flags & 8))) {
                        int v = oi_get_vlc(gb, &d->mb_vlc);
                        if (v < 0) return OI_ERR_INVALID;
                        mb->q_delta = oi_to_signed(v);
                    }
                }
                if (!mb->type) {
                    mb->mv_x = mb->mv_y = 0;
                } else if (band->inherit_mv && ref_mb) {
                    mb->mv_x = mv_scale ? (ref_mb->mv_x + (ref_mb->mv_x > 0) + (mv_scale - 1)) >> mv_scale : ref_mb->mv_x;
                    mb->mv_y = mv_scale ? (ref_mb->mv_y + (ref_mb->mv_y > 0) + (mv_scale - 1)) >> mv_scale : ref_mb->mv_y;
                } else {
                    int dy = oi_get_vlc(gb, &d->mb_vlc);
                    int dx = oi_get_vlc(gb, &d->mb_vlc);
                    if (dy < 0 || dx < 0) return OI_ERR_INVALID;
                    mv_y += oi_to_signed(dy);
                    mv_x += oi_to_signed(dx);
                    mb->mv_x = mv_x;
                    mb->mv_y = mv_y;
                }
            }
            mb->b_mv_x = mb->b_mv_y = 0;
            mb++;
            if (ref_mb) ref_mb++;
            mb_offset += mb_size;
        }
        offs += mb_size * band->pitch;
    }
    oi_align(gb);
    return gb->overread ? OI_ERR_INVALID : OI_OK;
}

/* Which buffer a frame decodes into and which it predicts from. Intra and inter frames alternate
 * two buffers; a scalable inter frame keeps a third; a non-reference frame decodes into the
 * buffer the next frame will overwrite. */
void oi_ivi5_switch_buffers(oi_decoder *d) {
    switch (d->prev_frame_type) {
    case OI_FRAME_INTRA:
    case OI_FRAME_INTER:
        d->buf_switch ^= 1;
        d->dst_buf = d->buf_switch;
        d->ref_buf = d->buf_switch ^ 1;
        break;
    case OI_FRAME_INTER_SCAL:
        if (!d->inter_scal) {
            d->ref2_buf = 2;
            d->inter_scal = 1;
        }
        {
            int t = d->dst_buf;
            d->dst_buf = d->ref2_buf;
            d->ref2_buf = t;
        }
        d->ref_buf = d->ref2_buf;
        break;
    default:
        break;
    }
    switch (d->frame_type) {
    case OI_FRAME_INTRA:
        d->buf_switch = 0;
        /* fall through */
    case OI_FRAME_INTER:
        d->inter_scal = 0;
        d->dst_buf = d->buf_switch;
        d->ref_buf = d->buf_switch ^ 1;
        break;
    default:
        break;
    }
}
