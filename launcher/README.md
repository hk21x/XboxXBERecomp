# Launcher

One exe a player runs to build and play a recompiled game from their own disc, with nothing installed in Windows.
A game ships no recompiled code (it is derived from the disc), so the player's PC builds it: the launcher fetches
portable build tools into its own folder, the game's source, takes the xbe from the player's disc image or folder,
builds, and from then on is the Play button (and Update, when the game's branch moves on).

## For a game

A folder in the game's repository (`launcher/` in PGR2's) with:
- `game.json`: `title`, `short` (the launcher's name and work folder), `exe`, `xbe`, `releases` (each supported
  xbe's SHA-1 and its name), `repo` and `branch` (git, public), `toolchain` (where toolchain.json is published),
  `music` (true: the game plays Xbox custom soundtracks, and the launcher makes them from a folder of the player's
  music, below).
- `toolchain.json`: the tools, each `id`, `title`, `kind` (`zip` or `tar.xz`), `url`, `sha256`, `size`, `strip`
  (leading path parts dropped), `keep` (what is unpacked: `dir/` or a path, `*` within a name) and `optional` (true:
  not part of the build, fetched when a feature needs it: `ffmpeg` for the music); `msvc` (a
  Visual Studio channel, an MSVC version and its packages) and `sdk` (a Windows SDK package and the .msi files taken
  from it). The launcher reads the published copy each build, so versions move without a new launcher; it keeps the
  last copy and has its own built in.
- `icon.ico` (optional).

Build it on Windows (Visual Studio's clang, CMake and Ninja on the PATH):
`cmake -S sdk/launcher -B out/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DLAUNCHER_GAME=<the folder>`.
The exe is `<short>Launcher.exe`, one file with no runtime beside it.

## What it does

Under `<short>\` beside itself:
- `tools\` clang (LLVM's release), CMake, Ninja, MinGit, Microsoft's C++ library packages (vsix) and the Windows SDK
  (.msi files extracted with `msiexec /a`, which needs no rights). Every file is checked against its SHA-256; the
  Microsoft ones come from the Visual Studio manifest with theirs. The player accepts Microsoft's license first.
- `source\` the game's repository; its xbe goes in `source\assets`.
- `play\` the built exe and what it writes (settings, saves, logs); `play\assets` is the player's game: a junction
  to their folder, or their disc image hard linked (copied when it is on another drive).
- `launcher.log`, `launcher.json` (the game chosen).

The build runs with the environment Visual Studio's developer prompt would set (INCLUDE, LIB, PATH) and links with
lld. Downloads resume and every step can run again: what is done is kept.

## Updates

A launcher knows its version: a release's tag when GitHub Actions builds it for one (`GITHUB_REF_TYPE` tag, so the
game's release workflow needs nothing), else `dev` (`-DLAUNCHER_VERSION=` overrides).

- A release launcher asks GitHub's API at start for the repository's latest release. When it is newer, the window
  offers it at the top (**Update the launcher**, **What's new**, **Later**): the release's `<short>Launcher.exe` is
  downloaded beside this one, checked by the SHA-256 GitHub lists for it (else its size), and swapped in; this one is
  renamed `.old` (a running exe can be renamed, not overwritten) and the new one starts. The next start removes the
  `.old`. Offline, or for a private repository, nothing is offered.
- A release launcher builds its own tag, not the branch, and launcher.json records the version each build came from
  (`built`). A game built by another version (or before versions were recorded) shows **Version vX of the game is
  ready to build** and the button says Update: the rebuild keeps the saves, settings and the player's copy.
- A `dev` launcher (an artifact of a push to main) builds the branch's head and offers an update when the branch has
  moved on, as before.

## Music (game.json `music`)

Section 4 of the window: the player chooses a folder of their music. Each subfolder becomes a soundtrack (everything
under it, in name order), the files in the folder itself one more, at most 100 of 504 songs (an Xbox's limits).
Each MP3, FLAC, OGG, Opus, M4A, AAC, WAV, WMA or AIFF is converted to WMA 2 (stereo, 44.1 kHz, 160 kbit/s) with the
toolchain's `ffmpeg` (fetched the first time), named from its tags ("Artist / Title") or its file name, and the
Xbox's soundtrack database ST.DB written beside them in `play\hdd\E\TDATA\FFFE0000\MUSIC`, where a game's XAPI
reads it and its own WMA decoder plays them (core/music.h has the format). `launcher-music.json` there remembers
what each file became: *Update my music* converts only what changed. A MUSIC folder with an ST.DB not made here (an
Xbox's, copied over) is left alone.

## Testing on Linux

`launcher_cli` (built on Linux from the same CMakeLists) runs the steps from a terminal: `plan` (resolves the tools
from the manifests), `tool <id>` (downloads and unpacks one; not the SDK, which needs msiexec), `game <iso|folder>`,
`source`, `env`, `music <folder>` (with the ffmpeg on the PATH), and `hash`/`untar` for the pieces. With a toolchain.json beside game.json it uses that one.

`launcher_cli <game.json> <root> latest [version]` prints the latest release and whether a launcher of that version
would be offered it; `LAUNCHER_VERSION=v1.0.1` makes any command act as that release's launcher.
