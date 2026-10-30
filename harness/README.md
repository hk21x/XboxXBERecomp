# Headless harness

Builds and runs one image of a game on Linux without a window, GPU, audio device or pad: the recompiled code and
the SDK runtime as they are, with these stand ins.

- `app_headless.cpp` the app side: default settings, the run folder as the exe folder, no splash or handover
- `pad_script.cpp` port 0 from a script, then `GameInfo::pad` as on Windows
- `null_audio.cpp` the mixer pulled every 10 ms, output dropped or written to a WAV file
- `null_backend.cpp` counts frames, lists draws, draws chosen frames with `reference_renderer.cpp` (CPU, into guest memory)
- `main_test.cpp` runs `GameInfo::init` and `window` (with no window), then boots the image
- `compat/` the few Win32, Winsock and DXGI names portable sources use

Needs clang and the game's generated code. The game repo supplies stand ins for its own Windows only sources.

## Build

```
sdk/harness/build.sh -r <repo> -g <generated/image> -o <out> [-n exe] [-x regex]... [-e source]... [-G gen objects]
```

`-x` leaves sources out, `-e` adds sources, `-G` shares the generated objects between builds.
Environment, to build as the Windows exe is built: `RAW=1` the guest's own vertices for the backend (the reference
renderer converts its own), `RT_THREAD=1` the render thread, `GEN_OPT=-O2` the generated code's optimisation
(default -O1; give such a build its own `-o` folder).

## Run

```
sdk/harness/run.sh -x <exe> -g <game folder> -o <out> [-t seconds] [-s script]... [-n instances] [-d seconds]
```

Instance k runs in `<out>/k` with its own `logs`, `hdd` and `cap`. With more than one instance each gets
`INSTANCE=k` and they find each other over system link on 127.0.0.k. The exe alone: `<exe> <game folder> [--instance N]`.

Environment:

- `SCRIPT` pad script: lines of `vblank buttons [vblanks held]` (default 8), `#` comments, buttons joined with `+`:
  `UP DOWN LEFT RIGHT START BACK A B X Y BLACK WHITE LT RT LS LSD LSL LSR RS RSD RSL RSR MOUSEL MOUSER MOUSEU MOUSED KEYS CLICKL CLICKR WHEELU WHEELD` (KEYS: on the keyboard, for `input::UsingKeyboard`; CLICK: mouse buttons held; WHEEL: a notch each poll). Times count vertical blanks, 60 a second: each
  presented frame adds the vertical blanks its present interval asked for (at least one), so a title that runs its
  menus at 60 and its races at 30 keeps one clock. `renderer: frame` log lines carry the count.
- `cmd` file in the run folder: `buttons [vblanks held]`, pressed once when read
- `REF=N` every Nth frame drawn by the reference renderer into `cap/ref_<frame>.tga`
- `snap` file in the run folder: the next two frames drawn into `snap_1.tga` and `snap_2.tga` (`SNAP_FRAMES=N`: N
  frames, `snap_1.tga` to `snap_N.tga`)
- `achievements.toml` in the run folder: the achievements unlocked, as on Windows (log line `achievement unlocked`)
- `poke` file in the run folder: lines `address size value` (hex address, size 1, 2 or 4, value decimal or `0x`
  hex) written into guest memory at the next frame, then the file is removed. Used to meet achievement conditions.
- `memdump` file in the run folder: guest memory to `mem_<frame>.bin` (the first 128 MB of virtual space) and
  `mem_<frame>_phys.bin` (physical memory at 0x80000000), uncommitted pages as zeros
- `REF_DRAWLOG` each drawn frame's draws in `cap/drawlog_<frame>.txt`: pixels and stencil values written, their box,
  depth/stencil/blend state, textures; diff two frames' lists to find what flickers
  (`REF_DRAWLOG=2` adds constant changes). With it: `REF_ZOWNER` which shader's depth stopped each failed pixel and
  by how much; `REF_VERTS_VS=handle` / `REF_VERTS_T0=texture` that shader's (or texture's) vertices; `REF_NODEPTH_VS`
  that shader without the depth test; `REF_NO_WBUFFER` W-buffered draws as z / w
- `CAP=N` every Nth frame's draws listed in `cap/draws_<frame>.txt` (each with its viewport, `vp=x,y,wxh`)
- `AUDIT=N` every Nth frame's draws: each shader the D3D12 backend would generate into `cap/hlsl/`, each texture
  sampled into `cap/textures.txt` with how far each mip level is from its parent shrunk (`audit.cpp`)
- `PROBES="x,y x,y"` log every colour the reference renderer writes at those pixels of linear `PROBE_WIDTH` (640) wide
  targets; `TRACE="x,y"` logs the combiner registers there
- `FPS=N` presents at most N frames a second, `FPS=0` unpaced; without it the settings' `fps_cap` (a game's
  `init` may set it), 60 when that is 0; within that, the title's present interval (`D3D_PresentInterval`): 60 a
  second over the number of vertical blanks it asks for
- `WAV=file` the mixed output also written to that file in the run folder (48 kHz, 16 bit stereo); the log's
  `audio: output started` line is the time of its first sample
- `OUTPUT=WxH` the display size the game is told about, 1280x720 by default: narrower than 3:2 the console reports
  a 4:3 TV instead of a widescreen one
- `HOLD_REPORTS=1` visibility reports held until a flush, as a GPU backend delivers them (by default written at once, which
  hides a title polling a report nothing kicks)
- `XBE_PERF`, `XBE_FRAMELOG` (a `framelog` file in the run folder starts it), `XBE_WBUFFER`, `XNET_TRACE` as on
  Windows
- Debugging: start the exe inside gdb (attaching may be refused); recompiled functions are
  `__imp__sub_XXXXXXXX(X86Context&, base)`, guest memory is at 0x100000000

A frame drawn by the reference renderer takes long; a button held across one can auto repeat in the game's menus.
