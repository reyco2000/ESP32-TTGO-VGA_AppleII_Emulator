# Supervisor Menu — Design

**Date:** 2026-07-14
**Status:** Approved

## Purpose

Add an on-device supervisor menu to the ESP32-VGA Apple II emulator that browses the SD card and mounts/unmounts `.nib` disk images into the two emulated Disk II drives, replacing the current hardcoded boot disk.

## Requirements

- **F1** opens the menu; **ESC** closes it and resumes emulation.
- Emulation is **paused** while the menu is open (CPU not stepped).
- Browse the SD card with **full subdirectory navigation**, showing directories and `.nib` files only (case-insensitive extension match).
- Mount a selected `.nib` into **Drive 1 or Drive 2**; mounting **never** resets the machine.
- **Unmount** either drive from the menu.
- Menu includes a **[ Reset machine ]** item (warm reset) so a newly mounted disk can be booted without typing `PR#6`.
- At power-on the machine boots with **no disk** (remove the hardcoded `/dkk.nib` mount).

## Architecture (Approach A — standalone Supervisor module)

Two new files: `src/Supervisor/Supervisor.h` and `src/Supervisor/Supervisor.cpp` containing a `Supervisor` class that sits beside the emulation core:

- Holds pointers to `Apple2Machine` (for `Reset()`), `Apple2Device` (mount/unmount/disk names), and `VGA` (output).
- Owns its **own** `AppleFont` instance and its **own** `AppleColor` backbuffer (`ps_malloc`, same size/format as the device's), so the paused emulator screen is never corrupted.
- The `.ino` `loop()` becomes a two-state machine:

```cpp
if (supervisor->IsActive()) {
    supervisor->Update();      // sole PS2 keyboard reader while active
    supervisor->Render(vga);   // AppleFont -> own backbuffer -> vga->dot()
} else {
    machine->Run(cycles);
    machine->Render(vga, frame);
}
```

Pausing = not calling `machine->Run()`. No changes to CPU/Memory core.

## Activation and keyboard ownership

PS2 keyboard events are destructive reads (`getNextVirtualKey`), so exactly one consumer reads at a time:

- `Apple2Device::UpdateKeyBoard()` gains one case: `VK_F1` sets a public flag `supervisorRequested`; nothing is fed to the Apple II keyboard latch.
- The `.ino` loop sees the flag, clears it, and calls `supervisor->Open()`.
- While active, `Supervisor::Update()` is the only reader of `keyboard_ptr`. ESC calls `Close()`; emulation resumes next loop iteration.

## Menu UX

Single scrollable list rendered as 40×24 Apple II text (white on black, inverse-video cursor bar):

```
    SUPERVISOR              (title)
 D1: /games/karateka.nib    (drive status lines)
 D2: (empty)
 ------------------------------------
 > [ Reset machine ]
   [ Unmount Drive 1 ]
   [ Unmount Drive 2 ]
   ..                       (parent dir, hidden at root)
   GAMES/
   KARATEKA.NIB
 ------------------------------------
 ARROWS:MOVE  ENTER:SELECT  ESC:EXIT
```

- **↑/↓** move the cursor; list scrolls in a window (~16 visible rows).
- **Enter** on a directory descends; on `..` goes up; on a `.nib` opens a one-line drive picker: `MOUNT TO: 1) DRIVE 1  2) DRIVE 2  ESC) CANCEL` — keys `1`/`2` mount, ESC cancels.
- **Enter** on action items executes them. Unmount entries display the mounted filename; when a drive is empty the entry still appears (rendered dim/normal-video) but Enter just sets the status line to `DRIVE EMPTY`.
- One status-message line shows results: `MOUNTED TO DRIVE 1`, `LOAD FAILED`, `SD ERROR`, `UNMOUNTED DRIVE 2`.
- Filenames longer than the display width are truncated with a trailing `~`.

## SD browsing and mounting

- Directory listing via `SD.open()` / `openNextFile()` (same API as existing `listDir()` in the `.ino`).
- Filter: directories + files ending `.nib` (case-insensitive). Sort: directories first, then alphabetical. Cap: 128 entries.
- Listing re-scans on menu open and on every directory change.
- `Apple2Device` gains two public methods wrapping existing internals:
  - `bool Mount(const char* path, int drive)` — calls private `InsertFloppy()`; on failure calls `disk[drive].Reset()` so a half-read image is never left mounted.
  - `void Unmount(int drive)` — `disk[drive].Reset()` (clears data, filename, motor state).
- `InsetFloppy()` (boot path) loses the hardcoded `/dkk.nib` mount; it only resets both drives. Machine powers on to BASIC/monitor with no disk.

## Resume correctness

`Apple2Device::Render()` is differential (`LoResCache` / `HiResCache` / `previousBit` skip unchanged cells). The Supervisor draws to the real scanlines while paused, so on close the emulator must repaint everything. `Apple2Device` gains `InvalidateRenderCache()` (resets those caches); `Supervisor::Close()` calls it.

## Reset machine item

`[ Reset machine ]` calls `Apple2Machine::Reset()`. That path exists but is currently dead (commented out of `Run()`); the implementation must verify it performs a correct warm reset (CPU reset via reset vector, soft-switch state reset, RAM preserved) and fix it if not. After reset the menu closes so the boot is visible.

## Error handling

- SD failures never crash the menu: status-line message, drive state unchanged (or cleanly unmounted on partial read).
- If the SD root can't be opened, the menu still shows the three action items plus `SD ERROR`; navigating retries the read.
- Entry-count overflow (>128) silently truncates the listing.

## Testing / verification

No test suite exists; verification is on-hardware:

1. `pio run` builds clean.
2. F1 opens the menu with emulation frozen (game visibly pauses).
3. Browse into and out of subdirectories; `..` hidden at root.
4. Mount a known game to Drive 1; `PR#6` boots it.
5. Mount without reset: machine state is untouched until reset chosen.
6. Unmount both drives; unmount entries grey out when empty.
7. `[ Reset machine ]` warm-resets and boots the mounted disk.
8. ESC resumes with a fully repainted screen (no menu remnants).
9. FPS unchanged when the menu is closed; heap/PSRAM stable across repeated open/close.
10. Power-on with no disk lands in BASIC/monitor.
