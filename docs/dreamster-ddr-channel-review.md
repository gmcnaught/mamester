# DreamSTer's DDR channel — review, and what of it applies here

**Date:** 2026-08-04
**Status:** research only. No code changed, no device run.
**Prompted by:** [issue #4](https://github.com/gmcnaught/mamester/issues/4) —
benchmark `/dev/mem_wc` write performance on MiSTer.
**Method:** static reading of `skmp/DreamSTer` and its two relevant submodules,
`skmp/minicast` (ARM app, kernel module) and `skmp/polly2-rtl` (fabric), against
this project's `nv_present.c` / `openbor_video_reader.sv` and
`docs/present-latency-and-cpu-offload.md`.
**Convention:** claims are tagged **[OBS]** (read from source), **[DER]**
(derived), or **[UNK]**.

---

## 0. Headline

"DreamSTer's custom DDR channel" is two separate things, and only one of them is
transferable.

| | What it is | Applies here? |
|---|---|---|
| **CPU side** | `mem_wc.ko` — a 120-line char driver giving a **write-combining** mmap of arbitrary physical DDR, because `/dev/mem` on ARM cannot | **Yes, directly.** Drop-in for `nv_present.c`'s mmap |
| **Fabric side** | Three private f2sdram masters in a from-scratch `sys_top` with no MiSTer framework at all | **No.** Structurally incompatible with this project's stated constraint |

**[DER]** The interesting finding is not that `mem_wc` exists — it is *why*
DreamSTer needed it, which is different from why we would. DreamSTer has **no
frame copy at all**: it maps FPGA-visible DDR straight into the emulated SH4's
address space, so guest VRAM writes land in DDR as they happen. That only works
if the CPU never reads back what it wrote — and it doesn't, because the
rasterizer is in the fabric. **We cannot copy that model** (§4), but we can copy
the mapping, and the mapping alone is the ~3.5 ms/frame in issue #4.

**[OBS]** `mem_wc` is also the *only* one of the three transports in issue #4
that is a drop-in. `/dev/fb0` answers the "is WC reachable?" question but cannot
be *used* without also adopting `MISTER_FB` (§5.2).

---

## 1. `mem_wc.ko` — what it actually does

**[OBS]** `minicast/mem_wc/mem_wc.c`, 123 lines including licence header. The
whole driver is one `.mmap` handler on a `miscdevice`:

```c
static int mem_wc_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	phys_addr_t offset = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;

	if (offset + size < offset)                      /* wrap */
		return -EINVAL;
	if (phys_size) {                                 /* optional allowlist */
		if (offset < phys_base ||
		    offset + size > (phys_addr_t)phys_base + phys_size)
			return -EPERM;
	}

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			    size, vma->vm_page_prot))
		return -EAGAIN;
	return 0;
}
```

That is the entire mechanism. Userspace usage is byte-identical to `/dev/mem` —
`open("/dev/mem_wc")`, `mmap(…, fd, phys_base)` — the physical base comes from
`vm_pgoff`, so nothing is hardcoded.

**[OBS]** Two module params, `phys_base` / `phys_size`, optionally restrict it to
a single window (`insmod mem_wc.ko phys_base=0x32000000 phys_size=0x00800000`).
Default is unrestricted. Node mode is `0600`.

**[OBS]** `mem_wc.mod.c` is committed alongside the source, and the README
documents the exact build recipe: MiSTer 5.15.1 kernel source, Arm 10.2-2020.11
toolchain, device's own `/proc/config.gz`, `make modules_prepare`. It records
that the running kernel has `CONFIG_MODVERSIONS` and `CONFIG_MODULE_SIG` off, so
**only the vermagic string has to match** — no CRC matching, no signing.

**[OBS]** The README's first line is "### What follows is ai slop." Take the prose
with that in mind; the code is small enough to read directly and it is correct.

### 1.1 Why this is the right mechanism, in our terms

**[OBS]** `§1` of `present-latency-and-cpu-offload.md` already worked out ARM's
`phys_mem_access_prot()`:

```c
if (!pfn_valid(pfn))            return pgprot_noncached(vma_prot);   /* strongly-ordered */
else if (file->f_flags & O_SYNC) return pgprot_writecombine(vma_prot);
return vma_prot;                                                     /* cacheable */
```

**[DER]** `mem_wc` sidesteps that function entirely — it never asks whether the
pfn is valid, it just sets `pgprot_writecombine` unconditionally and calls
`remap_pfn_range`. On ARM32 that is `L_PTE_MT_BUFFERABLE`, Normal Non-Cacheable,
which permits store merging in the write buffer; `pgprot_noncached` is
`L_PTE_MT_UNCACHED`, Strongly-Ordered, which does not. That difference *is* the
89.5 → ~540 MB/s gap.

### 1.2 The safety argument, which is stronger than it first looks

**[DER]** The obvious worry with a WC mapping of DDR is a **mismatched alias**: if
the kernel also has that physical memory in its cacheable linear map, two
mappings of different memory type to the same physical address is
architecturally unpredictable on ARMv7.

**[DER] That worry and the need for `mem_wc` are mutually exclusive**, which
settles it:

- If `pfn_valid(0x3A000000 >> 12)` is **false** — which `§1` of the present-path
  doc derives from the null `O_SYNC` A/B — the page is outside memblock, has **no
  kernel linear-map alias**, and a WC mapping is safe.
- If it were **true**, then `/dev/mem` **with `O_SYNC` would already be
  write-combined** (branch 2 above), the A/B would not have been a null, and we
  would not need the module at all.

So either the module is unnecessary, or it is safe. **[UNK]** The one gap: on
SPARSEMEM, `pfn_valid()` can be true for a hole inside a populated section. Worth
one `cat /proc/iomem` on device (is `0x3A000000` inside a "System RAM" range?)
rather than an argument.

---

## 2. How minicast uses it — the fallback pattern is worth stealing verbatim

**[OBS]** `minicast/libswirl/hw/mem/_vmem.cpp:22-38`:

```c
static int opem_dev_mem() {
	int fd = open("/dev/mem_wc", (O_RDWR | O_SYNC));
	if (fd != -1) return fd;
	printf("ERROR: could not open \"/dev/mem_wc\", trying \"/dev/mem\"...\n\n");
	return open("/dev/mem", (O_RDWR | O_SYNC));
}
```

**[DER]** Try WC, fall back to the strongly-ordered mapping, keep running either
way. This is exactly the shape we want: the module becomes a *performance*
dependency, not a *functional* one, so a MiSTer kernel bump that invalidates the
`.ko` costs frame rate, not boot. `mister_support.cpp:183-184` repeats the same
two-line fallback for the stats-OSD band FB.

**[OBS] Two things in minicast not to copy.** `mmap_fpga_region()` passes
`MAP_SYNC` (`_vmem.cpp:57`) — that is a DAX flag, ignored without
`MAP_SHARED_VALIDATE`, and does nothing here. And `O_SYNC` on `/dev/mem_wc` is
inert: `mem_wc_mmap()` never inspects `f_flags`.

---

## 3. The fabric side — and why it does not transfer

**[OBS]** `polly2-rtl/polly2-de10nano/rtl/sys_top.v:5` — *"Written from scratch -
NO MiSTer framework components."* DreamSTer's DDR channel in the fabric is three
private Avalon masters on a `sysmem_lite` instance:

| Port | Width | Direction | Owner |
|---|---|---|---|
| `ram1` | 64-bit | read-only | peel_core render/geometry reads |
| `ram2` | 64-bit | write-only | peel_core framebuffer writes |
| `vbuf` | 128-bit | read-only | SPG display scanout |

**[OBS]** The scanout (`spg.sv`) is a 128-bit Avalon read master doing 16-beat
(256-byte) bursts into a two-line ping-pong buffer, one source line prefetched
while the other is displayed, with ~29.7 µs of slack per line. It drives a fixed
1080p CEA-861 raster straight at the ADV7513: no ascal, no OSD, no scaler, no
analog output. `sys_top.v:24-26` — the ARM owns bitstream load, reset release and
ADV7513 init.

**[OBS]** The DreamSTer README is explicit about the cost: *"it kills the MiSTer
process, takes over the fpga, launches minicast and re-loads menu.rbf /
re-starts MiSTer once done"*, and *"is not a MiSTer core."*

**[DER]** That is a direct trade of the framework for control of the whole
pipeline, and it is the opposite of this project's stated constraint (README:
"Reuse the MiSTer framework scaler/shaders", an explicit project constraint).
`openbor_video_reader.sv` feeding `arcade_video`/`ascal` is the right shape for
us and nothing in polly2-rtl argues otherwise. **The fabric half of DreamSTer's
DDR channel is not a model to adopt; it is a different project's answer to a
different constraint.**

The one detail worth noting for later: their scanout burst length is 16×128-bit
= 256 B, serialized (a new command only after every beat of the previous
arrives), deliberately conservative about real-DDR3 read-beat desync on hardware.
If our reader ever gets a burst-length tuning pass, that is a calibrated data
point from the same silicon.

---

## 4. The deeper difference: they have no frame copy

**[OBS]** `_vmem.cpp:485-500`. The 16 MB FPGA DDR window at `0x32000000` is mapped
**into the guest address space** at `0x04000000` (SH4 Area 1, VRAM) and its
mirror at `0x06000000`, and `vram->data` is set to point inside the fastmem
region. Guest VRAM writes go straight to FPGA-visible DDR through fastmem — there
is no staging buffer, no per-frame `memcpy`, no doorbell for pixels.

**[DER] This works only because the PowerVR rasterizer is in the fabric.** The
ARM writes texture and framebuffer bytes; it does not read them back to composite.
WC memory has *fast merged writes and slow uncached reads*, so a
write-mostly access pattern is the one case where mapping the guest's memory
directly at DDR is a win rather than a disaster.

**[DER] MAME is the opposite.** A driver's rasterizer does read-modify-write on
its own bitmap all frame long — sprite blending, priority buffers, per-scanline
compositing. Pointing MAME's `osd_bitmap` at a WC DDR window would replace one
linear 384 KB streaming write with hundreds of thousands of scattered uncached
reads, and would be dramatically **slower** than what we do today.

**So the correct import is narrow and specific:** keep the cached staging buffer
and the single linear `memcpy` exactly as they are, and change *only the
destination mapping's memory type*. That is the whole of the applicable lesson,
and it is worth ~3.5 ms/frame.

---

## 5. What this means for issue #4

### 5.1 Three concrete code changes, not one

Adopting a WC mapping is **not** a one-line `open()` swap in `nv_present.c`.
Write-combining removes the implicit ordering that Strongly-Ordered gave us for
free, and three places currently depend on it:

**(a) `__sync_synchronize()` is the wrong barrier.** `nv_present.c:309` fences the
frame against the doorbell with `__sync_synchronize()`, which GCC lowers to
`dmb ish` on ARMv7 — the *inner-shareable* domain. The FPGA reaches DDR through
the f2h SDRAM ports, outside that domain. Today this is harmless because the
memory type itself orders the stores; under WC it is load-bearing and
insufficient. It must become what `mem_wc`'s README specifies:

```c
__asm__ volatile("dsb sy" ::: "memory");
```

**(b) The doorbell shares the mapping.** `nv_present.c:160` puts `nv_ctrl` at
offset 0 of the *same* single `mmap` as the pixel buffers, so a WC mapping makes
the control-word store write-combined too — it can sit in the write buffer with
nothing behind it to force a drain, and the reader's `ST_CHECK_CTRL` polls a
stale counter. Two fixes, either works: a second `dsb sy` *after* the doorbell
store, or — cleaner, and what I would do — **split the mapping**: keep page 0
(the control word) on the existing strongly-ordered `/dev/mem` mapping and put
only the two pixel buffers on `/dev/mem_wc`. The doorbell is one 32-bit store per
frame; its transaction cost is irrelevant and its ordering is not.

**(c) `nv_frame_repeat()` documents that it needs no barrier.**
`nv_present.c:334-336` — *"No barrier and no pixel traffic: the buffer's contents
were already published and fenced."* True under SO, false under WC: the counter
store itself is now posted. It needs a trailing `dsb sy` (or fix (b) makes it
moot, since the doorbell would no longer be WC).

### 5.2 `/dev/fb0` and `/dev/mem_wc` are not comparable transports

**[DER]** Issue #4 benches them side by side, which is right for the *question*
"is WC reachable?" but hides an asymmetry in what you can do with the answer:

- **`/dev/mem_wc`** maps *our* window at `0x3A000000`. A win here is directly
  bankable — the reader RTL, the buffer layout and the doorbell contract all
  stay as they are.
- **`/dev/fb0`** maps only the fbdev driver's own smem region. Even if it
  measures 540 MB/s, we **cannot write our framebuffer through it** without
  moving the whole present path onto `MISTER_FB` (item 3 of the present-path
  doc's §5 — RTL plus host, and `MISTER_FB` geometry coverage for our up-to-512×512
  range is still **[UNK]**). It is a *probe*, not a transport.

Both are still worth measuring — `/dev/fb0` reading fast is independent evidence
that WC works on this silicon and this kernel, and it costs one file edit — but
the bench should report them as "probe" and "candidate transport", not as two
options on equal footing.

### 5.3 The shipping cost is real and is ours, not DreamSTer's

**[DER]** DreamSTer can require a kernel module cheaply: it already kills
`Main_MiSTer`, takes the FPGA, and runs from the Scripts menu. We launch as a
conventional core through Master_Daemon and `_handler.sh`, so an out-of-tree
`.ko` is a heavier commitment for us than for them:

- it must be rebuilt against each MiSTer kernel (vermagic-exact),
- it must be `insmod`ed from `_handler.sh` before MAME starts, as root,
- and it silently stops loading after a MiSTer update, on users' machines.

**The mitigation is minicast's own fallback (§2)**: `open("/dev/mem_wc")` →
`open("/dev/mem")`. With that in place a stale module costs the 3.5 ms and
nothing else, which makes the whole thing a safe bet rather than a support
burden. **[DER]** It also means the ordering fixes in §5.1 must be
unconditional — correct under *both* mappings — not gated on which `open()`
succeeded. `dsb sy` on the SO path is a few cycles a frame; do not branch on it.

### 5.4 Suggested amendment to the issue's bench

Keep the frame size and the four store forms. Change:

1. **Add a fourth arm**: `/dev/mem_wc` with the region restricted via the
   `phys_base`/`phys_size` module params to `[0x3A000000, +4 MB)` — that is the
   form we would actually ship, and it verifies the allowlist path at the same
   time.
2. **Report NEON separately and expect the ranking to invert.** `docs/bench-results.md`
   records hand-written NEON measuring *slower* than glibc `memcpy` under the SO
   mapping (79.4 vs 89.5 MB/s) — the signature of a transaction-latency bound.
   Under WC that bound is gone and 128-bit stores should merge into full bursts.
   **If NEON does not overtake `memcpy` under `/dev/mem_wc`, the mapping did not
   change** — that is a free built-in sanity check on the result, worth more than
   the MB/s number alone.
3. **Time the doorbell too.** A separate micro-arm that stores the control word
   and `dsb sy`s, timed on its own, sizes whether §5.1(b)'s split mapping is
   necessary or merely tidy.

---

## 6. Recommended order (amends `present-latency-and-cpu-offload.md` §5)

| # | Item | Cost | Expected |
|---|---|---|---|
| 1 | Bench `/dev/mem_wc` (restricted) alongside `/dev/mem` and `/dev/fb0`, with the NEON-inversion check | one file, ~20 min + module build | decides everything below |
| 2 | `cat /proc/iomem` / `/proc/cmdline` on device | 30 s | closes §1.2's alias **[UNK]** and the doc's §6.2 |
| 3 | If WC lands: the three ordering fixes (§5.1), unconditional | host only, small | correctness *precondition* for 4 |
| 4 | Move the pixel buffers to `/dev/mem_wc`, doorbell stays SO, with `/dev/mem` fallback | host only | **~3.5 ms/frame, every driver** |
| 5 | `insmod` from `_handler.sh` + build recipe pinned in `tools/` | harness | makes 4 shippable |

**[DER]** Item 3 before item 4, not with it. The ordering bugs are silent and
intermittent under WC — a torn frame every few thousand presents — and they are
exactly the class of bug that gets misattributed to the reader RTL.

### 6.1 What landed

Items 1 and 3–5 are implemented; **nothing here has been run on hardware**, so
item 2 and every number in §7 are still open.

| Where | What |
|---|---|
| `tools/mister/mem_wc/` | the module, vendored from minicast (GPL-2.0), its Makefile and a build/install README. **Not compiled** — that needs a prepared MiSTer kernel tree, which this checkout does not have |
| `nv_present.c` | the page-exact WC overlay (below), `NV_FENCE()` = `dsb sy` at all three doorbell sites, `MISTER_NO_WC=1` to force the fallback |
| `nv_present.h` | `nv_is_write_combined()`, so a bench run can say which mapping it measured |
| `ddr-write-bench.c` | a `/dev/mem_wc` arm, `/dev/fb0` relabelled as a probe, a doorbell micro-arm, and the NEON-inversion verdict |
| `_handler.sh` | `insmod … phys_base=0x3A000000 phys_size=0x00400000`, unconditional-failure-tolerant |
| `deploy.py` | ships `mem_wc.ko` when built; warns and continues when not |
| `mister_video.cpp`, `host_main.c` | `present=write-combined\|strongly-ordered` on the bench lines |

**The mapping is split page-exactly**, which §5.1(b) argued for and which turns
out to be forced twice over:

```
0x000000..0x001000   control word + joystick words   strongly-ordered
0x001000..0x300000   BUF0 / BUF1                     write-combining
0x300000..0x400000   timing registers                strongly-ordered
```

The `MAP_FIXED` overlay *replaces* those pages rather than adding a second
mapping, so — beyond keeping the doorbell strongly-ordered — no physical page is
ever mapped at two memory types at once, which would be the same mismatched
alias §1.2 rules out against the kernel.

Two details the implementation had to deal with that the analysis did not
anticipate:

- **`BUF0` begins 0x40 into page 0**, so its first 4032 bytes stay
  strongly-ordered. That is ~4 lines of a 512-wide frame, ~45 µs against the
  ~700 µs the rest of the copy should cost under WC — ~1 %, in exchange for the
  window staying one linear pointer instead of `nv_frame()` growing a
  straddling special case.
- **`MAP_FIXED` unmaps its target range before the driver's `.mmap` runs**, so a
  mapping the driver then rejects — `mem_wc` loaded with an allowlist that does
  not cover our window returns `-EPERM` — would leave a *hole* mid-window rather
  than the SO mapping we started from, and the next present would take SIGSEGV.
  `nv_map_wc()` therefore probes at a scratch address first, unmaps, and only
  then overlays.

**[DER]** The barriers are unconditional rather than gated on which `open()`
succeeded, per §5.3. Cross-compiled for armhf, `dsb sy` appears exactly three
times, in `nv_frame`, `nv_frame_repeat` and `nv_set_mode` — which is the
invariant to check if this is ever refactored: *every doorbell store in
`nv_present.c` is preceded by `NV_FENCE()`*.

---

## 7. Unknowns

1. Does `0x3A000000` sit in a "System RAM" range (§1.2)? Gates the safety
   argument, though both branches are benign.
2. Actual WC throughput at 512×384 on this kernel — the ~540 MB/s target is the
   *cached* figure and WC will land below it; unknown how far.
3. Whether MiSTer's kernel exposes `/proc/config.gz` and whether the 5.15.1
   vermagic recipe in minicast's README still matches current MiSTer releases.
4. `MISTER_FB` geometry coverage (unchanged from the present-path doc's §6.4) —
   only relevant if we pursue `/dev/fb0` as a transport rather than a probe.

## Sources

**DreamSTer / minicast / polly2-rtl** (read at `main`, 2026-08-04):
- `minicast/mem_wc/mem_wc.c:51-78` — the whole driver; `:44-49` — the allowlist
  params; `mem_wc/README.md:11-21` — the `dsb sy` ordering caveat; `:32-61` — the
  build recipe and the MODVERSIONS/MODULE_SIG note.
- `minicast/libswirl/hw/mem/_vmem.cpp:22-38` — the WC→`/dev/mem` fallback;
  `:15-17` — the `0x32000000` / 16 MB window; `:57` — the inert `MAP_SYNC`;
  `:485-500` — FPGA DDR mapped into SH4 Area 1 via fastmem, i.e. no frame copy.
- `minicast/libswirl/rend/mister_rend/mister_support.cpp:183-200` — the same
  fallback for the OSD band FB, and `dsb sy` before enabling the band.
- `polly2-rtl/polly2-de10nano/rtl/sys_top.v:5` — "NO MiSTer framework
  components"; `:202-272` — the three f2sdram masters and their roles.
- `polly2-rtl/polly2-de10nano/rtl/spg.sv:46-60` — the 128-bit scanout master,
  16-beat bursts, two-line ping-pong, serialized commands.
- `DreamSTer/README.md` — kills `Main_MiSTer`, takes the FPGA, "is not a MiSTer
  core".

**This project:**
- `docs/present-latency-and-cpu-offload.md:§1` — `phys_mem_access_prot()` and the
  null `O_SYNC` A/B; `§2.1-2.2` — what is ruled out and the `/dev/fb0` probe;
  `§5` — the order this section amends.
- `docs/bench-results.md:87-135` — 89.5 MB/s, and NEON measuring slower than
  `memcpy` under the SO mapping.
- `tools/mame-frontend/mister-backend/nv_present.c:152-160` — the single
  `O_SYNC` mmap with the control word at offset 0; `:309-311` —
  `__sync_synchronize()` then doorbell; `:334-338` — `nv_frame_repeat()`'s
  no-barrier note.
- `tools/mister/ddr-write-bench.c` — the bench issue #4 extends.
