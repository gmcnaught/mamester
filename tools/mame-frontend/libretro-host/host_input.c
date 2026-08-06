/* host_input.c -- MiSTer joystick words into libretro button reads.
 *
 * The FPGA reader writes one 32-bit word per player back into the DDR block;
 * nv_pads() reads it. Stage 5 fixed the bit layout against the MiSTer arcade
 * convention and verified it by poking a sentinel into each word and watching
 * the FPGA overwrite it, so the layout below is measured, not assumed:
 *
 *   [0]right [1]left [2]down [3]up [4..9]fire1..6 [10]start [11]coin [12]pause
 *
 * There is no remapping layer, deliberately. The CONF_STR already names these
 * buttons, so a second mapping here would be a second place for the meaning of
 * "fire 2" to drift.
 *
 * The pads are latched once per retro_run() in host_input_poll() rather than
 * read per button: mame2003-plus calls input_cb once per input bit per frame
 * (mame2003.c:1221 and the analog paths around it), which for four players is
 * dozens of reads, and every one would otherwise be an uncached DDR load.
 */

#include <stdio.h>

#include "host.h"
#include "nv_present.h"

#define HOST_PADS 4

/* Bit 12 is pause. It is READ and deliberately NOT acted on: pausing is an OSD
 * concern, and wiring it here would put an untested freeze path into the
 * binary the benchmark runs. A spuriously set bit would then stop the emulator
 * with nothing to distinguish it from a hang. */
static uint32_t pad_state[HOST_PADS];

/* nv_pads() bit -> RETRO_DEVICE_ID_JOYPAD_*. 2003-plus supports four players
 * (MAX_PLAYER_COUNT) and nv_pads serves four words, so every port wires up
 * identically -- the same reason Stage 5 put Start and Coin on each player's
 * own pad rather than only on player 1. */
static const int pad_bit_to_retro[] = {
    RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_LEFT,
    RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_UP,
    RETRO_DEVICE_ID_JOYPAD_B,
    RETRO_DEVICE_ID_JOYPAD_A,
    RETRO_DEVICE_ID_JOYPAD_Y,
    RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_L,
    RETRO_DEVICE_ID_JOYPAD_R,
    RETRO_DEVICE_ID_JOYPAD_START,
    RETRO_DEVICE_ID_JOYPAD_SELECT,   /* coin */
};
#define PAD_BITS ((int)(sizeof pad_bit_to_retro / sizeof pad_bit_to_retro[0]))

/* Reverse map, built once: retro id -> pad bit, or -1. */
/* Built by a constructor rather than lazily. host_input_state() is called once
 * per input code per player per frame -- hundreds of times -- and a lazy table
 * puts a load-and-test of `reverse_built` in front of every one of them for a
 * table that is fully determined at compile time. The constructor runs before
 * main(), so there is no ordering hazard with the first poll. */
static int8_t retro_to_pad_bit[RETRO_DEVICE_ID_JOYPAD_R3 + 1];

__attribute__((constructor))
static void build_reverse(void)
{
    int i;
    for (i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++)
        retro_to_pad_bit[i] = -1;
    for (i = 0; i < PAD_BITS; i++)
        retro_to_pad_bit[pad_bit_to_retro[i]] = (int8_t)i;
}

void host_input_poll(void)
{
    int p;

    for (p = 0; p < HOST_PADS; p++)
        pad_state[p] = nv_pads(p);
}

int16_t host_input_state(unsigned port, unsigned device, unsigned index,
                         unsigned id)
{
    int bit;

    (void)index;

    /* options.input_interface defaults to "simultaneous", so the core asks for
     * keyboard and analog reads too. There is no keyboard and no analog stick
     * on this path; answering 0 is correct, not a stub. */
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    if (port >= HOST_PADS)             return 0;
    if (id > RETRO_DEVICE_ID_JOYPAD_R3) return 0;

    bit = retro_to_pad_bit[id];
    if (bit < 0) return 0;

    return (int16_t)((pad_state[port] >> bit) & 1u);
}
