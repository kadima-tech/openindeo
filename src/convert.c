/* YUV 4:1:0 to RGB, and the standard zigzag scan. */
#include "ivi.h"

/* The JPEG zigzag order (Indeo 5's first luma band). */
const uint8_t oi_zigzag_scan_8x8[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* BT.601 studio range, 16.16 fixed point; each chroma sample covers 4x4 luma pixels. */
static void convert(const oi_frame *f, uint8_t *dst, int dst_stride, int bpp) {
    for (int y = 0; y < f->height; y++) {
        const uint8_t *yr = f->y + (size_t)y * f->y_stride;
        const uint8_t *ur = f->u + (size_t)(y >> 2) * f->uv_stride;
        const uint8_t *vr = f->v + (size_t)(y >> 2) * f->uv_stride;
        const uint8_t *ar = f->alpha ? f->alpha + (size_t)y * f->alpha_stride : NULL;
        uint8_t *o = dst + (size_t)y * dst_stride;
        for (int x = 0; x < f->width; x++, o += bpp) {
            int c = (yr[x] - 16) * 76309;
            int u = ur[x >> 2] - 128, v = vr[x >> 2] - 128;
            o[0] = clamp8((c + 104597 * v + 32768) >> 16);
            o[1] = clamp8((c - 25675 * u - 53279 * v + 32768) >> 16);
            o[2] = clamp8((c + 132201 * u + 32768) >> 16);
            if (bpp == 4) o[3] = ar ? ar[x] : 255;
        }
    }
}

void oi_to_rgb24(const oi_frame *f, uint8_t *dst, int dst_stride) { convert(f, dst, dst_stride, 3); }
void oi_to_rgba32(const oi_frame *f, uint8_t *dst, int dst_stride) { convert(f, dst, dst_stride, 4); }
