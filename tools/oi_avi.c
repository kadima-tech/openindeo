/* oi-avi: decode an Indeo AVI's video stream.
 *
 *   oi-avi in.avi                  info and per-frame status
 *   oi-avi in.avi out.yuv          every decoded picture as planar YUV 4:1:0 (Y, U, V)
 *   oi-avi in.avi out.yuv --all    one picture per AVI frame, repeats included
 *   oi-avi in.avi out.ppm N        frame N as a PPM
 *
 * A small RIFF reader: the first video stream's fourcc and size from strh/strf, and its chunks
 * from the movi list (nested LISTs such as "rec " included). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openindeo.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    const uint8_t *data;
    uint32_t size;
} chunk;

typedef struct {
    uint32_t fourcc;
    int width, height;
    int stream;
    chunk *chunks;
    size_t count, cap;
} avi;

static void add_chunk(avi *a, const uint8_t *d, uint32_t n) {
    if (a->count == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->chunks = realloc(a->chunks, a->cap * sizeof(chunk));
    }
    a->chunks[a->count++] = (chunk){d, n};
}

static void walk(avi *a, const uint8_t *p, const uint8_t *end, int in_movi, int *stream_no, int *in_vids) {
    while (p + 8 <= end) {
        uint32_t id = rd32(p), n = rd32(p + 4);
        const uint8_t *body = p + 8;
        if (body + n > end) n = (uint32_t)(end - body);
        if (id == 0x5453494c /* LIST */ && n >= 4) {
            uint32_t type = rd32(body);
            if (type == 0x6c727473 /* strl */) {
                *in_vids = 0;
                walk(a, body + 4, body + n, in_movi, stream_no, in_vids);
                (*stream_no)++;
            } else {
                walk(a, body + 4, body + n, in_movi || type == 0x69766f6d /* movi */, stream_no, in_vids);
            }
        } else if (id == 0x68727473 /* strh */ && n >= 8) {
            if (rd32(body) == 0x73646976 /* vids */ && a->stream < 0) {
                a->stream = *stream_no;
                a->fourcc = rd32(body + 4);
                *in_vids = 1;
            }
        } else if (id == 0x66727473 /* strf */ && *in_vids && n >= 20) {
            a->width = (int)rd32(body + 4);
            a->height = abs((int)rd32(body + 8));
            if (rd32(body + 16)) a->fourcc = rd32(body + 16);
        } else if (in_movi && a->stream >= 0) {
            char s[3] = {(char)(id & 0xff), (char)((id >> 8) & 0xff), 0};
            int num = (s[0] - '0') * 10 + (s[1] - '0');
            uint16_t kind = (uint16_t)(id >> 16);
            if (num == a->stream && (kind == ('d' | 'c' << 8) || kind == ('d' | 'b' << 8))) add_chunk(a, body, n);
        }
        p = body + ((n + 1) & ~1u);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s in.avi [out.yuv [--all] | out.ppm N]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);
    if (len < 12 || rd32(buf) != 0x46464952) {
        fprintf(stderr, "not a RIFF file\n");
        return 1;
    }
    avi a = {0};
    a.stream = -1;
    int stream_no = 0, in_vids = 0;
    walk(&a, buf + 12, buf + len, 0, &stream_no, &in_vids);
    printf("%.4s %dx%d, %zu frames\n", (const char *)&a.fourcc, a.width, a.height, a.count);
    oi_decoder *d = oi_create(a.fourcc);
    if (!d) {
        fprintf(stderr, "unsupported codec\n");
        return 1;
    }
    const char *out = argc > 2 ? argv[2] : NULL;
    int ppm = out && strstr(out, ".ppm") != NULL;
    int all = argc > 3 && !strcmp(argv[3], "--all");
    long want = ppm && argc > 3 ? atol(argv[3]) : -1;
    FILE *o = out ? fopen(out, "wb") : NULL;
    int errors = 0, written = 0;
    for (size_t i = 0; i < a.count; i++) {
        oi_frame fr;
        int r = oi_decode(d, a.chunks[i].data, a.chunks[i].size, &fr);
        if (!out) printf("frame %3zu: %6u bytes  %s%s\n", i, a.chunks[i].size, oi_error_string(r), r == OI_OK && !fr.is_new ? " (repeat)" : r == OI_OK && fr.is_key ? " (key)" : "");
        if (r != OI_OK) {
            errors++;
            continue;
        }
        if (!o) continue;
        if (ppm) {
            if ((long)i != want) continue;
            uint8_t *rgb = malloc((size_t)fr.width * fr.height * 3);
            oi_to_rgb24(&fr, rgb, fr.width * 3);
            fprintf(o, "P6\n%d %d\n255\n", fr.width, fr.height);
            fwrite(rgb, 1, (size_t)fr.width * fr.height * 3, o);
            free(rgb);
            written++;
            continue;
        }
        if (!fr.is_new && !all) continue;
        for (int y = 0; y < fr.height; y++) fwrite(fr.y + y * fr.y_stride, 1, (size_t)fr.width, o);
        for (int y = 0; y < fr.chroma_height; y++) fwrite(fr.u + y * fr.uv_stride, 1, (size_t)fr.chroma_width, o);
        for (int y = 0; y < fr.chroma_height; y++) fwrite(fr.v + y * fr.uv_stride, 1, (size_t)fr.chroma_width, o);
        written++;
    }
    if (o) fclose(o);
    if (out) printf("%d pictures written, %d errors\n", written, errors);
    oi_destroy(d);
    free(a.chunks);
    free(buf);
    return errors ? 1 : 0;
}
