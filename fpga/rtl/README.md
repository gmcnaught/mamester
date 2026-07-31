# fpga/rtl/ — canonical RTL (planned)

**Status: stub — no RTL yet.** See `../README.md` for the module plan.

The scanout chain, once built:

```
HPS writes RGB565 double-buffer + per-game timing params to DDR
      │
      ▼
framebuffer_reader.sv  ── DDR double-buffer read, control-word doorbell,
      │                    stale-frame watchdog (freeze last good frame)
      ▼
video_timing.sv        ── REGISTER-PROGRAMMABLE per-game native timing
      │                    (H/V total, active, CE_PIXEL divider, refresh)
      ▼
scan_adapter.sv ──► arcade_video / video_mixer ──► video_freak ──► ascal ──► HDMI/analog
                    (scanlines, shadow-mask, gamma, aspect — all framework)
```

Reference implementations to adapt live in the sibling repos' `fpga/rtl/`:
`maldita.castilla-mister` and `solarus-mister` (`openbor_video_reader.sv`,
`openbor_video_timing.sv`, `ddr3_scan_adapter.sv`). The one genuinely new piece is
making the timing generator register-driven so each MAME driver presents at its
native resolution.
