# Kaizhar

Kaizhar is an unofficial Switch 2 port of the [Azahar](https://github.com/azahar-emu/azahar) 3DS emulator. Report problems here, not to the Azahar project.

It needs a Switch 2 running [kaiHEN](https://github.com/ouncebrew/kaihen).

Modified from Azahar 2126.2 (commit 219d743) in November 2026. Azahar's own README is [README.upstream.md](README.upstream.md).

## Status

Performance and stability are improved since 2126.2-kaizhar2. A title's first launch compiles code and shaders as it runs, so expect short hitches and breaks in the sound.

## Features

- Five screen layouts: stacked, large top screen, side by side and two portrait layouts. All are drawn at 4x the 3DS's resolution.
- 120 Hz in handheld mode, 60 Hz on a TV.
- The right stick is the C-Stick in titles that use one (New 3DS and Circle Pad Pro titles). In other titles it moves a pointer on the bottom screen, and clicking the stick touches the screen.
- Touch with a finger, with a Joy-Con used as a mouse, or with the stick pointer. Settings sets the pointer's size, style and speed, and how long it stays on screen.
- Local wireless play through rooms. From the Multiplayer page (− on the title list) you can host a room on the console, find rooms on your network, or join one by address.
- Online play through Pretendo, set up once with your own 3DS.
- HTTPS requests from titles and system modules.
- Microphone input.
- A Settings page (L on the title list). Its Clear caches entry deletes Kaizhar's shader and code caches and leaves saves alone.
- Decrypted `.cia` files install from the title list.
- New 3DS titles run on an emulated CPU clock that follows what the console can keep up with. While one runs, and while any title loads, the console's CPU clock is raised to 1523.2 MHz, docked and handheld. That is the rate the system's own boost mode uses, not an overclock.

## Install

No games are included.

- Copy `kaizhar.nro` to `/switch/kaizhar.nro` and start it from the HEN's homebrew menu.
- Put your own dumps in `/switch/Kaizhar/roms`: `.3ds`, `.cci`, `.cxi`, `.app`, `.3dsx`, or decrypted `.cia`.
- A `.cia` is listed as "Install ...". Once it is installed, the `.cia` file can be deleted. Installed titles and saves are in `/switch/Kaizhar/sdmc/Nintendo 3DS/`.
- 3DS homebrew goes in `/switch/Kaizhar/sdmc/3ds/`. The 3DS Homebrew Launcher is not included: put your own copy there and it is listed with the rest.

A `/switch/Azahar` folder left by an earlier build is renamed to `/switch/Kaizhar` on first start, unless `/switch/Kaizhar` already exists.

### Online play

Online play is set up under Multiplayer > Online play (Pretendo). It needs your own 3DS once, with Luma3DS 13.3.1 or newer and the Artic Setup Tool open on it, and Nimbus (`nimbus.cia` in `roms`, the `3ds` folder from its zip in `sdmc`). The page says what comes next: Link a 3DS, then Online system modules, which are off until you turn them on, then System Settings for the Pretendo ID. The same page starts HOME Menu for the friend list and friend codes.

If something gets in the way:

- "No 3DS found": the Artic Setup Tool must be open on the 3DS, on the same wifi. If the tool was used before, close it and open it again.
- Nimbus says "Nimbus has been updated!" at every start: its `3ds` folder is missing. Copy it from the zip so that `/switch/Kaizhar/sdmc/3ds/nimbus/` exists.
- Error 022-5432: in Nimbus switch to Nintendo, then back to Pretendo, and try again.
- Nimbus takes no touch when docked with the controllers attached. Use handheld mode.
- The linked 3DS and Kaizhar share one identity. Only one of them can be online at a time.
- If nothing helps, choose Unlink the 3DS on that page and link again.

## Controls

On the title list, A starts a title, X changes the layout, Y rescans, − opens Multiplayer, L opens Settings and + exits.

In a title, press both sticks in to return to the list. Clicking the left stick changes the layout, swaps the screens or mutes the sound, whichever is chosen in Settings.

| 3DS | Switch 2 |
|---|---|
| A, B, X, Y, D-pad, L, R, ZL, ZR | the buttons with the same labels |
| Start, Select | +, − |
| Circle Pad | left stick |
| C-Stick | right stick, in titles that use one |
| Touch screen | the touch screen, a Joy-Con as a mouse, or the right stick as a pointer |
| Motion | the console's sensor in handheld mode, otherwise the controller's |

Buttons are not remapped in the portrait layouts. To use a Joy-Con as a mouse, slide it on a surface. A pointer appears on the bottom screen, and ZL or ZR touches the screen.

## Known issues

- After an update, the first start of each title is slower while its shader cache is rebuilt. This happens once.
- Encrypted `.cia` files are refused.
- No camera.
- If the microphone stays off, delete `/switch/Kaizhar/mic-service.txt`. It records that the system's audio input service did not answer.

## Files and network

Everything Kaizhar writes is under `/switch/Kaizhar` on the SD card: its caches and settings, `multiplayer.txt` and `rooms.txt` for the Multiplayer page, and the logs in `log/` (`kaizhar_log.txt`, `last-session.txt`, `exit.txt`, and `hang.txt` after a hang). The previous copy of each log is kept as `*.old.txt`.

If Kaizhar is sent to the console from a computer over the network, it connects back to that computer and sends its log there. Started from the homebrew menu, it does not.

Link a 3DS downloads the 3DS system software from Nintendo's update servers, with the linked 3DS's device ID. If the console's DNS has no usable answer for those servers, Kaizhar asks 1.1.1.1, then 8.8.8.8 (`dns=` in `multiplayer.txt` names others).

With the online system modules on, the Friend List fetches the icon of a friend's game from Nintendo's icon server (`idbe-ctr.cdn.nintendo.net`), once per title each time Kaizhar runs. The request carries no certificate or account data. The server sees your IP address and the title's ID. Setting up online play has already contacted Nintendo's servers by then.

## Building

You need:

- the ounce devkit 20261117
- the Vulkan driver, ouncevk, at upstream commit f31c0d8 (tag 20261120): built from source, or the published ouncevk package once one carries that tag

Fetch the submodules and apply `hen/patches` (the commands are at the top of `hen/tools/configure.sh`), then:

```
DEVKITPRO=/path/to/devkit hen/tools/configure.sh build
cmake --build build --target kaizhar_nro -j 8
```

The result is `build/src/citra_switch/kaizhar.nro`.

The port's code is in `src/citra_switch` and `hen/` (see `hen/README.md`). Code runs in an interpreter until it is hot, and a title's compiled blocks are rebuilt ahead on its next launch.

## Credits and licence

Azahar is the work of the Azahar Emulator Project and is based on Citra. The idea of making fragment shader configurations canonical before hashing comes from dekopon, a Switch 1 port of Azahar. The menu font is Inter (`src/citra_switch/menu_font.LICENSE.txt`). The icon is original to this port.

Licensed under GPLv2 or any later version, like Azahar. See `license.txt`.
