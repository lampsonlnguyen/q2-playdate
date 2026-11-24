# Q2 Playdate

An experimental Playdate compatibility project for the Shanling Q2 running Q2 Pod. The first goal is to run a small, Lua-only Playdate demo from an unpacked `.pdx` directory as a standalone microSD app under **Library → Games**.

**Status:** M2 has an experimental microSD demo. The Lua loader has passed desktop and MIPS32 emulation tests. Physical Q2 testing is pending.

## Try the crank demo

Build the card ZIP using a MIPS32 little-endian Linux cross-compiler with its target zlib development files, `readelf`, `zip`, and the Playdate SDK's `pdc` compiler:

```sh
make card BUILD=build/mipsel CC=mipsel-linux-gnu-gcc LDFLAGS=-static PDC=/path/to/PlaydateSDK/bin/pdc
```

Extract `build/mipsel/q2-playdate-card.zip` to the microSD root so `.playdate/q2app`, `.playdate/q2-playdate`, and `.playdate/demo.pdx` are directly inside the hidden app folder. On Q2 Pod, reopen **Library → Games** and select **Q2 Playdate**. Spin the wheel to move the marker and press Back to exit. After a clean Back exit, `.playdate/last-run.txt` records frame count, update/render timings, and peak process RSS.

The bundled demo uses a small set of Lua APIs for drawing rectangles and reading crank movement. The 400 × 240 image scales to 320 × 192 and is centered on the Q2 display.

## Build the Lua probe

On Ubuntu or WSL, install Git, Make, a C compiler, Python 3, and the zlib development headers (`zlib1g-dev`). Then run:

```sh
make check
```

The standalone probe uses the same pinned Playdate-compatible Lua fork as Cranked to verify the bundle format and VM before integrating a fuller runtime. In headless mode the probe reads `main.pdz` from an unpacked bundle and runs its `main` Lua chunk once. It supplies an empty `playdate` table and is intended for trusted test fixtures. It does not provide the Playdate SDK APIs, load imported chunks, call the frame loop, or open the card app framebuffer.

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

## Milestones

- [x] **M0 — Source investigation.** Identify a runtime that loads Playdate bundles, review its dependencies, and find a Q2 Pod app to use as a platform reference.
- [ ] **M1 — Lua-only MIPS32 build.** Make Cranked's Lua path build without Unicorn or the desktop frontend. Verify that the cross-compiled executable starts on the Q2 and loads a minimal, unencrypted Lua-only `.pdx` directory.
- [ ] **M2 — First playable demo.** Package the runtime as a Q2 Pod card app. Render the Playdate's 400 × 240 frame at 320 × 192, map wheel deltas to crank rotation, and test a demo that uses sprites and the crank. Record frame time and peak RSS on a physical Q2 and verify that Back exits cleanly to Q2 Pod.
- [ ] **M3 — Controls and game library.** Add a microSD game picker, map D-pad and A/B inputs, test games that combine button presses with crank movement, and return a clear error for unsupported bundles.
- [ ] **M4 — Audio, persistence, and Lua compatibility.** Add Q2 audio output and per-game save data. Test graphics, sprites, timers, input, and save/relaunch behavior across several unencrypted Lua homebrew titles. Document supported titles and their limitations.
- [ ] **M5 — Native ARM feasibility.** Check whether the Unicorn version used by Cranked can execute an ARM Playdate binary on the Q2's MIPS host. Test one unencrypted `pdex.bin` demo and measure frame time and memory. Proceed with native-game support if those results are practical.
- [ ] **M6 — First public release.** Publish a reproducible MIPS build and installable microSD ZIP, with installation steps, controls, and a compatibility list based on hardware testing. A Lua-only release can ship if M5 remains experimental.

## Hardware and compatibility

The [Q2 has 128 MB of RAM with no swap](https://github.com/DiamondBond/q2-pod/blob/main/docs/hardware.md). Measure runtime memory and frame pacing alongside Q2 Pod on physical hardware. The touch wheel can stand in for crank rotation, while games that require precise crank angles or simultaneous D-pad input need individual testing.

An initial compatibility target is an unpacked, unencrypted Lua-only `.pdx` directory. Native C/C++ games contain ARM code and require the work in M5. Cranked currently reports that encrypted Catalog games are unsupported.
