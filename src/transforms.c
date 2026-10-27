/* Inverse transforms and band recomposition.
 *
 * The slant transform is computed in integers with butterflies and two "reflections" (rotations
 * approximated by shifts); the first pass (columns) keeps full precision and the second (rows)
 * halves with rounding. Columns flagged as all zero are skipped, as are all-zero rows. */
#include <string.h>

#include "ivi.h"

/* o1 = s1 + s2, o2 = s1 - s2 */
#define BFLY(s1, s2, o1, o2) \
    do {                     \
        int t_ = (s1) - (s2); \
        o1 = (s1) + (s2);    \
        o2 = t_;             \
    } while (0)

/* rotation by (1/2, 5/4) */
#define IREFLECT(s1, s2, o1, o2)                     \
    do {                                             \
        int t_ = (((s1) + (s2) * 2 + 2) >> 2) + (s1); \
        o2 = (((s1) * 2 - (s2) + 2) >> 2) - (s2);    \
        o1 = t_;                                     \
    } while (0)

/* rotation by (1/2, 7/8) */
#define SLANT_PART4(s1, s2, o1, o2)                   \
    do {                                              \
        int t_ = (s2) + (((s1) * 4 - (s2) + 4) >> 3); \
        o2 = (s1) + ((-(s1) - (s2) * 4 + 4) >> 3);    \
        o1 = t_;                                      \
    } while (0)

/* in[0..7] in transform order -> out[0..7]; `half` rounds and halves the result. */
static inline void inv_slant8(const int v[8], int o[8], int half) {
    int s1 = v[0], s4 = v[1], s8 = v[2], s5 = v[3], s2 = v[4], s6 = v[5], s3 = v[6], s7 = v[7];
    int t1, t2, t3, t4, t5, t6, t7, t8;
    SLANT_PART4(s4, s5, t4, t5);
    BFLY(s1, t5, t1, t5);
    BFLY(s2, s6, t2, t6);
    BFLY(s7, s3, t7, t3);
    BFLY(t4, s8, t4, t8);
    BFLY(t1, t2, t1, t2);
    IREFLECT(t4, t3, t4, t3);
    BFLY(t5, t6, t5, t6);
    IREFLECT(t8, t7, t8, t7);
    BFLY(t1, t4, t1, t4);
    BFLY(t2, t3, t2, t3);
    BFLY(t5, t8, t5, t8);
    BFLY(t6, t7, t6, t7);
    int r[8] = {t1, t2, t3, t4, t5, t6, t7, t8};
    for (int k = 0; k < 8; k++) o[k] = half ? (r[k] + 1) >> 1 : r[k];
}

static inline void inv_slant4(const int v[4], int o[4], int half) {
    int s1 = v[0], s4 = v[1], s2 = v[2], s3 = v[3];
    int t1, t2, t3, t4;
    BFLY(s1, s2, t1, t2);
    IREFLECT(s4, s3, t4, t3);
    BFLY(t1, t4, t1, t4);
    BFLY(t2, t3, t2, t3);
    int r[4] = {t1, t2, t3, t4};
    for (int k = 0; k < 4; k++) o[k] = half ? (r[k] + 1) >> 1 : r[k];
}

/* Column pass then row pass, n = 4 or 8. */
static void inverse_2d(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags, int n,
                       void (*f)(const int *, int *, int)) {
    int tmp[64];
    int v[8], o[8];
    for (int c = 0; c < n; c++) {
        if (flags[c]) {
            for (int k = 0; k < n; k++) v[k] = in[c + k * n];
            f(v, o, 0);
            for (int k = 0; k < n; k++) tmp[c + k * n] = o[k];
        } else {
            for (int k = 0; k < n; k++) tmp[c + k * n] = 0;
        }
    }
    for (int r = 0; r < n; r++, out += pitch) {
        const int *row = tmp + r * n;
        int any = 0;
        for (int k = 0; k < n; k++) any |= row[k];
        if (!any) {
            memset(out, 0, (size_t)n * sizeof(*out));
            continue;
        }
        for (int k = 0; k < n; k++) v[k] = row[k];
        f(v, o, 1);
        for (int k = 0; k < n; k++) out[k] = (int16_t)o[k];
    }
}

static void slant8_fn(const int *v, int *o, int half) { inv_slant8(v, o, half); }
static void slant4_fn(const int *v, int *o, int half) { inv_slant4(v, o, half); }

void oi_inverse_slant_8x8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    inverse_2d(in, out, pitch, flags, 8, slant8_fn);
}

void oi_inverse_slant_4x4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    inverse_2d(in, out, pitch, flags, 4, slant4_fn);
}

/* One-dimensional: each row transformed (rows of zeros stay zero). */
void oi_row_slant8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)flags;
    int v[8], o[8];
    for (int r = 0; r < 8; r++, in += 8, out += pitch) {
        int any = 0;
        for (int k = 0; k < 8; k++) any |= in[k];
        if (!any) {
            memset(out, 0, 8 * sizeof(*out));
            continue;
        }
        for (int k = 0; k < 8; k++) v[k] = in[k];
        inv_slant8(v, o, 1);
        for (int k = 0; k < 8; k++) out[k] = (int16_t)o[k];
    }
}

/* Each flagged column transformed. */
void oi_col_slant8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    int v[8], o[8];
    for (int c = 0; c < 8; c++) {
        if (flags[c]) {
            for (int k = 0; k < 8; k++) v[k] = in[c + k * 8];
            inv_slant8(v, o, 1);
            for (int k = 0; k < 8; k++) out[c + k * pitch] = (int16_t)o[k];
        } else {
            for (int k = 0; k < 8; k++) out[c + k * pitch] = 0;
        }
    }
}

void oi_row_slant4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)flags;
    int v[4], o[4];
    for (int r = 0; r < 4; r++, in += 4, out += pitch) {
        int any = in[0] | in[1] | in[2] | in[3];
        if (!any) {
            memset(out, 0, 4 * sizeof(*out));
            continue;
        }
        for (int k = 0; k < 4; k++) v[k] = in[k];
        inv_slant4(v, o, 1);
        for (int k = 0; k < 4; k++) out[k] = (int16_t)o[k];
    }
}

void oi_col_slant4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    int v[4], o[4];
    for (int c = 0; c < 4; c++) {
        if (flags[c]) {
            for (int k = 0; k < 4; k++) v[k] = in[c + k * 4];
            inv_slant4(v, o, 1);
            for (int k = 0; k < 4; k++) out[c + k * pitch] = (int16_t)o[k];
        } else {
            for (int k = 0; k < 4; k++) out[c + k * pitch] = 0;
        }
    }
}

/* Coefficients copied as samples (the high-pass band of a scalable picture). */
void oi_put_pixels_8x8(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)flags;
    for (int y = 0; y < 8; y++, out += pitch)
        for (int x = 0; x < 8; x++) out[x] = (int16_t)in[y * 8 + x];
}

void oi_put_pixels_4x4(const int32_t *in, int16_t *out, int pitch, const uint8_t *flags) {
    (void)flags;
    for (int y = 0; y < 4; y++, out += pitch)
        for (int x = 0; x < 4; x++) out[x] = (int16_t)in[y * 4 + x];
}

/* DC-only blocks. */
void oi_dc_slant_2d(const int32_t *in, int16_t *out, int pitch, int blk_size) {
    int16_t dc = (int16_t)((in[0] + 1) >> 1);
    for (int y = 0; y < blk_size; y++, out += pitch)
        for (int x = 0; x < blk_size; x++) out[x] = dc;
}

void oi_dc_row_slant(const int32_t *in, int16_t *out, int pitch, int blk_size) {
    int16_t dc = (int16_t)((in[0] + 1) >> 1);
    for (int x = 0; x < blk_size; x++) out[x] = dc;
    out += pitch;
    for (int y = 1; y < blk_size; y++, out += pitch) memset(out, 0, (size_t)blk_size * sizeof(*out));
}

void oi_dc_col_slant(const int32_t *in, int16_t *out, int pitch, int blk_size) {
    int16_t dc = (int16_t)((in[0] + 1) >> 1);
    for (int y = 0; y < blk_size; y++, out += pitch) {
        out[0] = dc;
        for (int x = 1; x < blk_size; x++) out[x] = 0;
    }
}

void oi_put_dc_pixel_8x8(const int32_t *in, int16_t *out, int pitch, int blk_size) {
    (void)blk_size;
    for (int y = 0; y < 8; y++) memset(out + y * pitch, 0, 8 * sizeof(*out));
    out[0] = (int16_t)in[0];
}

void oi_put_dc_pixel_4x4(const int32_t *in, int16_t *out, int pitch, int blk_size) {
    (void)blk_size;
    for (int y = 0; y < 4; y++) memset(out + y * pitch, 0, 4 * sizeof(*out));
    out[0] = (int16_t)in[0];
}
