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

## Choosing the engine

The port ships three emulators and the right one is a property of the driver, so
it is chosen here rather than at deploy time. A line of the form `engine <name>`
is a **directive, not a flag** — it is read by `game_engine()` and stripped
before the rest of the file reaches the emulator's command line.

| Name | Binary | What it is |
|---|---|---|
| *(omitted)* | `mame` | mame4all-pi (0.37b5). The default, and what every game launched under before this directive existed. |
| `mame4all` | `mame` | The same thing, named explicitly. |
| `mame2003` | `mame2003` | MAME 2003-Plus (0.78) — families mame4all lacks, and romset compatibility with the widely distributed reference set. |
| `lrmame` | `lrmame` | Current MAME (0.289), over a driver subset. Needs `lrmame_libretro.so` beside it in `games/mame/`. |

`default.opt` can set a house engine and a per-game file overrides it — but note
that the per-game file replaces `default.opt` **wholesale** rather than merging
with it, so a `<setname>.opt` that sets flags must repeat the engine line if it
wants a non-default engine. The last `engine` line in a file wins.

An unknown name, or an engine that was never deployed, falls back to the default
and says so in `<setname>.log` rather than failing to launch.

Example — `pacman.opt`:

```
# Gate build: current MAME, to bench 0.289's core overhead on a Z80 driver.
engine lrmame
```

Example — `1943.opt`:

```
# Real cabinet signal: landscape 256x224 @ 15.72 kHz, CRT-legal.
-norotate
```
