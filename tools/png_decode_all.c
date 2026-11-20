/* Decode every input PNG in several formats, dump raw output to stdout-ish
   per file into an output directory, for baseline-vs-patched comparison. */
#include "spng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)n);
    if(!b) { fclose(f); return NULL; }
    if(fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return b;
}

static const int fmts[] = {
    SPNG_FMT_RGBA8, SPNG_FMT_RGBA16, SPNG_FMT_RGB8, SPNG_FMT_G8
};
static const char *fmt_names[] = {"rgba8", "rgba16", "rgb8", "g8"};

int main(int argc, char **argv)
{
    const char *outdir = argv[1];
    int rc = 0;
    for(int ai = 2; ai < argc; ai++)
    {
        const char *path = argv[ai];
        size_t png_len;
        unsigned char *png = load_file(path, &png_len);
        if(!png) { fprintf(stderr, "load fail %s\n", path); rc = 1; continue; }

        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;

        for(int fi = 0; fi < 4; fi++)
        {
            spng_ctx *ctx = spng_ctx_new(0);
            spng_set_png_buffer(ctx, png, png_len);
            size_t sz = 0;
            int sr = spng_decoded_image_size(ctx, fmts[fi], &sz);
            if(sr)
            {
                /* format not applicable to this color type, skip */
                spng_ctx_free(ctx);
                continue;
            }
            unsigned char *img = malloc(sz);
            int r = spng_decode_image(ctx, img, sz, fmts[fi], SPNG_DECODE_TRNS);
            if(r)
            {
                fprintf(stderr, "%s %s decode: %s\n", path, fmt_names[fi],
                        spng_strerror(r));
                rc = 1;
                free(img);
                spng_ctx_free(ctx);
                continue;
            }
            char name[640];
            snprintf(name, sizeof(name), "%s/%s.%s.bin", outdir, base,
                     fmt_names[fi]);
            FILE *o = fopen(name, "wb");
            if(!o) { fprintf(stderr, "open %s\n", name); rc = 1; }
            else { fwrite(img, 1, sz, o); fclose(o); }
            free(img);
            spng_ctx_free(ctx);
        }
        free(png);
    }
    return rc;
}
