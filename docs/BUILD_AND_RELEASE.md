# Building and Publishing Firmware

How to produce flashable firmware for the ESP32-TTGO-VGA Apple II emulator and
publish it on GitHub. This is the reference for future "build the firmware and
post it" requests — follow it end to end.

## Prerequisites

| Requirement | Version used | Notes |
|---|---|---|
| [arduino-cli](https://arduino.github.io/arduino-cli/) | 1.3.1 | must be on `PATH` |
| `esp32:esp32` core | 2.0.17 | **2.0.x only** — the 3.x core changes APIs this sketch relies on |
| FabGL library | 1.0.9 | user library |
| Python 3 | any | only used to run the core's bundled `esptool.py` |
| `gh` (GitHub CLI) | any | only needed for publishing a release |

Install the core and library if a machine doesn't have them:

```bash
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install FabGL@1.0.9
```

## 1. Build

One command produces everything:

```bash
tools/build-firmware.sh
```

It compiles with the release FQBN, merges the four flash images into a single
file, drops the `.elf`/`.map` build intermediates, and writes `SHA256SUMS`.
Output goes to `build/` (git-ignored — binaries are published as release assets,
not committed).

### Versioning

`FW_VERSION_STR` in [`src/Version.h`](../src/Version.h) is the single source of
truth for the release version. `tools/build-firmware.sh` parses it out of that
header to name the output files, and the supervisor's `[ ABOUT ]` page displays
it on screen, so the firmware always reports the version it was built as.
**Bump it there for a new release**, then use the same number for the git tag.
For a throwaway build, override it without editing anything:

```bash
FW_VERSION=0.3.0-rc1 tools/build-firmware.sh
```

Artifacts in `build/` (at `FW_VERSION=0.2.0`):

| File | Purpose |
|---|---|
| `ESP32-AppleII-v0.2.0.bin` | **the one to publish** — full image, flash at offset `0x0` |
| `ESP32-AppleII-v0.2.0-app.bin` | application only, flash at `0x10000` |
| `ESP32-VGA_AppleII_Emulator.ino.bootloader.bin` | bootloader, `0x1000` |
| `ESP32-VGA_AppleII_Emulator.ino.partitions.bin` | partition table, `0x8000` |
| `boot_app0.bin` | OTA selector, `0xe000` |
| `SHA256SUMS` | checksums for all of the above |

The two publishable images carry the version; the bootloader, partition table
and OTA selector are version-neutral and keep the names arduino-cli and the
esp32 core give them.

### Doing it by hand

If you'd rather not use the script:

```bash
# compile
arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" \
  --output-dir ./build .

# merge (paths depend on the installed core version)
python3 ~/.arduino15/packages/esp32/tools/esptool_py/4.5.1/esptool.py \
  --chip esp32 merge_bin -o build/ESP32-AppleII-v0.2.0.bin \
  --flash_mode dio --flash_freq keep --flash_size 4MB \
  0x1000  build/ESP32-VGA_AppleII_Emulator.ino.bootloader.bin \
  0x8000  build/ESP32-VGA_AppleII_Emulator.ino.partitions.bin \
  0xe000  ~/.arduino15/packages/esp32/hardware/esp32/2.0.17/tools/partitions/boot_app0.bin \
  0x10000 build/ESP32-VGA_AppleII_Emulator.ino.bin
```

The FQBN options are load-bearing: `PSRAM=enabled` (FabGL needs it) and
`PartitionScheme=huge_app` (the sketch does not fit the default 1.2 MB app
partition scheme's headroom expectations used by this project).

## 2. Flash locally to test before publishing

```bash
arduino-cli board list                                   # find the port
arduino-cli upload --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" \
  -p /dev/ttyUSB0 .
arduino-cli monitor -p /dev/ttyUSB0 -c 115200
```

The port is `/dev/ttyACM0` on some boards depending on the USB-serial chip.

To verify the merged image specifically (what users will actually flash):

```bash
python3 ~/.arduino15/packages/esp32/tools/esptool_py/4.5.1/esptool.py \
  --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0 build/ESP32-AppleII-v0.2.0.bin
```

## 3. Publish to the remote repository

Binaries are **not** committed to git — they bloat history and can't be
rewritten. Publish them as GitHub Release assets instead.

```bash
# make sure the source that produced the binary is pushed first
git push origin HEAD

# tag and publish — keep the tag in step with FW_VERSION in the build script
VERSION=v0.2.0
git tag -a "$VERSION" -m "Firmware $VERSION"
git push origin "$VERSION"

gh release create "$VERSION" \
  --title "Firmware $VERSION" \
  --notes "Apple II emulator firmware for ESP32-TTGO-VGA (VGA32 v1.4)." \
  "build/ESP32-AppleII-$VERSION.bin" \
  "build/ESP32-AppleII-$VERSION-app.bin" \
  build/SHA256SUMS
```

To update an existing release's assets instead of making a new one:

```bash
gh release upload v0.2.0 build/ESP32-AppleII-v0.2.0.bin --clobber
```

## 4. Flashing instructions for end users

Worth pasting into the release notes:

```bash
# esptool (pip install esptool)
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0 ESP32-AppleII-v0.2.0.bin
```

Or drop the merged `.bin` at offset `0x0` into
[ESP Web Tools / esptool-js](https://espressif.github.io/esptool-js/) in a
Chrome-based browser — no toolchain needed.

## Checklist for a release

1. Bump `FW_VERSION_STR` in `src/Version.h`
2. `tools/build-firmware.sh` — clean build, no warnings that matter
3. Flash to hardware and confirm it boots to BASIC, F1 supervisor opens (check
   `[ ABOUT ]` reports the version you just bumped), F2 FPS toggles
4. Commit and push the source
5. Tag with the same version, push the tag, `gh release create` with the merged
   binary and `SHA256SUMS`
