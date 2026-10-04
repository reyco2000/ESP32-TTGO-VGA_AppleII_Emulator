/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Supervisor.cpp
 *  Module : F1 supervisor menu. Run() holds the emulation task
 *           until the menu is closed, which is what pauses the
 *           Apple. A grid of tiles leads to the disk manager, the
 *           setup list, reset and an About page; each screen is a
 *           function with its own key loop, drawn with the
 *           primitives in SupervisorUI. Settings changed in the
 *           menu are saved to NVS on the way out, and the
 *           emulator's render caches are invalidated so it paints
 *           over the menu.
 * ============================================================
*/

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "fabgl.h"
#include "Supervisor.h"
#include "SupervisorUI.h"
#include "../AppleII/Apple2Machine.h"
#include "../Version.h"
#include "../BuildConfig.h"
#include "../AppleII/RomLoader.h"
#include "../Tools/Settings.h"
#include "../Tools/KeyboardLayouts.h"
#include "../Tools/Bootloader.h"
#include "../AppleII/SuperSerialCard.h"

using namespace SupUI;

extern fabgl::Keyboard *keyboard_ptr;
extern fabgl::Canvas Canvas;

Supervisor::Supervisor(Apple2Machine* m)
{
	machine = m;
	requested = false;
	bootNoteShown = false;
	entries = NULL;
	entryCount = 0;
	listError = NULL;
	strcpy(curPath, "/");
}

//////////////////////////////////////////////////////////////////////////
// Main menu: icon tiles

#define TILE_W       160
#define TILE_H       62
#define TILE_X0      (BOX_X + 16)
#define TILE_Y0      HEAD_Y
#define TILE_PITCH_X 176
#define TILE_PITCH_Y 68
#define TILE_COLS    3
#define TILE_COUNT   5

enum { TILE_DISKS, TILE_SETUP, TILE_RESET, TILE_ABOUT, TILE_RESUME };
static const char* const tileLabels[TILE_COUNT] = { "Disks", "Setup", "Reset", "About", "Resume" };
static const uint8_t tileIcons[TILE_COUNT] = { ICON_DISK, ICON_SETUP, ICON_RESET, ICON_ABOUT, ICON_RESUME };

static void DrawTile(fabgl::Canvas& cv, int idx, bool selected)
{
	int x = TILE_X0 + (idx % TILE_COLS) * TILE_PITCH_X;
	int y = TILE_Y0 + (idx / TILE_COLS) * TILE_PITCH_Y;
	fabgl::RGB888 bg = selected ? T_ACCENT : T_TEXT;
	fabgl::RGB888 fg = selected ? T_TEXT : T_BG;

	FillBox(cv, bg, x, y, x + TILE_W - 1, y + TILE_H - 1);
	// thin accent border; the selected tile gets a double white one. A side
	// is two pixels for every row of the top and bottom.
	cv.setPenColor(selected ? T_TEXT : T_ACCENT);
	for (int i = 0; i < (selected ? 4 : 2); i++)
		cv.drawRectangle(x + i, y + i / 2, x + TILE_W - 1 - i, y + TILE_H - 1 - i / 2);

	DrawBigIcon(cv, tileIcons[idx], x + (TILE_W - 64) / 2, y + 8, fg, bg);

	cv.setBrushColor(bg);
	cv.setPenColor(fg);
	TextCentered(cv, &fabgl::FONT_8x14, x, TILE_W, y + 42, tileLabels[idx]);
}

// frame and every tile: the main menu's first paint, and what a popup that
// opens before it sits on
void Supervisor::DrawMain(int selected)
{
	char title[40];
	snprintf(title, sizeof(title), "%s SUPERVISOR", machine->profile.name);
	DrawFrame(Canvas, title, "Arrows   ENTER Select   ESC/F1 Exit");
	for (int i = 0; i < TILE_COUNT; i++)
		DrawTile(Canvas, i, i == selected);
}

// returns the chosen tile, or -1 on Esc/F1
int Supervisor::MainMenu(int selected)
{
	fabgl::Canvas& cv = Canvas;
	if (selected < 0 || selected >= TILE_COUNT)
		selected = 0;
	DrawMain(selected);

	int drawn = selected;
	for (;;)
	{
		// only the tile the cursor left and the one it reached
		if (drawn != selected)
		{
			DrawTile(cv, drawn, false);
			DrawTile(cv, selected, true);
			drawn = selected;
		}
		cv.waitCompletion();
		Trace("menu: SUPERVISOR > %s\n", tileLabels[selected]);

		switch (WaitKey())
		{
			case K_LEFT:  selected = GridMove(selected, -1, 0, TILE_COLS, TILE_COUNT); break;
			case K_RIGHT: selected = GridMove(selected, 1, 0, TILE_COLS, TILE_COUNT); break;
			case K_UP:    selected = GridMove(selected, 0, -1, TILE_COLS, TILE_COUNT); break;
			case K_DOWN:  selected = GridMove(selected, 0, 1, TILE_COLS, TILE_COUNT); break;
			case K_ENTER: return selected;
			case K_ESC:   return -1;
			case K_F1:    closeAll = true; return -1;
			default: break;
		}
	}
}

void Supervisor::Run()
{
	requested = false;
	serialKeys = (machine->device.SerialSlot() == 0);
	Begin(keyboard_ptr);
	Trace("menu: opened\n");

	// what the menu can change and only saves on the way out
	uint8_t kbd0 = CurrentKeyboardLayoutId();
	uint8_t speed0 = machine->device.speedMode;
	bool debug0 = debugOn;
	bool reset = false;

	// why the saved model could not boot, if it could not: said once
	if (!bootNoteShown && machine->bootNote[0])
	{
		bootNoteShown = true;
		DrawMain(0);
		Notice("ROMs missing", "Booted the Apple ][+ instead.", "ROM files go in /roms");
	}

	int sel = 0;
	while (!closeAll)
	{
		sel = MainMenu(sel);
		if (sel < 0 || sel == TILE_RESUME)
			break;
		switch (sel)
		{
			case TILE_DISKS: DiskMenu(); break;
			case TILE_SETUP: SetupMenu(); break;
			case TILE_RESET:
			{
				char msg[40];
				snprintf(msg, sizeof(msg), "Reset the %s?", machine->profile.name);
				if (Confirm("Reset", msg, nullptr))
				{
					reset = true;            // after closing, so the boot is visible
					closeAll = true;
				}
				break;
			}
			case TILE_ABOUT: AboutScreen(); break;
		}
	}

	if (CurrentKeyboardLayoutId() != kbd0)
		Settings::SaveKeyboard(CurrentKeyboardLayoutId());
	if (machine->device.speedMode != speed0)
		Settings::SaveSpeed(machine->device.speedMode);
	if (debugOn != debug0)
		Settings::SaveDebug(debugOn);
	if (reset)
		machine->Reset();
	Trace("menu: closed%s\n", reset ? ", reset" : "");
	// the menu drew over the Apple's picture: have it repaint everything
	machine->device.InvalidateRenderCache();
}

//////////////////////////////////////////////////////////////////////////
// About

// CoCo Byte Club logo, 91x12, one bit per pixel in XBM order (LSB first,
// 12 bytes a row).
#define LOGO_W         91
#define LOGO_H         12
#define LOGO_ROW_BYTES 12
static const unsigned char logoCocoByte[LOGO_H * LOGO_ROW_BYTES] =
{
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xf0, 0x01, 0x1b, 0xf0, 0x01, 0x1b, 0xfe, 0xf9, 0x7c, 0xff, 0xfd, 0x03,
	0xf8, 0x83, 0x3b, 0xf8, 0x83, 0x3b, 0xfe, 0xfb, 0x7c, 0xff, 0xfd, 0x03,
	0xfc, 0xc7, 0x7b, 0xfc, 0xc7, 0x7b, 0xfe, 0xfb, 0x7c, 0xff, 0xfd, 0x03,
	0xfe, 0xef, 0xfb, 0xfe, 0xef, 0xfb, 0xf0, 0xfb, 0x7f, 0xff, 0x7d, 0x00,
	0xfe, 0xe1, 0xfb, 0xfe, 0xe1, 0xfb, 0xfe, 0xfb, 0x7f, 0x7c, 0xfc, 0x03,
	0x7e, 0xe0, 0xfb, 0x7e, 0xe0, 0xfb, 0xfe, 0xf1, 0x3f, 0x7c, 0xfc, 0x03,
	0xfe, 0xe1, 0xfb, 0xfe, 0xe1, 0xfb, 0xfe, 0xe3, 0x1f, 0x7c, 0xfc, 0x03,
	0xfe, 0xef, 0xfb, 0xfe, 0xef, 0xfb, 0xe0, 0xc3, 0x0f, 0x7c, 0x7c, 0x00,
	0xfc, 0xc7, 0x7b, 0xfc, 0xc7, 0x7b, 0xfe, 0xc3, 0x0f, 0x7c, 0xfc, 0x07,
	0xf8, 0x83, 0x3b, 0xf8, 0x83, 0x3b, 0xfe, 0xc3, 0x0f, 0x7c, 0xfc, 0x07,
	0xf0, 0x01, 0x1b, 0xf0, 0x01, 0x1b, 0xfe, 0xc1, 0x0f, 0x7c, 0xfc, 0x07
};

// The logo at four screen pixels a dot, each run of set dots as one box.
static void DrawLogo(fabgl::Canvas& cv, int y)
{
	const int sx = 4;
	int x0 = BOX_X + (BOX_W - LOGO_W * sx) / 2;
	for (int row = 0; row < LOGO_H; row++)
	{
		int start = -1;
		for (int col = 0; col <= LOGO_W; col++)
		{
			bool on = col < LOGO_W && (logoCocoByte[row * LOGO_ROW_BYTES + (col >> 3)] & (1 << (col & 7)));
			if (on && start < 0)
				start = col;
			else if (!on && start >= 0)
			{
				FillBox(cv, T_GREEN, x0 + start * sx, y + row, x0 + col * sx - 1, y + row);
				start = -1;
			}
		}
	}
}

void Supervisor::AboutScreen()
{
	fabgl::Canvas& cv = Canvas;
	DrawFrame(cv, "About", "ESC Back   F1 Exit");
	DrawLogo(cv, HEAD_Y);

	static const char* const lines[] =
	{
		"Apple II emulator for ESP32",
		"Reinaldo Torres & Claude Code",
		"CoCoByte Club",
		"codesafe ESP32-VGA_AppleII_Emulator",
		"MIT License",
	};
	cv.setBrushColor(T_BG);
	int y = HEAD_Y + 16;
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++, y += 10)
	{
		cv.setPenColor(i < 3 ? T_TEXT : T_GREY);
		TextCentered(cv, &fabgl::FONT_6x8, BOX_X, BOX_W, y, lines[i]);
	}
	y += 1;
	FillBox(cv, T_ACCENT, BOX_X + 48, y, BOX_X + BOX_W - 49, y);
	y += 4;

	Row info[5];
	for (Row& r : info)
		r.dim = false;
	strcpy(info[0].label, "Machine");
	strlcpy(info[0].value, machine->profile.name, sizeof(info[0].value));
	strcpy(info[1].label, "Version");
#if BUILD_TARGET == BUILD_TARGET_BOOTLOADER
	strcpy(info[1].value, FW_VERSION_STR " (SD bootloader)");
#else
	strcpy(info[1].value, FW_VERSION_STR);
#endif
	strcpy(info[2].label, "Build date");
	strcpy(info[2].value, FW_BUILD_DATE);
	strcpy(info[3].label, "PSRAM free");
	snprintf(info[3].value, sizeof(info[3].value), "%u KB",
	         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	strcpy(info[4].label, "PSRAM total");
	snprintf(info[4].value, sizeof(info[4].value), "%u KB",
	         (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
	for (int i = 0; i < 5; i++, y += LIST_ROW_H + 1)
	{
		DrawRowAt(cv, LIST_X, y, LIST_W, 24, true, info[i], false, T_BG);
		Trace("about: %s %s\n", info[i].label, info[i].value);
	}
	cv.waitCompletion();

	for (;;)
	{
		SupKey key = WaitKey();
		if (key == K_F1)
			closeAll = true;
		if (key == K_F1 || key == K_ESC || key == K_ENTER)
			return;
	}
}

//////////////////////////////////////////////////////////////////////////
// Disk manager

// Drive buttons across the top, the SD card's folders and disk images below.

#define DRIVE_COUNT     3                // D1, D2 and the hard disk in slot 7
#define DRIVE_HD        2
#define DRIVE_BTN_W     168
#define DRIVE_BTN_H     36
#define DRIVE_BTN_PITCH 176
#define DRIVE_NAME_COLS 12               // 6x8 font at double width
#define DISK_LIST_Y     (HEAD_Y + DRIVE_BTN_H + 4)
#define DISK_LIST_ROWS  ((HINT_Y - 4 - DISK_LIST_Y) / LIST_ROW_H)

static const char* const driveLabels[DRIVE_COUNT] = { "D1", "D2", "HD" };

// file name without its directory
static const char* BaseName(const char* path)
{
	const char* p = strrchr(path, '/');
	return p ? p + 1 : path;
}

static int compareEntries(const void* a, const void* b)
{
	const SupEntry* ea = (const SupEntry*)a;
	const SupEntry* eb = (const SupEntry*)b;
	if (ea->isDir != eb->isDir)
		return ea->isDir ? -1 : 1;          // directories first
	return strcasecmp(ea->name, eb->name);
}

// Reads curPath into entries. On failure listError says why and the list is
// empty; a folder that has gone (the card was swapped) falls back to the root.
void Supervisor::ScanDir()
{
	entryCount = 0;
	listError = NULL;
	if (!entries)
		entries = (SupEntry*)heap_caps_calloc(SUP_MAX_ENTRIES, sizeof(SupEntry), MALLOC_CAP_SPIRAM);
	if (!entries)
	{
		listError = "(out of memory for the file list)";
		return;
	}

	File root = SD.open(curPath);
	if ((!root || !root.isDirectory()) && !AtRoot())
	{
		if (root)
			root.close();
		strcpy(curPath, "/");
		root = SD.open(curPath);
	}
	if (!root || !root.isDirectory())
	{
		if (root)
			root.close();
		listError = "(SD card error)";
		return;
	}

	for (File file = root.openNextFile(); file && entryCount < SUP_MAX_ENTRIES; file = root.openNextFile())
	{
		const char* name = file.name();
		// hidden entries, and names too long for an entry, are left out
		if (name[0] == '.' || strlen(name) >= SUP_NAME_LEN)
			continue;
		bool isDir = file.isDirectory();
		if (!isDir && DskImage::TypeFromPath(name) == DskImage::IMAGE_NONE)
			continue;
		strcpy(entries[entryCount].name, name);
		entries[entryCount].isDir = isDir;
		entryCount++;
	}
	root.close();

	qsort(entries, entryCount, sizeof(SupEntry), compareEntries);
	if (entryCount == 0 && AtRoot())
		listError = "(no disk images on the SD card)";
}

// rows of the list: ".." below the root, then the entries
int Supervisor::ListCount()
{
	if (listError)
		return 0;
	return (AtRoot() ? 0 : 1) + entryCount;
}

void Supervisor::ListLabel(int index, char* out, int outlen)
{
	if (!AtRoot())
	{
		if (index == 0)
		{
			snprintf(out, outlen, "..  (up one level)");
			return;
		}
		index--;
	}
	snprintf(out, outlen, entries[index].isDir ? "%s/" : "%s", entries[index].name);
}

// path of the image in a drive, "" when it is empty
const char* Supervisor::DriveImage(int btn, char* buf, int buflen)
{
	if (btn == DRIVE_HD)
		snprintf(buf, buflen, "%s", machine->device.hdd7.ImageName());
	else
		snprintf(buf, buflen, "%s", machine->device.GetDiskName(btn).c_str());
	return buf;
}

// Icon (grey when the drive is empty), drive name, and the image's name on
// two short lines without its extension.
void Supervisor::DrawDriveButton(int btn, bool focused)
{
	fabgl::Canvas& cv = Canvas;
	int x = BOX_X + 12 + btn * DRIVE_BTN_PITCH, y = HEAD_Y;
	char path[SUP_PATH_LEN];
	bool mounted = DriveImage(btn, path, sizeof(path))[0] != '\0';
	fabgl::RGB888 bg = focused ? T_ACCENT : T_TEXT;
	fabgl::RGB888 fg = focused ? T_TEXT : T_BG;

	FillBox(cv, bg, x, y, x + DRIVE_BTN_W - 1, y + DRIVE_BTN_H - 1);
	cv.setPenColor(focused ? T_TEXT : T_ACCENT);
	for (int i = 0; i < (focused ? 4 : 2); i++)
		cv.drawRectangle(x + i, y + i / 2, x + DRIVE_BTN_W - 1 - i, y + DRIVE_BTN_H - 1 - i / 2);

	DrawSmallIcon(cv, btn == DRIVE_HD ? ICON_HDD : ICON_DISK, x + 10, y + 4, fg, bg, !mounted);

	cv.setBrushColor(bg);
	cv.setPenColor(fg);
	Text(cv, &fabgl::FONT_8x8, x + 52, y + 6, driveLabels[btn]);

	char line1[DRIVE_NAME_COLS + 1], line2[DRIVE_NAME_COLS + 1];
	if (mounted)
		SplitName(path, DRIVE_NAME_COLS, line1, line2);
	else
	{
		strcpy(line1, "empty");
		line2[0] = '\0';
	}
	cv.setPenColor(mounted ? fg : T_GREY);
	Text(cv, &fabgl::FONT_6x8, x + 10, y + 17, line1);
	if (line2[0])
		Text(cv, &fabgl::FONT_6x8, x + 10, y + 26, line2);
}

void Supervisor::DrawListRow(int index, int top, bool hl)
{
	Row row;
	row.value[0] = 0;
	row.dim = false;
	ListLabel(index, row.label, sizeof(row.label));
	DrawRowAt(Canvas, LIST_X, DISK_LIST_Y + (index - top) * LIST_ROW_H, LIST_W, 8, true, row, hl, T_BG);
}

// the mount popup's rows: each floppy drive and what it holds, then Cancel
static Apple2Machine* popupMachine;

static void MountRow(int idx, Row& row)
{
	if (idx >= 2)
	{
		strcpy(row.label, "Cancel");
		return;
	}
	strcpy(row.label, driveLabels[idx]);
	std::string path = popupMachine->device.GetDiskName(idx);
	if (path.empty())
		strcpy(row.value, "(empty)");
	else
	{
		char unused[2];
		SplitName(path.c_str(), 20, row.value, unused);
	}
}

// Asks which drive takes a floppy image; a hard disk image has only slot 7 to
// go to. Asks again before replacing a disk or mounting an image twice.
void Supervisor::MountDialog(const char* path)
{
	char m1[40], m2[40], unused[2];
	const char* name = BaseName(path);

	if (HardDiskCard::IsHardDiskImage(path))
	{
		const char* cur = machine->device.hdd7.ImageName();
		if (strcmp(cur, path) == 0)
		{
			Notice("Hard disk", "Already mounted on HD.", nullptr);
			return;
		}
		if (cur[0])
		{
			SplitName(cur, 27, m2, unused);
			if (!Confirm("Replace disk?", "HD already has a disk:", m2))
				return;
		}
		if (machine->MountHardDisk(path))
			Trace("disks: mounted %s on HD\n", name);
		else
			Notice("Mount failed", "Could not load the image:", name);
		return;
	}

	int first = 0;
	for (int d = 1; d >= 0; d--)
		if (!machine->device.HasFloppy(d))
			first = d;
	popupMachine = machine;
	int drive = Popup("Mount disk on", name, nullptr, 3, MountRow, first);
	if (drive < 0 || drive >= 2)
		return;

	std::string cur = machine->device.GetDiskName(drive);
	if (cur == path)
		return;                              // already there
	if (!cur.empty())
	{
		snprintf(m1, sizeof(m1), "%s already has a disk:", driveLabels[drive]);
		SplitName(cur.c_str(), 27, m2, unused);
		if (!Confirm("Replace disk?", m1, m2))
			return;
	}
	else if (machine->device.GetDiskName(1 - drive) == path)
	{
		snprintf(m1, sizeof(m1), "Already mounted on %s.", driveLabels[1 - drive]);
		snprintf(m2, sizeof(m2), "Mount on %s as well?", driveLabels[drive]);
		if (!Confirm("Mount again?", m1, m2))
			return;
	}
	if (machine->Mount(path, drive))
		Trace("disks: mounted %s on %s\n", name, driveLabels[drive]);
	else
		Notice("Mount failed", "Could not load the image:", name);
}

void Supervisor::UnmountDialog(int btn)
{
	char path[SUP_PATH_LEN], m1[40], m2[40], unused[2];
	if (!DriveImage(btn, path, sizeof(path))[0])
		return;
	snprintf(m1, sizeof(m1), "Unmount %s?", driveLabels[btn]);
	SplitName(path, 27, m2, unused);
	if (!Confirm("Unmount disk", m1, m2))
		return;
	if (btn == DRIVE_HD)
		machine->UnmountHardDisk();
	else
		machine->Unmount(btn);
	Trace("disks: unmounted %s\n", driveLabels[btn]);
}

bool Supervisor::Activate(int index)
{
	if (!AtRoot())
	{
		if (index == 0)
		{
			// up one level
			char* p = strrchr(curPath, '/');
			if (p == curPath)
				curPath[1] = '\0';           // back at the root: keep "/"
			else
				*p = '\0';
			ScanDir();
			return true;
		}
		index--;
	}
	if (index >= entryCount)
		return false;

	char path[SUP_PATH_LEN];
	int len = snprintf(path, sizeof(path), AtRoot() ? "/%s" : "%s/%s",
	                   AtRoot() ? entries[index].name : curPath, entries[index].name);
	if (len >= (int)sizeof(path))
	{
		Notice("Path too long", "This folder is nested too", "deep to open.");
		return false;
	}
	if (entries[index].isDir)
	{
		strcpy(curPath, path);
		ScanDir();
		return true;
	}
	MountDialog(path);
	return false;
}

void Supervisor::DiskMenu()
{
	fabgl::Canvas& cv = Canvas;
	ScanDir();
	int btn = 0, sel = 0, top = 0;
	bool inDrives = (ListCount() == 0);      // focus: drive buttons or file list
	bool full = true;                        // repaint the whole screen
	int pBtn = -1, pSel = -1, pTop = -1;
	bool pIn = false;

	while (!closeAll)
	{
		int count = ListCount();
		if (full)
		{
			DrawFrame(cv, "Disk Manager", "Arrows   TAB Drives/Files   ENTER   ESC Back");
			pBtn = pSel = pTop = -1;
		}
		// buttons: every one on a full repaint, else the ones whose focus changed
		for (int b = 0; b < DRIVE_COUNT; b++)
		{
			bool now = inDrives && b == btn;
			bool was = pIn && b == pBtn;
			if (full || now != was)
				DrawDriveButton(b, now);
		}
		// list: all of it when it scrolled, else the two rows the cursor touched
		if (full || top != pTop)
		{
			FillBox(cv, T_BG, LIST_X, DISK_LIST_Y, LIST_X + LIST_W - 1,
			        DISK_LIST_Y + DISK_LIST_ROWS * LIST_ROW_H - 1);
			for (int r = 0; r < DISK_LIST_ROWS && top + r < count; r++)
				DrawListRow(top + r, top, !inDrives && top + r == sel);
			if (listError)
			{
				cv.setBrushColor(T_BG);
				cv.setPenColor(T_GREY);
				Text(cv, &fabgl::FONT_8x8, LIST_X + 8, DISK_LIST_Y, listError);
			}
			DrawScrollbar(cv, DISK_LIST_Y, DISK_LIST_ROWS * LIST_ROW_H, top, DISK_LIST_ROWS, count);
		}
		else if (count > 0 && (sel != pSel || inDrives != pIn))
		{
			if (pSel != sel && pSel >= top && pSel < top + DISK_LIST_ROWS)
				DrawListRow(pSel, top, false);
			DrawListRow(sel, top, !inDrives);
		}
		cv.waitCompletion();
		full = false;
		pBtn = btn; pSel = sel; pTop = top; pIn = inDrives;

		if (inDrives)
		{
			char path[SUP_PATH_LEN];
			DriveImage(btn, path, sizeof(path));
			Trace("disks: [%s] %s\n", driveLabels[btn], path[0] ? BaseName(path) : "(empty)");
		}
		else
		{
			char label[SUP_NAME_LEN + 4];
			ListLabel(sel, label, sizeof(label));
			Trace("disks: file %s\n", label);
		}

		SupKey key = WaitKey();
		if (key == K_ESC)
			return;
		if (key == K_F1)
		{
			closeAll = true;
			return;
		}

		if (inDrives)
		{
			switch (key)
			{
				case K_LEFT:  if (btn > 0) btn--; break;
				case K_RIGHT: if (btn < DRIVE_COUNT - 1) btn++; break;
				case K_DOWN:
				case K_TAB:   if (count > 0) inDrives = false; break;
				case K_ENTER: UnmountDialog(btn); full = true; break;
				default: break;
			}
		}
		else
		{
			switch (key)
			{
				case K_UP:
					if (sel > 0)
						ListMove(key, count, DISK_LIST_ROWS, sel, top);
					else
						inDrives = true;
					break;
				case K_TAB:
					inDrives = true;
					break;
				case K_ENTER:
					if (Activate(sel))
						sel = top = 0;
					if (ListCount() == 0)
						inDrives = true;
					full = true;
					break;
				default:
					ListMove(key, count, DISK_LIST_ROWS, sel, top);
					break;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Setup

void Supervisor::SetupMenu()
{
}
