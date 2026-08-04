# `mem_wc` — a write-combining `/dev/mem` for the present path

Vendored from [skmp/minicast](https://github.com/skmp/minicast/tree/main/mem_wc)
(GPL-2.0), where it gives DreamSTer's Dreamcast VRAM window a write-combining
mapping. Full analysis of why this project wants the same thing, and what of
DreamSTer does *not* transfer, is in
[`docs/dreamster-ddr-channel-review.md`](../../../docs/dreamster-ddr-channel-review.md).

## What problem it solves

The present path writes one frame-sized `memcpy` into the DDR window at
`0x3A000000` per frame. That window measures **89.5 MB/s — 4.19 ms for a
512×384 RGB565 frame**, against **540.5 MB/s (0.69 ms)** for the same copy into
cached RAM. On `galaga` the present is 22 % of frame time.

The cause is the memory type, not the bus. ARM's `phys_mem_access_prot()`:

```c
if (!pfn_valid(pfn))             return pgprot_noncached(vma_prot);   /* Strongly-Ordered */
else if (file->f_flags & O_SYNC) return pgprot_writecombine(vma_prot);
return vma_prot;                                                       /* cacheable */
```

`0x3A000000` is in the 512 MB handed to the fabric, outside the kernel's
memblock, so `pfn_valid()` is false and **every** `/dev/mem` mapping of it takes
the first branch — which is why the `O_SYNC` A/B in `docs/bench-results.md`
measured a null. Strongly-Ordered (`L_PTE_MT_UNCACHED`) stores cannot merge, so
each one is a separate bus transaction; the path is transaction-latency bound,
which is also why hand-written NEON measured *slower* than glibc `memcpy`
(79.4 vs 89.5 MB/s).

This driver never asks `pfn_valid()`. It sets `pgprot_writecombine()`
(`L_PTE_MT_BUFFERABLE`, Normal Non-Cacheable) unconditionally and calls
`remap_pfn_range()`. Stores can then merge into full bursts.

## Is a WC mapping of that region safe?

The worry is a **mismatched alias** — a WC mapping of physical memory the kernel
also has in its cacheable linear map is architecturally unpredictable on ARMv7.
That worry and the need for this module are mutually exclusive:

- If `pfn_valid()` is **false**, the page is outside memblock, has no linear-map
  alias, and WC is safe.
- If it were **true**, `/dev/mem` + `O_SYNC` would already be write-combined and
  this module would be unnecessary.

Confirm on device rather than by argument — `cat /proc/iomem` and check that
`0x3A000000` is *not* inside a `System RAM` range.

The host side avoids creating an alias of its own: `nv_present.c` overlays WC
only on the pixel-buffer pages and leaves the control word, joystick words and
timing registers on the original Strongly-Ordered mapping, with no physical page
mapped both ways.

## Build

Needs the MiSTer kernel source prepared for out-of-tree modules, built with the
same toolchain the kernel used (`arm-none-linux-gnueabihf-gcc 10.2.1` for
5.15.1-MiSTer — check `/proc/version` on your device).

```sh
# 1. Matching kernel source
git clone https://github.com/MiSTer-devel/Linux-Kernel_MiSTer
cd Linux-Kernel_MiSTer
# check out the commit that produces the device's kernel version

# 2. Prepare it with the DEVICE'S OWN config so vermagic matches
scp root@<device>:/proc/config.gz . && zcat config.gz > .config
export ARCH=arm
export CROSS_COMPILE=/opt/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
make olddefconfig
make modules_prepare

# 3. Build
cd /path/to/mamester/tools/mister/mem_wc
make KDIR=/path/to/Linux-Kernel_MiSTer
```

Produces `mem_wc.ko`. The running kernel has `CONFIG_MODVERSIONS` and
`CONFIG_MODULE_SIG` off, so there is no symbol-CRC matching and no signing —
**only the vermagic string has to match**, which building against the same
source + `.config` guarantees.

## Install

`deploy.py` pushes `mem_wc.ko` to `/media/fat/games/mame/mem_wc.ko` when it has
been built (it is optional — deploy warns and continues if absent), and
`games/MAMESTer/_handler.sh` loads it at core launch, restricted to this core's
own window:

```sh
insmod /media/fat/games/mame/mem_wc.ko phys_base=0x3A000000 phys_size=0x00400000
```

Manually:

```sh
insmod mem_wc.ko phys_base=0x3A000000 phys_size=0x00400000
dmesg | tail -1        # "mem_wc: loaded, restricted to [0x3a000000, 0x3a400000)"
ls -l /dev/mem_wc
```

## It is optional, on purpose

`nv_present.c` tries `/dev/mem_wc` and falls back to `/dev/mem` (the pattern is
minicast's, `_vmem.cpp:22-38`). A MiSTer kernel update invalidates the module's
vermagic and `insmod` starts failing on users' machines — that must cost frame
rate, not boot. `nv_open()` logs which mapping it got:

```
nv_present: pixel buffers write-combined via /dev/mem_wc
nv_present: pixel buffers strongly-ordered (/dev/mem_wc unavailable) — expect ~4.2 ms/frame at 512x384
```

Set `MISTER_NO_WC=1` to force the fallback path without unloading the module —
that is the A/B.

## Verifying it actually worked

`tools/mister/ddr-write-bench.c` has a `/dev/mem_wc` arm. Beyond the MB/s
figure it checks one thing that cannot be faked: **under a real WC mapping NEON
128-bit stores must overtake glibc `memcpy`**, inverting the Strongly-Ordered
result, because the transaction-latency bound is gone. If they do not, the
mapping did not change, whatever the throughput number says.
