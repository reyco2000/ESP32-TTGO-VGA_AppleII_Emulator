# ESP32-VGA Apple II Emulator

An Apple II+ emulator that runs entirely on an ESP32 (LilyGO TTGO VGA32-class board), rendering to a VGA monitor via the [FabGL](https://github.com/fdivitto/FabGL) library. A PS/2 keyboard provides input, and `.nib` floppy disk images are loaded from an SD card — no host computer involved.

## Features

- MOS 6502 CPU emulation (full instruction set)
- Apple II+ memory map with Language Card bank switching
- Text, lores, and hires video modes rendered to VGA 640×480@60Hz
- Two emulated Disk II drives with nibblized (`.nib`) disk images
- **Supervisor menu (F1)**: pauses emulation and opens an on-screen SD card browser — navigate subdirectories, mount/unmount `.nib` images into Drive 1 or Drive 2, and reset the machine. Mounting never resets, so mid-game disk swaps work (multi-disk games like Ultima).
- **FPS overlay (F2)**: toggles a live frames-per-second counter in the top right corner of the screen
- Boots to BASIC with no disk mounted; the Disk II boot PROM at `$C600` is managed automatically as disks are mounted/unmounted so `PR#6` always behaves correctly

## Hardware Requirements

- **[LilyGo TTGO VGA32 v1.4](https://lilygo.cc/en-us/products/fabgl-vga32?_pos=1&_sid=4c095f59b&_ss=r)** (ESP32-WROVER-E, 4 MB PSRAM, 4 MB flash)
- **VGA monitor** capable of 640×480 @ 60 Hz (most VGA CRTs and adapters; some modern LCDs accept this mode, others won't sync)
- **PS/2 keyboard** plugged into the board's mini-DIN PS/2 jack
- **MicroSD card** (FAT32 formatted) inserted in the on-board socket
- **3.5 mm audio output** (mono) on the board's jack
- **5 V USB-C** for power and serial programming

## Build & Flash

### Quick Flash (Pre-built Firmware)

If you just want to run the emulator without building from source, grab the pre-built firmware and use the browser-based flasher — no toolchain, no drivers to install beyond your board's USB-serial driver.

1. Download `ESP32-AppleII-v0.2.0.bin` from the [Releases](https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator/releases) page
2. Connect your TTGO VGA32 board via USB
3. Open [ESP Web Tool](https://espressif.github.io/esptool-js/) in a Chrome or Edge browser
4. Click **Connect** and select the board's serial port
5. Set the flash offset to `0x0000`
6. Choose the downloaded `ESP32-AppleII-v0.2.0.bin`
7. Click **Program** and wait for the flash to complete

Hold the **BOOT** button on the board while clicking **Connect** if the browser cannot reach the device.

That file is a complete image — bootloader, partition table, OTA selector and application merged together — so `0x0000` is the only offset you need. Verify the download against `SHA256SUMS` on the same release if you want to be sure it arrived intact.

### Flash from the command line

Same binary, if you'd rather not use a browser:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0 ESP32-AppleII-v0.2.0.bin
```

Depending on the board's USB-serial chip the port may enumerate as `/dev/ttyACM0` instead of `/dev/ttyUSB0`.

### Build from source

Built with [arduino-cli](https://arduino.github.io/arduino-cli/) — requires the `esp32:esp32` core (2.0.x, **not** 3.x) and the FabGL 1.0.9 library:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .   # build
arduino-cli upload  --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" -p /dev/ttyUSB0 .  # flash
arduino-cli monitor -p /dev/ttyUSB0 -c 115200                                             # serial monitor
```

`arduino-cli board list` shows which port the board is on.

To produce a release image of your own — bootloader + partition table + boot_app0 + app merged at offset `0x0`, plus checksums:

```bash
tools/build-firmware.sh          # output in build/, plus SHA256SUMS
```

The image is named after `FW_VERSION` at the top of that script — currently `ESP32-AppleII-v0.2.0.bin`. See [docs/BUILD_AND_RELEASE.md](docs/BUILD_AND_RELEASE.md) for the full build, test and release procedure.

## SD Card Setup

The Apple II+ system ROM and the Disk II boot PROM are **built into the firmware** — there are no ROM files to copy. The SD card holds only your disk images:

1. Format the card as FAT32
2. Copy `.nib` disk images onto it — either in the root or in subdirectories, the F1 browser walks both. Sample images are in [`data/`](data/)
3. Insert the card before powering on

The card is driven over SPI on the VGA32's on-board socket — SCK 14, MISO 2, MOSI 12, CS 13 — which matters only if you are adapting the firmware to a different board.

`.nib` (nibblized) is the only supported image format; `.dsk` and `.po` images need to be converted first. With no card or no disk mounted the machine still boots straight to BASIC.

## Usage

- The machine powers on into BASIC/monitor with no disk.
- Press **F1** to open the supervisor menu: arrow keys to move, **Enter** to open a directory or select a `.nib` (then `1`/`2` picks the drive), **ESC** to resume emulation.
- Press **F2** to show or hide the on-screen FPS counter.
- Use the menu's `[ RESET MACHINE ]` item (or `PR#6` from BASIC) to boot a mounted disk.

## License

MIT — see [LICENSE](LICENSE). Note that the embedded Apple II ROM and disk image
data (`src/AppleII/rombios.h`, `src/AppleII/LodeRunner.h`) and `src/Tools/Log.h`
(CC BY-SA 4.0, by bitluni) are third-party components under their own terms.

## Vibe Coding Alert

Full transparency: this project was built by an ESP32 hobbyist working with AI coding assistants, not a professional embedded/C++ developer. If you're an experienced embedded engineer, you might look at this codebase and wince. That's okay.

The goal here was to scratch an itch — get an Apple II emulator running on cheap VGA32 hardware — and learn along the way. The code works, but it's likely missing patterns, optimizations, or elegance that only years of embedded/C++ experience can provide.

This is where you come in. If you see something that makes you cringe, please consider contributing rather than just closing the tab. This is open source specifically because human expertise is irreplaceable. Whether it's refactoring, better error handling, cycle-accuracy fixes, or architectural guidance — PRs and issues are welcome.

Think of it as a chance to mentor an AI-assisted developer through code review. We all benefit when experienced developers share their knowledge.

## Credits

- **Reinaldo Torres / CoCo Byte Club** — ESP32 port and hardware design — reyco2000@cocobyte.club
- Based on the original [ESP32-VGA_AppleII_Emulator](https://github.com/codesafe/ESP32-VGA_AppleII_Emulator) by [codesafe](https://github.com/codesafe) — the 6502 core, Apple II machine emulation, and VGA rendering come from that project.
- [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio — VGA signal generation and PS/2 keyboard support.

## TODO

- [ ] Improve FPS
