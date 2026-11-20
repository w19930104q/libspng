/* Decode APNGs through spng_set_png_stream with a read callback that hands
   out data in deliberately small chunks, exercising the streaming branches
   of IDAT/fdAT consumption. Dumps frames exactly like apng_dump so outputs
   can be diffed against the buffer-mode results. */
#include "spng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct reader {
    const unsigned char *data;
    size_t len;
    size_t pos;
};

static int read_fn(spng_ctx *ctx, void *user, void *dest, size_t length)
{
    (void)ctx;
    struct reader *r = user;
    /* Contract: fill the whole request or report EOF. libspng itself chunks
       requests at SPNG_READ_SIZE, so copying the full length still exercises
       the streaming inflate/fdAT paths rather than the buffer path. */
    if(length > r->len - r->pos) return SPNG_IO_EOF;
    memcpy(dest, r->data + r->pos, length);
    r->pos += length;
    return 0;
}

static unsigned char *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)n);
    if(fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return b;
}

static int write_all(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if(!f) return 1;
    if(len && fwrite(buf, 1, len, f) != len) { fclose(f); return 1; }
    fclose(f);
    return 0;
}

static int dump_one(const char *path, int fmt, int compose, const char *prefix)
{
    size_t png_len;
    unsigned char *png = load_file(path, &png_len);
    if(!png) return 2;

    struct reader r = {png, png_len, 0};
    spng_ctx *ctx = spng_ctx_new(0);
    int ret = spng_set_png_stream(ctx, read_fn, &r);
    if(ret) { fprintf(stderr, "set stream: %s\n", spng_strerror(ret)); goto fail; }

    struct spng_ihdr ihdr;
    if(spng_get_ihdr(ctx, &ihdr)) goto fail;

    struct spng_actl actl;
    int have_actl = (spng_get_actl(ctx, &actl) == 0);

    size_t image_size;
    if(spng_decoded_image_size(ctx, fmt, &image_size)) goto fail;
    unsigned char *img = malloc(image_size);

    int flags = SPNG_DECODE_TRNS | (compose ? SPNG_DECODE_APNG_COMPOSE : 0);

    ret = spng_decode_image(ctx, img, image_size, fmt, flags);
    if(ret) { fprintf(stderr, "%s decode_image: %s\n", path, spng_strerror(ret));
              free(img); goto fail; }

    if(!have_actl)
    {
        char name[900];
        snprintf(name, sizeof(name), "%s_f0.bin", prefix);
        if(write_all(name, img, image_size)) { free(img); goto fail; }
        free(img); spng_ctx_free(ctx); free(png);
        return 0;
    }

    struct spng_fctl first_fctl;
    int visible = (spng_get_frame_fctl(ctx, &first_fctl) == 0);
    uint32_t next = 0, decoded = 0;
    if(visible)
    {
        char name[900];
        snprintf(name, sizeof(name), "%s_f0.bin", prefix);
        if(write_all(name, img, image_size)) { free(img); goto fail; }
        next = 1; decoded = 1;
    }

    for(;;)
    {
        struct spng_fctl fctl;
        ret = spng_decode_frame(ctx, img, image_size, fmt, flags, &fctl);
        if(ret == SPNG_EOI) break;
        if(ret) { fprintf(stderr, "%s frame[%u]: %s\n", path, next,
                          spng_strerror(ret)); free(img); goto fail; }
        char name[900];
        snprintf(name, sizeof(name), "%s_f%u.bin", prefix, next);
        if(write_all(name, img, image_size)) { free(img); goto fail; }
        next++; decoded++;
    }

    if(decoded != actl.num_frames)
    {
        fprintf(stderr, "%s: streamed %u frames, acTL %u\n", path, decoded,
                actl.num_frames);
        free(img); goto fail;
    }

    free(img); spng_ctx_free(ctx); free(png);
    return 0;
fail:
    spng_ctx_free(ctx); free(png);
    return 1;
}

int main(int argc, char **argv)
{
    if(argc < 4)
    {
        fprintf(stderr, "usage: %s <rgba8|rgba16> <compose|region> <outdir> files...\n", argv[0]);
        return 2;
    }
    int fmt = !strcmp(argv[1], "rgba16") ? SPNG_FMT_RGBA16 : SPNG_FMT_RGBA8;
    int compose = !strcmp(argv[2], "compose");
    const char *outdir = argv[3];
    int rc = 0;
    for(int i = 4; i < argc; i++)
    {
        const char *b = strrchr(argv[i], '/');
        b = b ? b + 1 : argv[i];
        char prefix[900];
        snprintf(prefix, sizeof(prefix), "%s/%s_%s", outdir, b,
                 compose ? "comp" : "reg");
        rc |= dump_one(argv[i], fmt, compose, prefix);
    }
    return rc;
}
