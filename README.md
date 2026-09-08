# ESP32-VGA Apple II Emulator

An Apple II+ emulator that runs entirely on an ESP32 (LilyGO TTGO VGA32-class board), rendering to a VGA monitor via the [FabGL](https://github.com/fdivitto/FabGL) library. A PS/2 keyboard provides input, and `.nib` floppy disk images are loaded from an SD card — no host computer involved.

## Features

- MOS 6502 CPU emulation (full instruction set)
- Apple II+ memory map with Language Card bank switching
- Text, lores, and hires video modes rendered to VGA 640×480@60Hz
- Two emulated Disk II drives with nibblized (`.nib`) disk images
- **Supervisor menu (F1)**: pauses emulation and opens an on-screen SD card browser — navigate subdirectories, mount/unmount `.nib` images into Drive 1 or Drive 2, and reset the machine. Mounting never resets, so mid-game disk swaps work (multi-disk games like Ultima).
- Boots to BASIC with no disk mounted; the Disk II boot PROM at `$C600` is managed automatically as disks are mounted/unmounted so `PR#6` always behaves correctly

## Hardware

- ESP32 board with VGA resistor DAC and PS/2 keyboard port (e.g., LilyGO TTGO VGA32 v1.4)
- SD card (SPI: SCK 14, MISO 2, MOSI 12, CS 13) with `.nib` disk images (sample images in `data/`)

## Building

Built with [arduino-cli](https://arduino.github.io/arduino-cli/) — requires the `esp32:esp32` core (2.0.x) and the FabGL 1.0.9 library:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .   # build
arduino-cli upload  --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" -p /dev/ttyUSB0 .  # flash
arduino-cli monitor -p /dev/ttyUSB0 -c 115200                                             # serial monitor
```

## Usage

- The machine powers on into BASIC/monitor with no disk.
- Press **F1** to open the supervisor menu: arrow keys to move, **Enter** to open a directory or select a `.nib` (then `1`/`2` picks the drive), **ESC** to resume emulation.
- Use the menu's `[ RESET MACHINE ]` item (or `PR#6` from BASIC) to boot a mounted disk.

## Credits

- Based on the original [ESP32-VGA_AppleII_Emulator](https://github.com/codesafe/ESP32-VGA_AppleII_Emulator) by [codesafe](https://github.com/codesafe) — the 6502 core, Apple II machine emulation, and VGA rendering come from that project.
- [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio — VGA signal generation and PS/2 keyboard support.

## TODO

- [ ] Improve FPS
