# PopUp16 roadmap

Every week ships **one quality-of-life improvement** and **one novel upgrade**, each on its own branch with
a pull request, test evidence and a short note in the changelog below. The owner merges.

Pick from the top of each list unless the owner asked for something else. Add new ideas as they come up,
and strike items through when they ship (with the PR link).

## Quality of life (one per week)

1. ~~**Cursor and text size for comfort:**~~ **partly shipped (PR #6):** high-contrast menu option. Still open:
   larger body text ("large text" setting) and a pointer size option.
2. **Recently played on launch:** open straight into the last game, with a one-tap "Choose another game"
   card instead of the full library.
3. **Library search:** jump by first letter with the right stick; show the letter while scrolling.
4. **Per-game controller presets:** suggested layouts for platformers, shooters and RPGs, picked from the
   game's genre the first time it runs.
5. **Rewind and fast-forward feedback:** a small on-screen indicator (and rewind seconds remaining).
6. **Sleep and battery safety:** pause and save when the headset is taken off; warn at low battery.
7. **Save slot labels:** rename a slot from a short list of presets (Boss, Before shop, ...).
8. **Comfort presets:** "Gentle 3D", "Normal", "Deep" one-click depth/convergence sets.
9. **Better covers:** let the cover picker skip text-only screens; "Use current screen as cover" in the menu.
10. **First-run tour:** three short cards (controls, moving the game, room mode).

## Novel upgrades (one per week)

1. **Widescreen worlds:** draw background layers past the 4:3 edges from the full tilemaps in the core, so
   the world wraps around the window.
2. ~~**Sprite thickness:**~~ **shipped (PR #7):** 4 mm card edges in pop-up box style. Still open: edge colour from the sprite palette.
3. **Living backdrop:** a slow parallax sky beyond the game's backdrop, matched to its colours.
4. **Mode 7 sky dome:** curve the horizon backdrop of Mode 7 racers into a dome around the track.
5. **Hand presence:** see your controllers as small SNES pads in the room.
6. **Diorama lighting:** a light direction you can move with the controller (shadows follow).
7. **Tabletop board mode:** lay overhead games (Zelda-style, RPG maps) flat on the table like a board game.
8. **Shared spectator view:** cast a stable 2D or side-by-side 3D view to a phone or PC.
9. **Per-game depth profiles:** hand-tuned layer depths for the most played games (DKC, Contra III, FF3).
10. **Achievements:** RetroAchievements support (needs the owner's own account and key).

## Ground rules for every change

- Branch `claude/<topic>` (or `astra/<topic>`), pull request into `main`; never push to `main` directly.
- `python3 test/run_regressions.py --binary frontend/popup16 --core snes9x/libretro/snes9x_libretro.dylib
  --rom-dir "/Volumes/AI MAIN DRIVE/snes-usa-english"` must stay all PASS; add a test for what you change
  when the Mac tools can check it.
- Quest builds must compile: `quest/build_apk.sh`. Do **not** install on the headset, launch the app, or
  simulate wearing it unless the owner asked; never while it might be worn.
- Never touch ROMs, covers, saves or settings on the Mac or the headset. Never commit ROMs or personal paths.
- Keep the licence obligations: Snes9x notices ship with every build; donations stay optional.

## Changelog

| Week | Quality of life | Novel upgrade |
|---|---|---|
| 2026-10-07 | Library, remapping, capture, pointer menus, grab bar, recenter (PRs #1-#4) | Diorama renderer, Mode 7 3D floor, room mode, window in the wall |
| 2026-10-09 | High-contrast menu option (PR #6) | Sprite thickness, 4 mm card edges (PR #7) |
