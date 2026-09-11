# Architecture

How the emulator is put together, and — more importantly — the handful of
constraints that are not visible from reading the code. Several of these were
found by profiling or debugging on real hardware; breaking them silently costs
performance or correctness rather than failing the build.

## The shape of it

The emulation core is decoupled from the ESP32/FabGL platform layer.
`Apple2Machine` and its components know nothing about VGA or FabGL directly.
Only `VGA` (`src/VGA/VGA.h`), the `.ino`, `src/Supervisor/`, and the PS/2
keyboard code in `Apple2Device.cpp` touch `fabgl::` types.

`AppleVideo.cpp` and `AppleFont.cpp` do include `VGA.h` (and so, transitively,
`fabgl.h`) in order to draw — but they use only the `VGA` wrapper, never
`fabgl::` types directly. **Keep it that way: `VGA` is the intended seam.** It is
the single place where the emulator meets real hardware pixels, and porting to
another display backend should mean rewriting that one file.

```
ESP32-VGA_AppleII_Emulator.ino   setup: SD, VGA, PS/2, picks the machine, starts EmulationTask
  └── Apple2Machine                one MachineProfile: Apple ][+ or Apple //e (enhanced)
        ├── CPU                    6502 / 65C02 interpreter
        ├── Memory                 page-table address map: Language Card, IIe MMU, $Cxxx ROM
        └── Apple2Device           soft switches, keyboard, speaker
              ├── Card* slots[8]   slot 6: DiskIICard
              └── AppleVideo       text / lores / hires / double hires -> VGA
  ├── MachineProfile               what distinguishes the models
  ├── RomLoader                    ROM files from /roms on the SD card
  ├── Settings                     NVS: the chosen machine, disks to remount
  ├── VGA                          the only fabgl:: seam (VGA16Controller, 640x200)
  └── Supervisor                   F1 menu
```

### Machines

`MachineProfile` (`src/AppleII/MachineProfile.*`) is a table: CPU type, whether
the IIe MMU and auxiliary memory exist, the ROM files and their sizes, whether a
built-in ROM may stand in, and the slot population. The core is written against
these fields rather than model names, so the Apple IIc and IIc Plus are meant to
arrive as new rows plus the hardware they add (new `Card`s, the IIc's extra
switches), not as a rewrite. The ids are stored in NVS: never renumber them.

At boot `setup()` reads the saved model (`Tools/Settings.h`), checks with
`RomLoader::FirstMissing` that its ROM files are on the card, and falls back to
the ][+ — whose ROMs are built in (`rombios.h`) — if they are not, keeping the
reason as `bootNote` for the supervisor. Switching model in the supervisor saves
the choice and the mounted disk paths and calls `ESP.restart()`, so each model's
memory is laid out on a clean heap; `setup()` remounts the disks once.

### Main loop

`setup()` brings up PSRAM, the SD card (SPI pins 14/2/12/13, CS 13), FabGL's VGA
controller (a 640x200 16-colour framebuffer, shown line-doubled on standard
640x480@60Hz timing) and the PS/2 keyboard, creates the machine and the
supervisor, and starts `EmulationTask` on **core 0** (see the constraints
below). The Arduino `loop()` task then deletes itself.

`RunFrame()` runs a fixed number of CPU cycles per frame (`17050 * 4`), renders
and counts frames for the F2 overlay. There is no host-speed throttling beyond
that fixed cycle count.

### Components

**`Apple2Machine`** (`src/AppleII/Apple2Machine.*`) owns `cpu`, `mem` and
`device` as plain members and installs the cards its profile has.
`InitMachine()` creates memory for the profile, then the device — which builds
the default font — and only then `LoadRoms()`: system ROM, Disk II PROM and, on
the //e, the character ROM. The order matters: loaded before the device, the
character ROM would be wiped by the default font.

**`CPU`** (`src/AppleII/AppleCpu.*`) is an instruction-level 6502 interpreter;
`AppleCpu65C02.cpp` adds the 65C02 opcodes, dispatched from the default case of
the opcode switch when `cmos` is set. It never touches raw memory — every read
and write goes through `Memory::ReadByte`/`WriteByte`. `tests/host/` builds this
code on the host and runs Klaus Dormann's 6502 and 65C02 functional tests
against it (`tests/host/run-cpu-tests.sh`).

**`Memory`** (`src/AppleII/AppleMem.*`) maps the 64K address space as 256 pages
through the `readPage`/`writePage` tables; a NULL entry sends the access down the
slow path, to the soft switches and peripheral ROM logic in `$C000`-`$CFFF`.
`Remap()` rebuilds the tables from the Language Card flags and, on the //e, the
MMU switches (80STORE, RAMRD, RAMWRT, ALTZP, INTCXROM, SLOTC3ROM, INTC8ROM). The
pages whose access changes which ROM is visible (`$C3xx` sets INTC8ROM, `$CFFF`
clears it) stay on the slow path so the access is seen. Writes to ROM land in a
sink page, so the write fast path needs no ROM check.

**`Apple2Device`** (`src/AppleII/Apple2Device.*`) is everything that is not CPU
or raw memory: the soft switches (the //e's `$C000`-`$C01F` block and its status
bits in `IIeSwitch()`), the keyboard, the speaker and the slot cards. `$C090`-
`$C0FF` goes to `slots[n]->Io()`, and `Memory` maps each card's `SlotRom()` at
`$Cn00`. `DiskIICard` is slot 6: two drives of nibblized `.nib` images, read
from SD into PSRAM via `Tools/FileSystem.h`. It hides its boot PROM while no disk
is inserted, so the machine boots to BASIC instead of hanging on an empty drive.

The keyboard drains FabGL's event queue every frame and uses each event's own
ASCII value — the character made with the modifiers as they were when the key
went down; translating later, with the modifiers as they are by then, reordered
fast typing. Characters wait in a 16-key type-ahead queue and are latched one at
a time as the program clears the strobe. Set `KEY_TRACE` to 1 at the top of
`Apple2Device.cpp` for a serial trace of every key event.

**`AppleVideo`** (`src/AppleII/AppleVideo.*`) renders into the framebuffer as
palette indices: a 560x192 picture at (40,4), each 40-column dot two pixels wide,
in a black border. The palette is the 16 lores colours in Apple order, so a lores
nibble is its own index; text is white. It reads screen memory straight from
`mem.ram`/`mem.auxRam`, as the video circuit does, whatever RAMRD says. There is
no intermediate backbuffer: redundant drawing is suppressed by the per-cell
dirty caches `TextCache` (80 wide), `LoResCache` and `HiResCache` (plus
`previousBit` for hires colour fringing), each forced to a full repaint every 30
frames via `!flashCycle` and on any change of video mode.

**`VGA`** (`src/VGA/VGA.h`) wraps the global `AppleVGAController`, a
`fabgl::VGA16Controller` subclass. Rows are 4-bit packed, even x in the high
nibble; `row(y)` returns a whole scanline, and `pair()` writes two identical
pixels as one byte, which is all double-width content needs. `setPalette()`
loads the 16 entries.

**`Supervisor`** (`src/Supervisor/`) is the F1 menu. It pauses emulation (the
frame loop skips `machine->Run()` while it is active), browses the SD card,
mounts/unmounts `.nib` images via `Apple2Machine::Mount`/`Unmount`, switches
machine and shows `[ ABOUT ]`. It paints directly into the live framebuffer in
its own palette and calls `Apple2Device::InvalidateRenderCache()` on close so the
emulator repaints fully. Its font has no box-drawing glyphs and no lowercase
(glyphs `0x20`-`0x5F`), so every bar, rule and panel is a pixel fill via
`VGA::fillRect`.

## Constraints

These are the things to be careful about. Each one is cheap to break by
accident and expensive to diagnose afterwards.

### Emulated memory must stay in internal SRAM

All memory blocks are allocated with `heap_caps_malloc(MALLOC_CAP_INTERNAL)`,
**not** `ps_malloc`. The 6502 core touches them on nearly every emulated cycle,
and PSRAM's SPI latency — plus a 32K cache that cannot hold the working set —
made the interpreter roughly **1.6x slower**.

Plain `malloc()` is not sufficient either: the Arduino core builds with
`CONFIG_SPIRAM_USE_MALLOC`, so large allocations can still land in PSRAM.
`Memory::Create()` allocates hottest-first, logs where each block ended up and
warns on a PSRAM fallback — watch that output if emulation speed suddenly drops.

The //e needs about 146K of internal RAM (main and auxiliary 64K each, a 16K
ROM, the page tables) and leaves only ~22K free, with a 14K largest block. That
is why `EmulationTask`'s stack is 8K, and why the `.ino` falls back to running
the emulator from `loop()` if the task cannot be created. The boot log prints
`[mem] internal free ..., largest block ...` just before creating it.

### The framebuffer must stay in internal SRAM too

`VGA16Controller`'s interrupt handler converts the framebuffer to signal levels
line by line. Some FabGL installs are patched to put the framebuffer in PSRAM
(this project's build machine has one); there the ISR streams it over the same
bus the flash-resident interpreter uses, and emulation lost ~20%.
`AppleVGAController` overrides `allocateViewPort()` to use internal RAM
whichever FabGL is installed. The framebuffer being internal also keeps the ISR
safe while flash is written (NVS saves), when PSRAM and flash are unreachable.

### Emulation runs off the video interrupt's core

The VGA16 ISR is pinned to core 1 (`FABGLIB_VIDEO_CPUINTENSIVE_TASKS_CORE`),
where the Arduino loop runs. Emulating there cost a third of the speed, so the
emulator runs in its own task on core 0, which has nothing else to do (no WiFi or
BT). The task never yields, so core 0's idle-task watchdog is disabled.

### Remap after every banking change

`Memory`'s page tables are only as current as the last `Remap()` or
`RemapLanguageCard()`. Anything that changes a Language Card or MMU flag,
PAGE2/HIRES while 80STORE is on, or what a card's `SlotRom()` returns (a disk
mounted or ejected) must remap, or the CPU keeps seeing the old mapping.

### Keep emulated time off the per-instruction path

`CPU::tick` is brought up to date once per `Run()` call; `CurrentTick()` adds the
cycles the call in progress has used through a pointer to its budget, for
readers like the //e's VBL flag at `$C019`. Updating `tick` after every
instruction cost 3-6%.

### Single-buffering is load-bearing

The dirty caches work only because the VGA framebuffer *persists between
frames*. An unchanged cell is simply not redrawn, so whatever is already in the
framebuffer must still be there on the next frame.

Enabling FabGL double buffering would silently break this: cached cells would be
stale in whichever buffer was not written, showing up as flickering leftover
glyphs. `vga->show()` is deliberately a no-op.

### Overlays must invalidate the cells they cover

The F2 FPS overlay (`fpsOverlay`/`fpsValue`, drawn by
`AppleVideo::RenderFpsOverlay()` in the top-right seven text cells) is host-side
UI, not emulated state — it is initialised in the `Apple2Device` constructor
rather than `Reset()`, and the frame loop feeds it the per-second frame count.

Because it paints over cells the caches consider clean, `AppleVideo::Render()`
calls `InvalidateFpsOverlayRegion()` for that corner on every overlaid frame
(40- and 80-column cells alike), and the F2 handler calls it again on toggle.
**Any other overlay drawn on top of the emulated screen needs the same
treatment**, or it will leave debris behind when it moves or disappears.

### The supervisor repaints only when dirty

Because the supervisor shares the live framebuffer with the emulator, it
repaints only when its `dirty` flag is set — every keypress sets it. Clearing
and repainting on every frame is visible as flicker. Any future state change
that does not originate from a keypress must set `dirty` itself.

### The palette has one owner at a time

The emulator and the supervisor share one 16-entry palette. The supervisor loads
its own on its first repaint after `Open()`; `InvalidateRenderCache()` makes
`AppleVideo` put the Apple palette back, clear the border and repaint every cell
on its next frame. Anything else that draws in its own colours has to follow the
same handover.

### No partitions.csv in the sketch folder

The ESP32 Arduino core uses a `partitions.csv` found in the sketch folder
instead of the `PartitionScheme` in the FQBN — and copies it into the build
cache, where it outlives the file. The repository used to carry one from the
upstream code: a 928K app and no partition named `nvs`, so every settings save
failed while arduino-cli still reported the 3 MB `huge_app` limit.
`tools/build-firmware.sh` compiles with `--clean` so a stale table cannot survive
into a release.

## Emulation speed

At the idle BASIC prompt both machines run at about 35 frames per second, and
*Karateka* loads in about 22.5 seconds. Three things dominate performance, all
found by measuring on hardware rather than by reading the code:

**Memory placement and core assignment** — see the constraints above.

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

**Binary layout** — two builds that differ by a line of code can differ by a few
percent in speed, from how the code and data happen to fall in the flash and
PSRAM caches. Don't chase differences under ~5% between builds; measure again
after a real change.

> When investigating a slowdown, measure where emulated *cycles* go before
> optimising the host. The first two host-side fixes here produced zero
> improvement in load time, because the bottleneck was the emulated machine
> doing 7.4x too much work.

## Odds and ends

`src/AppleII/LodeRunner.h` contains a large embedded byte array sized to match
`DISKSIZE`, but it is not wired into `DiskIICard` or referenced anywhere else.
Treat it as inert, orphaned data rather than an active code path, unless you are
specifically re-working floppy loading.

## Building

The build targets a plain ESP32 (TTGO VGA32-class board) via the FQBN
`esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app`; adjust the FQBN
board options for other ESP32 variants. Entry point is
`ESP32-VGA_AppleII_Emulator.ino`, with sources under `src/` per the Arduino
sketch layout. Requires arduino-cli core `esp32:esp32` 2.0.x (**not** 3.x) and
the FabGL 1.0.9 user library.

See [BUILD_AND_RELEASE.md](BUILD_AND_RELEASE.md) for the full build, flash and
release procedure.
