# Xbox XBE Recomp

A tool for creating unofficial native PC ports of original Xbox games. Games are statically recompiled from their x86 XBE into native 64 bit Windows executables. The game's x86 code is translated to C++ ahead of time. The Xbox kernel and the XDK libraries (Direct3D 8, DirectSound, XAPI, XNet) are implemented at the API level on Windows. Everything is drawn with a native Direct3D 12 renderer (or Direct3D 11) and played through XAudio2.

This tool was originally created for porting [Tom Clancy's Splinter Cell: Double Agent](https://github.com/TGP482/SCDoubleAgent-XBERecomp) and later used to port [BLACK](https://github.com/TGP482/BLACK-XBERecomp)

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## This Fork

This repository is a fork of [TGP482's XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), taken on 15 October 2026. One shared `main` serves every game ported with it: Project Gotham Racing 2, Project Gotham Racing, 007: Everything or Nothing, 007: Agent Under Fire, 007: NightFire and others. Each game pins its own commit of `main`, so a change only reaches a game when its pointer moves. Before `main` moves, a change has to pass the headless smoke test of every pinned game.

What the fork adds over the original, in short (details in [Features](#features)):
- **More XDK builds**: layouts and signatures for 3925, 3948, 4039, 4831 and 5659 beside 5849, with the Direct3D state numbering, frame buffer and DirectSound differences of the launch-era libraries.
- **A second renderer**: Direct3D 11, picked in the settings window or used when Direct3D 12 cannot start.
- **Console accuracy fixes** found across the games: W-buffered depth, push buffer recording and replay, vertex program memory, point sprites and lines at console size, depth buffers read as colour, read-back of targets the GPU drew, submix buffers and multichannel voices in DirectSound, and many more listed below.
- **Ultrawide and 16:10** screens, with 2D kept at its shape and hooks for a game to lay out its HUD.
- **In-between frames** for titles that draw one frame per game step, and a multi-threaded render path.
- **Image options**: MSAA, FXAA, SMAA, sharpening, a sharp scaling filter, smooth alpha edges, enhanced world textures, sharp 2D, a CRT look.
- **Input**: steering wheels with force feedback, mouse look, keyboard play, stick dead zone and sensitivity, a game's own keyboard actions.
- **A player's launcher** and **prebuilt packages**: the player installs from their own disc without build tools.
- **A redesigned settings window**, languages, achievements, custom soundtracks from the player's music.
- **Testing and diagnostics**: a fuller headless harness, shared test scripts, crash dumps with guest memory, frame logs and many `XBE_` switches.

## Installation

- Put this repository in your game repository as `sdk/`. This can be a copy or a submodule.
- In the game's `CMakeLists.txt`:
   ```cmake
   include(sdk/cmake/XbeRecomp.cmake)
   file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
   list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
   add_xbe_image(MyGame default.xbe game.toml 128 game)
   ```
- Build with Clang and Ninja (`clang++`, `-fuse-ld=lld`). Zydis, toml++, Dear ImGui and SDL3 are fetched by CMake.

## Game Setup

1. Copy `templates/game` to a new repository and put this SDK in it as `sdk/`. Set `GAME_TITLE`, `GAME_XBE` and `GAME_EXE` at the top of `build.bat` (`GAME_EXE` in `release.bat` too), and the target and xbe in `add_xbe_image` in `CMakeLists.txt`.
2. Put the game's XBE and its data folders in the `assets` folder. No game files, keys or generated code belong in any repository.
3. Run `xbe_symbols`, which `build.bat` does on the first build:
   ```
   xbe_symbols assets\default.xbe sdk\xdk --out config\game.toml
   ```
   This prints the XBE's library versions, picks `xdk/<build>/signatures.toml` and finds the XDK functions and data the host replaces, then writes the config: `[xbe]` with a partition count and the code sections, the `layout.toml` include, `[symbols]` and `[data]`. Misses and ambiguous names are listed and left at 0, which become trapping stubs. It stops when no `signatures.toml` exists for the XBE's build.
4. Fill in `app::kGame` in `src/common/game_info.cpp`: title, window name, log and settings file names, splash, the xbe to exe table, game options, and the hooks for loading, the window, drawing and the pads.
5. Game hooks go in `src/game`. Define `X86_FUNC(sub_XXXXXXXX)` to replace a recompiled function, and call `__imp__sub_XXXXXXXX` to reach the original. `python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe` writes a flat PE with the recompiled functions as exports for Ghidra.
6. Run `build.bat`. One game repo can build several images, one config and `add_xbe_image` each; `--name` sets the config's `generated/<name>`, which is the folder argument of `add_xbe_image`.

The template also carries what a game repository needs beyond the build: `scripts/` (one-line wrappers for the shared
scripts in `sdk/scripts`, the game's values in `scripts/game.env`: harness, smoke and soak tests, system link test,
Windows syntax check, shader check, Ghidra helpers), `launcher/` and `launcher.bat` (the player's launcher, see
`launcher/README.md`), `.github/workflows/release.yml` (builds the launcher and publishes a release on a tag `v*`),
and CLAUDE.md, PROGRESS.md and FINDINGS.md to start from.

A new XDK build needs `xdk/<build>/layout.toml` and signatures made from titles whose configs are complete: `xbe_symbols --sign config/a.toml [config/b.toml] --out sdk/xdk/<build>/signatures.toml`. Signatures are lengths and hashes of the functions with addresses masked, never library code. Seed a new build from the nearest one's files, then re-derive every `layout.toml` offset from the title's own code, citing the instruction address. `tools/symbols/xbe_port.py` maps addresses from one build of a game to another (`--toml` ports a recompiler config).

### Prebuilt Packages

A game that sets `XBE_GAME_DLL` builds its recompiled code into `<exe>.dll` on the player's PC, while the runtime, the exes and the compiler ship ready made (from TGP482's upstream, as an option). `sdk/scripts/release-package.bat` makes a package for one set of discs with identical code (`game.json` `packages`, by the xbe's SHA-1; `XBE_GAME_CONFIGS` names the configs a package carries). The launcher downloads the package, unpacks it beside the player's saves, music and settings, and builds the game's code with the exe's `--prepare-code`. An install built the old way updates in place through `play\.next`, so a failed update leaves the game playable.

## Features

### Recompiler
- **Function Discovery** - Finds every function in the XBE by following direct calls, code pointers and the gaps between known functions. Immediates that point at code but were not made functions are listed, so a game can name the real ones in `[functions]`.
- **Jump Tables** - Resolves switch tables of every form the Xbox compiler writes, including byte indexed and backwards tables, memcpy tables and copy sleds.
- **C++ Exceptions** - Supports C++ exceptions, with try and catch blocks working as on the Xbox.
- **setjmp and longjmp** - `[xbe] continuations` declares the places a `longjmp` returns to.
- **x87, SSE and MMX** - Translates x87, SSE and MMX instructions, with the x87 register stack tracked at run time.
- **Overridable Functions** - Makes every recompiled function replaceable, allowing a game to override any of them without editing generated code.
- **Instruction Hooks** - Adds `[[hook]]`, which calls a game function before or after any instruction, and can return from or jump within the function.
- **Analysis Overrides** - Adds `[[function]]`, `[[switch]]` and `[xbe] data_ranges` to add functions, set switch targets and mark data the analysis gets wrong.
- **Incremental Builds** - Only rewrites generated files when their content changes, keeping rebuilds fast.
- **XDK Signatures** - Finds the XDK functions and data in a new game automatically and writes its config. A byte match the image's `.XTLID` lists under another export is dropped, and in a non-LTCG image a function whose return size or entry registers differ from the reference is not taken.
- **LTCG Libraries** - Supports games built with link time code generation, where library functions take their arguments in registers. A hand `[abi]` replaces the scan's unproven placeholders.
- **Edge Cases** - An instruction may end in its section's zero filled tail, `int 2Dh` steps over its `int3`, and segment register pushes take 4 bytes.

### XDK Builds
- **5849, 5659, 4831, 4039, 3948 and 3925** - Each has its own `layout.toml`, derived from a title's code, and signatures.
- **Launch-Era Libraries** - 3948 and older keep their own Direct3D render and texture stage state numbers (translated at the title's tables, `StateNumbering` in the layout), frame buffers in the device's surface slots, the count-style vertical blank callback and present parameters that end at 0x34. DirectSound 3936 keeps mix bins as a bin mask and Lock's older flags.

### CPU and Kernel
- **Guest Calls** - Adds indirect calls and host calls into game code, with the Xbox's 733 MHz timer and Pentium III CPUID.
- **x87 Precision** - Follows the game's x87 precision setting, giving the same results as on the Xbox.
- **CRT Math** - Computes the game's math functions on the host, fixing math that returned invalid results at random.
- **Kernel** - Implements threads, waits, files, memory, crypto and EEPROM settings, with game files read from host folders or a disc image. Raising IRQL to dispatch level keeps DPCs out, as on the one processor.
- **Memory** - Physical memory is mapped to 128 MB as on a development kit, while the title's allocator keeps the console's 64 MB; the renderer's own buffers come from the part past it. Contiguous allocations take the best fit, so small blocks do not split the big ranges a title needs.
- **Hard Disk and Disc** - Hard disk partitions as volumes, FATX truncation, the DVD's volume information and authentication page, DOS wildcard rules (`*.*` matches a name with no dot), longest-prefix symbolic links, and disc reads paced at the console drive's speed (`GameInfo::disc_rate`, `XBE_DISC_RATE`).
- **Host Read Retries** - A failed host read (a disc image, an extracted file, a file's attributes) is tried again five times 50 ms apart and then logged, instead of reaching the title as a short read: from a network share a failed read had become missing game data.
- **Fibers** - XAPI fibers run on host coroutines.
- **Image Switching** - Relaunches into the matching executable when the game loads another XBE, keeping the window on screen without a gap. The new instance starts as any program does, with a named handover event, so antivirus does not flag it.
- **Region and Language** - `GameInfo::pal_console` runs a PAL release on a PAL console with PAL 60 Hz on; `GameInfo::languages` offers a title's languages in the settings window, answered through `XC_LANGUAGE` at the next start.

### Graphics
- **Direct3D 8** - Implements Direct3D 8 as the XDK does it, including immediate mode draws, palettes, gamma ramps and Direct3D code built into the game.
- **Native Direct3D 12 Renderer** - Translates the Xbox's vertex programs, pixel combiners and fixed function pipeline to HLSL, with rendering on its own thread.
- **Direct3D 11 Renderer** - The same drawing on Direct3D 11, chosen in the settings window or used when Direct3D 12 cannot start.
- **Multi-Threaded Rendering** - A prep thread assembles draws ahead of the render thread, draws are decided on the render thread and recorded into the command list on a record thread, and guest vertex copies run on helper threads. `docs/render-parallel.md` has the design.
- **Push Buffers** - Reads raw commands a title pushed (vertex shader constants, visibility tests, immediate mode) before every state or constant set, so they apply in the console's order. Push buffers recorded with BeginPushBuffer and run with RunPushBuffer are drawn, with fixups.
- **Vertex Programs** - Program memory as on the hardware: pushed loads, LoadVertexShader, SelectVertexShader, and the program read again when pushed instructions change it. Colour registers a shader does not supply start at the library's defaults, and a register with no stream reads the last value pushed.
- **Console Accurate Depth** - Draws W-buffered depth (D3DZB_USEW) from each pixel's perspective correct w, with polygon offsets in its units and the XDK's scale for fixed function and pretransformed vertices; W-buffered draws are not clipped past the far plane; a depth buffer bigger than the colour target serves it, as on the console. SetDepthClipPlanes clips vertex program draws.
- **Depth Read As Colour** - A depth buffer read through a colour format gives its bytes as the console does (D24S8, B8G8R8A8).
- **Visibility Tests** - Counts visible pixels with occlusion queries and delivers reports when a title polls for them inside a frame, kicking the renderer instead of waiting for it.
- **Render Targets** - Switches targets set with SetRenderTargetFast, copies cube faces drawn on the GPU into their cube, and keeps 8 bit targets as the console's B8. A title's own frame buffers become the device's. Targets the GPU drew are read back when the title reads them or draws vertices from them (swizzled 32 bit ones too), a colour target smaller than the frame is kept per size, and targets read as a format without alpha read alpha 1.
- **Vertex Inputs** - Supports SetVertexShaderInput and SetVertexShaderInputDirect, declarations that read past the stride, strides that are not whole dwords (repacked instead of converted on the CPU), and pushed vertex data in shorts.
- **Fixed Function** - Vertex blending (D3DRS_VERTEXBLEND), point sprites with stage 3's coordinates as the Xbox gives them, lines one console pixel wide, constant alpha blends, partial colour clears, the title's scissor in place of the viewport clip, and specular reads with specular off.
- **Multisample Modes** - MultiSampleMode asking more samples than a target has draws it smaller and stretched, as the console does.
- **Frame Pacing** - Paces each frame by the title's presentation interval, honours Swap's copy and finish flags and calls the swap callback. GetRasterStatus answers from the host's vertical blank. `app::g_pace_hz` sets the rate a present interval counts in.
- **In-Between Frames** - For a title that draws one frame a game step (`GameInfo::step_interpolation`), each step is replayed with its constants and matrices blended from the step before, as many times as fit before the next step, never making the title late.
- **Ultrawide** - The aspect setting "screen" shows the frame at the screen's shape: screen shaped targets get more pixels across than down, and 2D that does not span the frame is squeezed into a centred 16:9 part; the game widens its own 3D views. Pass-through and orthographic vertex programs are found as 2D too, full-width bars and fills keep their edge, full screen movies can be pillarboxed (`State::pillar_aspect`, `pillar_fit`) and a game's hook can spread a 4:3 HUD to the edges (`squeeze_beyond`, `squeeze_shift`, `squeeze_margin`, `hud_layout`).
- **16:10 Screens** - A title that widens its 4:3 views (`GameInfo::fills_narrow_screens`) fills a 16:10 screen instead of being letterboxed to 16:9, with 2D kept 4:3 in the middle (`app::HudAspect`). The TV mode and aspect choice are latched at boot unless the title follows them live (`GameInfo::aspect_live`).
- **Antialiasing and Image Options** - MSAA 2x, 4x or 8x on the scene through multisampled twins of the big targets; FXAA, SMAA, contrast adaptive sharpening, a sharp scaling filter and a CRT look on the finished frame; alpha tested edges as coverage with MSAA; 3D colour textures doubled in the background (enhanced world textures); sharp 2D (`GameInfo::sharpen_2d`), all but MSAA off by default.
- **Texture Replacements** - `gpu::SetReplacement` lets a game draw its own larger picture for a texture while the guest bytes stay unchanged.
- **Shader and Pipeline Caches** - Compiled shaders (`shadercache.bin`) and Direct3D 12 pipelines (`pipelinecache.bin`) are kept beside the exe between runs.
- **Draw Filters** - Allows a game to drop a draw, its depth test, or move it nearer with `gpu::g_draw_filter`, for game specific fixes.
- **Renderer Extensions** - Allows a game to change render targets, add passes and add to the generated shaders through `gpu::Extension`, set with `gpu::SetExtension` from `GameInfo::init`.

### Audio
- **DirectSound** - Mixes sound in software through XAudio2, with Xbox ADPCM, streams, mix bins and the effects image.
- **Effect Sends** - A voice heard only through its effect sends plays into the speakers in place of the DSP image (`GameInfo::send_level`).
- **Multichannel and Submixes** - Voices of more than two channels play them all, with the centre and back folded in; a voice routed into a submix buffer is heard through that buffer's volume, mix bins and 3D place. The listener's rolloff factor is honoured, and `GameInfo::sound_x_left` serves titles whose +x points left.
- **XACT** - The exports XACT titles call are hosted.
- **Custom Soundtracks** - The launcher turns a folder of the player's music into the game's Xbox soundtracks (WMA through FFmpeg, `ST.DB`).
- **No Audio Device** - If XAudio2 cannot start, the mixer is still pulled on the clock, so the game keeps its timing.

### Input
- **Controller** - Supports Xbox, PlayStation and Switch controllers through SDL3, with hot plugging and rumble. Controllers are read on their own thread, so a slow scan for devices never holds the game.
- **Remapping** - Maps any physical controller input to any console input.
- **Stick Tuning** - A dead zone and a sensitivity for each stick, in the Controls tab (`GameInfo::stick_deadzone` gives the game's own dead zone).
- **Keyboard** - Plays as controller 1 with two keys per console input, remappable, saved in the settings file's `[keys]` table. A game can list its own keyboard actions (`GameInfo::actions`, `input::ActionHeld`).
- **Mouse** - Mouse look as controller 1's right stick or the title's own look input (`GameInfo::mouse_look`, `mouse_direct`), with sensitivity and invert, mouse buttons as keys and the wheel for menus.
- **Steering Wheels** - Every DirectInput game controller but Xbox pads: wheel bases, pedal and button boxes and shifters, each control bound to a device in the Wheel tab, force feedback on the steering's axis, H-pattern shifters through the game's gear. A port a wheel steers reports itself as a wheel to the title. All DirectInput calls run on their own thread.

### General
- **Settings Window** - F1, or Back + Start on any controller, opens an in-game window, a section list and one full-width row per setting, for monitor, display mode, resolution, vsync, aspect ratio, renderer, render scale, anisotropic filtering, frame rate cap, the image options, a frame counter, pausing on focus loss, language, the game's own options (with labels, dropdowns and help lines), controls, sticks and wheels, saved at once, and a Quit game button. Made for a controller through SDL, so every pad type works: LB/RB change section, the d-pad or left stick moves and changes values, A presses, Y clears a binding, B or Start closes. `tools/ui_preview` runs it with no GPU: it checks the navigation and writes a PNG of each page.
- **Launcher** - One exe a player runs to install and play a game from their own disc: a prebuilt package, or a build with portable tools it fetches itself. It updates itself and the game from the game's GitHub releases, handles games of several programs and discs, and starts the game straight away when nothing needs updating. See `launcher/README.md`.
- **Achievements** - A game lists achievements as conditions on guest memory in RetroAchievements' syntax (`GameInfo::achievements`), or awards one from a hook (`Award`); they are checked each frame, unlock with a pop-up, are listed in the settings window and kept in `achievements.toml`.
- **Pause On Focus Loss** - The title waits at its next frame and the mixer is not pulled, so the game, its sound and its sample clock stand still while the window is behind another.
- **Hard Disk** - Emulates the hard disk's partitions, formats the cache partition and keeps save games beside the exe.
- **Disc Images** - Runs the game from an ISO in the `assets` folder, either a whole disc dump or the game partition, or from the extracted data files.
- **System Link** - System Link can be played over LAN, or by directly connecting to a friend's IP address (`join_ip`). `XNET_TRACE=1` logs every network call.
- **Bink Movies** - Fixes Bink movies playing too fast above 30 FPS and crashing on fast reads, for games that use Bink.
- **Screenshots and Full Screen** - F12 saves the frame as a PNG in `screenshots` beside the exe; Alt+Enter switches between a window and full screen.
- **Crash Dumps** - Writes crash dumps, including the guest's memory, to a `CrashDumps` folder beside the exe (`XBE_TEST_CRASH` to try one). The C runtime's invalid parameter handler dumps too.
- **Logs** - Lines are written by their own thread, so a log on a network share never holds the game; the last five runs' logs are kept, and the log names why the game ended.

### Miscellaneous
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.
- **Diagnostics** - Logs frame timing with `XBE_PERF` (busy summaries, 1% lows, slow frames and where their time went), captures frames with F3 when `XBE_CAPTURE_DIR` is set (`XBE_CAPTURE_FRAMES`, `XBE_CAPTURE_DRAWS` for several frames or every draw, `XBE_CAPTURE_FULL=1` small targets at their host size, plus the guest's memory), records draws with their input hashes at full speed with F4 (`XBE_FRAMELOG`), leaves out or alters draws (`XBE_DRAW_SKIP`, `XBE_DRAW_TEST`), logs vertex constant writes (`XBE_VSC`), files (`XBE_FILES_LOG`), memory (`XBE_MEM_LOG`), sound (`XBE_DSOUND_LOG`) and clears (`XBE_CLEAR_LOG`), lists the environment's switches at startup and enables the Direct3D 12 debug layer with `XBE_D3D12_DEBUG`. A lost Direct3D 12 device is reported with DRED's operations and the draws around where the GPU stopped; NVIDIA Aftermath is used when its DLL is beside the exe on a test PC.
- **Headless Harness** - Builds and runs a game on Linux without a window, GPU, audio or controller, for testing: scripted pads, a CPU reference renderer, draw lists, memory dumps and pokes, paced frames, disc images and relaunches. See `harness/README.md`.
- **Shared Scripts** - `scripts/` holds the harness, smoke, soak and system link tests, a Windows syntax check (`wincheck.sh`), a shader check and Ghidra helpers, each game's values in its `scripts/game.env`.
- **Preview Tools** - `tools/ui_preview` (the settings window) and `tools/filter_preview` (the final pass and the upscaler) run without a GPU.
- **SDK Sync** - Keeps the `sdk/` copies in game repositories in step with this repository.

## Credits

- [TGP482](https://github.com/TGP482/XboxXBE-Recomp) for XboxXBE-Recomp, which this fork builds on, including SMAA and the game code DLL used by prebuilt packages.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks this follows.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour. This project does not use emulation or any emulator code.
