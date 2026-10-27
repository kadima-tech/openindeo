/* The parts of Indeo 4 and 5 decoding they share: picture layout, bands, tiles, macroblocks,
 * coefficient blocks, motion compensation and output. */
#include <stdlib.h>
#include <string.h>

#include "ivi.h"

#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))

static int mbs_per_tile(int w, int h, int mb) { return ((h + mb - 1) / mb) * ((w + mb - 1) / mb); }

static int clip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void oi_free_planes(oi_decoder *d) {
    for (int p = 0; p < 3; p++) {
        for (int b = 0; b < d->planes[p].num_bands; b++) {
            oi_band *band = &d->planes[p].bands[b];
            for (int k = 0; k < 4; k++) free(band->bufs[k]);
            for (int t = 0; t < band->num_tiles; t++) free(band->tiles[t].mbs);
            free(band->tiles);
        }
    }
    memset(d->planes, 0, sizeof(d->planes));
    free(d->out_y);
    free(d->out_u);
    free(d->out_v);
    free(d->out_a);
    d->out_y = d->out_u = d->out_v = d->out_a = NULL;
    memset(d->buf_valid, 0, sizeof(d->buf_valid));
    d->have_output = 0;
}

/* Planes and band buffers for a picture configuration. A band of a multi-band plane is half the
 * plane's size; buffers are padded to 16 (luma) or 8 (chroma) samples in each direction. */
int oi_init_planes(oi_decoder *d, const oi_pic_conf *cfg, int indeo4) {
    (void)indeo4;
    oi_free_planes(d);
    d->planes[0].width = cfg->pic_width;
    d->planes[0].height = cfg->pic_height;
    d->planes[0].num_bands = cfg->luma_bands;
    d->planes[1].width = d->planes[2].width = cfg->chroma_width;
    d->planes[1].height = d->planes[2].height = cfg->chroma_height;
    d->planes[1].num_bands = d->planes[2].num_bands = cfg->chroma_bands;
    for (int p = 0; p < 3; p++) {
        oi_plane *pl = &d->planes[p];
        int bw = pl->num_bands == 1 ? pl->width : (pl->width + 1) >> 1;
        int bh = pl->num_bands == 1 ? pl->height : (pl->height + 1) >> 1;
        int align = p ? 8 : 16;
        int wa = ALIGN(bw, align), ha = ALIGN(bh, align);
        for (int b = 0; b < pl->num_bands; b++) {
            oi_band *band = &pl->bands[b];
            band->plane = p;
            band->band_num = b;
            band->width = bw;
            band->height = bh;
            band->pitch = wa;
            band->aheight = ha;
            for (int k = 0; k < 4; k++) {
                band->bufs[k] = calloc((size_t)wa * ha, sizeof(int16_t));
                if (!band->bufs[k]) return OI_ERR_NOMEM;
            }
        }
    }
    d->out_w = cfg->pic_width;
    d->out_h = cfg->pic_height;
    d->out_cw = cfg->chroma_width;
    d->out_ch = cfg->chroma_height;
    d->out_y = calloc((size_t)d->out_w * d->out_h, 1);
    d->out_u = calloc((size_t)d->out_cw * d->out_ch, 1);
    d->out_v = calloc((size_t)d->out_cw * d->out_ch, 1);
    d->out_a = calloc((size_t)d->out_w * d->out_h, 1);
    if (!d->out_y || !d->out_u || !d->out_v || !d->out_a) return OI_ERR_NOMEM;
    memset(d->out_u, 128, (size_t)d->out_cw * d->out_ch);
    memset(d->out_v, 128, (size_t)d->out_cw * d->out_ch);
    memset(d->out_a, 255, (size_t)d->out_w * d->out_h);
    return OI_OK;
}

/* Tiles of every band (luma tiles at the given size, chroma a quarter of it, the bands of a
 * four-band luma plane half of it) and their macroblocks. Every band but the first luma one
 * refers to the first luma band's tiles for inherited motion and quant. */
int oi_init_tiles(oi_decoder *d, int tile_width, int tile_height) {
    for (int p = 0; p < 3; p++) {
        int tw = !p ? tile_width : (tile_width + 3) >> 2;
        int th = !p ? tile_height : (tile_height + 3) >> 2;
        if (!p && d->planes[0].num_bands == 4) {
            tw >>= 1;
            th >>= 1;
        }
        if (tw <= 0 || th <= 0) return OI_ERR_INVALID;
        for (int b = 0; b < d->planes[p].num_bands; b++) {
            oi_band *band = &d->planes[p].bands[b];
            for (int t = 0; t < band->num_tiles; t++) free(band->tiles[t].mbs);
            free(band->tiles);
            band->tiles = NULL;
            int xt = (band->width + tw - 1) / tw, yt = (band->height + th - 1) / th;
            band->num_tiles = xt * yt;
            band->tiles = calloc((size_t)band->num_tiles, sizeof(oi_tile));
            if (!band->tiles) return OI_ERR_NOMEM;
            oi_tile *ref_tile = d->planes[0].bands[0].tiles;
            oi_tile *tile = band->tiles;
            for (int y = 0; y < band->height; y += th) {
                for (int x = 0; x < band->width; x += tw, tile++) {
                    tile->xpos = x;
                    tile->ypos = y;
                    tile->mb_size = band->mb_size;
                    tile->width = band->width - x < tw ? band->width - x : tw;
                    tile->height = band->height - y < th ? band->height - y : th;
                    tile->num_mbs = mbs_per_tile(tile->width, tile->height, band->mb_size);
                    tile->mbs = calloc((size_t)tile->num_mbs, sizeof(oi_mb));
                    if (!tile->mbs) return OI_ERR_NOMEM;
                    tile->ref_mbs = NULL;
                    if (p || b) {
                        if (!ref_tile || tile->num_mbs != ref_tile->num_mbs) return OI_ERR_INVALID;
                        tile->ref_mbs = ref_tile->mbs;
                        ref_tile++;
                    }
                }
            }
        }
    }
    return OI_OK;
}

/* A codebook: the default one (layout 7) unless coded; a coded one picks a static layout or,
 * with selector 7, sends its own (4-bit row count and 4-bit row widths). */
int oi_dec_huff_desc(oi_bits *b, int desc_coded, int blk, oi_vlc *out) {
    const oi_huff_desc *stat = blk ? oi_blk_huff_desc : oi_mb_huff_desc;
    if (!desc_coded) {
        oi_vlc_init(out, &stat[7]);
        return OI_OK;
    }
    int sel = (int)oi_get_bits(b, 3);
    if (sel != 7) {
        oi_vlc_init(out, &stat[sel]);
        return OI_OK;
    }
    oi_huff_desc custom;
    memset(&custom, 0, sizeof(custom));
    custom.num_rows = (int)oi_get_bits(b, 4);
    if (!custom.num_rows) return OI_ERR_INVALID;
    for (int i = 0; i < custom.num_rows; i++) custom.xbits[i] = (uint8_t)oi_get_bits(b, 4);
    oi_vlc_init(out, &custom);
    return OI_OK;
}

/* Extension blocks: byte counts followed by that many bytes, until a zero count. */
void oi_skip_hdr_extension(oi_bits *b) {
    for (;;) {
        int len = (int)oi_get_bits(b, 8);
        if (!len || b->overread) break;
        oi_skip_bits(b, (size_t)len * 8);
    }
}

/* --- motion compensation ------------------------------------------------------------------ */

/* The reference sample at r for a half-pixel position type (0 whole, 1 right, 2 down, 3 both). */
static inline int interp(const int16_t *r, int pitch, int type) {
    switch (type) {
    case 1: return (r[0] + r[1]) >> 1;
    case 2: return (r[0] + r[pitch]) >> 1;
    case 3: return (r[0] + r[1] + r[pitch] + r[pitch + 1]) >> 2;
    default: return r[0];
    }
}

static void mc_block(int16_t *buf, const int16_t *ref, int pitch, int type, int n, int delta) {
    for (int y = 0; y < n; y++, buf += pitch, ref += pitch)
        for (int x = 0; x < n; x++) {
            int v = interp(ref + x, pitch, type);
            buf[x] = (int16_t)(delta ? buf[x] + v : v);
        }
}

static void mc_avg_block(int16_t *buf, const int16_t *ref1, const int16_t *ref2, int pitch, int type1, int type2,
                         int n, int delta) {
    for (int y = 0; y < n; y++, buf += pitch, ref1 += pitch, ref2 += pitch)
        for (int x = 0; x < n; x++) {
            int v = (interp(ref1 + x, pitch, type1) + interp(ref2 + x, pitch, type2)) >> 1;
            buf[x] = (int16_t)(delta ? buf[x] + v : v);
        }
}

/* A block's prediction from the reference (type -1: none) and/or the backward reference
 * (type2 -1: none), added to the block when `delta`. */
static int do_mc(const oi_band *band, int offs, int mv_x, int mv_y, int mv_x2, int mv_y2, int type, int type2, int delta) {
    int n = band->blk_size;
    int buf_size = band->pitch * band->aheight;
    int min_size = band->pitch * (n - 1) + n;
    if (offs < 0 || offs > buf_size - min_size) return OI_ERR_INVALID;
    int ref_offs = offs + mv_y * band->pitch + mv_x;
    if (type != -1) {
        int ref_size = (type > 1) * band->pitch + (type & 1);
        if (ref_offs < 0 || ref_offs > buf_size - min_size - ref_size || !band->ref_buf) return OI_ERR_INVALID;
    }
    if (type2 == -1) {
        if (type == -1) return OI_OK;
        mc_block(band->buf + offs, band->ref_buf + ref_offs, band->pitch, type, n, delta);
        return OI_OK;
    }
    int ref_offs2 = offs + mv_y2 * band->pitch + mv_x2;
    int ref_size2 = (type2 > 1) * band->pitch + (type2 & 1);
    if (ref_offs2 < 0 || ref_offs2 > buf_size - min_size - ref_size2 || !band->b_ref_buf) return OI_ERR_INVALID;
    if (type == -1)
        mc_block(band->buf + offs, band->b_ref_buf + ref_offs2, band->pitch, type2, n, delta);
    else
        mc_avg_block(band->buf + offs, band->ref_buf + ref_offs, band->b_ref_buf + ref_offs2, band->pitch, type, type2, n, delta);
    return OI_OK;
}

/* A vector of the first luma band scaled to this band: (mv + (mv > 0) + scale - 1) >> scale. */
static int scale_mv(int mv, int scale) { return (mv + (mv > 0) + (scale - 1)) >> scale; }

/* --- blocks ------------------------------------------------------------------------------- */

/* A coded block: run/value symbols (or escapes: run, low six bits, high bits) placed through the
 * band's scan and dequantised, the intra DC predicted from the last one, inverse transformed,
 * then the motion compensated prediction added for inter blocks. */
static int decode_coded_block(oi_decoder *d, oi_band *band, const oi_rvmap *rv, int mv_x, int mv_y, int mv_x2, int mv_y2,
                              int *prev_dc, int is_intra, int type, int type2, int quant, int offs) {
    oi_bits *gb = &d->gb;
    int n = band->blk_size;
    int num_coeffs = n * n;
    int col_mask = n - 1;
    int32_t trvec[64];
    uint8_t col_flags[8];
    const uint16_t *base = is_intra ? band->intra_base : band->inter_base;
    int buf_size = band->pitch * band->aheight;
    int min_size = band->pitch * (band->transform_size - 1) + band->transform_size;
    if (!band->scan || offs < 0 || offs > buf_size - min_size) return OI_ERR_INVALID;
    memset(trvec, 0, (size_t)num_coeffs * sizeof(trvec[0]));
    memset(col_flags, 0, sizeof(col_flags));
    int scan_pos = -1;
    int sym = -1;
    int scan_size = band->scan_size ? band->scan_size : num_coeffs;
    while (scan_pos <= num_coeffs) {
        sym = oi_get_vlc(gb, &band->blk_vlc);
        if (sym < 0 || gb->overread) return OI_ERR_INVALID;
        if (sym == rv->eob_sym) break;
        int run, val;
        if (sym == rv->esc_sym) {
            run = oi_get_vlc(gb, &band->blk_vlc) + 1;
            int lo = oi_get_vlc(gb, &band->blk_vlc);
            int hi = oi_get_vlc(gb, &band->blk_vlc);
            if (run <= 0 || lo < 0 || hi < 0) return OI_ERR_INVALID;
            val = oi_to_signed((hi << 6) | lo);
        } else {
            run = rv->runtab[sym];
            val = rv->valtab[sym];
        }
        scan_pos += run;
        if (scan_pos >= scan_size || scan_pos < 0) break;
        int pos = band->scan[scan_pos];
        int q = (base[pos] * quant) >> 9;
        if (q > 1) val = val * q + (val > 0 ? 1 : val < 0 ? -1 : 0) * (((q ^ 1) - 1) >> 1);
        trvec[pos] = val;
        col_flags[pos & col_mask] |= !!val;
    }
    if (scan_pos < 0 || (scan_pos >= scan_size && sym != rv->eob_sym)) return OI_ERR_INVALID;
    if (is_intra && band->is_2d_trans) {
        *prev_dc += trvec[0];
        trvec[0] = *prev_dc;
        col_flags[0] |= !!*prev_dc;
    }
    if (band->transform_size > band->blk_size) return OI_ERR_INVALID;
    band->inv_transform(trvec, band->buf + offs, band->pitch, col_flags);
    if (!is_intra) return do_mc(band, offs, mv_x, mv_y, mv_x2, mv_y2, type, type2, 1);
    return OI_OK;
}

static int decode_blocks(oi_decoder *d, oi_band *band, oi_tile *tile, const oi_rvmap *rv) {
    int n = band->blk_size;
    int num_blocks = band->mb_size != n ? 4 : 1;
    int prev_dc = 0;
    oi_mb *mb = tile->mbs;
    for (int m = 0; m < tile->num_mbs; m++, mb++) {
        int is_intra = !mb->type;
        int cbp = mb->cbp;
        int offs = mb->buf_offs;
        int quant = band->glob_quant + mb->q_delta;
        quant = d->indeo4 ? clip(quant, 0, 31) : clip(quant, 0, 23);
        const uint8_t *scale = is_intra ? band->intra_scale : band->inter_scale;
        if (scale) quant = scale[quant];
        int mv_x = 0, mv_y = 0, mv_x2 = 0, mv_y2 = 0, type = 0, type2 = -1;
        if (!is_intra) {
            mv_x = mb->mv_x;
            mv_y = mb->mv_y;
            mv_x2 = mb->b_mv_x;
            mv_y2 = mb->b_mv_y;
            if (band->is_halfpel) {
                type = ((mv_y & 1) << 1) | (mv_x & 1);
                type2 = ((mv_y2 & 1) << 1) | (mv_x2 & 1);
                mv_x >>= 1;
                mv_y >>= 1;
                mv_x2 >>= 1;
                mv_y2 >>= 1;
            } else {
                type = 0;
                type2 = 0;
            }
            if (mb->type == 2) type = -1;
            if (mb->type != 2 && mb->type != 3) type2 = -1;
        }
        for (int blk = 0; blk < num_blocks; blk++) {
            if (blk & 1) {
                offs += n;
            } else if (blk == 2) {
                offs -= n;
                offs += n * band->pitch;
            }
            int r;
            if (cbp & 1) {
                r = decode_coded_block(d, band, rv, mv_x, mv_y, mv_x2, mv_y2, &prev_dc, is_intra, type, type2, quant, offs);
            } else if (is_intra) {
                int buf_size = band->pitch * band->aheight;
                if (offs < 0 || offs > buf_size - (band->pitch * (n - 1) + n)) return OI_ERR_INVALID;
                int32_t dc = prev_dc;
                band->dc_transform(&dc, band->buf + offs, band->pitch, n);
                r = OI_OK;
            } else {
                r = do_mc(band, offs, mv_x, mv_y, mv_x2, mv_y2, type, type2, 0);
            }
            if (r != OI_OK) return r;
            cbp >>= 1;
        }
    }
    oi_align(&d->gb);
    return OI_OK;
}

/* An empty tile: macroblocks inherit (or clear) vectors and quant; with any inherited motion the
 * tile is predicted block by block, else the reference tile is copied. */
static int process_empty_tile(oi_band *band, oi_tile *tile, int mv_scale) {
    int mb_size = band->mb_size;
    int pitch = band->pitch;
    int clear_first = !band->qdelta_present && !band->plane && !band->band_num;
    if (tile->num_mbs != mbs_per_tile(tile->width, tile->height, mb_size)) return OI_ERR_INVALID;
    int offs = tile->ypos * pitch + tile->xpos;
    oi_mb *mb = tile->mbs, *ref_mb = tile->ref_mbs;
    int need_mc = 0;
    for (int y = tile->ypos; y < tile->ypos + tile->height; y += mb_size) {
        int mb_offset = offs;
        for (int x = tile->xpos; x < tile->xpos + tile->width; x += mb_size) {
            mb->xpos = x;
            mb->ypos = y;
            mb->buf_offs = mb_offset;
            mb->type = 1;
            mb->cbp = 0;
            if (clear_first) {
                mb->q_delta = band->glob_quant;
                mb->mv_x = mb->mv_y = 0;
            }
            if (ref_mb) {
                if (band->inherit_qdelta) mb->q_delta = ref_mb->q_delta;
                if (band->inherit_mv) {
                    mb->mv_x = mv_scale ? scale_mv(ref_mb->mv_x, mv_scale) : ref_mb->mv_x;
                    mb->mv_y = mv_scale ? scale_mv(ref_mb->mv_y, mv_scale) : ref_mb->mv_y;
                    need_mc |= mb->mv_x || mb->mv_y;
                }
                int hp = band->is_halfpel;
                int dx = mb->mv_x >> hp, dy = mb->mv_y >> hp, cx = mb->mv_x & hp, cy = mb->mv_y & hp;
                if (mb->xpos + dx < 0 || mb->xpos + dx + mb_size + cx > pitch || mb->ypos + dy < 0 ||
                    mb->ypos + dy + mb_size + cy > band->aheight)
                    return OI_ERR_INVALID;
            }
            mb++;
            if (ref_mb) ref_mb++;
            mb_offset += mb_size;
        }
        offs += mb_size * pitch;
    }
    if (band->inherit_mv && need_mc) {
        int num_blocks = mb_size != band->blk_size ? 4 : 1;
        mb = tile->mbs;
        for (int m = 0; m < tile->num_mbs; m++, mb++) {
            int mv_x = mb->mv_x, mv_y = mb->mv_y, type = 0;
            if (band->is_halfpel) {
                type = ((mv_y & 1) << 1) | (mv_x & 1);
                mv_x >>= 1;
                mv_y >>= 1;
            }
            for (int blk = 0; blk < num_blocks; blk++) {
                int o = mb->buf_offs + band->blk_size * ((blk & 1) + !!(blk & 2) * pitch);
                int r = do_mc(band, o, mv_x, mv_y, 0, 0, type, -1, 0);
                if (r != OI_OK) return r;
            }
        }
    } else {
        if (!band->ref_buf) return OI_ERR_INVALID;
        const int16_t *src = band->ref_buf + tile->ypos * pitch + tile->xpos;
        int16_t *dst = band->buf + tile->ypos * pitch + tile->xpos;
        for (int y = 0; y < tile->height; y++, src += pitch, dst += pitch)
            memcpy(dst, src, (size_t)tile->width * sizeof(*dst));
    }
    return OI_OK;
}

/* A tile's data size: a flag, then 8 bits (24 more when 255); the reader is then aligned. */
static int tile_data_size(oi_bits *b) {
    int len = 0;
    if (oi_get_bit(b)) {
        len = (int)oi_get_bits(b, 8);
        if (len == 255) len = (int)oi_get_bits(b, 24);
    }
    oi_align(b);
    return len;
}

static void swap_corrections(oi_rvmap *rv, const oi_band *band, int reverse) {
    for (int k = 0; k < band->num_corr; k++) {
        int i = reverse ? band->num_corr - 1 - k : k;
        int a = band->corr[i * 2], b = band->corr[i * 2 + 1];
        uint8_t r = rv->runtab[a];
        rv->runtab[a] = rv->runtab[b];
        rv->runtab[b] = r;
        int8_t v = rv->valtab[a];
        rv->valtab[a] = rv->valtab[b];
        rv->valtab[b] = v;
        if (a == rv->eob_sym || b == rv->eob_sym) rv->eob_sym ^= (uint8_t)(a ^ b);
        if (a == rv->esc_sym || b == rv->esc_sym) rv->esc_sym ^= (uint8_t)(a ^ b);
    }
}

static int decode_band(oi_decoder *d, oi_band *band) {
    band->buf = band->bufs[d->dst_buf];
    band->ref_buf = band->bufs[d->ref_buf];
    band->b_ref_buf = d->indeo4 ? band->bufs[d->b_ref_buf] : NULL;
    int r = d->indeo4 ? oi_ivi4_decode_band_hdr(d, band) : oi_ivi5_decode_band_hdr(d, band);
    if (r != OI_OK) return r;
    if (band->is_empty) {
        /* nothing coded: the band stays as it was in the reference (assumed; empty bands do
         * not occur in the test streams) */
        if (band->ref_buf && band->ref_buf != band->buf)
            memcpy(band->buf, band->ref_buf, (size_t)band->pitch * band->aheight * sizeof(*band->buf));
        return OI_OK;
    }
    if (band->rvmap_sel < 0 || band->rvmap_sel > 8) return OI_ERR_INVALID;
    oi_rvmap *rv = &d->rvmap[band->rvmap_sel];
    swap_corrections(rv, band, 0);
    int mv_scale = (d->planes[0].bands[0].mb_size >> 3) - (band->mb_size >> 3);
    for (int t = 0; t < band->num_tiles && r == OI_OK; t++) {
        oi_tile *tile = &band->tiles[t];
        size_t pos = d->gb.pos;  /* a tile's data size counts from its first bit */
        if (tile->mb_size != band->mb_size) {
            r = OI_ERR_INVALID;
            break;
        }
        tile->is_empty = (int)oi_get_bit(&d->gb);
        if (tile->is_empty) {
            r = process_empty_tile(band, tile, mv_scale);
        } else {
            tile->data_size = tile_data_size(&d->gb);
            if (!tile->data_size) {
                r = OI_ERR_INVALID;
                break;
            }
            r = d->indeo4 ? oi_ivi4_decode_mb_info(d, band, tile) : oi_ivi5_decode_mb_info(d, band, tile);
            if (r == OI_OK) r = decode_blocks(d, band, tile, rv);
            /* the next tile starts where this one's size says, whatever was read */
            d->gb.pos = pos + (size_t)tile->data_size * 8;
            if (d->gb.pos > d->gb.size_bits) r = OI_ERR_INVALID;
        }
    }
    swap_corrections(rv, band, 1);
    oi_align(&d->gb);
    return r;
}

/* Band buffers -> 8-bit plane: + 128, clipped. */
static void output_plane(const oi_plane *plane, uint8_t *dst, int dst_pitch) {
    const int16_t *src = plane->bands[0].buf;
    int pitch = plane->bands[0].pitch;
    if (!src) return;
    for (int y = 0; y < plane->height; y++, src += pitch, dst += dst_pitch)
        for (int x = 0; x < plane->width; x++) dst[x] = (uint8_t)clip(src[x] + 128, 0, 255);
}

int oi_decode_frame(oi_decoder *d, const uint8_t *data, size_t size) {
    oi_bits_init(&d->gb, data, size);
    d->frame_data = data;
    d->frame_size = size;
    int r = d->indeo4 ? oi_ivi4_decode_pic_hdr(d) : oi_ivi5_decode_pic_hdr(d);
    if (r != OI_OK) return r;
    if (d->gop_invalid || !d->configured) return OI_ERR_INVALID;
    if (d->indeo4) oi_ivi4_switch_buffers(d);
    else oi_ivi5_switch_buffers(d);
    int nonnull = d->indeo4 ? oi_ivi4_is_nonnull(d) : d->frame_type != OI_FRAME_NULL;
    if (!nonnull) return d->have_output ? 1 : OI_ERR_NO_REFERENCE;  /* 1: repeat the last picture */
    int intra = d->indeo4 ? d->frame_type == 0 : d->frame_type == OI_FRAME_INTRA;
    if (!intra && !d->buf_valid[d->ref_buf]) return OI_ERR_NO_REFERENCE;
    d->buf_valid[d->dst_buf] = 0;
    for (int p = 0; p < 3; p++)
        for (int b = 0; b < d->planes[p].num_bands; b++) {
            r = decode_band(d, &d->planes[p].bands[b]);
            if (r != OI_OK) return r;
        }
    d->buf_valid[d->dst_buf] = 1;
    if (d->is_scalable) {
        if (d->indeo4) oi_recompose_haar(&d->planes[0], d->out_y, d->out_w);
        else oi_recompose53(&d->planes[0], d->out_y, d->out_w);
    } else {
        output_plane(&d->planes[0], d->out_y, d->out_w);
    }
    /* planes are Y, V, U */
    output_plane(&d->planes[2], d->out_u, d->out_cw);
    output_plane(&d->planes[1], d->out_v, d->out_cw);
    d->have_output = 1;
    d->out_is_key = intra;
    return OI_OK;
}

/* --- public interface --------------------------------------------------------------------- */

oi_decoder *oi_create(uint32_t fourcc) {
    uint32_t up = fourcc & ~0x20202020u;  /* letters upper case (digits unaffected below) */
    int is4 = (up & 0x0000ffff) == OI_FOURCC('I', 'V', 0, 0) && ((fourcc >> 16) & 0xffff) == ('4' | ('1' << 8));
    int is5 = (up & 0x0000ffff) == OI_FOURCC('I', 'V', 0, 0) && ((fourcc >> 16) & 0xffff) == ('5' | ('0' << 8));
    if (!is4 && !is5) return NULL;
    oi_decoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->indeo4 = is4;
    memcpy(d->rvmap, oi_rvmap_tabs, sizeof(d->rvmap));
    oi_vlc_init(&d->mb_vlc, &oi_mb_huff_desc[7]);
    d->ref_buf = 1;
    d->b_ref_buf = 3;
    return d;
}

void oi_destroy(oi_decoder *d) {
    if (!d) return;
    oi_free_planes(d);
    free(d);
}

static void fill_frame(const oi_decoder *d, oi_frame *out, int is_new) {
    memset(out, 0, sizeof(*out));
    out->width = d->out_w;
    out->height = d->out_h;
    out->chroma_width = d->out_cw;
    out->chroma_height = d->out_ch;
    out->y = d->out_y;
    out->u = d->out_u;
    out->v = d->out_v;
    out->y_stride = d->out_w;
    out->uv_stride = d->out_cw;
    out->alpha = NULL;  /* Indeo 4 transparency is not decoded yet */
    out->alpha_stride = d->out_w;
    out->is_new = is_new;
    out->is_key = is_new && d->out_is_key;
}

int oi_decode(oi_decoder *d, const void *data, size_t size, oi_frame *out) {
    if (!d || !out) return OI_ERR_INVALID;
    if (!size) {
        if (!d->have_output) return OI_ERR_NO_REFERENCE;
        fill_frame(d, out, 0);
        return OI_OK;
    }
    int r = oi_decode_frame(d, (const uint8_t *)data, size);
    if (r < 0) return r;
    fill_frame(d, out, r == OI_OK);
    return OI_OK;
}

const char *oi_error_string(int status) {
    switch (status) {
    case OI_OK: return "ok";
    case OI_ERR_INVALID: return "invalid data";
    case OI_ERR_UNSUPPORTED: return "unsupported feature";
    case OI_ERR_NOMEM: return "out of memory";
    case OI_ERR_NO_REFERENCE: return "no reference frame";
    default: return "unknown error";
    }
}
