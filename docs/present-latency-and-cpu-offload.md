# Present latency and CPU offload — what is left after staging and threading

**Date:** 2026-08-01
**Status:** research only. No code changed, no device run.
**Method:** static reading of `tools/mame-frontend/`, `fpga/rtl/` and
`tools/mister/`, plus arithmetic over measurements already recorded in
`docs/bench-results.md` and `docs/bench-results-2003plus.md`.
**Convention:** claims are tagged **[OBS]** (read from source or a logged
measurement), **[DER]** (arithmetic over those), or **[UNK]**.

---

## 0. Headline

Two fixes have already been applied to the present path and both worked: the
cached-staging conversion (`docs/bench-results.md`, 12–80 % on every 8bpp
driver) and the threaded present (`docs/bench-results-2003plus.md`, +9.7 % mean,
12 families converted).

**Neither deleted the uncached copy. They moved work around it.** The 4.19 ms
per 512×384 frame is still paid in full, every frame, on whichever core is
holding it.

| | |
|---|---|
| uncached DDR memcpy | **89.5 MB/s** → **4.19 ms** per 512×384 RGB565 frame **[OBS]** |
| same copy, cached→cached | 540.5 MB/s → 0.69 ms **[OBS]** |
| `galaga`, present vs no present | 149.0 → 191.3 fps = **22 % of frame time** **[OBS]** |
| threading returns | **+9.7 % mean**, not 22 % **[OBS]** |

**[DER]** Threading recovers less than half of what the present costs because it
*adds* a cached src→slot copy on the emulation thread (`host_present.c` rule 2 —
the source buffer cannot be handed over) while the worker still pays the whole
4.19 ms. The remaining lever is not to relocate that copy again but to make it
stop costing 89.5 MB/s.

**The single cheapest next action in this document is a ten-minute measurement**
(§2.2): point the existing `ddr-write-bench.c` at `/dev/fb0` instead of
`/dev/mem`. If that mapping is write-combined, 4.19 ms → ~0.7 ms is reachable
**with no kernel module, no DTS edit and no kernel rebuild** — a bigger win than
threading delivered, on every driver, without needing the second core.

---

## 1. Why the window is slow, and why `O_SYNC` made no difference

**[OBS]** `docs/bench-results.md:108` records that the mapping mode "makes no
difference (`O_SYNC` and not are within 3%)" and leaves it there.
`ddr-write-bench.c:74-78` shows the two arms differ only in the `O_SYNC` flag to
`open("/dev/mem", …)`.

**[OBS] There is a mechanism for that null, and it is not "the flag does
nothing".** ARM's `phys_mem_access_prot()` returns `pgprot_noncached` whenever
`pfn_valid(pfn)` is false, and only reaches the `O_SYNC` test — which would
return `pgprot_writecombine` — when it is true. `0x3A000000` sits inside the
512 MiB carved out of the DE10-Nano's 1 GiB for the fabric, outside the kernel's
memblock: Main_MiSTer bounds-checks every core-visible address against
`[0x20000000, 0x40000000)`.

**[DER]** So both arms of that A/B took the first branch and produced the *same*
mapping. The 3 % is run-to-run noise, exactly as predicted, and the O_SYNC
comparison carries no information about `O_SYNC`.

**[DER] The 89.5 MB/s figure itself is unaffected.** It is a direct timing of a
memcpy into the mapping, not a differential against the no-`O_SYNC` arm. It is
also cross-validated by outcomes: the staging fix it motivated moved `gng`
121.4→193.4 and `klax` 105.1→189.1, and `atarisy2` runs 44→154 fps with the
present disabled outright.

**[DER] And the null is itself the best available evidence that `pfn_valid()` is
false.** Had it been true, the no-`O_SYNC` arm would have been *cacheable* and
read near the 540.5 MB/s cached figure — not within 3 % of 89.5.

**[UNK]** `pgprot_noncached` on ARM is `L_PTE_MT_UNCACHED` (strongly-ordered),
not merely unbuffered, which is why no store form helps: hand-written NEON
measures *slower* than glibc memcpy (79.4 vs 89.5 MB/s), the signature of a
transaction-latency bound rather than an instruction bound. Not verified against
the MiSTer kernel tree, which is not checked out here. *Settles it on device in
30 s:* `cat /proc/iomem` (is `0x3A000000` inside a "System RAM" range?) and
`cat /proc/cmdline` (`mem=`).

---

## 2. The lever: stop paying 89.5 MB/s

### 2.1 What is ruled out, and why

**[OBS]** A userspace DMA engine (the HPS PL330) is the textbook answer and is
blocked the same way here as in the sibling project: `CONFIG_UIO is not set` in
the MiSTer kernel config, and dmaengine has no userspace API. A kernel module
plus a DTS change plus a kernel rebuild, off the stock MiSTer update path.

**[DER]** Reaching write-combining by changing the `mmap` call is likewise
impossible — per §1 there is no argument to `/dev/mem` that yields it for this
region.

### 2.2 What is NOT ruled out — and costs one measurement

**[OBS]** `mister_video.cpp:113-136` already opens and `mmap`s `/dev/fb0`,
behind `MISTER_FB=1`, as a debug blit target. MiSTer's `MiSTer_fb.c` framebuffer
lives in the same FPGA-side DDR, and fbdev drivers conventionally map
write-combined (`fb_pgprotect` on ARM).

**[UNK] Is `/dev/fb0`'s mapping write-combined?** If yes, a WC mapping of FPGA
DDR is reachable from userspace *today*, with none of §2.1's cost.

**The experiment, in full:** change `ddr-write-bench.c`'s `open`/`mmap` from
`/dev/mem` at `NV_BASE` to `/dev/fb0` at offset 0, keep every store form, run
it. One file, no rebuild of either engine, no device reconfiguration.

**[DER] What the answer is worth.** At 512×384: 4.19 ms → ~0.7 ms if WC lands
near the cached figure, i.e. **~3.5 ms/frame returned on every driver**, against
threading's +9.7 % mean. It also *composes* with threading rather than competing
— a worker that costs 0.7 ms instead of 4.19 ms is far less likely to saturate
and drop.

### 2.3 The structural form of the same fix

**[OBS]** This project already presents through the framework scaler
(`arcade_video` / `video_freak` / `ascal`, README) but hand-rolls
`openbor_video_reader.sv` against a private `0x3A000000` contract, while
`MISTER_FB` — the framework's native version of exactly this — supports RGB565,
does scaling *and* format conversion in hardware, and comes with
`FBIO_WAITFORVSYNC`. `fpga/MAME.sv:64` carries the `MISTER_FB` conditional but
this core does not define it.

**[DER]** The sibling project deferred the same decision but had a reason to keep
its bespoke reader — its fabric compositor needs that reader regardless. **This
project has no equivalent reason.** Adopting `MISTER_FB` would plausibly deliver
§2.2's mapping, a real vsync signal (§3) and the retirement of a custom RTL
module in one move.

**[UNK]** Whether `MISTER_FB`'s geometry handling covers this project's range of
native driver geometries (up to 512×512) and the deferred rotation story.
Not costed here.

---

## 3. Latency is unmeasured, and the pacing is on the wrong clock

**[OBS]** Nothing in `tools/mame-frontend/` reads a scanout counter or a vsync.
The loop is throttled on the **audio** clock — `host_main.c:219-226` states
"EXACTLY ONE CLOCK", and in a throttled run the blocking `snd_pcm_writei` is it.

**[OBS]** The reader adopts a new buffer once per frame:
`openbor_video_reader.sv:706-720`, `ST_CHECK_CTRL` compares
`ctrl_word[31:2]` against `prev_frame_counter` and latches `active_buffer` /
`buf_base_addr` on a change.

**[DER]** So the present lands at an arbitrary and drifting phase against the
reader's adoption point, and a frame published just after it waits a full
display period to be seen: **0–16.7 ms, mean ~8.3 ms**, on top of ascal's own
buffering. No instrument reads this.

**[UNK] Threading adds an unmeasured handoff.** The emulation thread queues a
job and returns; the worker publishes later (`host_present.c`, the 1-deep
queue). That is up to a frame of added latency, never measured — and it lands
specifically on the 12 marginal families threading exists to convert, which are
the ones most worth keeping responsive.

*What would answer §3:* a timestamp pair around the publish plus a read of the
reader's frame counter, histogrammed. If the phase clusters badly, the fix is a
host-side nudge, not RTL.

---

## 4. Two smaller items

**[OBS] The staging copy is full-view, not full-source.** `nv_present.c:304`
does `memcpy(dst, nv_stage, nv_pitch * nv_view_h * 2)` unconditionally, so a
driver whose native geometry is smaller than the view copies the zero padding
`nv_set_mode()` already wrote, every frame, at 89.5 MB/s. Bounded by
`(nv_view_h − ch) / nv_view_h` of the copy — worth checking against the actual
geometry table before acting.

**[OBS] `nv_frame_repeat()` exists but no content-unchanged caller was found.**
It republishes without copying (`nv_present.c:330-338`). A cheap
frame-hash compare would skip the whole 4.19 ms on static frames. Note the
project already hashes DDR content for a different reason — to tell a driver
that is running from one merely advancing its frame counter (`nv_present.c:316`
and the `klax` note) — so the primitive is present.

---

## 5. Recommended order

| # | Item | Cost | Expected |
|---|---|---|---|
| 1 | Benchmark `/dev/fb0`'s mapping mode (§2.2) | one file, ~10 min | decides 2–3 |
| 2 | If WC: move the present onto it | host only | **~3.5 ms/frame, every driver** |
| 3 | If WC: adopt `MISTER_FB` properly (§2.3) | RTL + host | 2, plus vsync, minus a module |
| 4 | Instrument publish→adoption phase and the threaded handoff (§3) | host probe | unknown; first latency number |
| 5 | `nv_frame_repeat()` on unchanged content (§4) | host only | skips static frames entirely |
| 6 | Trim the full-view staging copy (§4) | host only | geometry-dependent |

Item 1 gates the two largest items and costs almost nothing. Item 4 is the only
one addressing latency rather than throughput, and nothing measures it today.

---

## 6. Unknown

1. Whether `/dev/fb0`'s `mmap` is write-combined (§2.2). **Gates everything.**
2. `pfn_valid()` for `0x3A000000`, confirmed on device rather than inferred (§1).
3. Publish→adoption phase distribution, and the threaded handoff cost (§3).
4. Whether `MISTER_FB` covers this project's geometry range and rotation (§2.3).
5. Whether any driver's native geometry is small enough for §4's padding waste
   to matter.

## Sources

- `docs/bench-results.md:87-135` — the present-path bottleneck, the store-form
  table, the staging fix and its per-driver effect.
- `docs/bench-results-2003plus.md:278-300` — the threading A/B, zero drops, the
  affinity finding; `:313-352` — re-judged coverage and the twelve converts;
  `:358-368` — the idle-core option, recorded as costed and not taken.
- `tools/mister/ddr-write-bench.c:74-78` — the two `open` arms.
- `tools/mame-frontend/mister-backend/nv_present.c:1-15` — the two measured
  behaviours; `:152` — the `O_SYNC` map; `:260-274` — RGB565 direct-write
  carve-out; `:304` — the full-view staging copy; `:309-311` — barrier then
  doorbell; `:330-338` — `nv_frame_repeat`.
- `tools/mame-frontend/mister-backend/mister_video.cpp:113-136` — the existing
  `/dev/fb0` open + `mmap`.
- `tools/mame-frontend/libretro-host/host_present.c:1-52` — the threaded shape
  and its three rules; `host_main.c:219-226` — one clock, and it is audio.
- `fpga/rtl/openbor_video_reader.sv:706-720` — once-per-frame buffer adoption.
- `fpga/MAME.sv:64` — the unused `MISTER_FB` conditional.
