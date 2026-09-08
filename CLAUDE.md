# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An Apple II emulator that runs on ESP32 hardware, rendering to VGA output via the FabGL library (LilyGO TTGO VGA32-style boards). It's an Arduino sketch project (built with arduino-cli), not a hosted/desktop emulator — everything runs on-device.

## Build commands

Built with arduino-cli (PlatformIO's `platformio.ini` is legacy config; `pio` is not installed). Entry point `ESP32-VGA_AppleII_Emulator.ino`, sources under `src/` per Arduino sketch layout.

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .   # build
arduino-cli upload  --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" -p /dev/ttyUSB0 .  # flash
arduino-cli monitor -p /dev/ttyUSB0 -c 115200                                             # serial monitor
```

Requires arduino-cli core `esp32:esp32` 2.0.x and the FabGL 1.0.9 user library.

### Targeting different hardware

The build targets a plain ESP32 (TTGO VGA32-class board) via the FQBN in the build command; adjust FQBN board options (PSRAM, PartitionScheme) for other ESP32 variants. The `platformio.ini` and `boards/*.json` files are legacy PlatformIO configuration and are not used by the arduino-cli build.

## Architecture

The emulation core is fully decoupled from the ESP32/FabGL platform layer — `Apple2Machine` and its three owned components (`CPU`, `Memory`, `Apple2Device`) know nothing about VGA/FabGL directly; only `VGA` (src/VGA/VGA.h), the `.ino`, `src/Supervisor/`, and `Apple2Device.cpp`'s PS2 keyboard code touch `fabgl::` types. Since the backbuffer was removed, `Apple2Device.cpp` and `AppleFont.cpp` also include `VGA.h` (and so, transitively, `fabgl.h`) to draw — but they use only the `VGA` wrapper, never `fabgl::` types directly. Keep it that way: `VGA` is the intended seam.

**Main loop** (`ESP32-VGA_AppleII_Emulator.ino`): `setup()` brings up PSRAM, SD card (SPI pins 14/2/12/13, CS 13), FabGL's `DisplayController` (320x200 framebuffer scaled to 640x480@60Hz) and PS2 keyboard, then constructs `Apple2Machine` + `VGA` and calls `InitMachine()`. `loop()` runs a fixed number of CPU cycles per frame (`17050 * 4`, one Apple II video frame), renders, and presents — there's no host-speed throttling beyond the fixed cycle count.

**`Apple2Machine`** (`src/AppleII/Apple2Machine.*`) is the top-level orchestrator, owning `cpu` (CPU), `mem` (Memory), and `device` (Apple2Device) as plain members. `InitMachine()` creates memory, resets the CPU, wires `device` into `mem.device`, then `Booting()` `memcpy`s the embedded ROM images from `rombios.h` (`appleIIrom`, `diskII`) directly into `mem.rom` / `mem.sl6`.

**`CPU`** (`src/AppleII/AppleCpu.*`) is a MOS 6502 instruction-level emulator. It never touches raw memory — every read/write goes through `Memory::ReadByte`/`WriteByte`, and addressing modes (`addr_mode_ABS`, `addr_mode_ZP`, etc.) compute effective addresses before dispatch in `Run()`.

**`Memory`** (`src/AppleII/AppleMem.*`) implements the Apple II's segmented layout defined in `Predef.h` (`RAMSIZE=0xC000`, `ROMSTART=0xD000`, `SL6START=0xC600` for the Disk II slot-6 ROM, `LGCSTART`/`BK2START` for the 12K Language Card region). All five blocks (~76K total) are allocated with `heap_caps_malloc(MALLOC_CAP_INTERNAL)`, **not** `ps_malloc` — the 6502 core touches them on nearly every emulated cycle, and PSRAM's SPI latency plus a 32K cache that cannot hold the working set made the interpreter roughly 1.6x slower. Plain `malloc()` is not sufficient: the Arduino core builds with `CONFIG_SPIRAM_USE_MALLOC`, so large allocations can still land in PSRAM. `Create()` logs where each block landed and warns on a PSRAM fallback. Accesses to the `$C000-$C0FF` I/O page are routed to `Apple2Device::SoftSwitch`, which in turn flips `Memory`'s `LCReadable`/`LCWritable`/`LCBank2Enable`/`LCPreWriteFlipflop` bank-switching flags — this is the one place memory banking and device I/O are entangled by design.

**`Apple2Device`** (`src/AppleII/Apple2Device.*`) is everything that isn't CPU or raw memory: two `FloppyDrive` units (nibblelized `.nib` images loaded from SD via `Tools/FileSystem.h`, sample disks in `data/`), keyboard/gamepad input, and screen rendering (text/lores/hires modes tracked via `videoAddress`/`videoPage`) **straight into the VGA framebuffer**, using bitmap glyph data from `AppleFont`. There is no intermediate backbuffer: `Render()` takes a `VGA*`, stores it for the duration of the call, and `DrawPoint`/`DrawRect`/`AppleFont::RenderFont` write scanlines directly. Redundant drawing is suppressed by the per-cell dirty caches `TextCache`, `LoResCache`, `HiResCache` (plus `previousBit` for hires colour fringing), each forced to a full repaint every 30 frames via `!flashCycle`.

**`VGA`** (`src/VGA/VGA.h`) is a thin wrapper around the global `fabgl::VGAController DisplayController` — `dot()`/`row()`/`clear()` pack RGB222 values directly into VGA scanline buffers. It is the only translation point between the emulator and actual hardware pixels; `row(y)` returns a whole scanline so callers drawing many pixels fetch the pointer once per row rather than once per pixel. Every write preserves the top two bits of each byte (`& 0xC0`), which carry the sync signals, and `x ^ 2` is the required byte-order swizzle.

**Single-buffering is load-bearing.** The dirty caches above work only because the VGA framebuffer *persists between frames* — an unchanged cell is simply not redrawn, so whatever is already in the framebuffer must still be there next frame. Enabling FabGL double buffering would silently break this: cached cells would be stale in whichever buffer was not written, showing as flickering leftover glyphs. `vga->show()` is deliberately a no-op.

`src/AppleII/LodeRunner.h` contains a large embedded byte array sized to match `DISKSIZE`, but it is not currently wired into `Apple2Device::InsertFloppy` or referenced anywhere else — treat it as inert/orphaned data, not as an active code path, unless you're specifically re-wiring floppy loading.

**`Supervisor`** (`src/Supervisor/`) is the F1-activated supervisor menu: it pauses emulation (the main loop skips `machine->Run()` while active), browses the SD card, and mounts/unmounts `.nib` images via `Apple2Machine::Mount`/`Unmount` (which also manage the Disk II PROM at $C600). It owns an `AppleFont` and, like the emulator, paints directly into the VGA framebuffer, calling `Apple2Device::InvalidateRenderCache()` on close so the emulator repaints fully. Because it shares the live framebuffer, it repaints only when its `dirty` flag is set (every keypress sets it) — clearing and repainting on every frame is visible as flicker. Any future state change that does not originate from a keypress must set `dirty` itself.

## Emulation speed

Two non-obvious things dominate performance; both were found by profiling on hardware, not by reading the code.

**Apple II memory must stay in internal SRAM** — see `Memory` above.

**`CPU::fastDiskDelay`** (`src/AppleII/AppleCpu.cpp`, default on) skips DOS 3.3 RWTS's drive spin-up wait at `$BD9E`. That loop is a pure nested software timer with no I/O, there so a physical drive can reach speed. DOS re-arms it on essentially every RWTS call, and it was consuming ~70% of all emulated cycles — Karateka took 103s to load, of which ~112 seconds of *emulated* time was this wait. Skipping it took the load to 35s. The skip verifies all 13 opcode bytes of the loop before firing and writes back the `$46`/`$47` the loop would have left, so machine state is identical; a non-matching ROM or custom RWTS just executes normally. Note this only helps DOS 3.3-derived loaders.

When investigating a slowdown, measure where emulated *cycles* go before optimising the host — the first two host-side fixes here produced zero improvement in load time because the bottleneck was the emulated machine doing 7.4x too much work.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
