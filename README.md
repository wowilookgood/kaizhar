# Kaizhar

Kaizhar is an unofficial Switch 2 port of the [Azahar](https://github.com/azahar-emu/azahar) 3DS emulator, not an Azahar release: report problems to this fork, not to the Azahar project.

It needs a Switch 2 running kaiHEN (release 20261113).
<!-- LINK PLACEHOLDER: link kaiHEN here once its repository is public -->

Modified from Azahar 2126.2 (commit 219d743), November 2026. Upstream's README: [README.upstream.md](README.upstream.md).

## Status

Docked, replaying recorded input with caches filled, the five titles below and Xenoblade Chronicles 3D hold 99 to 100% speed after the first five seconds. Fire Emblem (0009F100), Shovel Knight and Pokémon Moon ran 60 s from empty caches without falling behind.

The table gives lag, the time the frame limiter gave up.

| Title | First launch, empty caches, first 55 s (3 runs) | Cached launch, whole route of 85 to 160 s (1 run) |
|---|---|---|
| Pokémon Omega Ruby | 19–22 ms | 0 ms |
| Ocarina of Time 3D | 0 ms | 5 ms |
| Kid Icarus: Uprising | 118–157 ms | 86 ms |
| Mario Kart 7 | 39–57 ms | 0 ms |
| Super Smash Bros. | 1–28 ms | 71 ms |

The table is from a development build. The release build lost 214 ms over the five first launches together. In handheld (a development build of 2126.2-kaizhar2, one first launch each with recorded input): Pokémon Omega Ruby, Ocarina of Time 3D and Kid Icarus: Uprising hold 98 to 100% speed after their first five seconds.

A first launch compiles code and shaders as it goes: expect short hitches and breaks in the sound. New 3DS titles get an emulated CPU clock that follows what the console keeps up with: a 130 s Xenoblade route held 29.8 to 30.0 fps.

Known issues:

- Kid Icarus: Uprising stutters in its first five seconds on a first launch.
- The CPU clock is raised while a title loads (docked) and while a New 3DS title runs (handheld too). Heat and battery cost are unmeasured.
- No camera.
- Microphone input works in a test program (record and play back); no commercial title was tried.
- Untested: touch in the right-side-up portrait layout, motion in portrait, quitting during a shader cache rebuild.

## Install

No games are included.

- Copy `kaizhar.nro` to `/switch/kaizhar.nro` and start it from the HEN's homebrew menu.
- Put your own dumps in `/switch/Kaizhar/roms`: `.3ds`, `.cci`, `.cxi`, `.app`, `.3dsx`, or decrypted `.cia`.
- A `.cia` is listed as "Install ..." and can be deleted afterwards. Installed titles and saves are in `/switch/Kaizhar/sdmc/Nintendo 3DS/`, 3DS homebrew in `/switch/Kaizhar/sdmc/3ds/`.

An existing `/switch/Azahar` folder (earlier test builds) is renamed to `/switch/Kaizhar` on first start, unless `/switch/Kaizhar` exists.

### What a release build writes and sends without being asked

Everything is under `/switch/Kaizhar` on the card:

- `log/`: the emulator's log (`kaizhar_log.txt`), a report of the last session (`last-session.txt`), what the program left behind as it closed (`exit.txt`), and after a crash or a hang `crash.txt` or `hang.txt`. The one before each is kept as `.old`.
- `mic-service.txt`: whether the lookup of the system's audio input service returned. Where it never did, the microphone stays off at later starts until the file is deleted.
- `ouncevk/`: the graphics driver's shader cache, and a mark (`copied-from-shared`) that the shared cache folder's files were offered to it once.
- `sdmc/3ds/dspfirm.cdc`: a placeholder, written only if the file is absent. 3DS homebrew looks for it; a file you put there is never touched.
- Shader caches, lists of compiled blocks, the list of installed titles and the settings of the Settings page.

A release build that is sent to the console from a computer over the network connects back to that computer and sends its log lines there, as a development build does. Started from the homebrew menu it opens no such connection.

## Controls

Title list: A starts, X changes the layout, Y rescans, + exits. In a title, pressing both sticks in returns to the list.

| 3DS | Switch 2 |
|---|---|
| A, B, X, Y, D-pad, L, R, ZL, ZR | the buttons with the same labels |
| Start, Select | +, − |
| Circle Pad, C-Stick | left stick, right stick |
| Touch screen | the touch screen, or a Joy-Con as a mouse |
| Motion | the console's sensor in handheld, else the controller's |

Five layouts: stacked, large top screen, side by side, two portrait. Buttons are not remapped in portrait. Slide a Joy-Con on a surface and a crosshair appears on the bottom screen; its L/ZL or R/ZR then press the stylus.

## Building

You need the ounce devkit 20261117, with the graphics driver the release is built on: upstream ouncevk at f31c0d8 (tag 20261120), or the published ouncevk package once one carries that tag. The release is tested with fragment-shader barycentrics not used. Fetch the submodules and apply `hen/patches` (commands at the top of `hen/tools/configure.sh`), then:

```
DEVKITPRO=/path/to/devkit hen/tools/configure.sh build
cmake --build build --target kaizhar_nro
```

The result is `build/src/citra_switch/kaizhar.nro`.

The port's code is in `src/citra_switch` and `hen/` (`hen/README.md`). An interpreter runs code until it is hot, and a title's compiled blocks are rebuilt ahead on its next launch.

## Credits and licence

Azahar is the work of the Azahar Emulator Project, and is based on Citra. Making fragment shader configurations canonical before hashing is an idea from dekopon, a Switch 1 port of Azahar. The menu font is Inter (`src/citra_switch/menu_font.LICENSE.txt`). The icon is original to this port.

GPLv2 or any later version, as Azahar. See `license.txt`.
