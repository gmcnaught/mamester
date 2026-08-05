// license:BSD-3-Clause
/***************************************************************************

    drc_diff.h

    Differential test of a native UML back-end against drcbe_c.

    Injected into src/devices/cpu/ by tests/drc-diff/inject.sh. The single
    entry point is called from the libretro host's retro_run(); it does
    nothing at all unless MAMESTER_DRC_DIFF is set in the environment, so a
    core built with the hook in it is the same core in normal use.

***************************************************************************/

#ifndef MAME_CPU_DRC_DIFF_H
#define MAME_CPU_DRC_DIFF_H

#pragma once

class device_t;

namespace drc {

// Runs the corpus once and terminates the process with 0 (all cases agree) or
// 1 (at least one disagreed). Returns immediately, every time, when
// MAMESTER_DRC_DIFF is unset.
//
// `device` only has to be a live device_t -- the harness needs it for
// machine().options() and for a drc_cache owner, not for anything it emulates,
// so the root device of a machine started with no content is enough.
void diff_run_once(device_t &device);

} // namespace drc

#endif // MAME_CPU_DRC_DIFF_H
