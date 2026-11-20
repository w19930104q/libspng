/* APNG frame decode/composite test harness.
 *
 * Dumps each decoded frame (region mode and composed mode) as raw RGBA to a
 * file, so an independent reference renderer can compare them byte-for-byte.
 * Also exercises plain-PNG behavior and error handling on malformed inputs.
 */
#include "spng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int write_all(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if(!f) return 1;
    if(len && fwrite(buf, 1, len, f) != len) { fclose(f); return 1; }
    fclose(f);
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
    if(!b) { fclose(f); return NULL; }
    if(fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return b;
}

/* One full APNG decode in the given mode, dumps canvas-sized frames.
   mode: 0 = region (no compose flag), 1 = composed. */
static int dump_frames(const char *path, int fmt, int compose, const char *out_prefix)
{
    size_t png_len;
    unsigned char *png = load_file(path, &png_len);
    if(!png) { fprintf(stderr, "cannot load %s\n", path); return 2; }

    spng_ctx *ctx = spng_ctx_new(0);
    if(!ctx) { free(png); return 2; }

    int ret = spng_set_png_buffer(ctx, png, png_len);
    if(ret) { fprintf(stderr, "set buffer: %s\n", spng_strerror(ret)); goto fail; }

    struct spng_ihdr ihdr;
    ret = spng_get_ihdr(ctx, &ihdr);
    if(ret) { fprintf(stderr, "get ihdr: %s\n", spng_strerror(ret)); goto fail; }

    int have_actl = 0;
    struct spng_actl actl = {0};
    ret = spng_get_actl(ctx, &actl);
    if(ret == 0) have_actl = 1;

    size_t image_size;
    ret = spng_decoded_image_size(ctx, fmt, &image_size);
    if(ret) { fprintf(stderr, "image size: %s\n", spng_strerror(ret)); goto fail; }

    unsigned char *img = malloc(image_size);
    if(!img) goto fail;

    /* tRNS must be applied explicitly in libspng; the reference renderer
       always applies it, so request it here to make the outputs comparable. */
    int flags = SPNG_DECODE_TRNS | (compose ? SPNG_DECODE_APNG_COMPOSE : 0);

    /* Frame 0: the default image */
    ret = spng_decode_image(ctx, img, image_size, fmt, flags);
    if(ret)
    {
        fprintf(stderr, "%s: decode_image: %s\n", path, spng_strerror(ret));
        free(img);
        goto fail;
    }

    if(!have_actl)
    {
        /* Plain PNG must behave exactly as before */
        char name[704];
        snprintf(name, sizeof(name), "%s_f0.bin", out_prefix);
        if(write_all(name, img, image_size)) { free(img); goto fail; }
        free(img);
        spng_ctx_free(ctx);
        free(png);
        return 0;
    }

    /* A visible default image has a pre-IDAT fcTL; a hidden one does not,
       in which case the decoded default image is not an animation frame. */
    struct spng_fctl first_fctl;
    int visible_default = (spng_get_frame_fctl(ctx, &first_fctl) == 0);

    uint32_t next;       /* output frame index for the next dump */
    uint32_t decoded = 0;

    if(visible_default)
    {
        char name[704];
        snprintf(name, sizeof(name), "%s_f0.bin", out_prefix);
        if(write_all(name, img, image_size)) { free(img); goto fail; }
        next = 1;
        decoded = 1;
    }
    else
    {
        /* Animation frame 0 comes from the first spng_decode_frame() call */
        next = 0;
    }

    for(;;)
    {
        struct spng_fctl fctl;
        ret = spng_decode_frame(ctx, img, image_size, fmt, flags, &fctl);
        if(ret == SPNG_EOI) break;
        if(ret)
        {
            fprintf(stderr, "%s: decode_frame[%u]: %s\n", path, next, spng_strerror(ret));
            free(img);
            goto fail;
        }

        char name[704];
        snprintf(name, sizeof(name), "%s_f%u.bin", out_prefix, next);
        if(write_all(name, img, image_size)) { free(img); goto fail; }
        next++;
        decoded++;
    }

    if(decoded != actl.num_frames)
    {
        fprintf(stderr, "%s: decoded %u frames, acTL says %u\n",
                path, decoded, actl.num_frames);
        free(img);
        goto fail;
    }

    free(img);
    spng_ctx_free(ctx);
    free(png);
    return 0;

fail:
    spng_ctx_free(ctx);
    free(png);
    return 1;
}

/* Malformed inputs: either an error is reported, or (in non-strict mode)
   the bad ancillary chunk is discarded and decoding continues safely.
   In the tolerated case the decoded frames must never exceed the acTL
   count and iteration must end on SPNG_EOI, never run away or crash. */
static int expect_error(const char *path, int fmt)
{
    size_t png_len;
    unsigned char *png = load_file(path, &png_len);
    if(!png) return 2;

    spng_ctx *ctx = spng_ctx_new(0);
    spng_set_png_buffer(ctx, png, png_len);

    size_t image_size = 0;
    spng_decoded_image_size(ctx, fmt, &image_size);
    unsigned char *img = image_size ? malloc(image_size) : NULL;

    int ret = spng_decode_image(ctx, img, image_size, fmt, SPNG_DECODE_TRNS);
    int frame_ret = SPNG_EOI;
    uint32_t n_decoded = 0;
    uint32_t actl_frames = 0;

    if(ret == 0)
    {
        struct spng_actl actl;
        if(spng_get_actl(ctx, &actl) == 0)
        {
            actl_frames = actl.num_frames;
            struct spng_fctl fctl;
            for(;;)
            {
                frame_ret = spng_decode_frame(ctx, img, image_size, fmt,
                                              SPNG_DECODE_TRNS, &fctl);
                if(frame_ret) break;
                n_decoded++;
                if(n_decoded > actl_frames)
                {
                    printf("  %s: FAIL decoded %u frames, exceeds acTL %u\n",
                           path, n_decoded, actl_frames);
                    free(img); spng_ctx_free(ctx); free(png);
                    return 1;
                }
            }
        }
    }

    free(img);
    spng_ctx_free(ctx);
    free(png);

    if(ret != 0)
    {
        printf("  %s: rejected at decode_image (ret=%d %s)\n",
               path, ret, spng_strerror(ret));
        return 0;
    }
    if(frame_ret != SPNG_EOI)
    {
        printf("  %s: rejected at decode_frame (ret=%d %s)\n",
               path, frame_ret, spng_strerror(frame_ret));
        return 0;
    }

    /* Tolerated: default image decoded, animation ended cleanly. The
       recovered frame count only needs to be bounded by the acTL value. */
    printf("  %s: tolerated, %u/%u animation frames recovered\n",
           path, n_decoded, actl_frames);
    return 0;
}

int main(int argc, char **argv)
{
    if(argc < 4)
    {
        fprintf(stderr,
                "usage: %s <rgba8|rgba16> <compose|region> <outdir> [files...]\n"
                "       %s error <fmt> <-> [files...]\n", argv[0], argv[0]);
        return 2;
    }

    int fmt = SPNG_FMT_RGBA8;
    if(!strcmp(argv[1], "rgba16")) fmt = SPNG_FMT_RGBA16;

    if(!strcmp(argv[1], "error"))
    {
        if(!strcmp(argv[2], "rgba16")) fmt = SPNG_FMT_RGBA16;
        int rc = 0;
        for(int i = 4; i < argc; i++) rc |= expect_error(argv[i], fmt);
        return rc;
    }

    int compose = !strcmp(argv[2], "compose");
    const char *outdir = argv[3];

    int rc = 0;
    for(int i = 4; i < argc; i++)
    {
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];

        char prefix[600];
        snprintf(prefix, sizeof(prefix), "%s/%s_%s",
                 outdir, base, compose ? "comp" : "reg");
        rc |= dump_frames(argv[i], fmt, compose, prefix);
    }
    return rc;
}
