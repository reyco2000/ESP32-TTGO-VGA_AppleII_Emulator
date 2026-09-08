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

The emulation core is fully decoupled from the ESP32/FabGL platform layer — `Apple2Machine` and its three owned components (`CPU`, `Memory`, `Apple2Device`) know nothing about VGA/FabGL directly; only `VGA` (src/VGA/VGA.h), the `.ino`, `src/Supervisor/`, and `Apple2Device.cpp`'s PS2 keyboard code touch `fabgl::` types.

**Main loop** (`ESP32-VGA_AppleII_Emulator.ino`): `setup()` brings up PSRAM, SD card (SPI pins 14/2/12/13, CS 13), FabGL's `DisplayController` (320x200 framebuffer scaled to 640x480@60Hz) and PS2 keyboard, then constructs `Apple2Machine` + `VGA` and calls `InitMachine()`. `loop()` runs a fixed number of CPU cycles per frame (`17050 * 4`, one Apple II video frame), renders, and presents — there's no host-speed throttling beyond the fixed cycle count.

**`Apple2Machine`** (`src/AppleII/Apple2Machine.*`) is the top-level orchestrator, owning `cpu` (CPU), `mem` (Memory), and `device` (Apple2Device) as plain members. `InitMachine()` creates memory, resets the CPU, wires `device` into `mem.device`, then `Booting()` `memcpy`s the embedded ROM images from `rombios.h` (`appleIIrom`, `diskII`) directly into `mem.rom` / `mem.sl6`.

**`CPU`** (`src/AppleII/AppleCpu.*`) is a MOS 6502 instruction-level emulator. It never touches raw memory — every read/write goes through `Memory::ReadByte`/`WriteByte`, and addressing modes (`addr_mode_ABS`, `addr_mode_ZP`, etc.) compute effective addresses before dispatch in `Run()`.

**`Memory`** (`src/AppleII/AppleMem.*`) implements the Apple II's segmented layout defined in `Predef.h` (`RAMSIZE=0xC000`, `ROMSTART=0xD000`, `SL6START=0xC600` for the Disk II slot-6 ROM, `LGCSTART`/`BK2START` for the 12K Language Card region). Accesses to the `$C000-$C0FF` I/O page are routed to `Apple2Device::SoftSwitch`, which in turn flips `Memory`'s `LCReadable`/`LCWritable`/`LCBank2Enable`/`LCPreWriteFlipflop` bank-switching flags — this is the one place memory banking and device I/O are entangled by design.

**`Apple2Device`** (`src/AppleII/Apple2Device.*`) is everything that isn't CPU or raw memory: two `FloppyDrive` units (nibblelized `.nib` images loaded from SD via `Tools/FileSystem.h`, sample disks in `data/`), keyboard/gamepad input, and screen rendering (text/lores/hires modes tracked via `videoAddress`/`videoPage`) into an `AppleColor` backbuffer, using bitmap glyph data from `AppleFont`.

**`VGA`** (`src/VGA/VGA.h`) is a thin wrapper around the global `fabgl::VGAController DisplayController` — `dot()`/`clear()` pack RGB222 values directly into VGA scanline buffers. This is the only translation point between the emulator's internal backbuffer and actual hardware pixels.

`src/AppleII/LodeRunner.h` contains a large embedded byte array sized to match `DISKSIZE`, but it is not currently wired into `Apple2Device::InsertFloppy` or referenced anywhere else — treat it as inert/orphaned data, not as an active code path, unless you're specifically re-wiring floppy loading.

**`Supervisor`** (`src/Supervisor/`) is the F1-activated supervisor menu: it pauses emulation (the main loop skips `machine->Run()` while active), browses the SD card, and mounts/unmounts `.nib` images via `Apple2Machine::Mount`/`Unmount` (which also manage the Disk II PROM at $C600). It owns its own `AppleColor` backbuffer and `AppleFont`, and calls `Apple2Device::InvalidateRenderCache()` on close so the emulator repaints fully.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
