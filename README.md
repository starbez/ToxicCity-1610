# Spider-Man: Toxic City — J2ME Recompilation

A static Java ME bytecode-to-C translator and SDL2 runtime for the 240×320
Gameloft game. The translator generates C from a locally supplied game JAR; the
runtime provides desktop input, graphics, audio, saves, and optional texture
tools.

**The game JAR and game assets are not included.** Provide your own legally
obtained compatible JAR. Generated game C, extracted resources, saves, texture
exports, and executables are local build/runtime files ignored by Git.

## Features

- **JAR-to-C translation:** translates Java class files into C, then builds
  them with the native runtime.
- **SDL2 desktop runtime:** 240×320 framebuffer, integer-scaled window,
  keyboard input, controller hot-plug, and SDL audio output.
- **Widescreen mode:** maximizes a 16:9 window and centers the portrait game
  image with side bars. It does not widen the camera or change the play area.
- **Frame pacing and metrics:** selectable frame-rate target up to 60 FPS. The
  window title shows paint FPS, display FPS, frame time, target FPS, and sleep
  adjustment. Actual rates depend on the device and game workload.
- **Controller input:** D-pad and left stick both send directional arrows. The
  Xbox button-to-keypad layout is listed in the controls table below, with
  controller hot-plug support and device-specific input filtering.
- **Texture export and replacement:** exports decoded images as numbered and
  stable-hash PNGs, and can load edited PNG overrides. See
  [Texture workflow](TEXTURES.md).
- **Java ME runtime services:** MIDlet startup, resources, RMS saves, core
  collections and strings, threads, image/graphics APIs, and a software audio
  player with WAV playback and an SMF synthesizer.
- **Headless mode:** render timed screenshots and optionally record WAV audio
  or inject scripted key events.

### Texture resolution note

Texture overrides can change the art, but they do not currently provide true
high-resolution in-game rendering. Matching-size images are supported, as are
integer 2×–4× PNGs with the same aspect ratio; enlarged images are downsampled to
the original texture dimensions. The game still draws in its original 240×320
coordinate space, including its sprite-sheet crops.

## Requirements

- Python 3
- GCC and Make
- SDL2 development libraries with `sdl2-config` on `PATH`
- A compatible 240×320 game JAR; the default MIDlet entry class is `GloftSPDN`

For Windows, use an MSYS2 UCRT64 shell with GCC, Make, Python, and SDL2 installed
in that environment. Keep `SDL2.dll` on `PATH` when launching; the Windows
launcher also checks common MSYS2 UCRT64 and MinGW64 folders.

## Build

Place your JAR in the project folder as `game.jar`, or pass its path with
`JAR=...`:

```sh
make gen JAR=game.jar       # translate the JAR into C files under gen/
make check JAR=game.jar     # compile-check the generated C
make res JAR=game.jar       # extract game resources under res/
make sdl JAR=game.jar       # build the SDL2 desktop executable
```

If the JAR uses a different MIDlet class, set `MIDLET`, for example:

```sh
make gen JAR=game.jar MIDLET=YourMidletClass
```

The Windows build links the multimedia timer library used for finer sleep
timing. Generated files and binaries can be recreated locally and are excluded
from the repository.

## Run

```sh
./toxiccity --res res --save save --scale 3 --fps 60
./toxiccity --res res --save save --fps 60 --wide
```

The Windows GUI launcher is `Launch Toxic City.bat`. It lets you choose the
executable, resources and save folders, scale, FPS target, widescreen mode,
texture export folder, and texture override folder. It also links to the
texture guide and override log.

### Controls

Keyboard:

| Input | Action |
| --- | --- |
| Arrow keys | Directional movement |
| Enter, Space, Z | Fire / confirm |
| A or F1 | Left soft key |
| S or F2 | Right soft key |
| 0–9 | Number keys |
| `[` / `]` | `*` / `#` |
| Esc | Quit |

Xbox controller:

| Toxic City input | Xbox controller | Action |
| --- | --- | --- |
| Arrow keys | D-pad and left stick | Move in that direction |
| **2** | **A** | Jump / climb up |
| **8** | **B** | Crouch / climb down |
| **5** | **X** | Attack |
| **7** | **LB** | Web attack left |
| **9** | **RB** | Web attack right |
| **\*** | **LT / RT** | Block / web shield |
| **#** | **Y** | Special / context action |
| **0** | **Start / Menu** | Pause |

LT and RT both trigger `*`; the key stays held until both triggers are released.

## Developer tools

```sh
make missing JAR=game.jar  # report unresolved runtime symbols
make headless JAR=game.jar # build and capture timed screenshots
make test                  # run the offline synthesizer check
```

The headless executable accepts `--wav out.wav` to record audio and
`--script "9000:-5;11000:-2"` to inject key events (`milliseconds:keycode`).
Screenshots are written to the selected `--shot-dir` at `--shot-ms` intervals.

## Known limitations

- Gameplay beyond the tested intro, soft-key behavior, and system-font text
  rendering still need verification.
- The software synthesizer uses approximate General MIDI timbres and has not
  been verified by ear against the original handset.
- There is no garbage collector yet; allocated objects are not freed.
- Frame-by-frame comparison with J2ME Loader is still outstanding.
- Raising the FPS target does not guarantee a higher game logic or paint rate;
  the target is capped at 60 and performance depends on the workload.

## License and original game material

This repository does not yet specify a project-wide license. Third-party
components retain their own notices in their source files. The original game
JAR, generated game translation, and extracted game assets are not distributed
here.
