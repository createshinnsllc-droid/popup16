# PopUp16

Play your 16-bit console games in layered 3D on Meta Quest. PopUp16 runs the
[Snes9x](https://github.com/snes9xgit/snes9x) emulator core and turns each graphics
layer (backgrounds, sprites, status bar, Mode 7 floor) into a sheet floating at its
own depth, like a pop-up book. Move your head and the scene has real parallax.

**No games are included.** Use only game files you are legally entitled to use.

## Features

- **Diorama 3D on Quest 3:** every layer drawn alone and placed in real 3D. Sheets
  follow the console's own priority order, so the picture seen head-on is exactly the
  original frame (tested pixel for pixel).
- **Mode 7 floors rebuilt in 3D and HD:** each scanline's distance comes from the game's own Mode 7
  zoom, forming one continuous sloped surface, and the floor is redrawn from the full 1024x1024
  track map at headset resolution. Racers stand on it; skylines sit beyond the horizon.
- **Comfort limits** scaled to your eye spacing, so your eyes never have to diverge.
- 60 fps emulation on a 120 Hz display; quick resume where you left off.
- Rewind (hold the left stick in), slow motion and fast speeds, four save slots with pictures.
- Mixed reality: place the game in your room, carry it, resize it, peek into the pop-up box.
- **Library:** cover grid with Recent, Favorites and All tabs; playtime and last played per game.
  Covers are your own: `tools/make_covers.sh <rom folder>` picks the best title-screen frame of
  each game, any `covers/<game>.png` or `.jpg` you add wins, and games without one get a cover
  captured after 30 seconds of play. No artwork ships with PopUp16.
- Touch controllers or a Bluetooth gamepad; in-headset settings and controls card.
- Mac version (SDL2) with side-by-side output for Virtual Desktop's 3D mode.

## Build

Requires macOS with Xcode command line tools and Homebrew.

```sh
./setup.sh            # fetch Snes9x and apply snes9x-stereo3d.patch, fetch the OpenXR loader
./build.sh            # Mac app -> ~/Applications/PopUp16.app
quest/build_apk.sh    # Quest APK -> quest/build/popup16.apk (needs Android SDK + NDK 27)
```

## Install on Quest

Enable developer mode, connect over USB, then:

```sh
quest/install_quest.sh "/path/to/your/game/folder"
```

Games go to `Android/data/com.createshinns.popup16/files/roms` on the headset (`.sfc`/`.smc`);
covers in `<rom folder>/covers` are copied to `files/covers`.
`tools/curate_roms.py` can pick one clean USA or English-translated file per game from
GoodTools-style `.7z` sets.

## Controls (Touch)

| Game | Quest |
|---|---|
| Move | either thumbstick |
| B / A | right A / right B |
| Y / X | right trigger / left trigger |
| L / R | left grip / right grip |
| Start | left menu button or right stick click |
| Select | left X |
| Rewind | hold the left stick in |
| PopUp16 menu | left Y |

## How it works

`snes9x-stereo3d.patch` makes the Snes9x renderer record which layer drew each pixel
and also draw each main-screen layer on its own. `shared/diorama.h` turns those layers
into depth-sorted sheets; `quest/src/renderer.h` draws them as geometry in an OpenXR
projection layer.

## License

PopUp16's own code is MIT licensed (see `LICENSE`). That license covers only PopUp16's
code: Snes9x and the other components keep their own licenses, listed in full in
`THIRD_PARTY_NOTICES.txt`. The Snes9x license allows non-commercial use only, so builds
that include the Snes9x core may not be sold. Super NES and Super Nintendo Entertainment System are
trademarks of Nintendo; this project is not affiliated with or endorsed by Nintendo.

Made by TyDroElite / CreateShinns LLC.
