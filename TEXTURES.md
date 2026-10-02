# Texture export and replacement

The runtime can export images after the game's asset loader has decoded them and
can load edited PNGs in their place. This avoids having to unpack the game's
numbered/encrypted resource bundles by hand.

## Export decoded textures on Windows

Create an output directory, then set `TOXICCITY_DUMP_TEXTURES` before launching
the game:

```powershell
New-Item -ItemType Directory -Force textures_original | Out-Null
$env:TOXICCITY_DUMP_TEXTURES = (Resolve-Path textures_original).Path
.\toxiccity.exe --res res --save save --scale 4
```

The export writes sequential names such as `texture_0000.png` and stable
content names such as `texture_a1b2c3d4.png`. The sequential names help browse
load order; use the hash-named files for replacements because they still match
if assets load in a different order. `texture_index.csv` maps each load-order
number to its stable hash and dimensions. Keep the hash filename unchanged.

## Load edited textures

Edit copies of the exported PNGs and put them in a separate folder. Then launch
with `TOXICCITY_TEXTURE_OVERRIDES` pointing to that folder:

```powershell
$env:TOXICCITY_TEXTURE_OVERRIDES = (Resolve-Path textures_mod).Path
.\toxiccity.exe --res res --save save --scale 4
```

Hash-named replacements can keep the original dimensions or use an integer 2x,
3x, or 4x enlargement with the same aspect ratio. Enlarged PNGs are
downsampled to the original game texture size when loaded, since the game still
draws in its original 240x320 pixel space. Keep alpha transparency where
present. Numeric filenames are also supported; if you edit one, it takes
priority over an unchanged hash-named copy. Skipped replacements and loaded
files are recorded in `texture_override.log` beside the executable.

## About higher-resolution art

The game logic and framebuffer use the handset's 240x320 coordinate space.
`--scale 4` enlarges that output for display but does not increase its detail.
The current replacement hook therefore preserves source dimensions. Truly
higher-resolution textures would also require scaling sprite-sheet crop
coordinates and rendering the game into a larger logical framebuffer; simply
dropping in a 2x PNG would select the wrong frames and draw at the wrong size.
