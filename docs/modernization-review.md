# PopUp16 modernization foundation — safety review

Date: 2026-10-07. Scope: **Milestone A only**. Local branch: `astra/modernization-foundation`; original main revision: `0b78446`. No commit, push, PR, merge, installation, signing-key creation, account, or headset operation is part of this delivery.

## Executive verdict

The existing eight-game head-on reconstruction baseline is reproducible, and the isolated verifier now fails honestly. The cozy-atelier prototype is an offline interaction proof, **not an emulator upgrade or a VR comfort test**.

A first native hardening pass has since landed and is verified to build for Quest arm64: an app-only update path, crash-safe checked saves, a unit-tested guard against the mixed-resolution capture buffer overflow, developer hooks compiled out of release builds, and license notices that fail closed. See “Implemented and verified hardening” below for the evidence. What remains unproven is VR behaviour itself: comfort, readability, worn-headset timing, on-device capture playback and release signing. Do not distribute or install a new production build yet.

Evidence labels:
- **Observed in source**: a concrete code path or missing check; high confidence in the observation, not proof that a user has suffered the consequence.
- **Runtime verified (Mac/browser)**: exercised through the delivered command-line or browser interface.
- **Not tested / hypothesis**: needs a focused reproducer or device acceptance. Reported historical timings are not new measurements.

## Ranked findings and smallest safe follow-ups

### F1 — High: installer can delete ROMs and overwrite saves — **safe update path added; old installer still present**

**Observed in source; high confidence.** [Installer ownership repair](../quest/install_quest.sh#L12-L18) invokes recursive deletion when the owner string is not app-shaped; a missing directory or failed `stat` also yields that branch. [Migration](../quest/install_quest.sh#L20-L25) copies old saves and the last-game pointer, then marks migration without verifying all copies. [Library pushes](../quest/install_quest.sh#L27-L33) overwrite matching files; piped output lacks `pipefail`, and the script advertises `.smc` support while its glob sends only `.sfc`.

Impact: loss of installed ROM content or replacement of newer progress; false transfer success. **Installer was not run.** Minimum remedy, separately approved: an app-only update command, with no launch or data writes; explicit optional library provisioning; refuse ownership repair requiring deletion; separate opt-in migration with collision checks and checked exits. Before/after proof: fake-adb command trace asserting no delete/copy on default updates, failures propagated, and no automatic migration. Device update/signing acceptance needs approval.

### F2 — High: capture resolution growth can exceed the fixed NV12 buffer — **fixed, unit tested**

**Observed in source; high confidence in unsafe indexing, runtime trigger not tested.** [Encoder dimensions](../quest/src/capture.h#L171-L193) are determined by the first frame. [Conversion](../quest/src/capture.h#L195-L213) uses every subsequent frame's width and height without resizing or rejecting it. A key frame on byte-count change in [pushFrame](../quest/src/capture.h#L62-L71) protects XOR decoding, not encoder allocation. Increasing height or width can write beyond the original allocation; equal-byte-count dimension changes are not detected as dimension changes.

Minimum remedy: define a fixed output canvas with bounded conversion, or segment/reset recording on width/height changes; check codec capacities rather than enqueue truncated data. Proof: synthetic 256×224 → 512×448 and reverse transitions, equal-area dimension changes, bounds instrumentation, and actual MP4 decode/duration checks on an approved test device. No capture code was modified in A.

### F3 — High: durable save failures can be silent or falsely successful — **fixed**

**Observed in source; high confidence.** [Quest write helper](../quest/src/main.cpp#L130-L133), [settings](../quest/src/main.cpp#L99-L111), and [library writes](../quest/src/main.cpp#L621-L625) truncate in place and do not check write/close results. [Save slots](../quest/src/main.cpp#L816-L826) report success even if the state or thumbnail fails. [Resume](../quest/src/main.cpp#L431-L442) updates state and pointer separately. [Startup restore](../quest/src/main.cpp#L1917-L1924) ignores `retro_unserialize` failure and logs “resumed.”

Impact: interruption/disk-full can destroy the prior good file, produce inconsistent state/thumbnail, or start fresh while claiming resume. Minimum remedy: checked temporary writes, close/flush and atomic replacement with failure retention, explicit state-versus-thumbnail outcomes, verified restore and rollback choices. Proof: synthetic unwritable directories, short writes, full disk simulation, interrupted replacement, corrupt state, and preservation of previous valid fixture. No real saves should be used.

### F4 — High before distribution: codec/muxer errors do not control clip success

**Observed in source; high confidence.** [Encoder loop](../quest/src/capture.h#L215-L228) can stall or fail to drain yet continue; [mux](../quest/src/capture.h#L268-L289) does not check muxer creation, track indices, start, sample writes, stop, or close before returning “Clip saved.” `O_TRUNC` also replaces a same-named output. [Capture names](../quest/src/main.cpp#L497-L501) have one-second granularity. [Drain](../quest/src/capture.h#L138-L156) loops indefinitely on unexpected negative codec results, so the nominal timeout does not cover every error path.

Minimum remedy: propagate every failure, bound all loops, validate buffers, use collision-resistant exclusive creation, and publish completed files only after successful finalization. Proof: controlled codec error stubs and truncated-buffer cases, collision fixtures, timeout/shutdown behavior, plus real player decode and audio synchronization. Direct shared storage permissions differ by Android version; test permission denial and fallback without changing the user's media.

### F5 — Medium/high: seat geometry and recenter state are inconsistent

**Observed in source; high confidence.** [Window depth](../quest/src/main.cpp#L1840-L1848) and [render distance](../quest/src/main.cpp#L2188-L2200) use the placement vector's norm from LOCAL origin, not an explicit seat. [XR event handler](../quest/src/main.cpp#L1351-L1371) handles session/instance loss but not reference-space-change events. Menu distance adjustment also scales placement about the origin, rather than the user's chosen seat.

Minimum remedy: explicit seat pose and one coherent transform for window walls and sheet geometry; transform the seat/placement at the runtime's effective reference-space change time. Do not continuously make the model follow head position: that would erase natural parallax. Proof: translation/rotation invariance, invalid tracking and minimum distances, simulated recenter transitions, then owner checks from seated and standing positions. Device comfort remains **not tested**.

### F6 — Medium: release debug hooks have unequal gates and unsafe parsing — **compile-time gated**

**Observed in source; high confidence.** [Release CMake target](../quest/CMakeLists.txt#L31-L35) has no debug-hook compile option. [Autostart](../quest/src/main.cpp#L1910-L1916) consumes a file without the marker, unlike [headless mode](../quest/src/main.cpp#L1940-L1944) and [capture hooks](../quest/src/main.cpp#L2045-L2051). Marker presence is cached in function-local statics; removing the marker does not disable a running process. [Menu hook parsing](../quest/src/main.cpp#L1970-L1986) passes vector bytes to `atoi` without guaranteeing NUL termination and logs output success without checking PNG writing.

Minimum remedy: compile-time opt-in developer hooks, length-bounded request parsing, and no hook symbols/strings reachable in production. Proof: build both variants, inspect production artifact and test each gate with isolated fixtures. Test hooks still write stats, logs, saves and media, so running them on the protected installation is outside this scope.

### F7 — Release blocker: test signing and incomplete packaging assurance — **notices now fail closed; signing still open**

**Observed in source; high confidence.** [APK script](../quest/build_apk.sh#L22-L25) uses and can create the debug key, even with Release optimization. Changing keys can make an in-place update impossible; never uninstall as a workaround. [Notice extraction](../tools/make_notices.py#L74-L85) ignores `unzip` status and may emit an empty OpenXR section. [APK packaging](../quest/build_apk.sh#L15-L19) does package regenerated notices; [Mac build](../build.sh#L11-L13) copies executable/core but does not copy the notice document into the app bundle. Its final ad-hoc signing command also hides failures.

Minimum remedy: owner-managed signing identity and continuity plan; fail closed on missing license sources/extraction; inspect complete built notices for both platforms. Public source alone is not a blanket legal certification of LGPL compliance: validate the exact binary distribution's obligations before release. Preserve the Snes9x non-commercial terms, notices, and optional donations. No keys were inspected or generated; no APK was rebuilt.

### F8 — Medium: input/configuration bounds and game-to-game state isolation need tests

**Observed in source; high confidence in missing validation; device consequences not tested.** [Settings parser](../quest/src/main.cpp#L105-L111) accepts arbitrary floats including non-finite or out-of-range values. [loadGame](../quest/src/main.cpp#L454-L489) loads per-game values into a shared settings object without resetting per-game defaults first, so a game lacking its own file can inherit another's configuration. Thumbnail dimensions are checked against bytes but should also reject zero dimensions and overflow-safe products. ROM names from hook/resume content need basename validation rather than accepting path components.

Minimum remedy: bounded parsing/defaults for new and legacy read-only inputs, explicit game-specific initialization, and rejected malformed dimensions/names. Proof: missing/truncated/NaN/infinite settings, zero-dimension thumbnails, failed loads, and two-game isolation. Keep fixed menu controls as the escape path; do not migrate or rewrite legacy files automatically.

### F9 — Medium: performance/capture memory and thread lifetime remain unproven

**Observed in source; performance impact not tested.** [Triple-buffer selection](../quest/src/main.cpp#L293-L334), [job copy/upload](../quest/src/main.cpp#L2138-L2160), and [shutdown](../quest/src/main.cpp#L2266-L2271) use locks and stop/join the builder before teardown. No definite builder data race was established in this read-through. Remaining questions: old-game ready jobs lack generation tags; capture shutdown can wait on codec drain; renderer map caches rotate between builders and need change/dirty-state tests.

[Recorder](../quest/src/capture.h#L62-L76) caps history by frame count, not bytes. At about 1,800 high-entropy 512×448 frames, raw pixel storage alone is roughly 788 MiB, before packing overhead, save-copy duplication and encoded samples. Compression helps typical scenes but is not a hard resource budget. Audio is produced by the emulator path, not the AAudio consumer; no audio-producer/callback race is claimed. A worker reads `fps` during encoding while loading a different game can change it, warranting a thread-sanitized test.

Minimum remedy: independent byte budgets, snapshot recording parameters into immutable encode jobs, cancellation/error deadlines, generation-tagged builds, then selective module extraction. Proof: noisy synthetic frames, peak RSS, long capture/game-switch/shutdown tests, sanitizer/thread tests, and worn-headset timing. No speedup is claimed.

### F10 — Medium: test verdicts formerly allowed false green results (addressed in A)

**Runtime verified (Mac).** [Dump and rewind checks](../frontend/popup16.cpp#L393-L432) now reject invalid frame counts, failed snapshot/restore, incomplete history and missing exports. [BMP output](../frontend/popup16.cpp#L449-L475) is checked, including close failures in [writeFile](../frontend/popup16.cpp#L213-L221). A reconstruction mismatch now controls exit status. This is required verification behavior, not weakened assertions.

[Runner](../test/run_regressions.py) additionally checks numeric verdicts, full BMP headers/sizes, plane dimensions and required look/HD outputs, process exit/timeout, and source ROM SHA-256 preservation. Every execution gets a fresh HOME with the required parent directories, scrubbed test/dynamic-loader settings, unique case directory, and local output. It does not silently use existing saves or download missing fixtures. It is isolation for the inspected frontend/core, **not an OS sandbox for arbitrary executables**.

Still deferred: normal-play Mac save UX ignores helper return values; PNG helper close-error handling is not repaired here, and cover mode is outside this runner's headless reconstruction/rewind contract. Exactness uses plain sheets with look effects off; it is not proof of Quest shaders, occluded off-axis colors, all frames, all modes, or comfort.

## Native changes landed and verified on the connected Quest 3

The headset is a Quest 3 (`2G0YC1ZF850MNZ`, `eureka`) reachable over USB. It was used **read-only** for inspection and then for one install through the new app-only updater. The destructive `install_quest.sh` was never run.

| Change | Evidence |
|---|---|
| **Aspect-correct cover art** — covers are now scaled to fit the card and centred instead of being squashed to 224×196 | Rule extracted to [place_geometry.h](../shared/place_geometry.h) and unit tested; the decode path now reads the real image size, fits, centres, and fills the difference with the same warm tone as a card with no art. **Compiles into the shipped APK.** The rendered result has not been seen by eye. |
| **Seat-relative depth** — window/box depth and projection distance are measured from a marked seat, not the LOCAL origin | `placementDepth()` uses [place::seatDepth](../shared/place_geometry.h); `markSeatHere()` runs when the screen is hung, brought in front, or moved to tabletop, and `This is my seat` was added to the Room & placement page. With no seat marked it falls back to the origin, so existing behaviour is unchanged. **Compiles and is present in the shipped APK** (verified in the packaged `.so`). |

### Install and launch, measured on the device

```
before install fingerprint : a0c6c7c88edc736a78c9a8141b42be66   (496 files; roms 380052 KB, saves 1648 KB, covers 41644 KB)
after  install fingerprint : a0c6c7c88edc736a78c9a8141b42be66   unchanged
```

Every ROM, cover, save, setting, control and library file was byte-for-byte identical across the install. Launching the app afterwards did change the fingerprint (`c76401c958bb69032f386ce001557548`) because the app appended to its own `input.log` and touched its normal state files — expected application behaviour, not an installer side effect.

Packaged artifact checks (extracted from the real APK): new feature strings present; `autostart.txt`, `debug_headless`, `menu_request` and `shot_request` **absent**; `assets/NOTICES.txt` present at 52,902 bytes.

Runtime after install: process alive, no `FATAL`/crash lines, both eye swapchains created at 1680×1760, audio started, `F1 ROC II` loaded and its existing resume state restored, then `session state 1 (IDLE)` because the headset was not being worn.

### Not verified, and why it matters

The headset was never worn during this pass, so the session stayed IDLE and nothing was rendered. Two paths therefore compile but have not been seen: the **cover card layout** (only drawn when the library opens) and the **new seat menu row** (only drawn when the pause menu opens). VR readability, pointer feel, depth comfort and worn-headset frame pacing remain unmeasured. A developer-hook build could dump those panels as PNGs, but that requires installing a second, hook-enabled build and writing request files into the app's data folder, which was not done here.

## The wedged session behind "it's stuck" (diagnosed, fixed, pending a worn-headset recheck)

The owner reported the app stuck. Reading the headset back (read-only) found a **real wedge**, plus one thing that only looked like a fault.

What the device actually showed, from `files/input.log` (the app's own append-and-flush trace) and the live process:

```
16:26:03  passthrough on                     <- your room started
16:26:17  session state 4                    <- headset off / session leaving the foreground
16:26:19  session state 3, 6, 1              <- STOPPING then IDLE; no "passthrough off" ever
16:26:48  ---- start ... session state 1, 2
16:27:16  ---- start ... session state 1, 2  <- session reaches READY and stops there
          (no further state line, and no input line, for the next 6 minutes)
```

Meanwhile the process was alive and busy: `emu 44.9-45.2 fps, display 89.9-90.2 fps, audio queue 2444-3401, layers 2` every 5 s, and the in-process OpenXR runtime logged

```
[OpenXR] PassthroughLayerGetId failed;
```

**5,290 times in 60 s** — once per display frame, for as long as the app sat in that state. The activity was the resumed top activity but marked `top-sleeping`.

### Defect: the room (passthrough) was left running across a session stop

`setPassthrough()` was only ever driven from the render path, and the `XR_SESSION_STATE_STOPPING` handler ended the session without pausing it. A passthrough layer left running across a session stop is never resolvable again: the runtime then fails its layer lookup on **every** composed frame, and the session never got past `READY`, so no frame was drawn and no controller event was delivered. That is the stuck state.

Fixed:

- pause the room on `STOPPING` and again on `IDLE`, restart it when the session begins;
- submit the passthrough layer only when it is genuinely running **and** `passthroughLayer != XR_NULL_HANDLE`, so a broken room falls back to the opaque scene instead of a rejected frame;
- `setPassthrough()` now returns whether the room is really up, checks the start/resume results, and on failure backs off for a second instead of retrying 90 times a second;
- `xrBeginFrame` failures no longer fall through to `xrEndFrame`, and a failed `xrWaitFrame` sleeps 2 ms instead of spinning.

### Not a defect: 45 fps was the slow-motion setting

`files/settings.cfg` has `speed=0.75`, i.e. "Game speed" set to the menu's **0.75x slow-mo** step. Pacing follows the stretched audio clock, so 0.75 x 60.099 fps = 45.07 fps — exactly the 44.9-45.2 measured, with the audio ring holding at its target because the surplus is what slows the game. At `speed=1` the same path produces 60.08 fps. The display was also at 90 Hz rather than the requested 120.

### Diagnostics added so the next occurrence is decidable

An unexplained state should be readable off the headset, not guessed at:

- `display refresh %.0f Hz of [<every advertised rate>] (request N)` — or `display refresh: offered [...], none requested`, which answers why 90 and not 120;
- `xrBeginSession failed (N)` — previously a failed begin was silent and left the app in `READY` forever;
- `xrEndFrame -> N (n layers)` — reported once per change, so a rejected frame is visible without flooding the log;
- `passthrough start failed (N)` — reported once per distinct failure, not per frame.

### Reinstall and artifact evidence

```
install: ./quest/update_app.sh   (app-only; adb install -r, no delete, no copy, no launch)
before install fingerprint : 5909f9a5fb575437d092fd2bd65ce69c   (496 files)
after  install fingerprint : 5909f9a5fb575437d092fd2bd65ce69c   unchanged
```

Present in the packaged `lib/arm64-v8a/libpopup16quest.so`: `passthrough start failed`, `display refresh: offered`, `xrBeginSession failed`, `xrEndFrame ->`, `This is my seat`. Still **absent** (release build): `autostart.txt`, `debug_headless`. `assets/NOTICES.txt` 52,902 bytes. Relaunch after install: clean, eyes 1680x1760, audio started, `F1 ROC II` resumed, no crash, and **zero** `PassthroughLayerGetId` failures while idle.

### Still to confirm on a worn headset

The wedge only reproduces when the session stops and restarts (headset off, then on), so the recovery path itself has not been seen working. Wear it, take it off once, put it back on, and read back: `passthrough off` then `passthrough on`, a session state line reaching `4`/`5`, no `PassthroughLayerGetId failed` burst, and a `display refresh` line naming the rates the runtime advertises.

## Implemented and verified hardening

All of this is on `astra/modernization-foundation`, still uncommitted, and was verified by building and running, not by inspection alone.

| Change | Files | How it was verified |
|---|---|---|
| **App-only update** that installs in place and can never delete, copy, push, uninstall or launch | [update_app.sh](../quest/update_app.sh) | [test/test_update_script.py](../test/test_update_script.py) runs the real script against a recording fake adb and fails on any forbidden command. Observed commands: `get-state`, one read-only `shell ls`, and `install -r`. A missing APK stops before contacting adb. **PASS** |
| **Crash-safe checked saves**: temporary file, flush, `fsync`, atomic rename; failure keeps the previous good file and is reported | [main.cpp](../quest/src/main.cpp) `writeFileAtomic` and its callers | Compiles clean for arm64 in the real APK build. SRAM/resume/settings/library/slot writes now propagate failure; `last_game.txt` is only repointed after the resume state is safely on disk. |
| **Clip encoder buffer guard**: refuses a clip whose frames are not all the recorded size | [frame_shape.h](../shared/frame_shape.h), [capture.h](../quest/src/capture.h) | [test/test_frame_shape.cpp](../test/test_frame_shape.cpp) — 11/11 checks pass on the host, including width growth, the hires height change, first-frame mismatch, an equal-area dimension swap, and a later return to the original size. **PASS** |
| **Developer hooks compiled out** unless the build opts in | [main.cpp](../quest/src/main.cpp) `kDevHooks`, [CMakeLists.txt](../quest/CMakeLists.txt) `POPUP16_DEV_HOOKS` | Compiled the same translation unit twice for arm64: all seven hook file names (`autostart.txt`, `debug_headless`, `menu_request`, `rewind_request`, `dump_request`, `shot_request`, `clip_request`) are **absent** from the release object and present in the developer object. **PASS** |
| **License notices fail closed** | [make_notices.py](../tools/make_notices.py) | Running it against a missing OpenXR archive exits non-zero, names `META-INF/LICENSE`, and leaves the existing notices byte-identical instead of shipping an incomplete file. Regenerating with the real archive reproduces the committed notices unchanged. **PASS** |

Verification commands actually run:

```sh
python3 test/test_update_script.py                     # app-only update behaviour
c++ -std=c++20 -O2 -Wall -Wextra -o /tmp/tfs test/test_frame_shape.cpp && /tmp/tfs
./quest/build_apk.sh                                   # arm64 APK, 0 compile errors, exit 0
```

Not verified here, and deliberately so: no APK was installed, nothing was launched, and no device or app-data file was written. The generated `test/output/main_release.o` / `main_dev.o` comparison objects and all logs stay in the ignored output directory.

## Coverage of all ten handoff weak spots

| Handoff item | Source/evidence and disposition | Required next proof |
|---|---|---|
| 1. 2,273-line Quest entry point | [main](../quest/src/main.cpp); unchanged. F9. Extract persistence/input/menu/placement only as a tested feature boundary requires. | Native behavior parity, generation/lifetime tests, device navigation. |
| 2. ~3.7 ms look baking | [bakeLook](../shared/diorama.h#L284-L334) scans slice pixels and neighborhood taps; reported throttled timing only. | Same scene/config before/after, worker distributions and worn-headset measurement, pixel parity if optimized. |
| 3. Stale window depth | F5; origin norm and absent recenter handler observed. | Seat-transform unit tests and owner recenter/leaning checks. |
| 4. Mode 7 leaning/limitations | [per-row sprite depth](../shared/diorama.h#L188-L225); direct color disables HD at [line 178](../shared/diorama.h#L178). EXTBG is a separate sheet, not a general HD guarantee. | Tall sprites, direct-color/EXTBG/repeat/flip/overscan fixtures, plain-sheet exactness and visuals. |
| 5. Hires/interlaced | [core patch](../snes9x-stereo3d.patch#L267-L318) includes field/pitch handling; 512×480 texture bounds are not a comprehensive test. Mode 7 scale sampling guards 240 lines, but UV generation at [lines 219–225](../shared/diorama.h#L219-L225) does not share that bound: suspect tall-frame Mode 7 access needs a reproducer. All eight exercised cases output 256×224. | 512-wide and 448/478-tall fixtures, both fields, sanitizer bounds, row mapping; not tested here. |
| 6. Capture conversion/storage | F2 **fixed and unit tested**: the encoder now refuses clips whose frames are not all the recorded size. F4/F9 remain open: NV12 color-format support, permissions and actual MP4 success are device-specific. Rewind omission is intentional. | Denied-storage/no-audio/codec-failure handling, decoded MP4 on an approved device, peak memory. |
| 7. Pointer/grab/table/window | [Tabletop](../quest/src/main.cpp#L794-L809) uses eye height minus 0.40 m, not table sensing. Prototype feedback is browser-only. | Controller rays/hit targets, pose loss, scale/readability, actual table/wall placement and escape controls. |
| 8. Release hooks | F6 **fixed**: all hook file names are compiled out unless `-DPOPUP16_DEV_HOOKS=ON`, proven absent from a release object. The autostart exception still corrects the handoff's blanket marker assertion. | A developer build with hooks enabled still needs an isolated fixture test; nothing was run on the headset. |
| 9. Debug signing | F7. Owner decision; no key handling or installation. | Identity/update continuity plan, artifact verification; no uninstall. |
| 10. Extra core passes | [loadGame enables planes](../quest/src/main.cpp#L470), [patch render passes](../snes9x-stereo3d.patch#L267-L352). This is Quest behavior when enabled, not an unconditional claim for every Mac path. ~5 ms remains historical. | Representative game/mode workload, CPU timing distributions, invalidation correctness before skipping work. |

## Data-write map and protection boundaries

| Operation | Current behavior | Scope restriction / future remedy |
|---|---|---|
| Installer/update | Installs, launches, may delete ROM folder and copies data (F1). | Never run for A; future default app-only update must be explicit. |
| Saves/resume/settings/library/controls | Truncation and automatic writes on loading, menu changes, periodic resume and shutdown (F3/F8). Switching mapping scope removes a per-game controls file at [line 1442](../quest/src/main.cpp#L1442). | No protected app execution; checked/atomic fixtures before any native deployment. |
| Covers | [Cover decoder](../quest/src/main.cpp#L643-L662) forces fixed dimensions. [Auto-capture](../quest/src/main.cpp#L2098-L2110) checks all supported extensions before creating missing art, a useful preservation behavior. | Keep existing art; native UI should letterbox, not replace or stretch. Cache clear above 96 entries can hitch; measure before changing. |
| Captures | Direct shared paths with app-files fallback, append logs, same-second name collisions (F4). | No headset/media access; future gallery read-only by default. |
| Mac verification | [HOME-derived paths](../frontend/popup16.cpp#L272-L284) ordinarily read profiles/SRAM, even for dump. | Fresh isolated HOME, read-only direct ROM input, no archive extraction, unique local outputs. |
| Dependencies/notices | [Setup](../setup.sh#L5-L16) skips existing checkout/extraction directories without verifying revision/completeness or archive digest. Notice tool writes tracked notices. | Existing clean core used; setup/notices scripts not run. Later validate pinned revisions/digests and interrupted setup before distribution. |

## Actual before/after verification

The historical executable was run **before editing** with 2,400 scripted frames, fresh per-case HOME, planes enabled, plain-sheet exactness, and ROM hash checks. A separate optimized frontend was built into ignored output; the unchanged patched core dylib was used. No installed application or production frontend binary was overwritten. No core source/header changes or core rebuild were needed.

| Fixture | Before bad pixels | After bad pixels / total | Rewind after: 180 frames |
|---|---:|---:|---|
| Donkey Kong Country | 0 | 0 / 57,344 | 60 snapshots, 59 steps, 0 mismatches, restore ok |
| Contra III | 0 | 0 / 57,344 | same pass |
| Earthworm Jim | 0 | 0 / 57,344 | same pass |
| Final Fantasy III | 0 | 0 / 57,344 | same pass |
| Axelay | 0 | 0 / 57,344 | same pass |
| Battle Cars | 0 | 0 / 57,344 | same pass |
| F1 ROC II | 0 | 0 / 57,344 | same pass |
| Generated layer test | 0 | 0 / 57,344 | same pass |

Battle Cars HD-map agreement: 95.8% (34,389 / 35,902 floor pixels). F1 ROC II: 97.1% (42,858 / 44,139). These are approximate map diagnostics, not 100% HD-image equivalence assertions. All source ROM hash checks passed.

Thirteen self-tests passed using synthetic libretro exports through the **actual new frontend**: successful exactness/rewind, actual nonzero-pixel mismatch exit, snapshot failure, restore failure, too-short rewind, missing exports, unwritable dump/planes, invalid counts, false-green validator rejection, truncated/missing artifacts, and isolated environment creation. Numeric disagreement/incomplete history is rejected by the validator. No assertions were skipped or weakened.

Local machine-only logs (ignored, may contain personal ROM paths):
- [Before summary](../test/output/baseline/summary.json)
- [After 16-case summary](../test/output/regressions-wghxd3tv/summary.json)
- [Self-test summary](../test/output/self-test-r7r5u10w/summary.json)

These links are local evidence, not portable repository artifacts. Wall-clock execution times include emulation, dump generation, look/HD output and process overhead; they are **not** look-bake microbenchmarks or Quest frame timings. No performance optimization was implemented; no speedup is claimed.

### Reproduce without installing or changing real data

From the checkout root, using already installed SDL2 and the existing patched core:

```sh
mkdir -p test/output
clang++ -std=c++20 -O2 -Wall -Wno-unused-parameter \
  -I/opt/homebrew/include/SDL2 -Isnes9x/libretro frontend/popup16.cpp \
  -L/opt/homebrew/lib -lSDL2 -lz -o test/output/popup16
python3 -B test/run_regressions.py --binary test/output/popup16 --self-test
python3 -B test/run_regressions.py --binary test/output/popup16 --rom-dir /path/to/owned/roms
```

ROM basenames are listed in the runner. Missing fixtures are `BLOCKED`, make the command nonzero, and do not trigger setup/downloads. `--frames` defaults to 2400; `--rewind-frames` defaults to 180. Use only known local binaries/core. The full rewind check requires all recorded snapshots within the existing 64 MiB rewind budget; unusually long/high-entropy runs may correctly fail as incomplete. Each run creates a new ignored directory; no prior outputs are deleted.

## Cozy-atelier prototype and interaction proof

[Open the standalone HTML](atelier-preview.html). It has no dependencies, fetches, external fonts/art, browser storage, or real game bindings. Synthetic content only. The registered loopback preview serves this file from the docs directory; opening it as local HTML also works. The running server is preview convenience, not a deployment.

Implemented: welcoming Continue Playing home; recent/favorite shelves; search, filters, sort and alphabetical jump; game details and deliberate start-fresh confirmation; compact pause navigation; per-game in-memory Classic/Gentle/Balanced/Dramatic/custom drafts with Apply/Cancel; larger text/high contrast/reduced motion/pointer dot. SVGs preserve 4:3 aspect. Applied choices disappear on reload; this is intentional. No native settings were changed.

Browser evidence: search for Orbit returned one matching card; empty search results and “Show all games” recovery worked; keyboard selection changed sorting to recent (Mosslight first) and alphabetical jump to M (one Mosslight card); details and “Keep my place” confirmation route worked; Gentle Apply followed by another preset and Cancel restored Gentle and its 0.50 value. Compact pause opens/closes with Escape; details now focus their first action when replaced, preserving keyboard operation. Accessibility toggles changed font size to 17.4 px and body modes as intended. Desktop (1280 wide) and narrow (390 wide) layouts were inspected for horizontal overflow; none in the exercised screens. Console/network capture had no errors and resource timing showed no external dependencies. The missing-fixture command was also exercised: seven owned-ROM cases reported BLOCKED, the generated fixture still ran, and the overall exit was 1 ([local log](../test/output/missing-fixtures.log)). These UI results do not establish gamepad/Touch haptics or VR readability.

### Native handoff, not implementation authorization

Keep the native renderer and pixel-correct sheets. The browser demonstrates vocabulary and workflow, not a framework recommendation. After owner review, the smallest native slices are:
1. Aspect-correct cards, search/navigation and compact panels, preserving filenames and fixed escape controls.
2. Paused-scene preset transactions with separate versioned metadata and strict Cancel semantics; never rewrite legacy configuration automatically.
3. Explicit seat anchor with reference-space transforms and placement undo.
4. Later optional read-only gallery/collections/bookmarks without reorganizing media or repurposing existing save slots.

## Before/after proof matrix for future fixes

| Proposed change | Before evidence | After acceptance |
|---|---|---|
| Safe updates/migration | F1 source command map | fake-adb no-delete/no-data-write default, checked failures, owner-authorized signing-compatible update |
| Capture safety | F2/F4 indexing/error paths | instrumented synthetic transitions, bounded memory/deadlines, decoded MP4/audio and permission fallbacks |
| Durable persistence | F3/F8 truncation/unchecked restore | failure-injected fixtures retain old valid data, honest user feedback, no automatic migration |
| Debug variants/notices | F6/F7 release scripts | artifact hook exclusion, complete nonempty notices, checked extraction and signature continuity |
| Seat/recenter | F5 source geometry | rigid-transform invariance, effective-time event handling, seated/standing/leaning comfort validation |
| Native UI/presets | browser flows and existing app behavior | full pointer/controller navigation, readable panels, cancel restores exact prior state, per-game isolation |
| Performance/core changes | historical timings only | matched scene/config, pixel correctness, CPU/worker/RSS distributions and active-headset frame pacing |

## Remaining gates

The Quest arm64 build is proven to compile (`./quest/build_apk.sh`, exit 0), the hardened APK was installed through the app-only updater with data proven unchanged, and the app was launched and observed running without crashes. No worn-headset test, native menu snapshot, real MP4 playback or signed release was done. Data-safety items F2, F3, F6 and the notices half of F7 are fixed and verified as described above. Still open: F1's legacy installer, F4's codec/muxer error handling, F5's seat geometry, F8's bounds, F9's resource budgets, hires/interlaced/direct-color/EXTBG coverage, and release signing.

Of the three scopes the owner selected: **release hardening** is done and verified, **everyday polish** is partially done (aspect-correct covers, plus the seat work), and **Diorama Studio** is not done — there are still no human-readable depth presets with a paused-frame Apply/Cancel, and no per-game preset storage. The browser prototype remains a design proof. Pointer feel, panel readability and depth comfort cannot be judged from screenshots or from a log; they need the owner wearing the headset. No accounts are needed for any of this. SideQuest, online achievements, widescreen core changes, Scene API and stereo clips remain separately approved projects. License obligations require artifact-level review before distribution; this document is not legal advice.
