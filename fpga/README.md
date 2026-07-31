# fpga/ — the passthrough scanout core (planned)

**Status: stub.** No RTL yet. This directory will hold the MiSTer core that scans
the ARM-written DDR framebuffer into the **standard MiSTer video pipeline**.

## What this core does (and does not)

- **Does:** read the RGB565 double-buffer the HPS writes to DDR, generate per-game
  native video timing from HPS-supplied parameters, and feed
  `arcade_video`/`video_mixer` → `video_freak` → `ascal`. Scaling, aspect (incl.
  tate), scanlines, shadow-mask, and gamma are then the framework's job.
- **Does NOT:** emulate any arcade hardware, composite, rasterize, or run game
  logic. All of that is software on the HPS (see `../tools/mame-frontend/`). This is
  pure scanout — the `sonic-mania-mister` model, wired through the scaler rather
  than bypassing it.

## Modules to build (adapt from siblings — do not copy blindly)

| Planned file | Adapt from | Change needed |
|---|---|---|
| `MAME.sv` (`emu` top) | `solarus-mister/fpga/Solarus.sv`, `maldita.castilla-mister/fpga/Maldita.sv` | strip the blitter/compositor; keep the DDR reader + standard video pipeline wiring |
| `rtl/framebuffer_reader.sv` | `maldita`/`solarus` `openbor_video_reader.sv` | keep DDR double-buffer + control-word doorbell + stale-frame watchdog; feed the mixer, not a custom DAC path |
| `rtl/video_timing.sv` | `openbor_video_timing.sv` (fixed) | **make register-programmable** — per-game H/V total, active W/H, `CE_PIXEL` divider, refresh, set by the HPS at game launch |
| `rtl/scan_adapter.sv` | `solarus-mister/fpga/rtl/ddr3_scan_adapter.sv` | the "feed the standard pipeline" pattern (PR #138) |
| `rtl/ddram.sv`, `pll/` | either sibling | mostly reusable |
| `sys/` | MiSTer `Template_MiSTer` / sibling `sys/` | keep read-only, update from upstream |

## Build

Quartus **17.0 Lite only** (newer Quartus is incompatible with MiSTer cores).
Build script (`build_mame.sh`) and Quartus project (`MAME.qpf/.qsf/.sdc`) TBD —
model on `solarus-mister/fpga/build_solarus.sh`.

**Do not start here.** Per `../docs/feasibility.md` §6, the first step is empirical
CPU validation on real hardware; the present-path RTL only matters once a set of
drivers is confirmed to run at speed.
