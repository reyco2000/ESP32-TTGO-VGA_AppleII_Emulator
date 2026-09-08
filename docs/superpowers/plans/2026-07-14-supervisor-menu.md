# Supervisor Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an F1-activated supervisor menu that pauses emulation, browses the SD card (subdirectories included), and mounts/unmounts `.nib` disk images into the two emulated Disk II drives.

**Architecture:** A standalone `Supervisor` class (new `src/Supervisor/` module) renders 40×24 Apple II text into its own PSRAM backbuffer using `AppleFont`, and the `.ino` main loop becomes a two-state machine (menu active → menu updates/renders; otherwise → emulator runs). Mount/unmount go through new public methods on `Apple2Machine` (which also manage the Disk II PROM at $C600) that delegate to `Apple2Device`.

**Tech Stack:** Arduino ESP32 core 2.0.17, FabGL 1.0.9 (VGA + PS2 keyboard), SD library over SPI, built with `arduino-cli`.

**Spec:** `docs/superpowers/specs/2026-07-14-supervisor-menu-design.md`

## Global Constraints

- **Build command (run from repo root; this is the verification step for every task):**
  ```bash
  arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .
  ```
  Expected: ends with a flash/RAM usage summary and exit code 0. (`pio` is NOT installed on this machine; ignore the PlatformIO instructions in CLAUDE.md — Task 6 corrects them.)
- **Flash command (only for on-hardware verification, needs the board attached):**
  ```bash
  arduino-cli upload --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" -p /dev/ttyUSB0 .
  arduino-cli monitor -p /dev/ttyUSB0 -c 115200
  ```
- **No test suite exists** and the code is hardware-bound (FabGL, SD, PSRAM). The test cycle per task is: clean compile + the on-hardware checks listed in Task 6. Do not invent a host-side test harness.
- **Layering rule:** only the `.ino`, `src/VGA/VGA.h`, and the new `src/Supervisor/` files may touch `fabgl::` types. `Apple2Machine`/`CPU`/`Memory`/`Apple2Device` stay FabGL-free except for `Apple2Device.cpp`'s existing keyboard code.
- **Filenames are case-sensitive on this machine.** Match existing header case exactly: `AppleCpu.h`, `Apple2Device.h`, `Apple2Machine.h`, `AppleMem.h`, `AppleFont.h`, `Predef.h`.
- Commit after every task. Commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- After the final task run `graphify update .` (project rule).

## Key existing facts (read this before Task 1)

- `Apple2Device::InsertFloppy(const char*, int)` (`src/AppleII/Apple2Device.cpp:651`) is **private**; reads `DISKSIZE` (232960) bytes from SD into `disk[drv].data` (PSRAM), sets `disk[drv].filename`. Returns false on short read **but leaves the buffer clobbered** — that's why the new `Mount` wrapper must call `disk[drv].Reset()` on failure.
- `FloppyDrive::Reset()` (`src/AppleII/Apple2Device.h:36`) zeroes data, filename, motor state — it is the unmount primitive. `HasFloppy(drv)` tests `filename[0]`.
- `Apple2Device::InsetFloppy()` [sic] (`src/AppleII/Apple2Device.cpp:723`) resets both drives then hardcodes `InsertFloppy("/dkk.nib", 0)` — that line must be removed.
- `Apple2Machine::Booting()` (`src/AppleII/Apple2Machine.cpp:35`) copies the Apple II ROM, and loads the Disk II PROM into `mem.sl6` **only if `device.HasFloppy(0)`**, else zeroes `mem.sl6` (that is what makes a disk-less boot land in BASIC instead of hanging). `diskII` and `SL6SIZE` come from `rombios.h`/`Predef.h`, already included there.
- `Apple2Machine::Reset()` (`src/AppleII/Apple2Machine.cpp:95`) is a full reboot: clears RAM/ROM, `cpu.Reboot`, `device.Reset()` (which does **not** touch the `disk[]` drives — mounted disks survive), magic bytes, then `Booting()`. It is currently never called (dead code path) but is correct for the menu's "Reset machine" item.
- `Apple2Device::UpdateKeyBoard()` (`src/AppleII/Apple2Device.cpp:754`) consumes PS2 events destructively via `keyboard_ptr->getNextVirtualKey()`. F1 is currently unhandled.
- Render caches: `LoResCache[24][40]` (int), `HiResCache[192][40]` (int), `previousBit[192][40]` (BYTE) are **public** members of `Apple2Device`; differential rendering skips cells whose cache matches, so after the menu draws over the screen the caches must be invalidated (`0xFF` bytes force int mismatch).
- Text rendering: `AppleFont::RenderFont(AppleColor* backbuffer, int glyph, int pixelX, int pixelY, bool inverse)` draws a 7×8 glyph (green on black; inverse = black on green) into a 280-wide `AppleColor` buffer. For ASCII `0x20`–`0x5F` the glyph index **is** the ASCII code (see mapping at `src/AppleII/Apple2Device.cpp:606-608`). No lowercase exists — uppercase everything.
- Screen constants (`src/AppleII/Predef.h`): `SCREENSIZE_X` 280, `SCREENSIZE_Y` 192, `SCREENTEXT_X` 40, `SCREENTEXT_Y` 24, `FONT_X` 7, `FONT_Y` 8. `AppleColor` is 3 bytes `{r,g,b}` with no default constructor — allocate raw with `ps_malloc`, clear with `memset`.
- `VGA::dot(x, y, rgb)` / `VGA::rgb(r,g,b)` (`src/VGA/VGA.h`) write pixels; copy loop pattern is in `Apple2Machine::Render` (`src/AppleII/Apple2Machine.cpp:129`).
- Globals in the `.ino`: `fabgl::Keyboard *keyboard_ptr`, `Apple2Machine *machine`, `VGA *vga`. `Apple2Machine` exposes `device` and `mem` as public members.
- Baseline build fixes were already applied and committed before this plan (case-sensitive includes `AppleCPU.h`→`AppleCpu.h` / `Apple2device.h`→`Apple2Device.h`, and UTF-8 BOMs stripped from `AppleCpu.h`, `AppleMem.h`, `rombios.h`). The tree builds clean as a baseline: `Sketch uses 507573 bytes (16%) of program storage space.` — every task's build step should end with a summary like that.

---

### Task 1: Apple2Device supervisor API (mount/unmount/cache-invalidate/F1 flag)

**Files:**
- Modify: `src/AppleII/Apple2Device.h` (public section, ~line 131-153)
- Modify: `src/AppleII/Apple2Device.cpp` (Reset ~line 95, InsetFloppy ~line 723, UpdateKeyBoard ~line 754, new methods near InsertFloppy ~line 667)

**Interfaces:**
- Consumes: existing private `InsertFloppy(const char*, int)`, `FloppyDrive::Reset()`, cache arrays.
- Produces (used by Tasks 2, 3, 5):
  - `bool Apple2Device::Mount(const char* path, int drive)` — drive is 0 or 1; false on read failure (drive left cleanly empty).
  - `void Apple2Device::Unmount(int drive)`
  - `void Apple2Device::InvalidateRenderCache()`
  - `bool Apple2Device::supervisorRequested` — public flag, set by F1, cleared by the `.ino`.

- [ ] **Step 1: Declare the API in `src/AppleII/Apple2Device.h`**

In the existing `public:` section (after `bool HasFloppy(int drive) ...`, line ~137), add:

```cpp
	// Supervisor menu support
	bool supervisorRequested;
	bool Mount(const char* path, int drive);
	void Unmount(int drive);
	void InvalidateRenderCache();
```

- [ ] **Step 2: Implement in `src/AppleII/Apple2Device.cpp`**

Directly after the closing brace of `Apple2Device::InsertFloppy` (line ~667), add:

```cpp
bool Apple2Device::Mount(const char* path, int drive)
{
	if (drive < 0 || drive > 1)
		return false;
	if (!InsertFloppy(path, drive))
	{
		// short/failed read clobbered the buffer - never leave it half-mounted
		disk[drive].Reset();
		return false;
	}
	return true;
}

void Apple2Device::Unmount(int drive)
{
	if (drive < 0 || drive > 1)
		return;
	disk[drive].Reset();
}

void Apple2Device::InvalidateRenderCache()
{
	memset(LoResCache, 0xFF, sizeof(LoResCache));
	memset(HiResCache, 0xFF, sizeof(HiResCache));
	memset(previousBit, 0, sizeof(previousBit));
}
```

(`0xFF` bytes make every int cache entry -1, which can never equal a fetched screen byte, forcing a full repaint.)

- [ ] **Step 3: Initialize the flag in `Apple2Device::Reset()`**

In `Apple2Device::Reset()` (line ~95), after `keyboard = 0;` add:

```cpp
	supervisorRequested = false;
```

- [ ] **Step 4: Detect F1 in `UpdateKeyBoard()`**

In `src/AppleII/Apple2Device.cpp` inside `UpdateKeyBoard()`, immediately after the line `fabgl::VirtualKey vk = keyboard_ptr->getNextVirtualKey(&keyDown, 0);` and its `if (vk != fabgl::VK_NONE && keyDown)` opening brace, add as the FIRST statements of that block (before `char ascii = ...`):

```cpp
            if (vk == fabgl::VK_F1)
            {
                supervisorRequested = true;
                return;
            }
```

(Checked before the ASCII conversion so it works regardless of what `virtualKeyToASCII` returns for function keys; nothing is fed to the Apple II keyboard latch.)

- [ ] **Step 5: Remove the hardcoded boot disk**

In `Apple2Device::InsetFloppy()` (line ~723), delete the line:

```cpp
	InsertFloppy("/dkk.nib", 0);
```

Leave the `disk[0].Reset(); disk[1].Reset();` calls and the commented examples in place.

- [ ] **Step 6: Build**

Run: `arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .`
Expected: exit 0 with flash/RAM summary.

- [ ] **Step 7: Commit**

```bash
git add src/AppleII/Apple2Device.h src/AppleII/Apple2Device.cpp
git commit -m "feat: add mount/unmount/cache-invalidate API and F1 flag to Apple2Device

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Apple2Machine mount/unmount with Disk II PROM management

**Files:**
- Modify: `src/AppleII/Apple2Machine.h` (public section, ~line 26-32)
- Modify: `src/AppleII/Apple2Machine.cpp` (Booting ~line 40, new methods after Reset ~line 108)

**Interfaces:**
- Consumes (Task 1): `device.Mount(path, drive)`, `device.Unmount(drive)`, `device.HasFloppy(drive)`.
- Produces (used by Task 5):
  - `bool Apple2Machine::Mount(const char* path, int drive)` — mounts AND ensures the Disk II PROM is at $C600 so `PR#6` works after a disk-less boot.
  - `void Apple2Machine::Unmount(int drive)` — unmounts; clears the PROM when both drives end up empty so a later Reset lands in BASIC instead of hanging on an empty-drive boot scan.

- [ ] **Step 1: Declare in `src/AppleII/Apple2Machine.h`**

In the `public:` section after `void Reset();`, add:

```cpp
	bool Mount(const char* path, int drive);
	void Unmount(int drive);
```

- [ ] **Step 2: Implement in `src/AppleII/Apple2Machine.cpp`**

After the closing brace of `Apple2Machine::Reset()` (line ~108), add:

```cpp
bool Apple2Machine::Mount(const char* path, int drive)
{
	if (!device.Mount(path, drive))
		return false;
	// machine may have booted disk-less with a cleared slot 6 ROM;
	// PR#6 needs the Disk II PROM present
	memcpy(mem.sl6, diskII, SL6SIZE);
	return true;
}

void Apple2Machine::Unmount(int drive)
{
	device.Unmount(drive);
	// no disk left: clear the PROM so a Reset boots to BASIC
	// instead of hanging on an empty drive scan
	if (!device.HasFloppy(0) && !device.HasFloppy(1))
		memset(mem.sl6, 0, SL6SIZE);
}
```

(`diskII` and `SL6SIZE` are already in scope via the existing `#include "rombios.h"` / `#include "Predef.h"`.)

- [ ] **Step 3: Fix `Booting()` to consider drive 2**

In `Apple2Machine::Booting()` (line ~40), change:

```cpp
	if (device.HasFloppy(0))
```

to:

```cpp
	if (device.HasFloppy(0) || device.HasFloppy(1))
```

(Otherwise Reset with a disk mounted only in drive 2 would strip the PROM and break `PR#6`.)

- [ ] **Step 4: Build**

Run: `arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/AppleII/Apple2Machine.h src/AppleII/Apple2Machine.cpp
git commit -m "feat: machine-level Mount/Unmount managing the Disk II PROM at C600

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Supervisor class shell + main-loop integration

**Files:**
- Create: `src/Supervisor/Supervisor.h`
- Create: `src/Supervisor/Supervisor.cpp`
- Modify: `ESP32-VGA_AppleII_Emulator.ino` (includes ~line 5, globals ~line 16, setup ~line 115, loop ~line 123)

**Interfaces:**
- Consumes: `machine->device.InvalidateRenderCache()`, `machine->device.GetDiskName(i)` (existing), `machine->device.supervisorRequested`, `AppleFont::RenderFont`, `VGA::dot/rgb`, global `keyboard_ptr`.
- Produces (used by Tasks 4-5): the complete `Supervisor` class declaration (all members/methods below are final — Tasks 4 and 5 only fill in stub bodies), plus `.ino` wiring. Deliverable: F1 freezes emulation and shows the menu chrome (title, drive lines, separators, help); ESC resumes cleanly.

- [ ] **Step 1: Create `src/Supervisor/Supervisor.h`** (complete, final version)

```cpp
#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include "../AppleII/Predef.h"
#include "../AppleII/AppleFont.h"

class Apple2Machine;
class VGA;

#define SUP_MAX_ENTRIES 128
#define SUP_NAME_LEN    64
#define SUP_PATH_LEN    256
#define SUP_LIST_TOP    4    // first text row of the list window
#define SUP_LIST_ROWS   16   // visible list rows

// one SD directory entry shown in the browser
struct SupEntry
{
	char name[SUP_NAME_LEN];
	bool isDir;
};

// SD card browser / disk mount menu, shown while emulation is paused
class Supervisor
{
public:
	Supervisor(Apple2Machine* machine);
	~Supervisor();

	bool IsActive() { return active; }
	void Open();
	void Close();
	void Update();               // sole PS2 keyboard reader while active
	void Render(VGA* vga);

private:
	enum Mode { BROWSE, PICK_DRIVE };

	Apple2Machine* machine;
	AppleColor* backbuffer;
	AppleFont font;

	bool active;
	Mode mode;

	char curPath[SUP_PATH_LEN];
	SupEntry entries[SUP_MAX_ENTRIES];
	int entryCount;
	bool sdError;

	int cursor;                  // index into the virtual list
	int scroll;                  // first visible virtual index
	char status[SCREENTEXT_X + 1];
	char pickPath[SUP_PATH_LEN]; // full path of file awaiting drive choice

	// virtual list layout: [0]=reset [1]=unmount d1 [2]=unmount d2
	// then ".." when not at root, then entries[]
	bool AtRoot() { return curPath[1] == '\0'; }
	int VirtualCount();
	void VirtualLabel(int index, char* out, int outlen);

	void ScanDir();
	void EnterDir(const char* name);
	void UpDir();
	void Select();               // Enter pressed in BROWSE mode
	void MountTo(int drive);
	void MoveCursor(int delta);

	void SetStatus(const char* msg);
	void DrawText(int col, int row, const char* text, bool inverse);
	void DrawRow(int row, const char* text, bool inverse);
};

#endif
```

- [ ] **Step 2: Create `src/Supervisor/Supervisor.cpp`** (shell version; Tasks 4-5 replace the stubs at the bottom)

```cpp
#include <Arduino.h>
#include <SD.h>
#include "fabgl.h"
#include "Supervisor.h"
#include "../AppleII/Apple2Machine.h"
#include "../VGA/VGA.h"

extern fabgl::Keyboard *keyboard_ptr;

Supervisor::Supervisor(Apple2Machine* m)
{
	machine = m;
	backbuffer = (AppleColor*)ps_malloc(SCREENSIZE_X * SCREENSIZE_Y * sizeof(AppleColor));
	font.Create();
	active = false;
	mode = BROWSE;
	strcpy(curPath, "/");
	entryCount = 0;
	sdError = false;
	cursor = 0;
	scroll = 0;
	status[0] = '\0';
	pickPath[0] = '\0';
}

Supervisor::~Supervisor()
{
	free(backbuffer);
}

void Supervisor::Open()
{
	active = true;
	mode = BROWSE;
	SetStatus("");
	ScanDir();
	if (sdError && !AtRoot())
	{
		// current dir vanished (SD swapped): retry from root
		strcpy(curPath, "/");
		ScanDir();
	}
}

void Supervisor::Close()
{
	active = false;
	// menu drew over the scanlines; force the emulator to repaint everything
	machine->device.InvalidateRenderCache();
}

void Supervisor::SetStatus(const char* msg)
{
	snprintf(status, sizeof(status), "%s", msg);
}

// ASCII -> Apple II font glyph: identity for 0x20-0x5F, no lowercase in the font
void Supervisor::DrawText(int col, int row, const char* text, bool inverse)
{
	for (int i = 0; text[i] != '\0' && (col + i) < SCREENTEXT_X; i++)
	{
		BYTE g = (BYTE)text[i];
		if (g >= 'a' && g <= 'z')
			g -= 32;
		if (g < 0x20 || g > 0x5F)
			g = 0x20;
		font.RenderFont(backbuffer, g, (col + i) * FONT_X, row * FONT_Y, inverse);
	}
}

// full 40-column row: pads with spaces (so inverse bars span the line),
// truncates over-long text with a trailing '~'
void Supervisor::DrawRow(int row, const char* text, bool inverse)
{
	char line[SCREENTEXT_X + 1];
	int len = strlen(text);
	if (len > SCREENTEXT_X)
	{
		memcpy(line, text, SCREENTEXT_X - 1);
		line[SCREENTEXT_X - 1] = '~';
		line[SCREENTEXT_X] = '\0';
	}
	else
		snprintf(line, sizeof(line), "%-40s", text);
	DrawText(0, row, line, inverse);
}

void Supervisor::Update()
{
	if (!keyboard_ptr)
		return;

	while (keyboard_ptr->virtualKeyAvailable())
	{
		bool keyDown = false;
		fabgl::VirtualKey vk = keyboard_ptr->getNextVirtualKey(&keyDown, 0);
		if (vk == fabgl::VK_NONE || !keyDown)
			continue;

		if (mode == PICK_DRIVE)
		{
			char ascii = keyboard_ptr->virtualKeyToASCII(vk);
			if (ascii == '1')
				MountTo(0);
			else if (ascii == '2')
				MountTo(1);
			else if (vk == fabgl::VK_ESCAPE)
				mode = BROWSE;
			continue;
		}

		switch (vk)
		{
			case fabgl::VK_UP:     MoveCursor(-1); break;
			case fabgl::VK_DOWN:   MoveCursor(1);  break;
			case fabgl::VK_RETURN: Select();       break;
			case fabgl::VK_ESCAPE: Close();        return;
			default: break;
		}
	}
}

void Supervisor::Render(VGA* vga)
{
	memset(backbuffer, 0, SCREENSIZE_X * SCREENSIZE_Y * sizeof(AppleColor));

	DrawRow(0, "              SUPERVISOR", false);

	char line[64];
	std::string d1 = machine->device.GetDiskName(0);
	std::string d2 = machine->device.GetDiskName(1);
	snprintf(line, sizeof(line), "D1: %s", d1.empty() ? "(EMPTY)" : d1.c_str());
	DrawRow(1, line, false);
	snprintf(line, sizeof(line), "D2: %s", d2.empty() ? "(EMPTY)" : d2.c_str());
	DrawRow(2, line, false);

	DrawRow(3, "----------------------------------------", false);

	char label[SUP_NAME_LEN + 24];
	int count = VirtualCount();
	for (int i = 0; i < SUP_LIST_ROWS; i++)
	{
		int idx = scroll + i;
		if (idx >= count)
			break;
		VirtualLabel(idx, label, sizeof(label));
		DrawRow(SUP_LIST_TOP + i, label, idx == cursor);
	}

	DrawRow(20, "----------------------------------------", false);

	if (mode == PICK_DRIVE)
		DrawRow(21, "MOUNT TO: 1)DRIVE 1 2)DRIVE 2 ESC)BACK", false);
	else
		DrawRow(21, status, false);

	DrawRow(22, curPath, false);
	DrawRow(23, " ARROWS:MOVE  ENTER:SELECT  ESC:EXIT", false);

	// same backbuffer->scanline copy the emulator uses (Apple2Machine::Render)
	for (int y = 0; y < SCREENSIZE_Y; y++)
		for (int x = 0; x < SCREENSIZE_X; x++)
		{
			AppleColor c = backbuffer[y * SCREENSIZE_X + x];
			vga->dot(x, y, vga->rgb(c.r, c.g, c.b));
		}
}

//////////////////////////////////////////////////////////////////////////
// Stubs - implemented in the next tasks (Task 4: browsing, Task 5: actions)

int Supervisor::VirtualCount()
{
	return 0;
}

void Supervisor::VirtualLabel(int index, char* out, int outlen)
{
	out[0] = '\0';
}

void Supervisor::ScanDir()
{
}

void Supervisor::EnterDir(const char* name)
{
}

void Supervisor::UpDir()
{
}

void Supervisor::Select()
{
}

void Supervisor::MountTo(int drive)
{
	mode = BROWSE;
}

void Supervisor::MoveCursor(int delta)
{
}
```

- [ ] **Step 3: Wire into `ESP32-VGA_AppleII_Emulator.ino`**

Add the include after the existing ones (line ~6):

```cpp
#include "src/Supervisor/Supervisor.h"
```

Add the global next to `Apple2Machine *machine;` (line ~16):

```cpp
Supervisor *supervisor;
```

In `setup()`, after `machine->InitMachine();` (line ~115), add:

```cpp
    supervisor = new Supervisor(machine);
```

Replace the first five lines of `loop()` (the `cycles`/`Run`/`Render`/`show` block, lines ~125-128) with:

```cpp
    if (supervisor->IsActive())
    {
        supervisor->Update();
        if (supervisor->IsActive())      // may have closed itself on ESC
            supervisor->Render(vga);
    }
    else
    {
        long long cycles = 17050 * 4;
        machine->Run(cycles);
        if (machine->device.supervisorRequested)
        {
            machine->device.supervisorRequested = false;
            supervisor->Open();
        }
        machine->Render(vga, frame);
    }
    vga->show();
```

Leave the frame counter, heap, and FPS code below it unchanged.

- [ ] **Step 4: Build**

Run: `arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/Supervisor/Supervisor.h src/Supervisor/Supervisor.cpp ESP32-VGA_AppleII_Emulator.ino
git commit -m "feat: Supervisor menu shell with pause/resume main-loop integration

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: SD directory browsing and list navigation

**Files:**
- Modify: `src/Supervisor/Supervisor.cpp` (replace the stub bodies of `VirtualCount`, `VirtualLabel`, `ScanDir`, `EnterDir`, `UpDir`, `MoveCursor`, `Select`; add one static comparator above them)

**Interfaces:**
- Consumes: `SD.open()`, `File::openNextFile/name/isDirectory` (Arduino ESP32 core 2.x: `name()` returns the basename, `path()` the full path — same as the `.ino`'s `listDir`), `machine->device.GetDiskName(drv)`.
- Produces: fully navigable browser. Action items (indices 0-2) render with real labels but `Select()` still ignores them (Task 5 wires them).

- [ ] **Step 1: Replace the stub section of `src/Supervisor/Supervisor.cpp`**

Delete everything below the `// Stubs - implemented in the next tasks` comment line (inclusive) and append:

```cpp
//////////////////////////////////////////////////////////////////////////
// Virtual list: [0]=reset [1]=unmount d1 [2]=unmount d2, then ".." when
// not at root, then the scanned entries

int Supervisor::VirtualCount()
{
	return 3 + (AtRoot() ? 0 : 1) + entryCount;
}

void Supervisor::VirtualLabel(int index, char* out, int outlen)
{
	if (index == 0)
	{
		snprintf(out, outlen, " [ RESET MACHINE ]");
		return;
	}
	if (index == 1 || index == 2)
	{
		int drv = index - 1;
		std::string name = machine->device.GetDiskName(drv);
		if (name.empty())
			snprintf(out, outlen, " [ UNMOUNT DRIVE %d ]", drv + 1);
		else
			snprintf(out, outlen, " [ UNMOUNT D%d: %s ]", drv + 1, name.c_str());
		return;
	}

	int idx = index - 3;
	if (!AtRoot())
	{
		if (idx == 0)
		{
			snprintf(out, outlen, " ..");
			return;
		}
		idx--;
	}

	if (entries[idx].isDir)
		snprintf(out, outlen, " %s/", entries[idx].name);
	else
		snprintf(out, outlen, " %s", entries[idx].name);
}

static int compareEntries(const void* a, const void* b)
{
	const SupEntry* ea = (const SupEntry*)a;
	const SupEntry* eb = (const SupEntry*)b;
	if (ea->isDir != eb->isDir)
		return ea->isDir ? -1 : 1;          // directories first
	return strcasecmp(ea->name, eb->name);
}

void Supervisor::ScanDir()
{
	entryCount = 0;
	sdError = false;
	cursor = 0;
	scroll = 0;

	File root = SD.open(curPath);
	if (!root || !root.isDirectory())
	{
		sdError = true;
		SetStatus("SD ERROR");
		return;
	}

	File file = root.openNextFile();
	while (file && entryCount < SUP_MAX_ENTRIES)
	{
		const char* name = file.name();
		if (name[0] != '.')                  // skip hidden entries
		{
			if (file.isDirectory())
			{
				snprintf(entries[entryCount].name, SUP_NAME_LEN, "%s", name);
				entries[entryCount].isDir = true;
				entryCount++;
			}
			else
			{
				int len = strlen(name);
				if (len > 4 && strcasecmp(name + len - 4, ".nib") == 0)
				{
					snprintf(entries[entryCount].name, SUP_NAME_LEN, "%s", name);
					entries[entryCount].isDir = false;
					entryCount++;
				}
			}
		}
		file = root.openNextFile();
	}
	root.close();

	qsort(entries, entryCount, sizeof(SupEntry), compareEntries);
}

void Supervisor::EnterDir(const char* name)
{
	char newPath[SUP_PATH_LEN];
	if (AtRoot())
		snprintf(newPath, sizeof(newPath), "/%s", name);
	else
		snprintf(newPath, sizeof(newPath), "%s/%s", curPath, name);
	strcpy(curPath, newPath);
	SetStatus("");
	ScanDir();
}

void Supervisor::UpDir()
{
	char* p = strrchr(curPath, '/');
	if (p == curPath)
		curPath[1] = '\0';                   // back at root: keep "/"
	else
		*p = '\0';
	SetStatus("");
	ScanDir();
}

void Supervisor::MoveCursor(int delta)
{
	int count = VirtualCount();
	if (count == 0)
		return;
	cursor += delta;
	if (cursor < 0) cursor = 0;
	if (cursor > count - 1) cursor = count - 1;
	if (cursor < scroll) scroll = cursor;
	if (cursor >= scroll + SUP_LIST_ROWS) scroll = cursor - SUP_LIST_ROWS + 1;
}

void Supervisor::Select()
{
	int index = cursor;

	if (index < 3)
	{
		// action items - wired up in Task 5
		return;
	}
	index -= 3;

	if (!AtRoot())
	{
		if (index == 0)
		{
			UpDir();
			return;
		}
		index--;
	}

	if (index >= entryCount)
		return;

	if (entries[index].isDir)
	{
		EnterDir(entries[index].name);
	}
	else
	{
		if (AtRoot())
			snprintf(pickPath, sizeof(pickPath), "/%s", entries[index].name);
		else
			snprintf(pickPath, sizeof(pickPath), "%s/%s", curPath, entries[index].name);
		mode = PICK_DRIVE;
	}
}

void Supervisor::MountTo(int drive)
{
	// wired up in Task 5
	mode = BROWSE;
}
```

- [ ] **Step 2: Build**

Run: `arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .`
Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git add src/Supervisor/Supervisor.cpp
git commit -m "feat: SD directory scanning, .nib filtering, and list navigation in Supervisor

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Mount, unmount, and reset actions

**Files:**
- Modify: `src/Supervisor/Supervisor.cpp` (`Select()` action branch and `MountTo()` only)

**Interfaces:**
- Consumes (Task 2): `machine->Mount(path, drive)`, `machine->Unmount(drive)`; (Task 1 via device): `machine->device.HasFloppy(drv)`; `machine->Reset()` (existing).
- Produces: feature-complete menu.

- [ ] **Step 1: Wire the action items in `Select()`**

In `src/Supervisor/Supervisor.cpp`, replace this block inside `Select()`:

```cpp
	if (index < 3)
	{
		// action items - wired up in Task 5
		return;
	}
	index -= 3;
```

with:

```cpp
	if (index == 0)                          // [ RESET MACHINE ]
	{
		machine->Reset();
		Close();                             // close so the boot is visible
		return;
	}
	if (index == 1 || index == 2)            // [ UNMOUNT DRIVE n ]
	{
		int drv = index - 1;
		if (machine->device.HasFloppy(drv))
		{
			machine->Unmount(drv);
			char msg[32];
			snprintf(msg, sizeof(msg), "UNMOUNTED DRIVE %d", drv + 1);
			SetStatus(msg);
		}
		else
			SetStatus(drv == 0 ? "DRIVE 1 EMPTY" : "DRIVE 2 EMPTY");
		return;
	}
	index -= 3;
```

- [ ] **Step 2: Implement `MountTo()`**

Replace the `MountTo` stub body:

```cpp
void Supervisor::MountTo(int drive)
{
	mode = BROWSE;
	if (machine->Mount(pickPath, drive))
	{
		char msg[32];
		snprintf(msg, sizeof(msg), "MOUNTED TO DRIVE %d", drive + 1);
		SetStatus(msg);
	}
	else
		SetStatus("LOAD FAILED");
}
```

(The ~230KB SD read blocks the loop for well under a second at 4MHz SPI — acceptable while paused.)

- [ ] **Step 3: Build**

Run: `arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .`
Expected: exit 0.

- [ ] **Step 4: Commit**

```bash
git add src/Supervisor/Supervisor.cpp
git commit -m "feat: mount/unmount/reset actions in Supervisor menu

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: On-hardware verification, docs, graph update

**Files:**
- Modify: `CLAUDE.md` (build commands section)

**Interfaces:**
- Consumes: everything above, a flashed board with an SD card containing `.nib` files (samples in `data/`: karateka, loderunner, u4/u5 images) in the root plus at least one subdirectory with a `.nib` inside.

- [ ] **Step 1: Flash and open the serial monitor**

```bash
arduino-cli upload --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" -p /dev/ttyUSB0 .
arduino-cli monitor -p /dev/ttyUSB0 -c 115200
```

(Adjust `-p` if the board enumerates elsewhere; check with `arduino-cli board list`.)

- [ ] **Step 2: Run the manual checklist** (all must pass; note failures verbatim)

1. Power-on with no disk lands at the BASIC/monitor prompt (no endless drive scan).
2. F1 opens the menu; emulation is visibly frozen behind/instead of the running program.
3. Arrow keys move the inverse cursor bar; long directories scroll; `..` is absent at root.
4. Enter on a subdirectory descends; `..` returns; path line updates.
5. Enter on a `.nib` → drive picker; `1` mounts to Drive 1; status shows `MOUNTED TO DRIVE 1`; D1 header line updates.
6. Machine state is untouched by mounting (resumes exactly where paused).
7. `[ RESET MACHINE ]` reboots and boots the mounted disk.
8. Mount a disk to Drive 2 only, reset: it still boots (PROM kept via the `HasFloppy(0) || HasFloppy(1)` fix).
9. Unmount both drives (status messages appear; empty-drive unmount says `DRIVE n EMPTY`), then reset → lands in BASIC.
10. ESC resumes with a fully repainted screen in a hires game (no menu remnants — cache invalidation works).
11. Serial heap/PSRAM logs stable across ≥5 open/close cycles (no leak).
12. FPS log unchanged vs. before when the menu is closed.

- [ ] **Step 3: Correct the build documentation in `CLAUDE.md`**

Replace the `## Build commands` section's PlatformIO commands with the verified reality (PlatformIO is not installed here; arduino-cli + ESP32 Dev Module FQBN is what builds):

```markdown
## Build commands

Built with arduino-cli (PlatformIO's `platformio.ini` is legacy config; `pio` is not installed). Entry point `ESP32-VGA_AppleII_Emulator.ino`, sources under `src/` per Arduino sketch layout.

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" .   # build
arduino-cli upload  --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app" -p /dev/ttyUSB0 .  # flash
arduino-cli monitor -p /dev/ttyUSB0 -c 115200                                             # serial monitor
```

Requires arduino-cli core `esp32:esp32` 2.0.x and the FabGL 1.0.9 user library.
```

Keep the rest of CLAUDE.md intact.

- [ ] **Step 4: Update the knowledge graph and commit**

```bash
graphify update .
git add CLAUDE.md graphify-out/
git commit -m "docs: correct build commands to arduino-cli; update knowledge graph

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
