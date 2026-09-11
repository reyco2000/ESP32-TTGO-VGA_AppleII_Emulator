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
 *           via Apple2Machine, and shows an ABOUT page with the
 *           firmware version and credits. Paints directly into the
 *           live VGA framebuffer in its own palette, repainting
 *           only when the dirty flag is set, and invalidates the
 *           emulator's render caches on close.
 * ============================================================
*/

#include <Arduino.h>
#include <SD.h>
#include "fabgl.h"
#include "Supervisor.h"
#include "../AppleII/Apple2Machine.h"
#include "../VGA/VGA.h"
#include "../Version.h"

extern fabgl::Keyboard *keyboard_ptr;

//////////////////////////////////////////////////////////////////////////
// Palette. The framebuffer holds 4-bit indices; while the menu is up the
// 16 palette entries are these colours instead of the Apple's, loaded on
// the first repaint after Open(). AppleVideo puts its own back on Close().
enum
{
	C_BLACK, C_BG, C_BARBG, C_WHITE, C_GREY, C_DIM, C_CYAN, C_DIMCYAN,
	C_GREEN, C_YELLOW, C_AMBER, C_ORANGE, C_RED, C_MAGENTA, C_BLUE
};

// RGB222: levels 0-3 per channel, all the DAC has
#define LVL(n) ((n) * 85)
static const VGAColor supPalette[16] =
{
	{ LVL(0), LVL(0), LVL(0) },   // C_BLACK
	{ LVL(0), LVL(0), LVL(1) },   // C_BG      dark navy page field
	{ LVL(0), LVL(0), LVL(2) },   // C_BARBG   title / footer bar fill
	{ LVL(3), LVL(3), LVL(3) },   // C_WHITE
	{ LVL(2), LVL(2), LVL(2) },   // C_GREY
	{ LVL(1), LVL(1), LVL(1) },   // C_DIM
	{ LVL(0), LVL(3), LVL(3) },   // C_CYAN
	{ LVL(0), LVL(2), LVL(2) },   // C_DIMCYAN
	{ LVL(0), LVL(3), LVL(0) },   // C_GREEN
	{ LVL(3), LVL(3), LVL(0) },   // C_YELLOW
	{ LVL(3), LVL(2), LVL(0) },   // C_AMBER
	{ LVL(3), LVL(1), LVL(0) },   // C_ORANGE
	{ LVL(3), LVL(0), LVL(0) },   // C_RED
	{ LVL(3), LVL(0), LVL(3) },   // C_MAGENTA
	{ LVL(1), LVL(1), LVL(3) },   // C_BLUE
	{ LVL(0), LVL(0), LVL(0) },   // unused
};
#undef LVL

// Apple logo stripe order, used for the rule under the title and the
// accent band down the right margin.
static const int stripe[6] = { C_GREEN, C_YELLOW, C_ORANGE,
                               C_RED,   C_MAGENTA, C_BLUE };

// The 40x24 text grid is drawn at double width, 560x192 on the 640x200
// framebuffer: a 40px margin either side and 4px top and bottom for chrome.
#define SUP_ORIGIN_X 40
#define SUP_ORIGIN_Y 4
#define SUP_CELL_W   (FONT_X * 2)

static inline int ColX(int col) { return SUP_ORIGIN_X + col * SUP_CELL_W; }
static inline int RowY(int row) { return SUP_ORIGIN_Y + row * FONT_Y; }

Supervisor::Supervisor(Apple2Machine* m)
{
	machine = m;
	vga = NULL;
	dirty = true;
	paletteSet = false;
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
	paletteSet = false;
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
void Supervisor::DrawText(int col, int row, const char* text, int fg, int bg)
{
	for (int i = 0; text[i] != '\0' && (col + i) < SCREENTEXT_X; i++)
	{
		BYTE g = (BYTE)text[i];
		if (g >= 'a' && g <= 'z')
			g -= 32;
		if (g < 0x20 || g > 0x5F)
			g = 0x20;
		font.RenderFont(vga, g, ColX(col + i), RowY(row), false, fg, bg);
	}
}

// full 40-column row: pads with spaces (so selection bars span the line),
// truncates over-long text with a trailing '~'
void Supervisor::DrawRow(int row, const char* text, int fg, int bg)
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
	DrawText(0, row, line, fg, bg);
}

// A row whose background bleeds past the text grid to the screen edge, so the
// title and footer read as full-width bars rather than floating strips.
void Supervisor::DrawBar(int row, const char* text, int fg, int bg)
{
	int y   = RowY(row);
	int top = (row == 0) ? 0 : y;
	int bot = (row == SCREENTEXT_Y - 1) ? VGA_HEIGHT : y + FONT_Y;
	vga->fillRect(0, top, VGA_WIDTH, bot - top, bg);
	DrawRow(row, text, fg, bg);
}

// Six-band Apple stripe rule, drawn in the middle of a text row. The font has
// no box-drawing glyphs, so every rule and panel here is a pixel fill.
void Supervisor::DrawRule(int row)
{
	int y = RowY(row) + 2;
	for (int i = 0; i < 6; i++)
	{
		int x0 = (VGA_WIDTH * i) / 6;
		int x1 = (VGA_WIDTH * (i + 1)) / 6;
		vga->fillRect(x0, y, x1 - x0, 3, stripe[i]);
	}
}

// Page field plus the vertical stripe accent in the right margin.
void Supervisor::DrawChrome()
{
	vga->clear(C_BG);

	int top    = RowY(SUP_LIST_TOP);
	int bottom = RowY(SUP_LIST_TOP + SUP_LIST_ROWS);
	int band   = (bottom - top) / 6;
	for (int i = 0; i < 6; i++)
		vga->fillRect(612, top + i * band, 16, band, stripe[i]);
}

// Colour for one list row. The selection bar is a swapped fg/bg pair rather
// than the font's inverse glyph table, so it can be any colour.
int Supervisor::RowColor(int index, bool selected, int* bg)
{
	if (selected)
	{
		*bg = C_CYAN;
		return C_BLACK;
	}

	*bg = C_BG;
	if (index < SUP_ACTION_COUNT)
		return C_YELLOW;                     // [ ... ] action items

	int idx = index - SUP_ACTION_COUNT;
	if (!AtRoot())
	{
		if (idx == 0)
			return C_CYAN;                   // ".." is a directory too
		idx--;
	}
	if (idx < entryCount && entries[idx].isDir)
		return C_CYAN;
	return C_WHITE;                          // .nib files
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

		if (mode == ABOUT)
		{
			// ESC backs out to the browser here; it must not close the
			// supervisor and resume emulation.
			if (vk == fabgl::VK_ESCAPE || vk == fabgl::VK_RETURN)
				mode = BROWSE;
			continue;
		}

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

	// The framebuffer holds palette indices: switch to the menu's colours
	// before painting with them.
	if (!paletteSet)
	{
		vga->setPalette(supPalette);
		paletteSet = true;
	}

	// The menu is painted straight into the live framebuffer, so clearing and
	// repainting it every frame is visible as flicker. Nothing else draws
	// while the supervisor is up, so repaint only when something changed.
	if (!dirty)
		return;
	dirty = false;

	if (mode == ABOUT)
		RenderAbout();
	else
		RenderBrowse();
}

void Supervisor::RenderBrowse()
{
	DrawChrome();
	DrawBar(0, "               SUPERVISOR", C_WHITE, C_BARBG);

	char line[64];
	std::string d1 = machine->device.GetDiskName(0);
	std::string d2 = machine->device.GetDiskName(1);
	snprintf(line, sizeof(line), "D1: %s", d1.empty() ? "(EMPTY)" : d1.c_str());
	DrawRow(1, line, d1.empty() ? C_GREY : C_GREEN, C_BG);
	snprintf(line, sizeof(line), "D2: %s", d2.empty() ? "(EMPTY)" : d2.c_str());
	DrawRow(2, line, d2.empty() ? C_GREY : C_GREEN, C_BG);

	DrawRule(3);

	char label[SUP_NAME_LEN + 24];
	int count = VirtualCount();
	for (int i = 0; i < SUP_LIST_ROWS; i++)
	{
		int idx = scroll + i;
		if (idx >= count)
			break;
		VirtualLabel(idx, label, sizeof(label));
		int bg;
		int fg = RowColor(idx, idx == cursor, &bg);
		DrawRow(SUP_LIST_TOP + i, label, fg, bg);
	}

	vga->fillRect(0, RowY(20) + 3, 320, 1, C_DIM);

	if (mode == PICK_DRIVE)
		DrawRow(21, "MOUNT TO: 1)DRIVE 1 2)DRIVE 2 ESC)BACK", C_YELLOW, C_BG);
	else
		DrawRow(21, status, sdError ? C_RED : C_AMBER, C_BG);

	DrawRow(22, curPath, C_DIMCYAN, C_BG);
	DrawBar(23, " ARROWS:MOVE  ENTER:SELECT  ESC:EXIT", C_GREY, C_BARBG);
}

// Version and credits. The font is uppercase-only, glyphs 0x20-0x5F, so every
// string here stays inside that range.
void Supervisor::RenderAbout()
{
	DrawChrome();
	DrawBar(0, "                 ABOUT", C_WHITE, C_BARBG);

	DrawRow(2, "        APPLE II EMULATOR FOR ESP32", C_WHITE, C_BG);
	DrawRule(3);

	DrawText(2, 5, "VERSION", C_YELLOW, C_BG);
	DrawText(13, 5, FW_VERSION_STR, C_WHITE, C_BG);
	DrawText(2, 6, "BUILT", C_YELLOW, C_BG);
	DrawText(13, 6, FW_BUILD_DATE, C_WHITE, C_BG);
	DrawText(2, 7, "DISPLAY", C_YELLOW, C_BG);
	DrawText(13, 7, "320X200 VGA / 64 COLORS", C_WHITE, C_BG);
	DrawText(2, 8, "CPU", C_YELLOW, C_BG);
	DrawText(13, 8, "MOS 6502", C_WHITE, C_BG);

	DrawText(2, 10, "CREDITS", C_YELLOW, C_BG);
	DrawText(2, 11, "REINALDO TORRES / COCO BYTE CLUB", C_WHITE, C_BG);
	DrawText(2, 12, "BASED ON CODESAFE", C_GREY, C_BG);
	DrawText(4, 13, "ESP32-VGA_APPLEII_EMULATOR", C_GREY, C_BG);
	DrawText(2, 14, "FABGL BY FABRIZIO DI VITTORIO", C_GREY, C_BG);
	DrawText(2, 15, "CO-DEVELOPED WITH CLAUDE CODE", C_GREY, C_BG);
	DrawText(2, 16, "MIT LICENSE", C_GREY, C_BG);

	DrawText(2, 18, "GITHUB.COM/REYCO2000/", C_DIMCYAN, C_BG);
	DrawText(4, 19, "ESP32-TTGO-VGA_APPLEII_EMULATOR", C_DIMCYAN, C_BG);

	DrawBar(23, "           PRESS ESC TO RETURN", C_GREY, C_BARBG);
}

//////////////////////////////////////////////////////////////////////////
// Virtual list: [0]=reset [1]=unmount d1 [2]=unmount d2 [3]=about, then ".."
// when not at root, then the scanned entries

int Supervisor::VirtualCount()
{
	return SUP_ACTION_COUNT + (AtRoot() ? 0 : 1) + entryCount;
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
	if (index == 3)
	{
		snprintf(out, outlen, " [ ABOUT ]");
		return;
	}

	int idx = index - SUP_ACTION_COUNT;
	if (!AtRoot())
	{
		if (idx == 0)
		{
			// " .." alone is two baseline pixels in this font — near
			// invisible at 7x8. Spell the action out instead.
			snprintf(out, outlen, " .. UP ONE LEVEL");
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
	if (index == 3)                          // [ ABOUT ]
	{
		mode = ABOUT;
		return;
	}
	index -= SUP_ACTION_COUNT;

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
