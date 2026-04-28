<p align="center">
  <img src="https://res.cloudinary.com/df2kxvrua/image/upload/v1777349710/hlgraff_logo_text_banner_ankjay.png" alt="Banner" width="600">
</p>
HLGraff is a spray management launcher for GoldSrc engine games (Half-Life, Adrenaline Gamer,
Counter-Strike, Team Fortress Classic, Day of Defeat, Sven Co-op, etc.).

## Installation (Half-Life & Mods)
<details>
<summary>Click to expand</summary>
Extract the released zip package into your game root directory eg: <em>C:\Program Files (x86)\Steam\steamapps\common\Half-Life</em> (Non-steam clients are also supported) so that <strong>hlgraff.exe</strong>, <strong>spraysettings.ini</strong> and <strong>sprays</strong> folder sits next to <strong>hl.exe</strong>.<br/><br/>
Make shortcuts to <strong>hlgraff.exe</strong> and set their properties to launch your games as following:<br/>
<details>
<summary>Half-Life</summary>
"C:\Program Files (x86)\Steam\steamapps\common\Half-Life\hlgraff.exe" <strong>-game valve</strong>
</details>
<details>
<summary>Counter-Strike</summary>
"C:\Program Files (x86)\Steam\steamapps\common\Half-Life\hlgraff.exe" <strong>-game cstrike</strong>
</details>
<details>
<summary>Adrenaline Gamer</summary>
"C:\Program Files (x86)\Steam\steamapps\common\Half-Life\hlgraff.exe" <strong>-game ag</strong>
</details>
<details>
<summary>The Specialists</summary>
"C:\Program Files (x86)\Steam\steamapps\common\Half-Life\hlgraff.exe" <strong>-game ts</strong>
</details>
<details>
<summary>Earth's Special Forces</summary>
"C:\Program Files (x86)\Steam\steamapps\common\Half-Life\hlgraff.exe" <strong>-game esf</strong>
</details>
Every mod is supported by setting -game <strong>modfolder</strong> (as it's named in your game root directory)
</details>

## Installation (Sven Co-op Steam)
<details>
<summary>Click to expand</summary>
Extract the released zip package into your game root directory eg: <em>C:\Program Files (x86)\Steam\steamapps\common\Sven Co-op</em> so <strong>hlgraff.exe</strong>, <strong>spraysettings.ini</strong> and <strong>sprays</strong> folder sits next to <strong>svencoop.exe</strong>.<br/>
Make a shortcut to hlgraff.exe and set it's properties to launch your game as following:<br/>
"C:\Program Files (x86)\Steam\steamapps\common\Sven Co-op\hlgraff.exe" <strong>-game svencoop</strong>

Edit <strong>spraysettings.ini</strong> and change launch=<strong>hl.exe</strong> to launch=<strong>svencoop.exe</strong>
</details>


---
<em>
For both installation proccesses every parameter is parsed directly to the game untouched, so you can set them as if they were hl.exe or svencoop.exe itself, eg: "C:\Program Files (x86)\Steam\steamapps\common\Half-Life\hlgraff.exe" -game cstrike -nofbo -fullscreen -nomsaa -wide -novid +_set_vid_level 1 +_sethdmodels 0 -noforcemparms -noforcemaccel -noforcemspd -nodirectblit
</em><br/><br/>

Drop your desired images into the <strong>sprays</strong> folder <em>(See supported formats below)</em> and launch your games through their shortcut. <strong><em>Enjoy!</em></strong>

---

## Supported Formats
<details>
<summary>Click to expand</summary>

| Format | Extension(s) | Loader | Notes |
|---|---|---|---|
| WAD3 | `.wad` | Native parser | Single-texture: used directly. Multi-texture: frame selected by `_f` flag. |
| PNG | `.png` | GDI+ | Full alpha support. |
| JPEG | `.jpg`, `.jpeg` | GDI+ | No alpha (all pixels treated as opaque). |
| BMP | `.bmp` | GDI+ | Windows Bitmap. |
| GIF | `.gif` | GDI+ | frame selected by `_f` flag. Default: frame 0. |
| TIFF | `.tiff`, `.tif` | GDI+ | |
| Targa | `.tga` | Custom loader | Supports uncompressed (type 2/3) and RLE (type 10/11), 24-bit and 32-bit, bottom-up and top-down.|

</details>

---

## Name modifiers (Name flags)

<details>
<summary>Click to expand</summary>

This software is capable of adjusting it's behavior by filename flags.

<strong>_small</strong>/<strong>medium</strong>/<strong>large</strong> set's the output spray size.<br/>
<strong>_static</strong> sets a specific image as static (ignores every other file in the folder)<br/>
<strong>_f</strong><em>1/2/3/4/etc</em> (used for gif files and wad files with multiple textures) defines which frame is going to be used as a spray. (defaults to 0 if tag is not present, and to the last available frame if the defined frame is higher than the available frames present in the file)<br/>
<strong>_game-</strong><em>cstrike/valve/ag/etc</em> sets the image as exclusive for said mod/game<br/>

It is preferred that said flags are used at the end of the filename before the extension.<br/>
Flags can be mixed in any order eg:<br/>
Spray0432<font color="green">_game-ag</font><font color="purple">_medium</font><font color="blue">_static</font>.png (makes said image medium sized and static only for Adrenaline Gamer)<br/>
MyNiceSpray<font color="green">_large</font><font color="blue">_f3</font>.gif (uses the third frame of the file as a spray, sized in large)<br/><br/>
<font color="red">
Files without tags will be used respecting the global settings defined in <font color="Black"><strong>spraysettings.ini</strong></font>
</font>

</details>

---

## Windows Compatibility

| Version | Status |
|---|---|
| Windows 11 | Fully supported |
| Windows 10 | Fully supported |
| Windows 8 / 8.1 | Supported (requires MSVC 2017+ runtime) |
| Windows 7 | Supported (requires MSVC 2017+ runtime) |
| Windows Vista / XP | Not supported (`std::filesystem` requires kernel APIs unavailable on these versions) |

---
# Detailed Technical Documentation

<details>
<summary>Click to expand</summary>

---

## Directory Structure

```
<game root>/
├── hlgraff.exe
├── spraysettings.ini
├── last_spray.txt
├── spray_hashes.txt
└── sprays/
    ├── graffiti.png
    ├── logo_small_f2.gif
    ├── banner_game-cstrike.jpg
    ├── main_static.tga
    ├── textures.wad
    └── converted/
        ├── graffiti_large_0.wad
        ├── logo_small_0.wad
        └── banner_large_0.wad
```

| Path | Description |
|---|---|
| `spraysettings.ini` | User configuration file. Auto-generated with defaults on first run. Missing keys are appended automatically on subsequent runs without modifying existing values. |
| `last_spray.txt` | Persistent state file. Line 1: last applied spray filename. Line 2: remaining shuffle deck for random mode (space-separated filenames). |
| `spray_hashes.txt` | SHA-256 hash store. Each line: `filename:sizemode:frameindex\thash`. Used to detect image changes, size mode switches, and frame index changes. |
| `sprays/` | User spray collection. Supported formats: `.wad`, `.png`, `.jpg`, `.jpeg`, `.bmp`, `.gif`, `.tiff`, `.tif`, `.tga`. |
| `sprays/converted/` | Cache directory for converted image sprays. Managed automatically. Orphaned entries removed when `convert-autodeletion=true`. |

## Configuration — spraysettings.ini

The INI file is auto-generated on first run with all defaults. Any keys missing
from an existing file are appended automatically on the next run without
modifying existing values. Keys are case-insensitive. Inline comments use `;`.

### Settings Reference

#### `method`
**Default:** `sequential`
**Options:** `sequential` / `random` / `static`

Controls how the next spray is selected on each launch.

- `sequential` — Advances one position forward through the spray list sorted
  by Windows natural order (same order as Explorer). Wraps to the beginning
  after the last spray.
- `random` — Uses a no-repeat shuffle deck. Every eligible spray is shown
  exactly once per cycle before any repeats. The deck persists in
  `last_spray.txt`. When exhausted a new Fisher-Yates shuffle is generated.
  If the first entry of the new shuffle matches the last used spray, the deck
  rotates one position to prevent consecutive repeats at cycle boundaries.
- `static` — Always uses the spray defined in `sprayname=`. Falls back to
  the last used spray if the configured file is not found.

Note: the `_static` name flag overrides this setting entirely (see Name Flags).

---

#### `include-mods`
**Default:** `true`
**Options:** `true` / `false`

When `true`, reads the `-game` argument from the command line and copies the
spray to `<gameDir>/<mod>/tempdecal.wad`. When `false`, always writes to
`valve/` regardless of the active mod.

---

#### `sprayname`
**Default:** *(empty)*
**Example:** `sprayname=logo.png`

Used only when `method=static`. Specifies the exact filename (including
extension) of the spray to apply. If the file is not found the system falls
back to the last used spray.

---

#### `launch`
**Default:** `hl.exe`
**Example:** `launch=svencoop.exe`

The executable to launch after applying the spray. Resolved relative to the
detected game directory. All command-line arguments passed to hlgraff are
forwarded verbatim to this executable.

---

#### `size`
**Default:** `large`
**Options:** `large` / `medium` / `small`

Controls the output resolution of the spray texture. GoldSrc renders decals
at `texture_pixels / 8` world-units per side, so this directly controls
the in-game visual size. Can be overridden per-file with the `_large`,
`_medium`, or `_small` name flags.

| Mode | Area budget | Approx. output (square image) |
|---|---|---|
| `large` | 14,336 px (112×128 max) | ~112×112 |
| `medium` | 7,168 px (½ of large) | ~64×64 |
| `small` | 3,584 px (¼ of large) | ~48×48 |

All modes use proportional scaling — aspect ratio is always preserved.
Both upscaling (small sources) and downscaling (large sources) are applied
so the chosen mode has a consistent effect regardless of source resolution.
Output dimensions are always multiples of 16 as required by WAD3.

---

#### `convert-autodeletion`
**Default:** `true`
**Options:** `true` / `false`

When `true`, scans `converted/` on each run and removes `.wad` files whose
source image no longer exists in `sprays/`. Corresponding entries in
`spray_hashes.txt` are also removed.

---

#### `cycle-on-runtime`
**Default:** `false`
**Options:** `true` / `false`

> **[EXPERIMENTAL]** Servers cache sprays in `custom.hpk`. The change may
> not be immediately visible after retry or rejoin to the same server.
> For best results
> reconnect from the server browser so the client re-sends the cache.

When `true`, hlgraff stays alive after launching the game and applies a new
spray every `cycle-delay` seconds for as long as the game process is running.
The spray list and hash store are reloaded on every cycle so new images added
during a session are picked up automatically. The process exits immediately
when the game closes.

---

#### `cycle-delay`
**Default:** `60`
**Unit:** seconds (minimum: 10)

Interval between spray cycles when `cycle-on-runtime=true`. Values below 10
are clamped to 10. Invalid values fall back to 60.

---

## Name Flags

Name flags are underscore-prefixed tokens appended to the filename stem
**before the extension**, in any order. They are parsed right-to-left;
unrecognised tokens stop parsing and remain in the clean stem, ensuring
forward compatibility with future flags.

### Recognised Flags

| Flag | Effect |
|---|---|
| `_large` / `_medium` / `_small` | Overrides the global `size=` setting for this file only. |
| `_static` | This spray is always selected regardless of `method=`. Among multiple `_static` sprays the first in natural sort order wins. |
| `_game-<modname>` | Restricts the spray to a specific mod. Sprays without this flag are universal. |
| `_f<N>` | Selects frame N (0-based) from a GIF or multi-texture WAD. Ignored for single-frame formats. If N exceeds the available frames the last frame is used. Default: 0. |

### Combination Rules

- Flags can be combined in any order: `_f3_medium_game-cstrike` and
  `_game-cstrike_medium_f3` are equivalent.
- `_static` + `_game-cstrike` = always use this spray, but only in cstrike.
- If all sprays have a `_game-` flag and none matches the active mod, no spray
  is applied and the game launches with the existing `tempdecal.wad` unchanged.
- `_f<N>` on a non-animated file (PNG, JPG, TGA, BMP, TIFF, single-texture WAD)
  is silently ignored.

### Examples

| Filename | Size | Static | Mod | Frame |
|---|---|---|---|---|
| `graffiti.png` | from INI | no | all | — |
| `logo_small.jpg` | small | no | all | — |
| `main_static.png` | from INI | yes | all | — |
| `spray_game-cstrike.png` | from INI | no | cstrike only | — |
| `main_static_game-valve.png` | from INI | yes | valve only | — |
| `anim_f3_medium_game-ts.gif` | medium | no | ts only | 3 |
| `textures_f1.wad` | from INI | no | all | 1 (alphabetical) |

### Cache Filename Format

Each converted `.wad` is cached as `cleanStem_sizemode_fN.wad` where `_fN`
is omitted when N=0. Examples:

- `graffiti.png` + large + frame 0 → `graffiti_large.wad`
- `anim_f3.gif` + medium + frame 3 → `anim_medium_f3.wad`
- `logo_small.jpg` + small + frame 0 → `logo_small.wad`

---

## Internal Pipeline — Image to WAD Conversion

When an image or multi-texture WAD is selected, the following pipeline runs.

<details>
<summary>Click to see image conversion flowchart</summary>
  <img src="https://res.cloudinary.com/df2kxvrua/image/upload/v1777350450/image_to_wad_pipeline_lakumj.svg" width="1200">
</details>

### 1. Cache Check

Before any processing:
- SHA-256 hash of the source file is computed.
- The store key `<filename>:<sizemode>:<frameindex>` is looked up in
  `spray_hashes.txt`.
- If the key exists, the stored hash matches, and the cached `.wad` is present
  on disk → the cached file is returned immediately, skipping all conversion.
- Any of these conditions failing (modified file, changed size mode, changed
  frame, missing `.wad`) triggers full conversion.

### 2. Source Loading

The source is loaded into a 32-bit RGBA pixel buffer:

- **GDI+ formats** (PNG, JPG, BMP, GIF, TIFF): decoded via GDI+ into a
  unified 32-bit ARGB buffer. For animated GIFs, `SelectActiveFrame` is called
  with the time dimension GUID to access the requested frame. Frame index is
  clamped to the last available frame if out of range.
- **TGA**: decoded by the custom TGA loader supporting uncompressed (types 2/3)
  and RLE-compressed (types 10/11) variants, 24-bit and 32-bit depth. Bottom-up
  images (TGA default) are flipped to top-down on load.
- **WAD3 multi-texture**: the WAD directory is parsed, type-0x43 (mip-texture)
  entries are collected and sorted alphabetically by internal texture name,
  then the entry at the requested frame index is decoded using its embedded
  palette. WAD palette index 255 (the GoldSrc transparency key, opaque blue)
  is mapped to alpha=0.

### 3. Proportional Resize

The pixel buffer is scaled to a WAD-compatible resolution:
- Maximum uniform scale: `sqrt(areaBudget / sourceArea)` where `areaBudget`
  depends on the size mode (14336 / 7168 / 3584 pixels).
- Scale is stepped down in 0.5% increments until both dimensions are multiples
  of 16.
- No cap at 1.0 — small images are upscaled to fill the budget; large images
  are downscaled. The reference point is always the `large` output, so
  `medium` and `small` produce consistent relative sizes regardless of the
  source resolution.
- Resize uses bilinear interpolation with premultiplied alpha to prevent
  colour fringing at transparency edges.

### 4. Binary Transparency Normalisation

GoldSrc WAD3 supports only binary transparency:
- Pixels with `alpha >= 128` → fully opaque (alpha forced to 255).
- Pixels with `alpha < 128` → replaced with opaque blue `(0, 0, 255, alpha=0)`,
  the WAD palette index 255 transparency key colour.

### 5. Median-Cut Colour Quantization

The texture is quantized to a 255-colour palette (index 255 reserved for the
transparency key):
- A frequency histogram of all opaque pixel colours is built.
- The median-cut algorithm iteratively splits colour space into up to 255
  boxes, always splitting the box with the highest score
  (total pixel count × widest colour axis range).
- Each palette entry is the pixel-count-weighted average of the colours in
  its box, minimising expected quantization error.

### 6. Floyd-Steinberg Dithering (Serpentine)

The index map is generated using serpentine Floyd-Steinberg dithering rather
than plain nearest-colour mapping:
- A floating-point working buffer accumulates colour errors per pixel.
- Rows alternate left-to-right and right-to-left (serpentine scan) to suppress
  the directional bias that unidirectional scanning produces.
- Per opaque pixel: nearest palette entry found, quantization error computed,
  distributed to four neighbours using the 7/16 · 3/16 · 5/16 · 1/16 kernel
  (mirrored on right-to-left rows).
- Transparent pixels receive no error and contribute none, preserving crisp
  transparency boundaries.

### 7. WAD3 Assembly

The palette and index map are assembled into a valid GoldSrc WAD3 binary:
- 16-byte WAD3 header (`WAD3` magic, entry count, directory offset).
- Texture entry: name `{LOGO`, dimensions, four mip level offsets.
- Mip level 0: full-resolution index map.
- Mip levels 1–3: filled with `0xFF` (GoldSrc regenerates at load time).
- 256-entry RGB palette preceded by a 2-byte count.
- Padding to 16-byte alignment.
- Directory entry.

### 8. Cache Update

The new `.wad` is written to `converted/<cleanStem>_<sizemode>_<fN>.wad` and
the SHA-256 hash is saved to `spray_hashes.txt` under the key
`<filename>:<sizemode>:<frameindex>`.

---

## Spray Application

Before copying the selected `.wad` to `<gameDir>/<mod>/tempdecal.wad`:
- File sizes are compared (cheap check).
- If sizes match, SHA-256 hashes are compared.
- If the files are **identical the copy is skipped entirely** — no disk write,
  no attribute changes. This is the common case in static mode and
  single-spray collections, and during `cycle-on-runtime` cycles where the
  spray did not change.

When a copy is necessary:
- Read-only attribute is cleared (GoldSrc marks `tempdecal.wad` read-only).
- File is copied with overwrite.
- Read-only attribute is re-applied so the engine does not clobber it.

---

## Game Directory Detection

hlgraff searches for the game executable in order:
1. Directory containing `hlgraff.exe`
2. Parent of that directory (for mod subfolder installations)
3. Current working directory
4. Parent of the current working directory

The first directory containing the configured `launch=` executable is used
as the game root. Falls back to the exe directory if none found.

---

## SHA-256 Implementation

Implemented internally without external dependencies (FIPS 180-4, standard IV
and round constants). Used for:
- Cache invalidation: detecting modified source images.
- Copy-skip: comparing `tempdecal.wad` with the source before overwriting.

Results stored as lowercase 64-character hex strings.

---

## Persistent State Format

### last_spray.txt
```
graffiti.png
logo_small.jpg banner_medium.png tag.png
```
Line 1: last applied spray filename (all modes).
Line 2: space-separated remaining shuffle deck (random mode).
Empty or absent line 2 = deck exhausted, rebuild on next run.

### spray_hashes.txt
```
graffiti.png:large:0	a3f2c1d4...
logo_small.jpg:small:0	b7e4d923...
anim_f3.gif:medium:3	c91a0fe2...
```
Format per line: `<source_filename>:<sizemode>:<frameindex>\t<sha256_hex>`

<details>
<summary>Click to see program flowchart</summary>
  <img src="https://res.cloudinary.com/df2kxvrua/image/upload/v1777350450/hlgraff_program_flow_jnzakr.svg" width="1200">
</details>

</details>

---

## Special Thanks to
<strong>JamWayne</strong> - Inspiration.<br/>
<strong>CAGE</strong> - Logo design & tester.<br/>
<strong>Proditae</strong> - Slogan Idea & tester.<br/>
<strong>John Doe</strong> - Tester.<br/>
<strong>Nexus</strong> - Tester.<br/>
<strong>LuChOgRoX</strong> - Tester.<br/>

