/* nv_present_test.c -- host-native unit tests for the format converters.
 *
 * These deliberately do NOT cross-compile: the converters are pure functions,
 * so testing them on the build machine is faster and needs no device. Anything
 * touching /dev/mem is verified on hardware instead.
 *
 *   make -C tests run
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../tools/mame-frontend/mister-backend/nv_present.h"

static int pass = 0, fail = 0;

static void check_u16(const char *label, uint16_t expected, uint16_t actual)
{
    if (expected == actual) { pass++; printf("ok   %s\n", label); }
    else { fail++; printf("FAIL %s: expected 0x%04x got 0x%04x\n",
                          label, expected, actual); }
}

static void check_u64(const char *label, int cond)
{
    if (cond) { pass++; printf("ok   %s\n", label); }
    else { fail++; printf("FAIL %s\n", label); }
}

int main(void)
{
    /* 0RGB1555 -> RGB565. Red and blue keep their 5 bits and shift; green gains
     * a bit, and the new low bit is replicated from the top so that full green
     * reaches 0x3F instead of stopping one step short at 0x3E. That off-by-one
     * would be invisible on a test pattern and wrong on every frame. */
    {
        uint16_t src[3] = { 0x7C00, 0x03E0, 0x001F };   /* red, green, blue */
        uint16_t dst[3] = { 0, 0, 0 };
        nv_convert_1555_to_565(dst, src, 3);
        check_u16("1555 full red   -> 565", 0xF800, dst[0]);
        check_u16("1555 full green -> 565", 0x07E0, dst[1]);
        check_u16("1555 full blue  -> 565", 0x001F, dst[2]);
    }
    {
        uint16_t src[2] = { 0x0000, 0x7FFF };
        uint16_t dst[2] = { 0xFFFF, 0x0000 };
        nv_convert_1555_to_565(dst, src, 2);
        check_u16("1555 black -> 565", 0x0000, dst[0]);
        check_u16("1555 white -> 565", 0xFFFF, dst[1]);
    }
    {
        /* Half green: 0x0F in 5 bits -> 0x1E in 6, not 0x1F. */
        uint16_t src[1] = { (uint16_t)(0x0F << 5) };
        uint16_t dst[1] = { 0 };
        nv_convert_1555_to_565(dst, src, 1);
        check_u16("1555 mid green -> 565", (uint16_t)(0x1E << 5), dst[0]);
    }

    /* XRGB8888 -> RGB565: truncate 8 -> 5/6/5. */
    {
        uint32_t src[4] = { 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF };
        uint16_t dst[4] = { 0, 0, 0, 0 };
        nv_convert_8888_to_565(dst, src, 4);
        check_u16("8888 full red   -> 565", 0xF800, dst[0]);
        check_u16("8888 full green -> 565", 0x07E0, dst[1]);
        check_u16("8888 full blue  -> 565", 0x001F, dst[2]);
        check_u16("8888 white      -> 565", 0xFFFF, dst[3]);
    }
    {
        uint32_t src[2] = { 0x00808080, 0xFF000000 };  /* mid grey; alpha ignored */
        uint16_t dst[2] = { 0, 0xFFFF };
        nv_convert_8888_to_565(dst, src, 2);
        check_u16("8888 mid grey -> 565",
                  (uint16_t)((16 << 11) | (32 << 5) | 16), dst[0]);
        check_u16("8888 ignores the X byte", 0x0000, dst[1]);
    }

    /* PAL8 -> RGB565 is an indexed gather; the point is no surprise at 255. */
    {
        uint16_t pal[256];
        uint8_t  src[4] = { 0, 1, 128, 255 };
        uint16_t dst[4] = { 0, 0, 0, 0 };
        int i;
        for (i = 0; i < 256; i++) pal[i] = (uint16_t)(i * 257);
        nv_convert_pal8_to_565(dst, src, 4, pal);
        check_u16("pal8 index 0",   pal[0],   dst[0]);
        check_u16("pal8 index 1",   pal[1],   dst[1]);
        check_u16("pal8 index 128", pal[128], dst[2]);
        check_u16("pal8 index 255", pal[255], dst[3]);
    }

    /* The frame hash must be order-sensitive and content-sensitive, or it would
     * pass every comparison it is used for. This is the property that makes
     * MISTER_FRAME_HASH evidence rather than decoration. */
    {
        uint16_t a[4] = { 1, 2, 3, 4 };
        uint16_t b[4] = { 1, 2, 3, 5 };
        uint16_t c[4] = { 4, 3, 2, 1 };
        uint64_t ha = nv_hash_pixels(a, 4);
        check_u64("hash is repeatable",           ha == nv_hash_pixels(a, 4));
        check_u64("hash detects a changed pixel", ha != nv_hash_pixels(b, 4));
        check_u64("hash detects reordering",      ha != nv_hash_pixels(c, 4));
        check_u64("hash of empty is the basis",
                  nv_hash_pixels(a, 0) == 1469598103934665603ULL);
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
