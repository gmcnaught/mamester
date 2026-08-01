/*
 * mister_profile.cpp — SIGPROF program-counter sampler for the MiSTer build.
 *
 * mame4all-pi ships no profiler that survives -O3, the device has no perf, and
 * gdb cannot unwind the ARM frames. What it can do is answer "which code is the
 * CPU actually in", which is all the driver triage needs. This samples the PC on
 * ITIMER_PROF (process CPU time, so idle/sleep does not count) and dumps a flat
 * profile at exit.
 *
 *   MISTER_PROFILE=1       sample at 1000 Hz
 *   MISTER_PROFILE=<hz>    sample at <hz> (10..10000)
 *
 * Output goes to stderr as MISTER-PROF lines: module + file offset + sample
 * count. Offsets are file-relative (the ELF is PIE), so tools/symbolize-prof.py
 * maps them back to function names using the UNSTRIPPED binary — build with
 * `tools/build-mame.sh STRIP=true all` to keep the symbol table.
 *
 * The handler only writes to a preallocated open-addressing table, so it is
 * async-signal-safe; SA_RESTART keeps the ALSA writes from seeing EINTR.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>

#include "mister_profile.h"

// ---- sample table ---------------------------------------------------------
// Open addressing, power-of-two, never resized: a full table drops samples
// rather than allocating in a signal handler. 64 K slots covers a MAME run's
// hot text many times over.
#define PROF_SLOTS 65536u
#define PROF_MASK  (PROF_SLOTS - 1u)

struct prof_slot { unsigned long pc; unsigned long count; };

static struct prof_slot *prof_table  = 0;
static volatile unsigned long prof_dropped = 0;   // table full
static int  prof_hz      = 0;
static int  prof_enabled = 0;

static void prof_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig; (void)info;
    const ucontext_t *uc = (const ucontext_t*)uctx;
    unsigned long pc = (unsigned long)uc->uc_mcontext.arm_pc;

    // Knuth multiplicative hash; PCs are 2/4-byte aligned so the low bits are
    // poor keys on their own.
    unsigned int h = (unsigned int)((pc >> 1) * 2654435761u) & PROF_MASK;
    for (unsigned int probe = 0; probe < 64; probe++) {
        struct prof_slot *s = &prof_table[(h + probe) & PROF_MASK];
        if (s->pc == pc)  { s->count++; return; }
        if (s->pc == 0)   { s->pc = pc; s->count = 1; return; }
    }
    prof_dropped++;
}

// ---- module map (resolved at dump time, not in the handler) ---------------
#define PROF_MAX_MODULES 64

struct prof_module {
    unsigned long start, end, offset;
    char path[192];
};

static struct prof_module prof_mod[PROF_MAX_MODULES];
static int prof_nmod = 0;

static void prof_read_maps(void)
{
    prof_nmod = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (prof_nmod < PROF_MAX_MODULES && fgets(line, sizeof line, f)) {
        unsigned long start, end, off;
        char perms[8], dev[16], path[256];
        unsigned long inode;
        path[0] = 0;
        int n = sscanf(line, "%lx-%lx %7s %lx %15s %lu %255[^\n]",
                       &start, &end, perms, &off, dev, &inode, path);
        if (n < 6 || perms[2] != 'x') continue;      // executable segments only
        struct prof_module *m = &prof_mod[prof_nmod++];
        m->start = start; m->end = end; m->offset = off;
        // Skip the leading spaces sscanf's %[^\n] keeps.
        const char *p = path;
        while (*p == ' ') p++;
        snprintf(m->path, sizeof m->path, "%s", *p ? p : "[anon]");
    }
    fclose(f);
}

static const struct prof_module *prof_find_module(unsigned long pc)
{
    for (int i = 0; i < prof_nmod; i++)
        if (pc >= prof_mod[i].start && pc < prof_mod[i].end) return &prof_mod[i];
    return 0;
}

// ---- dump -----------------------------------------------------------------
// Enough PCs for tools/symbolize-prof.py to aggregate per function without the
// tail mattering; MISTER_PROFILE_TOP raises it.
#define PROF_TOP_DEFAULT 400

static int prof_cmp(const void *a, const void *b)
{
    unsigned long ca = ((const struct prof_slot*)a)->count;
    unsigned long cb = ((const struct prof_slot*)b)->count;
    return (cb > ca) - (cb < ca);
}

static void prof_dump(void)
{
    if (!prof_enabled) return;

    // Stop the timer first so the handler cannot mutate the table under us.
    struct itimerval off;
    memset(&off, 0, sizeof off);
    setitimer(ITIMER_PROF, &off, 0);
    prof_enabled = 0;

    prof_read_maps();

    unsigned long total = 0;
    for (unsigned int i = 0; i < PROF_SLOTS; i++) total += prof_table[i].count;

    fprintf(stderr, "MISTER-PROF samples=%lu dropped=%lu hz=%d modules=%d\n",
            total, prof_dropped, prof_hz, prof_nmod);
    if (!total) return;

    // Per-module totals: the CPU/video split inside the ELF needs symbols, but
    // "how much is libc/libasound" is answerable right here.
    for (int i = 0; i < prof_nmod; i++) {
        unsigned long mt = 0;
        for (unsigned int j = 0; j < PROF_SLOTS; j++) {
            if (!prof_table[j].count) continue;
            if (prof_table[j].pc >= prof_mod[i].start &&
                prof_table[j].pc <  prof_mod[i].end) mt += prof_table[j].count;
        }
        if (mt)
            fprintf(stderr, "MISTER-PROF-MOD %lu %5.2f%% %s\n",
                    mt, 100.0 * mt / total, prof_mod[i].path);
    }

    // Compact the live slots and sort them; the table is static, so this costs
    // one pass plus a qsort at exit.
    unsigned int live = 0;
    for (unsigned int i = 0; i < PROF_SLOTS; i++)
        if (prof_table[i].count) prof_table[live++] = prof_table[i];
    qsort(prof_table, live, sizeof prof_table[0], prof_cmp);

    const char *top_env = getenv("MISTER_PROFILE_TOP");
    unsigned int top = top_env ? (unsigned int)atoi(top_env) : PROF_TOP_DEFAULT;
    if (top > live) top = live;
    for (unsigned int i = 0; i < top; i++) {
        unsigned long pc = prof_table[i].pc;
        const struct prof_module *m = prof_find_module(pc);
        unsigned long fileoff = m ? (pc - m->start + m->offset) : pc;
        fprintf(stderr, "MISTER-PROF-PC %lu %5.2f%% 0x%08lx %s\n",
                prof_table[i].count, 100.0 * prof_table[i].count / total,
                fileoff, m ? m->path : "[unmapped]");
    }
    fflush(stderr);
}

// ---- entry points ---------------------------------------------------------
void mister_profile_init(void)
{
    const char *want = getenv("MISTER_PROFILE");
    if (!want || !*want || want[0] == '0') return;

    prof_hz = atoi(want);
    if (prof_hz <= 1)    prof_hz = 1000;      // "MISTER_PROFILE=1" = default rate
    if (prof_hz < 10)    prof_hz = 10;
    if (prof_hz > 10000) prof_hz = 10000;

    prof_table = (struct prof_slot*)calloc(PROF_SLOTS, sizeof(struct prof_slot));
    if (!prof_table) { fprintf(stderr, "mister_profile: table alloc failed\n"); return; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, 0) != 0) {
        fprintf(stderr, "mister_profile: sigaction failed\n");
        free(prof_table); prof_table = 0; return;
    }

    struct itimerval it;
    it.it_interval.tv_sec  = 0;
    it.it_interval.tv_usec = 1000000 / prof_hz;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, 0) != 0) {
        fprintf(stderr, "mister_profile: setitimer failed\n");
        free(prof_table); prof_table = 0; return;
    }

    prof_enabled = 1;
    atexit(prof_dump);          // the bench exit path is exit(0), so this fires
    fprintf(stderr, "mister_profile: sampling at %d Hz\n", prof_hz);
}

void mister_profile_dump(void) { prof_dump(); }
