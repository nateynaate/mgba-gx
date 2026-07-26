# mGBA-GX

A Game Boy / Game Boy Color / Game Boy Advance emulator for Wii,
combining the menu/UI frontend of [Visual Boy Advance
GX](https://github.com/dborth/vbagx) (by Tantric) with
[mGBA](https://github.com/mgba-emu/mgba) as the emulation core in
place of the original VBA-M backend.

## Features

- GB / GBC / GBA emulation via mGBA
- New tabbed interface - press L and R to cycle through tabs
- GBA/Game Boy Player and Super Game Boy border support (no borders
  are included with this project)
- Device color emulation
- Classic Game Boy color palettes (monochrome, green, Game Boy
  Pocket, Game Boy Light)
- Constant fast forwarding through the settings menu (2x, 3x, 4x
  speed)
- Rumble support
- Wii Remote tilt support for games like Kirby Tilt 'n' Tumble
  (experimental - still a work in progress for games like WarioWare
  Twisted)
- Cheat code support (add/edit/delete/toggle, in-game)
- IPS/UPS/BPS patch support - drop a patch file next to a ROM with a
  matching filename and it's applied automatically at load time
- Save states and SRAM saves
- Boktai solar sensor support

## Building

Built with [devkitPro](https://devkitpro.org/) (devkitPPC + libogc).

1. Install devkitPro and the `wii-dev` package group.
2. Clone this repo, then pull in submodules:

       git clone https://github.com/nateynaate/mgba-gx.git
       cd mgba-gx
       git submodule update --init --recursive

3. Build from the devkitPro MSYS2 terminal:

       make

## Installing

Copy the built `.dol`/`.elf` to an `apps/mgba-gx/` folder on your SD
card for use with the Homebrew Channel. ROM and save folders are
created automatically on first run.

## Credits

- **Tantric** - Visual Boy Advance GX (coding & menu design), the
  base this project's frontend is built on
- **softdev, emu_kidid** - original VBA GameCube/Wii port
- **The mGBA team / Jeffrey Pfau** - mGBA, the emulation core this
  project now uses
- **The VBA-M team** - Visual Boy Advance - M
- **shagkur & wintermute** - libogc/devkitPPC
- And everyone else credited in the upstream VBA-GX project this
  builds on

## License

This project is GPLv2, as a derivative of VBA-GX. The mGBA core it
links against is separately licensed under MPL-2.0. See
[LICENSES.md](LICENSES.md) for full license text and details on how
the two fit together.
