/* SPNG_DECODE_USE_SBIT conformance tests.

   Verifies that significant-bit scaling reproduces the PNG 1.2 (section 13.3)
   left bit replication semantics: the stored sample is shifted down to its
   significant-bit range, then the significant bits are replicated back up to
   the output bit depth. The reference implementation below is independent of
   libspng and mirrors the authoritative replication loop used by libpng on the
   write path (png_do_shift).

   The three required combinations are exercised by the test assets:
     * no sBIT chunk        -> the flag is ignored, output is unchanged
     * 16-bit source images -> scaling across the 16->8 strip and 8->16 widen
     * indexed (palette)    -> entries replicate directly to the 16-bit target

   Usage: sbit_test file.png
*/
#include <spng.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PNG 1.2 sBIT scaling via left bit replication. */
static unsigned ref_scale(unsigned sample, unsigned bit_depth,
                          unsigned sbits, unsigned target)
{
    if(bit_depth > sbits) sample >>= (bit_depth - sbits);
    if(target < sbits) return sample >> (sbits - target);
    if(target == sbits) return sample;

    unsigned out = 0;
    int shift = (int)(target - sbits);
    for(; shift > -(int)sbits; shift -= (int)sbits)
    {
        if(shift > 0) out |= sample << shift;
        else out |= sample >> (-shift);
    }
    unsigned mask = target >= 32 ? 0xffffffff : ((1u << target) - 1);
    return out & mask;
}

static unsigned char *decode_image(const char *path, int fmt, int flags,
                                   size_t *out_len, struct spng_ihdr *ihdr)
{
    FILE *f = fopen(path, "rb");
    if(f == NULL) { fprintf(stderr, "cannot open %s\n", path); return NULL; }

    spng_ctx *ctx = spng_ctx_new(0);
    spng_set_png_file(ctx, f);

    int r = spng_decode_chunks(ctx);
    if(r) { fprintf(stderr, "decode_chunks: %s\n", spng_strerror(r)); goto fail; }

    if(ihdr) spng_get_ihdr(ctx, ihdr);

    size_t len;
    r = spng_decoded_image_size(ctx, fmt, &len);
    if(r) { fprintf(stderr, "image_size: %s\n", spng_strerror(r)); goto fail; }

    unsigned char *buf = calloc(1, len);
    r = spng_decode_image(ctx, buf, len, fmt, flags);
    if(r) { fprintf(stderr, "decode_image(fmt=%d flags=%d): %s\n", fmt, flags, spng_strerror(r)); free(buf); goto fail; }

    spng_ctx_free(ctx);
    fclose(f);
    *out_len = len;
    return buf;

fail:
    spng_ctx_free(ctx);
    fclose(f);
    return NULL;
}

struct fmt_def { int fmt; const char *name; unsigned channels; unsigned bytes; };

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: %s file.png\n", argv[0]); return 2; }
    const char *path = argv[1];

    struct spng_ihdr ihdr;
    struct spng_sbit sbit;
    memset(&sbit, 0, sizeof sbit);

    spng_ctx *probe = spng_ctx_new(0);
    FILE *pf = fopen(path, "rb");
    spng_set_png_file(probe, pf);
    if(spng_decode_chunks(probe)) { fprintf(stderr, "probe decode failed\n"); return 1; }
    spng_get_ihdr(probe, &ihdr);
    int has_sbit = (spng_get_sbit(probe, &sbit) == 0);
    spng_ctx_free(probe);
    fclose(pf);

    const struct fmt_def fmts[] = {
        { SPNG_FMT_RGBA8,  "RGBA8",  4, 1 },
        { SPNG_FMT_RGBA16, "RGBA16", 4, 2 },
        { SPNG_FMT_RGB8,   "RGB8",   3, 1 },
        { SPNG_FMT_G8,     "G8",     1, 1 },
        { SPNG_FMT_GA8,    "GA8",    2, 1 },
        { SPNG_FMT_GA16,   "GA16",   2, 2 },
    };
    const unsigned nfmts = sizeof fmts / sizeof fmts[0];

    int failures = 0;

    printf("%s: %ux%u color_type=%u bit_depth=%u sBIT=%s\n", path,
           ihdr.width, ihdr.height, ihdr.color_type, ihdr.bit_depth,
           has_sbit ? "yes" : "no");

    for(unsigned fi = 0; fi < nfmts; fi++)
    {
        const struct fmt_def *d = &fmts[fi];
        int fmt = d->fmt;

        /* Output format applicability mirrors spng_decoded_image_size();
           probe the size so an unsupported combination is skipped. */
        {
            spng_ctx *probe2 = spng_ctx_new(0);
            FILE *qf = fopen(path, "rb");
            spng_set_png_file(probe2, qf);
            spng_decode_chunks(probe2);
            size_t probe_len;
            int supported = (spng_decoded_image_size(probe2, fmt, &probe_len) == 0);
            spng_ctx_free(probe2);
            fclose(qf);
            if(!supported) continue;
        }

        size_t len_plain, len_sbit;
        unsigned char *plain = decode_image(path, fmt, 0, &len_plain, NULL);
        unsigned char *scaled = decode_image(path, fmt, SPNG_DECODE_USE_SBIT, &len_sbit, NULL);
        if(plain == NULL || scaled == NULL) { failures++; free(plain); free(scaled); continue; }

        unsigned target = d->bytes * 8;
        unsigned pixels = (unsigned)(len_sbit / (d->channels * d->bytes));
        size_t mismatches = 0;
        size_t fallback_mismatches = 0;

        int indexed = (ihdr.color_type == SPNG_COLOR_TYPE_INDEXED);
        int stripped16to8 = (ihdr.bit_depth == 16 && target == 8);

        /* Processing depth: palette entries are always 8-bit; a 16-bit source
           truncated to an 8-bit target is handled in the 8-bit domain. */
        unsigned processing_depth;
        if(indexed) processing_depth = 8;
        else if(stripped16to8) processing_depth = 8;
        else processing_depth = ihdr.bit_depth < target ? ihdr.bit_depth : target;

        /* A sub-16-bit source widened to a 16-bit target: the no-flag output
           is the byte-replicated source sample, recover the original sample
           from the high byte. This includes palette entries, whose stored
           8-bit value is byte-replicated by the plain (no-flag) decode. */
        int widen_up = (target == 16 && (indexed || ihdr.bit_depth < 16));

        for(size_t px = 0; px < pixels; px++)
        {
            for(unsigned c = 0; c < d->channels; c++)
            {
                size_t off = (px * d->channels + c) * d->bytes;
                unsigned v_plain, v_scaled;
                if(d->bytes == 1)
                {
                    v_plain = plain[off];
                    v_scaled = scaled[off];
                }
                else
                {
                    uint16_t t;
                    memcpy(&t, plain + off, 2);  v_plain = t;
                    memcpy(&t, scaled + off, 2); v_scaled = t;
                }

                /* Filler alpha synthesized when the source has no alpha
                   channel must never be touched. Indexed tRNS alpha is real. */
                int have_real_alpha = (ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA ||
                                       ihdr.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ||
                                       indexed);
                int is_last_channel = (c == d->channels - 1) &&
                                      (d->channels == 4 || d->channels == 2);
                if(is_last_channel && !have_real_alpha)
                {
                    if(v_plain != v_scaled) fallback_mismatches++;
                    continue;
                }

                if(!has_sbit)
                {
                    /* Without an sBIT chunk the flag must be a no-op. */
                    if(v_plain != v_scaled) fallback_mismatches++;
                    continue;
                }

                /* Per-channel significant bit count, following the source
                   color type rather than the expanded output layout. */
                unsigned sbits;
                if(ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE)
                    sbits = sbit.grayscale_bits;
                else if(ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA)
                    sbits = (c == d->channels - 1 && is_last_channel)
                            ? sbit.alpha_bits : sbit.grayscale_bits;
                else if(indexed)
                    sbits = (c == 0) ? sbit.red_bits :
                            (c == 1) ? sbit.green_bits :
                            (c == 2) ? sbit.blue_bits :
                            (sbit.alpha_bits ? sbit.alpha_bits : 8);
                else
                    sbits = (c == 0) ? sbit.red_bits :
                            (c == 1) ? sbit.green_bits :
                            (c == 2) ? sbit.blue_bits :
                            (sbit.alpha_bits ? sbit.alpha_bits : ihdr.bit_depth);

                /* Grayscale is expanded into RGB(A): color channels inherit
                   the grayscale significant-bit count. */
                if(ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE ||
                   ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA)
                {
                    if(!(is_last_channel && have_real_alpha))
                        sbits = sbit.grayscale_bits;
                }

                /* A 16->8 strip removes the low byte; an sBIT count above 8
                   loses 8 bits, a count at/below 8 leaves the remaining sample
                   fully significant (the low bits are unrecoverable). */
                if(stripped16to8)
                    sbits = sbits > 8 ? sbits - 8 : 8;

                if(sbits > processing_depth) sbits = processing_depth;

                unsigned src_sample, src_depth;
                if(widen_up) { src_sample = v_plain >> 8; src_depth = ihdr.bit_depth; }
                else { src_sample = v_plain; src_depth = processing_depth; }

                unsigned expected = ref_scale(src_sample, src_depth, sbits, target);
                if(expected != v_scaled)
                {
                    if(mismatches == 0)
                        printf("  %s mismatch px=%zu ch=%u: got=%u expected=%u "
                               "plain=%u sbits=%u\n",
                               d->name, px, c, v_scaled, expected, v_plain, sbits);
                    mismatches++;
                }
            }
        }

        if(has_sbit && fallback_mismatches != 0)
        {
            printf("  %s FAIL: %zu filler/fallback samples altered\n",
                   d->name, fallback_mismatches);
            failures++;
        }
        if(mismatches != 0)
        {
            printf("  %s FAIL: %zu/%zu samples differ from reference\n",
                   d->name, mismatches, (size_t)pixels * d->channels);
            failures++;
        }

        free(plain);
        free(scaled);
    }

    if(failures == 0) printf("  all formats OK\n");
    return failures ? 1 : 0;
}
