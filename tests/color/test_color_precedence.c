/* Standalone regression tests for the PNG color-space chunk precedence
   rules used by SPNG_DECODE_GAMMA (PNG 1.2 13.13 / PNG Third Edition):

     1. sRGB takes precedence over a (possibly non-conformant) gAMA, even
        when an iCCP chunk is also present.
     2. Without sRGB, gAMA drives the correction; iCCP alone does not.
     3. Without sRGB/gAMA there is no gamma source, so the request falls
        back to a no-op even if iCCP is present.
     4. The sRGB rendering intent is reported verbatim.

   Unlike testsuite.c this does not compare against libpng and therefore
   does not rely on any approximate tolerance.  Because a conformant sRGB
   file gamma (1/0.45455) combined with the 2.2 screen gamma yields an
   exponent of ~0.99999, the sRGB-corrected output is bit-identical to the
   uncorrected output; a wrongly applied non-conformant gAMA (0.55) would
   instead move samples by up to 17 levels, which is asserted directly.
*/
#include <spng.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONSTD_FILE_GAMMA 0.55
#define SCREEN_GAMMA 2.2

static int failures;

static unsigned char *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if(f == NULL)
    {
        printf("cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if(len <= 0)
    {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)len);
    if(buf == NULL)
    {
        fclose(f);
        return NULL;
    }
    if(fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

static int decode_rgb8(const unsigned char *png, size_t png_len, int flags,
                       unsigned char **out, size_t *out_len)
{
    spng_ctx *ctx = spng_ctx_new(0);
    if(ctx == NULL) return 1;

    int r = spng_set_png_buffer(ctx, png, png_len);
    if(r) goto fail;

    size_t len;
    r = spng_decoded_image_size(ctx, SPNG_FMT_RGB8, &len);
    if(r) goto fail;

    unsigned char *img = calloc(1, len);
    if(img == NULL)
    {
        r = 1;
        goto fail;
    }

    r = spng_decode_image(ctx, img, len, SPNG_FMT_RGB8, flags);
    if(r)
    {
        free(img);
        goto fail;
    }

    spng_ctx_free(ctx);
    *out = img;
    *out_len = len;
    return 0;

fail:
    spng_ctx_free(ctx);
    return r ? r : 1;
}

static uint8_t expected_sample(uint8_t in, double file_gamma)
{
    /* Same single-precision power law and truncation as the spng gamma LUT */
    float exponent = 1.0f / ((float)file_gamma * (float)SCREEN_GAMMA);
    float c = powf((float)in / 255.0f, exponent) * 255.0f;
    if(c > 255.0f) c = 255.0f;
    return (uint8_t)c;
}

int main(int argc, char **argv)
{
    if(argc != 2)
    {
        printf("usage: %s <test_png_dir>\n", argv[0]);
        return 2;
    }

    const char *dir = argv[1];
    char path[512];

    /* Cases where sRGB must win: even the files carrying a non-conformant
       gAMA (0.55) must decode as if only sRGB were present, i.e. identical
       to the uncorrected output. */
    const char *srgb_cases[] = {
        "color_srgb_gama_bad_iccp.png",
        "color_srgb_gama_bad.png",
        "color_srgb_gama_iccp.png",
        "color_srgb_iccp.png",
        "color_srgb_intent0.png",
        NULL
    };
    for(int i = 0; srgb_cases[i] != NULL; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, srgb_cases[i]);
        size_t png_len;
        unsigned char *png = load_file(path, &png_len);
        if(png == NULL)
        {
            failures++;
            continue;
        }

        unsigned char *corrected = NULL, *plain = NULL;
        size_t corrected_len = 0, plain_len = 0;
        if(decode_rgb8(png, png_len, SPNG_DECODE_GAMMA, &corrected,
                       &corrected_len) ||
           decode_rgb8(png, png_len, 0, &plain, &plain_len) ||
           corrected_len != plain_len)
        {
            printf("FAIL %s: decode failed\n", srgb_cases[i]);
            failures++;
        }
        else if(memcmp(corrected, plain, corrected_len) != 0)
        {
            printf("FAIL %s: sRGB did not take precedence over gAMA\n",
                   srgb_cases[i]);
            failures++;
        }
        else
        {
            printf("ok   %s: sRGB precedence applied\n", srgb_cases[i]);
        }

        free(corrected);
        free(plain);
        free(png);
    }

    /* Cases where gAMA must drive the correction (no sRGB chunk).  The
       conformant gAMA (1/0.45455) collapses to an identity LUT with the
       2.2 screen gamma, so it is compared against the sRGB-style identity;
       the non-conformant gAMA (0.55) must produce a distinct, predictable
       correction. */
    struct
    {
        const char *file;
        int expect_identity;
    } gama_cases[] = {
        {"color_gama_only.png", 0},
        {"color_gama_iccp.png", 1},
        {NULL, 0}
    };
    for(int i = 0; gama_cases[i].file != NULL; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, gama_cases[i].file);
        size_t png_len;
        unsigned char *png = load_file(path, &png_len);
        if(png == NULL)
        {
            failures++;
            continue;
        }

        unsigned char *corrected = NULL, *plain = NULL;
        size_t corrected_len = 0, plain_len = 0;
        if(decode_rgb8(png, png_len, SPNG_DECODE_GAMMA, &corrected,
                       &corrected_len) ||
           decode_rgb8(png, png_len, 0, &plain, &plain_len) ||
           corrected_len != plain_len)
        {
            printf("FAIL %s: decode failed\n", gama_cases[i].file);
            failures++;
            free(png);
            continue;
        }

        int differs = memcmp(corrected, plain, corrected_len) != 0;
        int ok;
        if(gama_cases[i].expect_identity)
        {
            /* conformant gAMA: correction resolves to an identity mapping */
            ok = !differs;
        }
        else
        {
            /* non-conformant gAMA (0.55): distinct output and a known LUT
               value for the first color sample (raw value 7 -> 13) */
            ok = differs && corrected[3] ==
                            expected_sample(7, NONSTD_FILE_GAMMA);
        }

        if(!ok)
        {
            printf("FAIL %s: gAMA correction not applied as expected\n",
                   gama_cases[i].file);
            failures++;
        }
        else
        {
            printf("ok   %s: gAMA drives the correction\n", gama_cases[i].file);
        }

        free(corrected);
        free(plain);
        free(png);
    }

    /* Fallback: without sRGB/gAMA the correction must not touch the output,
       regardless of whether an iCCP profile is embedded.  Covers 8-bit and
       16-bit sources. */
    struct
    {
        const char *file;
        int fmt;
    } fallback_cases[] = {
        {"color_iccp_only.png", SPNG_FMT_RGB8},
        {"color_none.png", SPNG_FMT_RGB8},
        {"color16_iccp_only.png", SPNG_FMT_RGBA16},
        {"color16_none.png", SPNG_FMT_RGBA16},
        {NULL, 0}
    };
    for(int i = 0; fallback_cases[i].file != NULL; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, fallback_cases[i].file);
        size_t png_len;
        unsigned char *png = load_file(path, &png_len);
        if(png == NULL)
        {
            failures++;
            continue;
        }

        spng_ctx *ctx = spng_ctx_new(0);
        spng_set_png_buffer(ctx, png, png_len);
        size_t len;
        if(spng_decoded_image_size(ctx, fallback_cases[i].fmt, &len))
        {
            printf("FAIL %s: size query failed\n", fallback_cases[i].file);
            failures++;
            spng_ctx_free(ctx);
            free(png);
            continue;
        }
        unsigned char *a = calloc(1, len), *b = calloc(1, len);
        int ra = spng_decode_image(ctx, a, len, fallback_cases[i].fmt,
                                   SPNG_DECODE_GAMMA);
        spng_ctx_free(ctx);

        ctx = spng_ctx_new(0);
        spng_set_png_buffer(ctx, png, png_len);
        int rb = spng_decode_image(ctx, b, len, fallback_cases[i].fmt, 0);
        spng_ctx_free(ctx);

        if(ra || rb)
        {
            printf("FAIL %s: decode failed (%d/%d)\n", fallback_cases[i].file,
                   ra, rb);
            failures++;
        }
        else if(memcmp(a, b, len) != 0)
        {
            printf("FAIL %s: output changed without an sRGB/gAMA chunk\n",
                   fallback_cases[i].file);
            failures++;
        }
        else
        {
            printf("ok   %s: gamma request is a no-op fallback\n",
                   fallback_cases[i].file);
        }

        free(a);
        free(b);
        free(png);
    }

    /* Rendering intent distinction */
    struct
    {
        const char *file;
        uint8_t intent;
    } intent_cases[] = {
        {"color_srgb_intent0.png", 0},
        {"color_srgb_intent1.png", 1},
        {NULL, 0}
    };
    for(int i = 0; intent_cases[i].file != NULL; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, intent_cases[i].file);
        size_t png_len;
        unsigned char *png = load_file(path, &png_len);
        if(png == NULL)
        {
            failures++;
            continue;
        }

        spng_ctx *ctx = spng_ctx_new(0);
        if(spng_set_png_buffer(ctx, png, png_len) || spng_decode_chunks(ctx))
        {
            printf("FAIL %s: could not decode chunks\n",
                   intent_cases[i].file);
            failures++;
        }
        else
        {
            uint8_t intent = 0xFF;
            if(spng_get_srgb(ctx, &intent) ||
               intent != intent_cases[i].intent)
            {
                printf("FAIL %s: rendering intent %u, expected %u\n",
                       intent_cases[i].file, intent, intent_cases[i].intent);
                failures++;
            }
            else
            {
                printf("ok   %s: rendering intent %u\n",
                       intent_cases[i].file, intent);
            }
        }

        spng_ctx_free(ctx);
        free(png);
    }

    if(failures)
    {
        printf("\n%d color precedence check(s) failed\n", failures);
        return 1;
    }
    printf("\nall color precedence checks passed\n");
    return 0;
}
