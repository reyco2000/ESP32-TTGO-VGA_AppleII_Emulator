# Architecture

How the emulator is put together, and — more importantly — the handful of
constraints that are not visible from reading the code. Several of these were
found by profiling on real hardware; breaking them silently costs performance
or correctness rather than failing the build.

## The shape of it

The emulation core is fully decoupled from the ESP32/FabGL platform layer.
`Apple2Machine` and its three owned components (`CPU`, `Memory`,
`Apple2Device`) know nothing about VGA or FabGL directly. Only `VGA`
(`src/VGA/VGA.h`), the `.ino`, `src/Supervisor/`, and the PS/2 keyboard code in
`Apple2Device.cpp` touch `fabgl::` types.

`Apple2Device.cpp` and `AppleFont.cpp` do include `VGA.h` (and so, transitively,
`fabgl.h`) in order to draw — but they use only the `VGA` wrapper, never
`fabgl::` types directly. **Keep it that way: `VGA` is the intended seam.** It is
the single place where the emulator meets real hardware pixels, and porting to
another display backend should mean rewriting that one file.

```
ESP32-VGA_AppleII_Emulator.ino     setup/loop, FabGL + SD + PS/2 bring-up
  └── Apple2Machine                 orchestrator
        ├── CPU                     MOS 6502 interpreter
        ├── Memory                  segmented address map, Language Card banking
        └── Apple2Device            floppies, input, video, soft switches
  ├── VGA                           the only fabgl:: seam
  └── Supervisor                    F1 menu
```

### Main loop

`setup()` brings up PSRAM, the SD card (SPI pins 14/2/12/13, CS 13), FabGL's
`DisplayController` (a 320x200 framebuffer scaled to 640x480@60Hz) and the PS/2
keyboard, then constructs `Apple2Machine` + `VGA` and calls `InitMachine()`.

`loop()` runs a fixed number of CPU cycles per frame (`17050 * 4`, one Apple II
video frame), renders, and presents. There is no host-speed throttling beyond
that fixed cycle count.

### Components

**`Apple2Machine`** (`src/AppleII/Apple2Machine.*`) owns `cpu`, `mem` and
`device` as plain members. `InitMachine()` creates memory, resets the CPU, wires
`device` into `mem.device`, then `Booting()` `memcpy`s the embedded ROM images
from `rombios.h` (`appleIIrom`, `diskII`) into `mem.rom` / `mem.sl6`.

**`CPU`** (`src/AppleII/AppleCpu.*`) is a MOS 6502 instruction-level emulator. It
never touches raw memory — every read and write goes through
`Memory::ReadByte`/`WriteByte`, and the addressing modes (`addr_mode_ABS`,
`addr_mode_ZP`, …) compute effective addresses before dispatch in `Run()`.

**`Memory`** (`src/AppleII/AppleMem.*`) implements the segmented layout defined
in `Predef.h`: `RAMSIZE=0xC000`, `ROMSTART=0xD000`, `SL6START=0xC600` for the
Disk II slot-6 ROM, and `LGCSTART`/`BK2START` for the 12K Language Card region.
Accesses to the `$C000-$C0FF` I/O page are routed to `Apple2Device::SoftSwitch`,
which in turn flips `Memory`'s `LCReadable` / `LCWritable` / `LCBank2Enable` /
`LCPreWriteFlipflop` banking flags. That is the one place memory banking and
device I/O are entangled, and it is deliberate.

**`Apple2Device`** (`src/AppleII/Apple2Device.*`) is everything that is not CPU
or raw memory: two `FloppyDrive` units (nibblized `.nib` images loaded from SD
via `Tools/FileSystem.h`), keyboard and gamepad input, and screen rendering
(text/lores/hires, tracked via `videoAddress`/`videoPage`) written **straight
into the VGA framebuffer** using glyph data from `AppleFont`. There is no
intermediate backbuffer: `Render()` takes a `VGA*`, holds it for the duration of
the call, and `DrawPoint`/`DrawRect`/`AppleFont::RenderFont` write scanlines
directly. Redundant drawing is suppressed by the per-cell dirty caches
`TextCache`, `LoResCache` and `HiResCache` (plus `previousBit` for hires colour
fringing), each forced to a full repaint every 30 frames via `!flashCycle`.

**`VGA`** (`src/VGA/VGA.h`) is a thin wrapper around the global
`fabgl::VGAController DisplayController`. `dot()`, `row()` and `clear()` pack
RGB222 values directly into VGA scanline buffers. `row(y)` returns a whole
scanline, so callers drawing many pixels fetch the pointer once per row rather
than once per pixel. Every write preserves the top two bits of each byte
(`& 0xC0`) — they carry the sync signals — and `x ^ 2` is the required
byte-order swizzle.

**`Supervisor`** (`src/Supervisor/`) is the F1 menu. It pauses emulation (the
main loop skips `machine->Run()` while it is active), browses the SD card, and
mounts/unmounts `.nib` images via `Apple2Machine::Mount`/`Unmount`, which also
manage the Disk II PROM at `$C600`. It owns its own `AppleFont` and paints
directly into the live VGA framebuffer, calling
`Apple2Device::InvalidateRenderCache()` on close so the emulator repaints fully.

## Constraints

These are the things to be careful about. Each one is cheap to break by
accident and expensive to diagnose afterwards.

### Apple II memory must stay in internal SRAM

All five memory blocks (~76K total) are allocated with
`heap_caps_malloc(MALLOC_CAP_INTERNAL)`, **not** `ps_malloc`. The 6502 core
touches them on nearly every emulated cycle, and PSRAM's SPI latency — plus a
32K cache that cannot hold the working set — made the interpreter roughly
**1.6x slower**.

Plain `malloc()` is not sufficient either: the Arduino core builds with
`CONFIG_SPIRAM_USE_MALLOC`, so large allocations can still land in PSRAM.
`Memory::Create()` logs where each block ended up and warns on a PSRAM fallback
— watch that output if emulation speed suddenly drops.

### Single-buffering is load-bearing

The dirty caches described above work only because the VGA framebuffer
*persists between frames*. An unchanged cell is simply not redrawn, so whatever
is already in the framebuffer must still be there on the next frame.

Enabling FabGL double buffering would silently break this: cached cells would be
stale in whichever buffer was not written, showing up as flickering leftover
glyphs. `vga->show()` is deliberately a no-op.

### Overlays must invalidate the cells they cover

The F2 FPS overlay (`fpsOverlay`/`fpsValue`, drawn by `RenderFpsOverlay()` in
the top-right seven text cells) is host-side UI, not emulated state — it is
initialised in the constructor rather than `Reset()`, and the `.ino` loop feeds
it the per-second frame count.

Because it paints over cells the caches consider clean, `Render()` calls
`InvalidateFpsOverlayRegion()` for that corner on every overlaid frame, and the
F2 handler calls it again on toggle. **Any other overlay drawn on top of the
emulated screen needs the same treatment**, or it will leave debris behind when
it moves or disappears.

### The supervisor repaints only when dirty

Because the supervisor shares the live framebuffer with the emulator, it
repaints only when its `dirty` flag is set — every keypress sets it. Clearing
and repainting on every frame is visible as flicker. Any future state change
that does not originate from a keypress must set `dirty` itself.

## Emulation speed

Two non-obvious things dominate performance. Both were found by profiling on
hardware, not by reading the code.

**Memory placement** — see the SRAM constraint above.

**`CPU::fastDiskDelay`** (`src/AppleII/AppleCpu.cpp`, on by default) skips DOS
3.3 RWTS's drive spin-up wait at `$BD9E`. That loop is a pure nested software
timer with no I/O, present so a physical drive can reach speed. DOS re-arms it
on essentially every RWTS call, and it was consuming **~70% of all emulated
cycles** — Karateka took 103s to load, of which ~112 seconds of *emulated* time
was this wait. Skipping it brought the load down to 35s.

The skip verifies all 13 opcode bytes of the loop before firing and writes back
the `$46`/`$47` the loop would have left, so machine state is identical. A
non-matching ROM or a custom RWTS just executes normally. This only helps DOS
3.3-derived loaders.

> When investigating a slowdown, measure where emulated *cycles* go before
> optimising the host. The first two host-side fixes here produced zero
> improvement in load time, because the bottleneck was the emulated machine
> doing 7.4x too much work.

## Odds and ends

`src/AppleII/LodeRunner.h` contains a large embedded byte array sized to match
`DISKSIZE`, but it is not wired into `Apple2Device::InsertFloppy` or referenced
anywhere else. Treat it as inert, orphaned data rather than an active code path,
unless you are specifically re-working floppy loading.

## Building

The build targets a plain ESP32 (TTGO VGA32-class board) via the FQBN
`esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app`; adjust the FQBN
board options for other ESP32 variants. Entry point is
`ESP32-VGA_AppleII_Emulator.ino`, with sources under `src/` per the Arduino
sketch layout. Requires arduino-cli core `esp32:esp32` 2.0.x (**not** 3.x) and
the FabGL 1.0.9 user library.

See [BUILD_AND_RELEASE.md](BUILD_AND_RELEASE.md) for the full build, flash and
release procedure.
