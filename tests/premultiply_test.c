/* SPDX-License-Identifier: BSD-2-Clause */

/* Tests for SPNG_DECODE_PREMULTIPLIED_ALPHA.
   The contract under test:
   - premultiplied output is derivable from straight output of the same
     format/flags: out_c = round(straight_c * a / max), alpha untouched
   - fully transparent (a=0) pixels get zeroed color components
   - fully opaque (a=max) pixels keep their color components bit-for-bit
   - holds on 8-bit and 16-bit paths, with tRNS, gAMA, sBIT, interlacing
   - straight mode (flag absent) produces bit-identical output either way
   - one-shot and progressive decode agree
*/

#include <spng.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while(0)

static uint32_t crc_table[256];
static int crc_inited = 0;

static void crc_init(void)
{
    uint32_t n, c, k;
    for(n=0; n < 256; n++)
    {
        c = n;
        for(k=0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_inited = 1;
}

static uint32_t crc_buf(const uint8_t *buf, size_t len)
{
    uint32_t c = 0xffffffffu;
    size_t n;
    for(n=0; n < len; n++) c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static size_t append_u32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;  p[3] = v & 0xff;
    return 4;
}

static size_t append_chunk(uint8_t *out, const char type[4], const uint8_t *data, size_t len)
{
    size_t n = 0;
    n += append_u32(out + n, (uint32_t)len);
    memcpy(out + n, type, 4); n += 4;
    if(len) memcpy(out + n, data, len);
    n += len;
    uint32_t crc = crc_buf(out + n - (len + 4), len + 4);
    n += append_u32(out + n, crc);
    return n;
}

/* Minimal zlib stream (method 8, no compression) of a single buffer */
static size_t zlib_store(uint8_t *out, const uint8_t *data, size_t len)
{
    size_t n = 0;
    uint32_t adler = 1u; /* adler32 of data */
    {
        uint32_t s1 = 1, s2 = 0;
        size_t i;
        for(i=0; i < len; i++)
        {
            s1 = (s1 + data[i]) % 65521u;
            s2 = (s2 + s1) % 65521u;
        }
        adler = (s2 << 16) | s1;
    }

    out[n++] = 0x78; out[n++] = 0x01; /* zlib header, no preset dict */
    size_t off = 0;
    do
    {
        size_t block = len - off;
        int final = 0;
        if(block > 65535) block = 65535;
        else final = 1;
        out[n++] = final ? 1 : 0;
        out[n++] = block & 0xff; out[n++] = (block >> 8) & 0xff;
        out[n++] = (~block) & 0xff; out[n++] = ((~block) >> 8) & 0xff;
        if(block) { memcpy(out + n, data + off, block); n += block; }
        off += block;
    } while(off < len);
    n += append_u32(out + n, adler);
    return n;
}

/* Build a PNG in *out (caller frees). Optional chunks:
   gama: when >= 0 insert gAMA (value * 100000)
   trns: when not NULL insert tRNS chunk (raw chunk data) */
static int make_png(uint8_t **out, size_t *out_len,
                    uint32_t width, uint32_t height,
                    uint8_t color_type, uint8_t bit_depth,
                    uint8_t interlace,
                    const uint8_t *raw, size_t raw_len,
                    int gama, const uint8_t *trns, size_t trns_len,
                    const uint8_t *plte, size_t plte_len,
                    const uint8_t *sbit, size_t sbit_len)
{
    /* upper bound: signature + IHDR + optional chunks + IDAT(fixed overhead) + IEND */
    size_t cap = 8 + 12 + 13 + raw_len + 6 + 12 + (trns ? 12 + trns_len : 0)
               + (gama >= 0 ? 12 + 4 : 0) + (plte ? 12 + plte_len : 0)
               + (sbit ? 12 + sbit_len : 0) + 64;
    uint8_t *buf = malloc(cap);
    if(!buf) return -1;

    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    memcpy(buf, sig, 8);
    size_t n = 8;

    uint8_t ihdr[13];
    size_t k = 0;
    k += append_u32(ihdr + k, width);
    k += append_u32(ihdr + k, height);
    ihdr[k++] = bit_depth;
    ihdr[k++] = color_type;
    ihdr[k++] = 0; /* compression */
    ihdr[k++] = 0; /* filter */
    ihdr[k++] = interlace;
    n += append_chunk(buf + n, "IHDR", ihdr, 13);

    if(gama >= 0)
    {
        uint8_t g[4];
        append_u32(g, (uint32_t)gama);
        n += append_chunk(buf + n, "gAMA", g, 4);
    }
    if(plte) n += append_chunk(buf + n, "PLTE", plte, plte_len);
    if(trns) n += append_chunk(buf + n, "tRNS", trns, trns_len);
    if(sbit) n += append_chunk(buf + n, "sBIT", sbit, sbit_len);

    /* stored zlib stream, worst-case 5 bytes per 65535 + 4 trailer + 2 header */
    uint8_t *zbuf = malloc(raw_len + (raw_len / 65535 + 1) * 5 + 16);
    if(!zbuf) { free(buf); return -1; }
    size_t zlen = zlib_store(zbuf, raw, raw_len);
    n += append_chunk(buf + n, "IDAT", zbuf, zlen);
    free(zbuf);

    n += append_chunk(buf + n, "IEND", NULL, 0);

    *out = buf;
    *out_len = n;
    return 0;
}

/* Add a per-row filter byte (filter 0) to packed scanlines */
static uint8_t *add_filter_bytes(const uint8_t *pixels, uint32_t height, size_t rowstride, size_t *out_len)
{
    size_t len = height * (rowstride + 1);
    uint8_t *raw = malloc(len);
    uint32_t y;
    for(y=0; y < height; y++)
    {
        raw[y * (rowstride + 1)] = 0;
        memcpy(raw + y * (rowstride + 1) + 1, pixels + y * rowstride, rowstride);
    }
    *out_len = len;
    return raw;
}

static int decode_once(const uint8_t *png, size_t png_len, int fmt, int flags,
                       uint8_t **out, size_t *out_size, int progressive)
{
    spng_ctx *ctx = spng_ctx_new(0);
    int r;
    if((r = spng_set_png_buffer(ctx, png, png_len))) goto err;
    if((r = spng_decoded_image_size(ctx, fmt, out_size))) goto err;

    *out = calloc(1, *out_size);
    if(!*out) { r = -1; goto err; }

    if(!progressive)
    {
        r = spng_decode_image(ctx, *out, *out_size, fmt, flags);
    }
    else
    {
        struct spng_ihdr ihdr;
        struct spng_row_info ri;
        size_t row_width;
        if((r = spng_get_ihdr(ctx, &ihdr))) goto err;
        r = spng_decode_image(ctx, NULL, 0, fmt, flags | SPNG_DECODE_PROGRESSIVE);
        if(r) goto err;
        row_width = *out_size / ihdr.height;
        do {
            r = spng_get_row_info(ctx, &ri);
            if(r) break;
            r = spng_decode_row(ctx, *out + ri.row_num * row_width, row_width);
        } while(!r);
        if(r != SPNG_EOI) goto err;
        r = 0;
    }

    spng_ctx_free(ctx);
    return r;
err:
    spng_ctx_free(ctx);
    free(*out);
    *out = NULL;
    return r ? r : -1;
}

static uint8_t pm8(uint8_t c, uint8_t a)
{
    return (uint8_t)(((uint32_t)c * a + 127u) / 255u);
}

static uint16_t pm16(uint16_t c, uint16_t a)
{
    return (uint16_t)(((uint32_t)c * a + 32767u) / 65535u);
}

/* Generic straight -> premultiplied reference for the supported layouts */
static void ref_premultiply(uint8_t *img, size_t size, int fmt)
{
    if(fmt == SPNG_FMT_RGBA8)
    {
        size_t i;
        for(i=0; i < size; i += 4)
        {
            img[i+0] = pm8(img[i+0], img[i+3]);
            img[i+1] = pm8(img[i+1], img[i+3]);
            img[i+2] = pm8(img[i+2], img[i+3]);
        }
    }
    else if(fmt == SPNG_FMT_RGBA16)
    {
        size_t i;
        for(i=0; i < size; i += 8)
        {
            uint16_t *px = (uint16_t *)(img + i);
            px[0] = pm16(px[0], px[3]);
            px[1] = pm16(px[1], px[3]);
            px[2] = pm16(px[2], px[3]);
        }
    }
    else if(fmt == SPNG_FMT_GA8)
    {
        size_t i;
        for(i=0; i < size; i += 2)
            img[i] = pm8(img[i], img[i+1]);
    }
    else if(fmt == SPNG_FMT_GA16)
    {
        size_t i;
        for(i=0; i < size; i += 4)
        {
            uint16_t *px = (uint16_t *)(img + i);
            px[0] = pm16(px[0], px[1]);
        }
    }
}

static const char *fmt_name(int fmt)
{
    switch(fmt)
    {
        case SPNG_FMT_RGBA8: return "RGBA8";
        case SPNG_FMT_RGBA16: return "RGBA16";
        case SPNG_FMT_GA8: return "GA8";
        case SPNG_FMT_GA16: return "GA16";
        case SPNG_FMT_RGB8: return "RGB8";
        default: return "?";
    }
}

/* Decode straight (base_flags) and premultiplied, verify the conversion
   contract on every pixel, plus opaque/transparent invariants.
   require_partial=0 for tRNS fixtures which only produce 0/max alpha. */
static void test_contract(const char *name, const uint8_t *png, size_t png_len,
                          int fmt, int base_flags, int channels, int is16,
                          int require_partial)
{
    uint8_t *straight = NULL, *pre = NULL, *pre_prog = NULL, *expected = NULL;
    size_t ssz = 0, psz = 0, ppsz = 0;
    int r;

    r = decode_once(png, png_len, fmt, base_flags, &straight, &ssz, 0);
    CHECK(r == 0, "%s/%s: straight decode failed (%d)", name, fmt_name(fmt), r);
    r = decode_once(png, png_len, fmt, base_flags | SPNG_DECODE_PREMULTIPLIED_ALPHA, &pre, &psz, 0);
    CHECK(r == 0, "%s/%s: premult decode failed (%d)", name, fmt_name(fmt), r);
    r = decode_once(png, png_len, fmt, base_flags | SPNG_DECODE_PREMULTIPLIED_ALPHA, &pre_prog, &ppsz, 1);
    CHECK(r == 0, "%s/%s: progressive premult decode failed (%d)", name, fmt_name(fmt), r);

    if(r) goto done;

    CHECK(ssz == psz && psz == ppsz, "%s/%s: size mismatch", name, fmt_name(fmt));
    CHECK(memcmp(pre, pre_prog, psz) == 0,
          "%s/%s: one-shot and progressive premult output differ", name, fmt_name(fmt));

    expected = malloc(ssz);
    memcpy(expected, straight, ssz);
    ref_premultiply(expected, ssz, fmt);

    CHECK(memcmp(expected, pre, psz) == 0,
          "%s/%s: premultiplied output does not match contract", name, fmt_name(fmt));

    /* alpha channel must be untouched and opaque/transparent invariants */
    size_t stride = (size_t)channels * (is16 ? 2 : 1);
    size_t npix = ssz / stride, i;
    int saw_transparent = 0, saw_opaque = 0, saw_partial = 0;
    for(i=0; i < npix; i++)
    {
        const uint8_t *sp = straight + i * stride;
        const uint8_t *pp = pre + i * stride;
        int c;
        if(!is16)
        {
            uint8_t a = sp[stride - 1];
            CHECK(pp[stride - 1] == a, "%s/%s: alpha changed at pixel %zu", name, fmt_name(fmt), i);
            if(a == 0)
            {
                saw_transparent = 1;
                for(c=0; c < channels - 1; c++)
                    CHECK(pp[c] == 0, "%s/%s: transparent pixel color %d not zero", name, fmt_name(fmt), c);
            }
            else if(a == 255)
            {
                saw_opaque = 1;
                for(c=0; c < channels - 1; c++)
                    CHECK(pp[c] == sp[c], "%s/%s: opaque pixel color %d modified", name, fmt_name(fmt), c);
            }
            else saw_partial = 1;
        }
        else
        {
            uint16_t a;
            memcpy(&a, sp + stride - 2, 2);
            uint16_t pa;
            memcpy(&pa, pp + stride - 2, 2);
            CHECK(pa == a, "%s/%s: alpha changed at pixel %zu", name, fmt_name(fmt), i);
            if(a == 0)
            {
                saw_transparent = 1;
                for(c=0; c < channels - 1; c++)
                {
                    uint16_t v;
                    memcpy(&v, pp + c * 2, 2);
                    CHECK(v == 0, "%s/%s: transparent pixel color %d not zero", name, fmt_name(fmt), c);
                }
            }
            else if(a == 65535)
            {
                saw_opaque = 1;
                for(c=0; c < channels - 1; c++)
                {
                    uint16_t sv, pv;
                    memcpy(&sv, sp + c * 2, 2);
                    memcpy(&pv, pp + c * 2, 2);
                    CHECK(pv == sv, "%s/%s: opaque pixel color %d modified", name, fmt_name(fmt), c);
                }
            }
            else saw_partial = 1;
        }
    }
    CHECK(saw_transparent, "%s/%s: no fully transparent pixel in fixture", name, fmt_name(fmt));
    CHECK(saw_opaque, "%s/%s: no fully opaque pixel in fixture", name, fmt_name(fmt));
    if(require_partial)
        CHECK(saw_partial, "%s/%s: no partial-alpha pixel in fixture", name, fmt_name(fmt));

done:
    free(straight); free(pre); free(pre_prog); free(expected);
}

/* Flag must not change straight output and must be ignored for alpha-less formats */
static void test_straight_unchanged(const char *name, const uint8_t *png, size_t png_len,
                                    int fmt, int base_flags)
{
    uint8_t *a = NULL, *b = NULL;
    size_t sa = 0, sb = 0;
    int r1 = decode_once(png, png_len, fmt, base_flags, &a, &sa, 0);
    int r2 = decode_once(png, png_len, fmt, base_flags | SPNG_DECODE_PREMULTIPLIED_ALPHA, &b, &sb, 0);
    CHECK(r1 == 0 && r2 == 0, "%s/%s: decode failed", name, fmt_name(fmt));
    CHECK(sa == sb && memcmp(a, b, sa) == 0,
          "%s/%s: premultiply flag changed straight-alpha output", name, fmt_name(fmt));
    free(a); free(b);
}

int main(void)
{
    crc_init();

    /* ---- Fixture 1: RGBA8, contains a=0, a=255 and partial alphas ---- */
    {
        const uint32_t w = 4, h = 2;
        uint8_t px[4*2*4] = {
              0, 100, 200,   0,   /* fully transparent */
             10, 110, 210,  64,  /* partial */
            200, 100,  50, 128,  /* partial */
            255, 255, 255, 255,  /* fully opaque */
             37, 173,  91,   1,  /* near-transparent: rounding edge */
            250,   5, 130, 254,  /* near-opaque: rounding edge */
              1,   2,   3,  17,
            128,  64,  32, 200,
        };
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(px, h, w * 4, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 8, 0,
                 raw, raw_len, -1, NULL, 0, NULL, 0, NULL, 0);
        free(raw);

        test_contract("rgba8", png, png_len, SPNG_FMT_RGBA8, 0, 4, 0, 1);

        /* interlaced variant: re-encode raw with Adam7 layout is complex,
           instead the encoder writes interlaced PNGs; build via spng encoder */
        free(png);
    }

    /* ---- Fixture 2: RGBA16 ---- */
    {
        const uint32_t w = 4, h = 1;
        uint16_t px16[4*4] = {
                0, 1000, 60000,     0,     /* transparent */
            12345, 54321,  1111, 16384,     /* partial */
            65535, 32768,     1, 32767,     /* partial */
             1000,  2000,  3000, 65535,     /* opaque */
        };
        /* big-endian on disk */
        uint8_t bytes[sizeof(px16)];
        size_t i;
        for(i=0; i < 16; i++)
        {
            bytes[i*2]   = (px16[i] >> 8) & 0xff;
            bytes[i*2+1] = px16[i] & 0xff;
        }
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(bytes, h, w * 8, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 16, 0,
                 raw, raw_len, -1, NULL, 0, NULL, 0, NULL, 0);
        free(raw);

        test_contract("rgba16", png, png_len, SPNG_FMT_RGBA16, 0, 4, 1, 1);

        /* 16-bit source decoded to 8-bit exercises the downscale path */
        test_contract("rgba16->8", png, png_len, SPNG_FMT_RGBA8, 0, 4, 0, 1);
        free(png);
    }

    /* ---- Fixture 3: gray (no alpha) + tRNS -> GA8/GA16, the only
       color types these formats accept; tRNS makes one pixel fully
       transparent, the rest opaque ---- */
    {
        const uint32_t w = 3, h = 1;
        uint8_t gray8[3] = { 0, 128, 255 };
        uint8_t trns8[2] = { 0, 128 }; /* grayscale tRNS is a 2-byte sample: 128 -> transparent */
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(gray8, h, w, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_GRAYSCALE, 8, 0,
                 raw, raw_len, -1, trns8, sizeof(trns8), NULL, 0, NULL, 0);
        free(raw);
        test_contract("ga8", png, png_len, SPNG_FMT_GA8, SPNG_DECODE_TRNS, 2, 0, 0);
        free(png);

        uint16_t gray16[3] = { 0, 32768, 65535 };
        uint8_t bytes[6];
        size_t i;
        for(i=0; i < 3; i++)
        {
            bytes[i*2]   = (gray16[i] >> 8) & 0xff;
            bytes[i*2+1] = gray16[i] & 0xff;
        }
        uint8_t trns16[2] = { 0x80, 0x00 }; /* 32768 -> transparent */
        raw = add_filter_bytes(bytes, h, w * 2, &raw_len);
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_GRAYSCALE, 16, 0,
                 raw, raw_len, -1, trns16, sizeof(trns16), NULL, 0, NULL, 0);
        free(raw);
        test_contract("ga16", png, png_len, SPNG_FMT_GA16, SPNG_DECODE_TRNS, 2, 1, 0);
        free(png);
    }

    /* ---- Fixture 4: RGB8 + tRNS -> RGBA8, transparency synthesized ---- */
    {
        const uint32_t w = 3, h = 1;
        uint8_t rgb[3*3] = { 255, 0, 0,   0, 255, 0,   0, 0, 255 };
        /* tRNS for truecolor 8-bit: R G B (2 bytes each) */
        uint8_t trns[6] = { 0,0, 0,255, 0,0 }; /* green pixel is transparent */
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(rgb, h, w * 3, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR, 8, 0,
                 raw, raw_len, -1, trns, sizeof(trns), NULL, 0, NULL, 0);
        free(raw);
        test_contract("trns-rgb8", png, png_len, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS, 4, 0, 0);
        free(png);
    }

    /* ---- Fixture 5: gray 16 + tRNS -> RGBA16 ---- */
    {
        const uint32_t w = 3, h = 1;
        uint16_t gray[3] = { 0, 32768, 65535 };
        uint8_t bytes[6];
        size_t i;
        for(i=0; i < 3; i++){ bytes[i*2] = gray[i] >> 8; bytes[i*2+1] = gray[i]; }
        uint8_t trns[2] = { 0x80, 0x00 }; /* 32768 -> transparent */
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(bytes, h, w * 2, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_GRAYSCALE, 16, 0,
                 raw, raw_len, -1, trns, sizeof(trns), NULL, 0, NULL, 0);
        free(raw);
        test_contract("trns-g16", png, png_len, SPNG_FMT_RGBA16, SPNG_DECODE_TRNS, 4, 1, 0);
        free(png);
    }

    /* ---- Fixture 6: gAMA present, gamma + premultiply ordering ---- */
    {
        const uint32_t w = 4, h = 1;
        uint8_t px[4*4] = {
            10, 20, 30, 0,
            40, 50, 60, 64,
            70, 80, 90, 128,
            100, 200, 250, 255,
        };
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(px, h, w * 4, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 8, 0,
                 raw, raw_len, 45455, NULL, 0, NULL, 0, NULL, 0); /* gAMA 0.45455 */
        free(raw);
        test_contract("gamma-rgba8", png, png_len, SPNG_FMT_RGBA8, SPNG_DECODE_GAMMA, 4, 0, 1);
        test_contract("gamma-rgba16", png, png_len, SPNG_FMT_RGBA16, SPNG_DECODE_GAMMA, 4, 1, 1);
        free(png);
    }

    /* ---- Fixture 7: indexed PNG with tRNS palette alpha ---- */
    {
        const uint32_t w = 3, h = 1;
        uint8_t indices[3] = { 0, 1, 2 };
        uint8_t plte[3*3] = { 255, 0, 0,  0, 255, 0,  0, 0, 255 };
        uint8_t trns[3] = { 0, 128, 255 };
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(indices, h, w, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_INDEXED, 8, 0,
                 raw, raw_len, -1, trns, sizeof(trns), plte, sizeof(plte), NULL, 0);
        free(raw);
        test_contract("indexed8", png, png_len, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS, 4, 0, 1);
        free(png);
    }

    /* ---- Fixture 8: alpha-less formats, flag must be a no-op ---- */
    {
        const uint32_t w = 2, h = 1;
        uint8_t rgb[2*3] = { 1, 2, 3, 4, 5, 6 };
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(rgb, h, w * 3, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR, 8, 0,
                 raw, raw_len, -1, NULL, 0, NULL, 0, NULL, 0);
        free(raw);
        test_straight_unchanged("rgb8-noalpha", png, png_len, SPNG_FMT_RGB8, 0);
        free(png);

        uint8_t gray[2] = { 7, 9 };
        raw = add_filter_bytes(gray, h, w, &raw_len);
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_GRAYSCALE, 8, 0,
                 raw, raw_len, -1, NULL, 0, NULL, 0, NULL, 0);
        free(raw);
        test_straight_unchanged("g8-noalpha", png, png_len, SPNG_FMT_G8, 0);
        free(png);
    }

    /* ---- Fixture 9: interlaced RGBA8 via the real encoder ---- */
    {
        const uint32_t w = 9, h = 7; /* odd dims exercise all Adam7 passes */
        size_t n = (size_t)w * h;
        uint8_t *px = malloc(n * 4), *png = NULL;
        size_t i;
        for(i=0; i < n; i++)
        {
            px[i*4+0] = (uint8_t)(i * 7 + 1);
            px[i*4+1] = (uint8_t)(i * 13 + 2);
            px[i*4+2] = (uint8_t)(i * 29 + 3);
            px[i*4+3] = (uint8_t)(i % 3 == 0 ? 0 : (i % 3 == 1 ? 255 : (i * 37 + 5)));
        }

        spng_ctx *enc = spng_ctx_new(SPNG_CTX_ENCODER);
        spng_set_option(enc, SPNG_ENCODE_TO_BUFFER, 1);
        struct spng_ihdr ihdr = {0};
        ihdr.width = w; ihdr.height = h;
        ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
        ihdr.bit_depth = 8;
        ihdr.interlace_method = SPNG_INTERLACE_ADAM7;
        spng_set_ihdr(enc, &ihdr);
        int r = spng_encode_image(enc, px, n * 4, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
        CHECK(r == 0, "interlaced fixture encode failed (%d)", r);
        if(!r)
        {
            size_t png_size; int err;
            png = spng_get_png_buffer(enc, &png_size, &err);
            CHECK(png != NULL, "spng_get_png_buffer failed");
            if(png) test_contract("interlaced-rgba8", png, png_size, SPNG_FMT_RGBA8, 0, 4, 0, 1);
        }
        spng_ctx_free(enc);
        free(px);
        free(png);
    }

    /* ---- Fixture 10: real sBIT chunks, scaling runs before premultiply ---- */
    {
        /* 16-bit RGBA with a 10-bit sBIT, 16-bit output goes through
           10->16 bit replication in scale_row */
        const uint32_t w = 3, h = 1;
        uint16_t px16[3*4] = {
                  0 << 6,    0 << 6,    0 << 6,     0 << 6, /* transparent */
                 16 << 6,   31 << 6,   47 << 6,   256 << 6, /* partial */
                 63 << 6,   79 << 6,   95 << 6,  1023 << 6, /* opaque */
        };
        uint8_t bytes[sizeof(px16)];
        size_t i;
        for(i=0; i < 12; i++)
        {
            bytes[i*2]   = (px16[i] >> 8) & 0xff;
            bytes[i*2+1] = px16[i] & 0xff;
        }
        uint8_t sbit16[4] = { 10, 10, 10, 10 }; /* color type 6: R G B A */
        size_t raw_len;
        uint8_t *raw = add_filter_bytes(bytes, h, w * 8, &raw_len);
        uint8_t *png; size_t png_len;
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 16, 0,
                 raw, raw_len, -1, NULL, 0, NULL, 0, sbit16, sizeof(sbit16));
        free(raw);
        test_contract("rgba16-sbit-path", png, png_len, SPNG_FMT_RGBA16,
                      SPNG_DECODE_USE_SBIT, 4, 1, 1);
        free(png);

        /* 8-bit RGBA with a 5-bit sBIT, 8-bit output goes through
           5->8 bit replication; samples use the full 8-bit range */
        uint8_t px8[3*4] = {
              0,   0,   0,   0,   /* transparent */
             32,  64,  96, 128,   /* partial */
            255, 200, 150, 255,   /* opaque */
        };
        uint8_t sbit8[4] = { 5, 5, 5, 5 };
        raw = add_filter_bytes(px8, h, w * 4, &raw_len);
        make_png(&png, &png_len, w, h, SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 8, 0,
                 raw, raw_len, -1, NULL, 0, NULL, 0, sbit8, sizeof(sbit8));
        free(raw);
        test_contract("rgba8-sbit-path", png, png_len, SPNG_FMT_RGBA8,
                      SPNG_DECODE_USE_SBIT, 4, 0, 1);
        free(png);
    }

    if(failures) printf("\n%d/%d checks FAILED\n", failures, checks);
    else printf("all %d checks passed\n", checks);

    return failures ? 1 : 0;
}
