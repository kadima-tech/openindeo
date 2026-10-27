/* openindeo: a small, dependency-free decoder for Intel Indeo Video Interactive 4 and 5
 * ("IV41", "IV50"), as found in 1990s AVI files and games.
 *
 * Usage:
 *   oi_decoder *d = oi_create(fourcc);          // the AVI stream's compression fourcc
 *   for each AVI video chunk:
 *       oi_frame f;
 *       if (oi_decode(d, data, size, &f) == OI_OK) use f.y / f.u / f.v (or oi_to_rgb24)
 *   oi_destroy(d);
 *
 * Frames are planar YUV 4:1:0 (chroma a quarter of the luma width and height, rounded up).
 * A frame stays valid until the next oi_decode or oi_destroy call.
 *
 * MIT licence; see LICENSE.
 */
#ifndef OPENINDEO_H
#define OPENINDEO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OI_FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

enum {
    OI_OK = 0,
    OI_ERR_INVALID = -1,      /* the data is not a valid frame */
    OI_ERR_UNSUPPORTED = -2,  /* a feature this decoder does not handle */
    OI_ERR_NOMEM = -3,
    OI_ERR_NO_REFERENCE = -4  /* an inter frame with no frame decoded before it */
};

typedef struct oi_decoder oi_decoder;

typedef struct oi_frame {
    int width, height;                /* luma */
    int chroma_width, chroma_height;  /* (width + 3) / 4, (height + 3) / 4 */
    const uint8_t *y, *u, *v;
    int y_stride, uv_stride;
    /* Indeo 4 transparency: 255 opaque, 0 transparent, one byte per luma pixel; NULL when the
     * stream has none. */
    const uint8_t *alpha;
    int alpha_stride;
    /* 1 when this packet held a new picture; 0 for an empty or "null" packet, which repeats the
     * previous one. */
    int is_new;
    int is_key;  /* an intra frame */
} oi_frame;

/* A decoder for this fourcc ("IV41", "IV50"; case does not matter), or NULL if unsupported. */
oi_decoder *oi_create(uint32_t fourcc);
void oi_destroy(oi_decoder *d);

/* Decodes one compressed frame (one AVI chunk). An empty packet repeats the previous frame. */
int oi_decode(oi_decoder *d, const void *data, size_t size, oi_frame *out);

/* Converts a frame to packed RGB (ITU-R BT.601, studio range), top row first. */
void oi_to_rgb24(const oi_frame *f, uint8_t *dst, int dst_stride);
/* As above, RGBA with the frame's transparency (opaque when it has none). */
void oi_to_rgba32(const oi_frame *f, uint8_t *dst, int dst_stride);

const char *oi_error_string(int status);

#ifdef __cplusplus
}
#endif

#endif
