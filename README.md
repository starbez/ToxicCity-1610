# Spider-Man: Toxic City J2ME Recompilation

This project translates Java ME bytecode to C and provides a native SDL2 runtime
for the 240×320 Gameloft game. The game JAR and its assets are not included.
Supply your own legally obtained JAR; generated game code, extracted resources,
saves, texture dumps, and compiled binaries stay local and are excluded from Git.

## Requirements

- Python 3
- GCC and Make
- SDL2 development libraries (`sdl2-config` must be available)
- A compatible 240×320 game JAR (set `MIDLET` if its entry class differs from
  `GloftSPDN`)

On Windows, build from an MSYS2 UCRT64 shell with GCC, Make, Python, and SDL2
installed in that environment. Keep `SDL2.dll` on `PATH` when launching the
result. The included launcher also checks the common MSYS2 UCRT64 and MinGW64
directories.

## Build

Put your JAR in the project folder (the default name is `game.jar`), then run:

```sh
make gen JAR=game.jar       # translate the JAR to C under gen/
make check JAR=game.jar     # compile-check generated C
make res JAR=game.jar       # extract resources under res/
make sdl JAR=game.jar       # build the SDL2 game executable
```

On Windows, `make sdl` links the Windows multimedia timer library as well. The
generated files and executable are ignored by Git and can be recreated locally.

## Run

```sh
./toxiccity --res res --save save --scale 3 --fps 60
./toxiccity --res res --save save --fps 60 --wide
```

On Windows, double-click `Launch Toxic City.bat` to open the settings launcher.
It configures window scale, FPS target, widescreen mode, resource/save folders,
and optional texture export/overrides. Widescreen mode maximizes the window and
centers the original portrait game image with side bars; it does not change the
game's camera or 240×320 play area. The game window title reports painted FPS,
display FPS, frame time, selected target, and pacing adjustment.

Controls: arrows, Enter/Space/Z = fire, A/F1 = left soft key, S/F2 = right soft
key, 0–9, `[` = `*`, `]` = `#`, Esc = quit. SDL GameController devices are
supported.

Texture extraction and replacement steps are in [TEXTURES.md](TEXTURES.md).

## Development

```sh
make missing JAR=game.jar
make headless JAR=game.jar
make test
```

The headless target writes PNG screenshots; `--wav out.wav` records audio and
`--script "9000:-5;11000:-2"` injects key events (milliseconds:keycode).

## Current limits

- No garbage collector; allocated objects are not freed.
- Synthesized instrument timbres approximate General MIDI and have not been
  verified by ear against the original handset.
- Soft-key codes, system-font rendering, and gameplay beyond the tested intro
  still need verification.
- Frame-by-frame comparison with J2ME Loader has not been completed.

## License and game assets

No project license is specified yet. Choose and add a license before accepting
outside contributions or publishing the source under an open-source license.
The original game JAR, generated translation, and extracted game assets are not
provided by this repository.
