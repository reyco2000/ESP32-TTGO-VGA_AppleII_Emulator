/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Supervisor.cpp
 *  Module : F1 supervisor menu. Pauses emulation, browses the SD
 *           card and mounts/unmounts .nib images into either drive
 *           via Apple2Machine. Paints directly into the live VGA
 *           framebuffer, repainting only when the dirty flag is
 *           set, and invalidates the emulator's render caches on
 *           close.
 * ============================================================
*/

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
	vga = NULL;
	dirty = true;
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
}

void Supervisor::Open()
{
	active = true;
	dirty = true;
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
		font.RenderFont(vga, g, (col + i) * FONT_X, row * FONT_Y, inverse);
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

		// every state change in this menu originates from a keypress
		dirty = true;

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
			case fabgl::VK_RETURN:
				Select();
				if (!active)
					return;
				break;
			case fabgl::VK_ESCAPE: Close();        return;
			default: break;
		}
	}
}

void Supervisor::Render(VGA* vgaOut)
{
	vga = vgaOut;
	if (vga == NULL)
		return;

	// The menu is painted straight into the live framebuffer, so clearing and
	// repainting it every frame is visible as flicker. Nothing else draws
	// while the supervisor is up, so repaint only when something changed.
	if (!dirty)
		return;
	dirty = false;

	vga->clear(0);

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
}

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
