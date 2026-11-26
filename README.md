# Q2 Playdate

An experimental Playdate compatibility project for the Shanling Q2 running Q2 Pod. The first goal is to run a small, Lua-only Playdate demo from an unpacked `.pdx` directory as a standalone microSD app under **Library → Games**.

**Status:** M4 adds experimental audio, saves, timers and procedural sprites. The Lua loader has passed desktop and MIPS32 emulation tests. Physical Q2 testing is pending.

## Try the crank demo

Build the card ZIP using a MIPS32 little-endian Linux cross-compiler with its target zlib development files, `readelf`, `zip`, Go 1.23 or newer, and system CA certificates:

```sh
make card BUILD=build/mipsel CC=mipsel-linux-gnu-gcc LDFLAGS=-static
```

The build uses `pdc` from PATH or downloads the pinned Linux compiler from Panic with `curl` and `tar`. Run `make sdk` to fetch it separately, or pass `PDC=/path/to/PlaydateSDK/bin/pdc` to use your own SDK.

Extract `build/mipsel/q2-playdate-card.zip` to the microSD root. In Q2 Pod, reopen **Library → Games → Q2 Playdate**. Copy trusted, unpacked Lua bundles to `.playdate/games/<title>.pdx/` and reopen the app to refresh the library. The built-in Crank Demo is always the first card. Other cards use their folder names and generic artwork. The single-screen picker supports up to 63 user bundles.

| Q2 control | Library | In a game |
| --- | --- | --- |
| Wheel | Browse covers | Crank |
| Previous / Next | Browse covers | Left / Right |
| Return / Play-Pause | Exit / Get Games | Up / Down |
| Centre | Open selected game | A |
| Touch | Swipe, tap side covers, or tap the centre cover to play | B or menu |
| Hold Return | Exit | Exit |

The in-game menu offers Resume, Game Library, and Exit to Q2 Pod. It sits above the game picture and B sits below it. The Crank Demo doubles crank travel while A is held and resets the marker on B. Q2 Pod stops music when launching the app so it can forward the side keys. Returning to the library or exiting a game writes frame timing and peak process RSS to `.playdate/last-run.txt`.

The 400 × 240 game picture scales to 320 × 192 using centered bilinear smoothing. The picture is centered in the Q2’s 375 × 320 view. On the 320 × 375 framebuffer, the renderer writes it 90° clockwise, following the same screen mapping as q2-pico8. It flips framebuffer pages when supported and writes to the visible page otherwise. Graphics support includes rectangle outlines and one-pixel lines. The runtime supports a small graphics API, crank deltas, and held/just-pressed/just-released button queries. The loader resolves bundled Lua imports in `main.pdz`. Games may still require Playdate APIs that are unavailable. The current player requires an unencrypted Lua bundle. The picker displays the loader or Lua error and stays open.

## Download free games from itch.io

Connect the Q2 to Wi-Fi. In the game library, press **Play/Pause** or tap **Get Games** to open itch.io's free Playdate catalog. Browse with the wheel or Previous/Next, press **Centre** to download, and press **Return** to go back or cancel. The library refreshes when you return.

The downloader accepts guest-accessible ZIPs containing one unpacked Lua bundle with `main.pdz`. It skips paid downloads, ARM binaries and encrypted bundles. A downloaded game may still require Playdate APIs the player does not implement. Only launch games from creators you trust. Downloads are staged on the microSD card, with a 48 MiB download and 64 MiB unpacked limit. Existing installs are preserved. The helper uses the packaged TLS trust bundle, which can be supplied at build time with `Q2_CA_BUNDLE`.

Run `make check-itch` for the host-side downloader checks.

## M4 compatibility

| Bundle | Exercises | Status |
| --- | --- | --- |
| Crank Demo | Wheel, D-pad, simultaneous A and crank, rectangle graphics | Desktop simulation |
| Save and Tone | A presses, procedural sprite, one-shot timer, saves and square-wave audio | Desktop simulation with a PCM test sink |

In **Save and Tone**, press Centre to advance the marker and play a note. Reopen the game to check that the marker is restored. An underline below the markers indicates a save error. Saves stay in `.playdate/saves/` and are scoped to the bundle folder name. The player supports `playdate.datastore.read/write` for nested tables, booleans, finite numbers and strings, with a 64 KiB file limit.

The audio path targets the built-in headphone jack via ALSA at 48 kHz. The Lua sound API can play one square-wave voice. Bluetooth and USB audio, samples, and image sprites are pending. SDK CoreLibs can be imported when bundled, but modules that depend on missing native APIs can still fail. The procedural sprite subset supports custom drawing and position/size changes. The timer subset supports callbacks and repeating timers advanced by `playdate.timer.updateTimers()`. Images support `draw`, `drawScaled`, `drawRotated` (centered, nearest sampling, angle in degrees, optional scales) and `drawFaded`. The Lua `debug` library is open, matching the device runtime — official CoreLibs such as `CoreLibs/utilities/where` (which calls `debug.getinfo`) depend on it. The `json` global supports `encode`, `encodePretty`, `decode`, `decodeFile` and `encodeToFile(path, [pretty], table)` (the file forms go through `playdate.file`), plus `json.null`. `buttonIsPressed` / `buttonJustPressed` / `buttonJustReleased` accept the `kButton*` constants or the strings `"a"`, `"b"`, `"up"`, `"down"`, `"left"`, `"right"` (any case — Frogger passes all-caps `"UP"` / `"LEFT"` for movement). Draw patterns: `setPattern(table)` (8 row bytes, bit set = white, optional second 8 numbers as an alpha mask) and `setPattern(image, [x, y])` (8x8 block of the image), `getPattern()`, and `setDitherPattern(alpha, [ditherType])` (default Bayer 8x8; the `kDitherType*` constants live on `playdate.graphics.image`). `setColor` / `setPattern` / `setDitherPattern` are mutually exclusive, per the SDK. A dither uses the current colour: black draws `alpha` of the pixels, white inverts it (the SDK's known bug: 1.0 transparent, 0 opaque). Round rects (`fillRoundRect` / `drawRoundRect`, 5 numbers or a rect) and line caps (`setLineCapStyle` / `getLineCapStyle`, `kLineCapStyleButt` / `Square` / `Round`) are implemented. Images support masks (`addMask` / `removeMask` / `hasMask` / `clearMask` / `setMaskImage` / `getMaskImage`; `getMaskImage` returns a copy, not a live view). Fonts support embedded PDZ imports, height, leading and tracking; glyph rendering is still pending. Sound is silent stubs: `fileplayer`, `sampleplayer` and `channel` (`new` / `addSource` / `setVolume` / `setPan` / effects) accept the calls games make and play nothing.

## playdate.file

`playdate.file` is implemented with a two-root path model. Reads check the per-game saves directory first, then the bundle directory (matching the SDK's `kFileRead` rule: Data folder first, then the `.pdx`). Writes, `mkdir`, `delete`, and `rename` operate only in the saves directory; bundles are read-only on the device. A leading `/` maps to the saves root, so XTRIS's `/Shared/...` lands in `.playdate/saves/Shared/...`. Paths with `..` components are rejected. Supported: `open` (`kFileRead` = 1, `kFileReadData` = 2, `kFileWrite` = 4, `kFileAppend` = 8), file `read` / `readline` / `write` / `flush` / `seek` / `tell` / `close`, plus `exists`, `isdir`, `listFiles` (merged bundle + saves listing, directories with a trailing `/`), `mkdir`, `delete`, `getSize`, `getType` (`"file"` / `"directory"`), `modtime`, and `rename`. `read(n)` returns up to `n` bytes; `read()` with no argument reads the rest of the file; both return `nil` at end of file. Each read is capped at 1 MiB and at most 64 files may be open at once. `playdate.metadata` is populated from the bundle's `pdxinfo` (bundleID, version, buildNumber, and the other keys as strings); a missing `pdxinfo` yields an empty table. `playdate.file.load` / `run` (`.pdz` loading) is not implemented.

## Build the Lua probe

On Ubuntu or WSL, install Git, Make, a C compiler, Python 3, and the zlib development headers (`zlib1g-dev`). Then run:

```sh
make check
```

The standalone probe uses the same pinned Playdate-compatible Lua fork as Cranked to verify the bundle format and VM before integrating a fuller runtime. In headless mode the probe reads `main.pdz` from an unpacked bundle and runs its `main` Lua chunk once. It supplies an empty `playdate` table and supports bundled imports for trusted test fixtures. In headless mode the frame loop and card display are inactive.

To build with a MIPS32 little-endian Linux compiler and its target zlib development files:

```sh
make BUILD=build/mipsel CC=mipsel-linux-gnu-gcc LDFLAGS=-static
```

With QEMU user-mode installed, run the same fixtures against that build with `make check BUILD=build/mipsel CC=mipsel-linux-gnu-gcc LDFLAGS=-static RUNNER=qemu-mipsel`.

## Upstream projects

- [Cranked](https://github.com/TheLogicMaster/Cranked) is the candidate runtime. It loads Playdate bundles and implements portions of the Playdate SDK. Its current core build links Unicorn for native ARM execution, so the first build task is to make the Lua path work independently.
- [Q2 Arcade](https://github.com/lampsonlnguyen/q2-arcade) provides a reference for the MIPS32 card app launcher, Linux framebuffer, and Q2 Pod input socket.
- [CrankBoy](https://github.com/CrankBoyHQ/crankboy-app) runs Game Boy and Game Boy Color games on a Playdate. It is a separate port candidate for Game Boy emulation.
- [Playdate SDK documentation](https://sdk.play.date/inside-playdate) describes the graphics, input, audio, and file APIs the runtime needs to support.

## M5 native ARM feasibility probe

The separate MIPS32 probe builds the ARM-only Unicorn revision pinned by Cranked. It loads a real, unencrypted `pdex.bin` produced by Playdate SDK 2.7.1 from an original Thumb fixture. It checks the init event argument, the return address, and the result of a 10,000-iteration loop. It reports cold-call time, 120 warm-call timings, and peak process RSS.

On Ubuntu or WSL, use CMake, a MIPS little-endian Linux C toolchain with static target zlib, Arm GNU binutils, the Playdate SDK's `pdc`, and QEMU user-mode:

```sh
make check-native BUILD=build/mipsel CC=mipsel-linux-gnu-gcc \
    PDC=/path/to/PlaydateSDK/bin/pdc RUNNER='qemu-mipsel -cpu XBurstR2'
```

To build the hardware test ZIP, run the same command with `native-card` in place of `check-native`. Extract `build/mipsel/q2-playdate-native-probe.zip` to the microSD root, keeping the hidden `.pdprobe` directory. On the Q2, open **Library → Games → Playdate ARM Probe** and press **Centre**. The benchmark runs once and returns to Games. Open `playdate-native-report.txt` at the card root to see the timings, peak RSS, and final PASS or FAIL. Each launch replaces the report. A report containing only the start line indicates the probe did not finish. The Lua player and this optional diagnostic install independently.

The proof covers ARM execution only. The fixture has no relocations, BSS, API calls or graphics. The regular card app still supports Lua bundles. Native game support will need a general loader, the Playdate C API bridge, and measurements on the device.

## Milestones

- [x] **M0 — Source investigation.** Identify a runtime that loads Playdate bundles, review its dependencies, and find a Q2 Pod app to use as a platform reference.
- [ ] **M1 — Lua-only MIPS32 build.** Make Cranked's Lua path build without Unicorn or the desktop frontend. Verify that the cross-compiled executable starts on the Q2 and loads a minimal, unencrypted Lua-only `.pdx` directory.
- [ ] **M2 — First playable demo.** Package the runtime as a Q2 Pod card app. Render the Playdate's 400 × 240 frame at 320 × 192, map wheel deltas to crank rotation, and test a demo that uses sprites and the crank. Record frame time and peak RSS on a physical Q2 and verify that Back exits cleanly to Q2 Pod.
- [ ] **M3 — Controls and game library.** Add a microSD game picker, map D-pad and A/B inputs, test games that combine button presses with crank movement, and return a clear error for unsupported bundles.
- [ ] **M4 — Audio, persistence, and Lua compatibility.** Add Q2 audio output and per-game save data. Test graphics, sprites, timers, input, and save/relaunch behavior across several unencrypted Lua homebrew titles. Document supported titles and their limitations.
- [ ] **M5 — Native ARM feasibility.** The pinned Unicorn build executes an SDK-packed `pdex.bin` fixture in a MIPS32 user process under QEMU. The optional Games entry records timing and memory to microSD. Collect a physical Q2 report before deciding whether to integrate broader native support.
- [ ] **M6 — First public release.** Publish a reproducible MIPS build and installable microSD ZIP, with installation steps, controls, and a compatibility list based on hardware testing. A Lua-only release can ship if M5 remains experimental.

## Hardware and compatibility

The [Q2 has 128 MB of RAM with no swap](https://github.com/DiamondBond/q2-pod/blob/main/docs/hardware.md). Measure runtime memory and frame pacing alongside Q2 Pod on physical hardware. The touch wheel can stand in for crank rotation, while games that require precise crank angles or simultaneous D-pad input need individual testing.

An initial compatibility target is an unpacked, unencrypted Lua-only `.pdx` directory. Native C/C++ games contain ARM code and require the work in M5. Cranked currently reports that encrypted Catalog games are unsupported.
