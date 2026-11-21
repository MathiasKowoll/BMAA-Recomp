# BMAA-Recomp

Batman: Arkham Asylum GOTY for the Mac, running natively on Apple silicon.

Feral's Mac version of the game (2011) is a 32-bit Intel program. macOS stopped running those with Catalina, and
Rosetta never translated them, so the game its owners have in Steam no longer starts. This project is a static
recompilation: the game's own executable is translated to C and compiled for arm64, and runs against the game's
data where Steam installed it.

**Nothing of the game is here.** Not its executable, not the C generated from it, not anything built from that.
You build the port on your Mac from your own copy.

## What you need

- An Apple silicon Mac and the Mac version of the game installed from Steam (app 35140), with Steam running: the
  game asks Steam who owns it, and the port passes the question on unchanged.
- Apple's command line tools (`xcode-select --install`), `python3`.
- FFmpeg and pkg-config, for the game's movies: `brew install ffmpeg pkg-config`.
- Beside this folder, [Recomp-Framework](https://github.com/MathiasKowoll/Recomp-Framework) (branch
  `mac32-function-starts`) and DecompRecomp, whose runtime the port uses.

## Build and play

```sh
./build.sh "/path/to/steamapps/common/Batman Arkham Asylum GOTY/Batman Arkham Asylum GOTY.app"
./play.sh
```

The first build translates the game's code (several minutes) and compiles it (about four minutes on twelve cores,
some 3 GB of disk). After an update of these sources, `./build.sh` again rebuilds only what changed.

To start the port from Steam's own Play button, put this in the game's Properties › General › Launch Options:

```
"/path/to/BMAA-Recomp/steam-play.sh" %command%
```

## State

Playable from the title into the game, with movies, sound and a controller. In the first corridor it runs at about
59 frames a second on an M4 Max (the render thread is the limit); other areas are not measured yet. It still draws
through OpenGL, by way of the game's own Direct3D 9 layer; a Metal backend for that layer is the next step.
Known: the title screen stays black when the movies are skipped with `-nomovie`.

## How it is put together

- `Makefile`, `build.sh`: the build. `hle.txt`: the few functions of the game answered natively.
- `aot.c`: the translated code as the CPU of DecompRecomp's runtime, and the diagnostics (`DR_WATCH`,
  `BMAA_INTERP`, `BMAA_ENTRIES`; those need `make dr TRACE=1 BUILD=build-trace`).
- `glx.c`, `bink.c`, `au.c`, `al.c`, `pad.m`, `steam.c`, `threads.c`, `fast.c`: OpenGL, movies, audio, controllers,
  Steam, threads and the C library, as this game uses them.
