/* Cross-format gamma correction consistency tests.
 *
 * The same encoded image is decoded to every applicable output format with
 * SPNG_DECODE_GAMMA set, the decoded color channels must match between
 * formats (up to a 1-level rounding difference when an 8-bit and a 16-bit
 * per channel format are compared), alpha channels must match exactly
 * (after scaling to the common 8-bit width).
 *
 * The test images are generated in-memory with the encoder so the test does
 * not depend on external files or libpng.
 */

#include <spng.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 16
#define HEIGHT 16
#define NPIXELS ((size_t)WIDTH * HEIGHT)
#define GAMA_INT 25000 /* gamma = 0.25, exponent 1/(0.25*2.2) ~= 1.82 */

struct test_image
{
    const char *name;
    uint8_t color_type;
    uint8_t bit_depth;
    int have_trns;
    int interlaced;
    size_t raw_size; /* image size in SPNG_FMT_PNG layout */
    const unsigned char *raw;
    const struct spng_plte *plte;
    const struct spng_trns *trns;
};

static int encode_png(const struct test_image *img, unsigned char **out_buf, size_t *out_len)
{
    int ret;
    spng_ctx *ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    if(ctx == NULL) return 1;

    spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1);

    struct spng_ihdr ihdr = {0};
    ihdr.width = WIDTH;
    ihdr.height = HEIGHT;
    ihdr.bit_depth = img->bit_depth;
    ihdr.color_type = img->color_type;
    ihdr.interlace_method = img->interlaced ? SPNG_INTERLACE_ADAM7 : SPNG_INTERLACE_NONE;

    ret = spng_set_ihdr(ctx, &ihdr);
    if(ret) goto fail;

    ret = spng_set_gama_int(ctx, GAMA_INT);
    if(ret) goto fail;

    if(img->plte != NULL)
    {
        ret = spng_set_plte(ctx, (struct spng_plte *)img->plte);
        if(ret) goto fail;
    }

    if(img->have_trns && img->trns != NULL)
    {
        ret = spng_set_trns(ctx, (struct spng_trns *)img->trns);
        if(ret) goto fail;
    }

    ret = spng_encode_image(ctx, img->raw, img->raw_size, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    if(ret)
    {
        printf("%s: encode failed: %s\n", img->name, spng_strerror(ret));
        goto fail;
    }

    int err;
    *out_buf = spng_get_png_buffer(ctx, out_len, &err);
    if(*out_buf == NULL)
    {
        printf("%s: spng_get_png_buffer failed: %s\n", img->name, spng_strerror(err));
        ret = 1;
        goto fail;
    }

    spng_ctx_free(ctx);
    return 0;

fail:
    spng_ctx_free(ctx);
    return ret ? ret : 1;
}

static unsigned char *decode_png(const unsigned char *buf, size_t len, int fmt, int flags, size_t *out_size)
{
    spng_ctx *ctx = spng_ctx_new(0);
    if(ctx == NULL) return NULL;

    int ret = spng_set_png_buffer(ctx, (void *)buf, len);
    if(ret) goto fail;

    size_t image_size;
    ret = spng_decoded_image_size(ctx, fmt, &image_size);
    if(ret) goto fail;

    unsigned char *out = malloc(image_size);
    if(out == NULL) goto fail;

    ret = spng_decode_image(ctx, out, image_size, fmt, flags);
    if(ret)
    {
        printf("decode to fmt %d failed: %s\n", fmt, spng_strerror(ret));
        free(out);
        goto fail;
    }

    spng_ctx_free(ctx);
    *out_size = image_size;
    return out;

fail:
    spng_ctx_free(ctx);
    return NULL;
}

struct decoded
{
    int fmt;
    size_t size;
    unsigned char *data;
};

/* Number of color (non-alpha) components in an output format. */
static int color_components(int fmt)
{
    switch(fmt)
    {
        case SPNG_FMT_RGBA8:
        case SPNG_FMT_RGBA16:
        case SPNG_FMT_RGB8: return 3;
        case SPNG_FMT_G8:
        case SPNG_FMT_GA8:
        case SPNG_FMT_GA16: return 1;
        default: return 0;
    }
}

static int has_alpha(int fmt)
{
    return fmt == SPNG_FMT_RGBA8 || fmt == SPNG_FMT_RGBA16 ||
           fmt == SPNG_FMT_GA8 || fmt == SPNG_FMT_GA16;
}

static int is_16bit_fmt(int fmt)
{
    return fmt == SPNG_FMT_RGBA16 || fmt == SPNG_FMT_GA16;
}

/* Read one sample of an output format and normalize it to 8-bit width. */
static uint8_t get_sample8(const struct decoded *d, size_t pixel, int component)
{
    int n_comp = color_components(d->fmt) + (has_alpha(d->fmt) ? 1 : 0);
    size_t offset;

    if(is_16bit_fmt(d->fmt))
    {
        uint16_t v;
        offset = (pixel * n_comp + component) * 2;
        memcpy(&v, d->data + offset, 2);
        return (uint8_t)(v >> 8);
    }

    offset = pixel * n_comp + component;
    return d->data[offset];
}

static const char *fmt_name(int fmt)
{
    switch(fmt)
    {
        case SPNG_FMT_RGBA8: return "RGBA8";
        case SPNG_FMT_RGBA16: return "RGBA16";
        case SPNG_FMT_RGB8: return "RGB8";
        case SPNG_FMT_G8: return "G8";
        case SPNG_FMT_GA8: return "GA8";
        case SPNG_FMT_GA16: return "GA16";
        default: return "?";
    }
}

/* Returns 0 if the gamma-corrected images are consistent, 1 otherwise. */
static int compare_pair(const struct test_image *img, const struct decoded *a, const struct decoded *b)
{
    /* An 8-bit and a 16-bit per channel output differ by at most one
       8-bit level because each width uses its own lookup table. */
    int cross_depth = is_16bit_fmt(a->fmt) != is_16bit_fmt(b->fmt);
    unsigned tolerance = cross_depth ? 1u : 0u;

    /* Grayscale outputs only carry one color component; compare it against
       the (identical) red component of RGB(A) outputs. */
    int comps = color_components(a->fmt) < color_components(b->fmt)
              ? color_components(a->fmt) : color_components(b->fmt);

    size_t p;
    int c;

    for(p = 0; p < NPIXELS; p++)
    {
        for(c = 0; c < comps; c++)
        {
            unsigned x = get_sample8(a, p, c);
            unsigned y = get_sample8(b, p, c);
            unsigned diff = x > y ? x - y : y - x;

            if(diff > tolerance)
            {
                printf("%s: color mismatch %s vs %s at pixel %zu channel %d: %u != %u\n",
                       img->name, fmt_name(a->fmt), fmt_name(b->fmt), p, c, x, y);
                return 1;
            }
        }

        if(has_alpha(a->fmt) && has_alpha(b->fmt))
        {
            unsigned x = get_sample8(a, p, color_components(a->fmt));
            unsigned y = get_sample8(b, p, color_components(b->fmt));
            unsigned diff = x > y ? x - y : y - x;

            /* Alpha is never gamma corrected; only table-width rounding
               may differ between 8-bit and 16-bit outputs. */
            if(diff > tolerance)
            {
                printf("%s: alpha mismatch %s vs %s at pixel %zu: %u != %u\n",
                       img->name, fmt_name(a->fmt), fmt_name(b->fmt), p, x, y);
                return 1;
            }
        }
    }

    return 0;
}

static int run_case(const struct test_image *img)
{
    int ret = 1;
    unsigned char *png_buf = NULL;
    size_t png_len = 0;

    struct decoded decs[8];
    int n_decs = 0;

    int fmts[8];
    int n_fmts = 0;

    /* Every input can be converted to the universal formats */
    fmts[n_fmts++] = SPNG_FMT_RGBA8;
    fmts[n_fmts++] = SPNG_FMT_RGBA16;
    fmts[n_fmts++] = SPNG_FMT_RGB8;

    if(img->color_type == SPNG_COLOR_TYPE_GRAYSCALE && img->bit_depth <= 8)
    {
        fmts[n_fmts++] = SPNG_FMT_G8;
        fmts[n_fmts++] = SPNG_FMT_GA8;
    }
    else if(img->color_type == SPNG_COLOR_TYPE_GRAYSCALE && img->bit_depth == 16)
    {
        fmts[n_fmts++] = SPNG_FMT_GA16;
    }

    if(encode_png(img, &png_buf, &png_len))
    {
        printf("%s: encoding test image failed\n", img->name);
        return 1;
    }

    int i, j;

    for(i = 0; i < n_fmts; i++)
    {
        int flags = SPNG_DECODE_GAMMA;
        if(img->have_trns) flags |= SPNG_DECODE_TRNS;

        decs[n_decs].fmt = fmts[i];
        decs[n_decs].data = decode_png(png_buf, png_len, fmts[i], flags, &decs[n_decs].size);

        if(decs[n_decs].data == NULL)
        {
            printf("%s: decoding fmt %s failed\n", img->name, fmt_name(fmts[i]));
            goto cleanup;
        }

        n_decs++;
    }

    /* Also decode without gamma to prove the flag changes the output. */
    {
        size_t plain_size;
        unsigned char *plain = decode_png(png_buf, png_len, SPNG_FMT_RGBA8,
                                          img->have_trns ? SPNG_DECODE_TRNS : 0, &plain_size);
        if(plain == NULL)
        {
            printf("%s: plain decode failed\n", img->name);
            goto cleanup;
        }

        if(!memcmp(plain, decs[0].data, plain_size < decs[0].size ? plain_size : decs[0].size))
        {
            printf("%s: gamma flag had no effect on RGBA8 output\n", img->name);
            free(plain);
            goto cleanup;
        }
        free(plain);
    }

    for(i = 0; i < n_decs; i++)
    {
        for(j = i + 1; j < n_decs; j++)
        {
            if(compare_pair(img, &decs[i], &decs[j])) goto cleanup;
        }
    }

    printf("%s: OK (%d formats cross-checked)\n", img->name, n_decs);
    ret = 0;

cleanup:
    for(i = 0; i < n_decs; i++) free(decs[i].data);
    free(png_buf);
    return ret;
}

static unsigned char raw_rgb8[NPIXELS * 3];
static unsigned char raw_rgba8[NPIXELS * 4];
static unsigned char raw_gray8[NPIXELS];
static unsigned char raw_ga8[NPIXELS * 2];
static unsigned char raw_rgb16[NPIXELS * 6];
static unsigned char raw_rgba16[NPIXELS * 8];
static unsigned char raw_gray16[NPIXELS * 2];
static unsigned char raw_ga16[NPIXELS * 4];
static unsigned char raw_indexed8[NPIXELS];
static unsigned char raw_indexed4[NPIXELS / 2];

static struct spng_plte plte;
static struct spng_trns trns;

static void fill_pattern(void)
{
    size_t i;
    for(i = 0; i < NPIXELS; i++)
    {
        uint8_t v = (uint8_t)((i * 17 + 7) & 0xFF);
        uint8_t w = (uint8_t)((i * 31 + 3) & 0xFF);

        raw_rgb8[i * 3 + 0] = v;
        raw_rgb8[i * 3 + 1] = (uint8_t)(v + 64);
        raw_rgb8[i * 3 + 2] = (uint8_t)(255 - v);

        raw_rgba8[i * 4 + 0] = v;
        raw_rgba8[i * 4 + 1] = (uint8_t)(v + 64);
        raw_rgba8[i * 4 + 2] = (uint8_t)(255 - v);
        raw_rgba8[i * 4 + 3] = (i % 5 == 0) ? 0 : (uint8_t)(128 + w % 127);

        raw_gray8[i] = v;
        raw_ga8[i * 2] = v;
        raw_ga8[i * 2 + 1] = (i % 5 == 0) ? 0 : 255;

        uint16_t v16 = (uint16_t)(v | (v << 8));
        uint16_t a16 = (i % 5 == 0) ? 0 : (uint16_t)(w | (w << 8));
        uint16_t g16 = (uint16_t)(v16 + 1000);
        uint16_t b16 = (uint16_t)(65535 - v16);

        memcpy(raw_rgb16 + i * 6 + 0, &v16, 2);
        memcpy(raw_rgb16 + i * 6 + 2, &g16, 2);
        memcpy(raw_rgb16 + i * 6 + 4, &b16, 2);

        memcpy(raw_rgba16 + i * 8 + 0, &v16, 2);
        memcpy(raw_rgba16 + i * 8 + 2, &g16, 2);
        memcpy(raw_rgba16 + i * 8 + 4, &b16, 2);
        memcpy(raw_rgba16 + i * 8 + 6, &a16, 2);

        memcpy(raw_gray16 + i * 2, &v16, 2);
        memcpy(raw_ga16 + i * 2, &v16, 2);
        memcpy(raw_ga16 + i * 2 + 2, &a16, 2);

        raw_indexed8[i] = (uint8_t)(i % 8);
    }

    /* 4-bit packed indices */
    for(i = 0; i < NPIXELS / 2; i++)
    {
        raw_indexed4[i] = (uint8_t)(((i * 2) % 16) << 4 | ((i * 2 + 1) % 16));
    }

    memset(&plte, 0, sizeof(plte));
    plte.n_entries = 16;
    for(i = 0; i < 16; i++)
    {
        plte.entries[i].red = (uint8_t)(i * 17);
        plte.entries[i].green = (uint8_t)(255 - i * 13);
        plte.entries[i].blue = (uint8_t)(i * 29 + 11);
    }

    memset(&trns, 0, sizeof(trns));
    trns.gray = 102;
    trns.red = 34;
    trns.green = 98;
    trns.blue = 221;
    trns.n_type3_entries = 6;
    for(i = 0; i < trns.n_type3_entries; i++) trns.type3_alpha[i] = (uint8_t)(i * 40);
}

int main(void)
{
    fill_pattern();

    struct test_image cases[] =
    {
        {/* 8-bit truecolor with tRNS */
            "rgb8+tRNS", SPNG_COLOR_TYPE_TRUECOLOR, 8, 1, 0,
            sizeof(raw_rgb8), raw_rgb8, NULL, &trns
        },
        {/* 8-bit truecolor, interlaced */
            "rgb8-interlaced", SPNG_COLOR_TYPE_TRUECOLOR, 8, 0, 1,
            sizeof(raw_rgb8), raw_rgb8, NULL, NULL
        },
        {/* 16-bit truecolor with tRNS */
            "rgb16+tRNS", SPNG_COLOR_TYPE_TRUECOLOR, 16, 1, 0,
            sizeof(raw_rgb16), raw_rgb16, NULL, &trns
        },
        {/* 16-bit truecolor, interlaced */
            "rgb16-interlaced", SPNG_COLOR_TYPE_TRUECOLOR, 16, 0, 1,
            sizeof(raw_rgb16), raw_rgb16, NULL, NULL
        },
        {/* 8-bit RGBA */
            "rgba8", SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 8, 0, 0,
            sizeof(raw_rgba8), raw_rgba8, NULL, NULL
        },
        {/* 16-bit RGBA */
            "rgba16", SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 16, 0, 0,
            sizeof(raw_rgba16), raw_rgba16, NULL, NULL
        },
        {/* 8-bit indexed palette with tRNS alpha entries */
            "indexed8+tRNS", SPNG_COLOR_TYPE_INDEXED, 8, 1, 0,
            sizeof(raw_indexed8), raw_indexed8, &plte, &trns
        },
        {/* 4-bit indexed palette with tRNS, interlaced */
            "indexed4+tRNS-interlaced", SPNG_COLOR_TYPE_INDEXED, 4, 1, 1,
            sizeof(raw_indexed4), raw_indexed4, &plte, &trns
        },
        {/* <=8-bit grayscale with tRNS (G8/GA8 gray path) */
            "gray8+tRNS", SPNG_COLOR_TYPE_GRAYSCALE, 8, 1, 0,
            sizeof(raw_gray8), raw_gray8, NULL, &trns
        },
        {/* <=8-bit grayscale, no tRNS (covers filler alpha) */
            "gray8", SPNG_COLOR_TYPE_GRAYSCALE, 8, 0, 0,
            sizeof(raw_gray8), raw_gray8, NULL, NULL
        },
        {/* <=8-bit grayscale, interlaced (Adam7 + G8/GA8 path) */
            "gray8-interlaced", SPNG_COLOR_TYPE_GRAYSCALE, 8, 0, 1,
            sizeof(raw_gray8), raw_gray8, NULL, NULL
        },
        {/* 16-bit grayscale with tRNS (GA16 gray path) */
            "gray16+tRNS", SPNG_COLOR_TYPE_GRAYSCALE, 16, 1, 0,
            sizeof(raw_gray16), raw_gray16, NULL, &trns
        },
        {/* 16-bit grayscale, interlaced */
            "gray16-interlaced", SPNG_COLOR_TYPE_GRAYSCALE, 16, 0, 1,
            sizeof(raw_gray16), raw_gray16, NULL, NULL
        },
        {/* 8-bit grayscale + alpha (type 4), decoded to the universal formats;
            GA8/GA16 only accept type 0 grayscale images */
            "gray-alpha8", SPNG_COLOR_TYPE_GRAYSCALE_ALPHA, 8, 0, 0,
            sizeof(raw_ga8), raw_ga8, NULL, NULL
        },
        {/* 16-bit grayscale + alpha (type 4) */
            "gray-alpha16", SPNG_COLOR_TYPE_GRAYSCALE_ALPHA, 16, 0, 0,
            sizeof(raw_ga16), raw_ga16, NULL, NULL
        },
    };

    int ret = 0;
    size_t k;
    for(k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
    {
        if(run_case(&cases[k])) ret = 1;
    }

    if(ret) printf("gamma cross-format tests FAILED\n");
    else printf("all gamma cross-format tests passed\n");

    return ret;
}
