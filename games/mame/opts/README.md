# Per-game launch options

`game_manager.sh` appends the contents of `<setname>.opt` to MAME's command line
when it launches that romset, falling back to `default.opt` when there is no
per-game file. One flag per line; `#` starts a comment.

The flags that matter on this port:

| Flag | Effect |
|---|---|
| `-norotate` | Present a portrait driver as its real cabinet signal (landscape, 4:3). `1943` publishes 224×256 @ 16.32 kHz 3:4 by default, and 256×224 @ 15.72 kHz 4:3 with this. |
| `-ror` / `-rol` | Rotate right / left, for a monitor that is physically turned. |
| `-frameskip N` | Fixed frameskip (`auto` in `mame.cfg` is the default). |
| `-nosound` | Sound costs about 5× the CPU on the A9 (contra 472 → 94 fps unthrottled). A driver that cannot hold 60 fps with sound may be playable without it. |
| `-drz80_snd` | Re-enable the DrZ80 ARM assembly core for *sound* Z80s. Off by default because it crashes on gng/1943 (`DrZ80Run` → `drz80_execute`); it measured free to disable. |

Example — `1943.opt`:

```
# Real cabinet signal: landscape 256x224 @ 15.72 kHz, CRT-legal.
-norotate
```
