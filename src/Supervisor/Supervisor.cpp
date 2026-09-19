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
 *           card and mounts/unmounts .nib/.dsk/.do/.po images into
 *           either drive via Apple2Machine, and shows an ABOUT page with the
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
#include "../AppleII/RomLoader.h"
#include "../Tools/Settings.h"
#include "../Tools/KeyboardLayouts.h"
#include "../Tools/Bootloader.h"

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

// Buttons sit between the two stripe rules (rows 2 and 7) in pixel space, off
// the text grid, so each can have a 2px margin around its label. The unmount
// row spans the columns above it: D1 under RESET+MACHINE, D2 under
// KEYBOARD+ABOUT, which is also where UP/DOWN land.
#define BTN_H     12
#define BTN_TOP_Y 29
#define BTN_BOT_Y 46

struct ButtonRect { const char* label; int x, y, w; };
static const ButtonRect buttonRects[BTN_COUNT] =
{
	{ "RESET",      40,  BTN_TOP_Y,  98 },
	{ "MACHINE",    170, BTN_TOP_Y, 126 },
	{ "KEYBOARD",   328, BTN_TOP_Y, 140 },
	{ "ABOUT",      500, BTN_TOP_Y,  98 },
	{ "UNMOUNT D1", 40,  BTN_BOT_Y, 256 },
	{ "UNMOUNT D2", 328, BTN_BOT_Y, 270 },
};

static inline bool TopRow(int btn) { return btn <= BTN_ABOUT; }

// file name without its directory, for the D1/D2 line and hints
static const char* BaseName(const char* path)
{
	const char* p = strrchr(path, '/');
	return p ? p + 1 : path;
}

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
	focusBtn = BTN_RESET;
	lastTop = BTN_RESET;
	lastBottom = BTN_UNMOUNT1;
	cursor = 0;
	scroll = 0;
	status[0] = '\0';
	pickPath[0] = '\0';
	machineCursor = 0;
	for (int i = 0; i < MACHINE_COUNT; i++)
		machineMissing[i] = NULL;
	bootNoteShown = false;
	keyboardCursor = 0;
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
	focusBtn = lastTop = BTN_RESET;
	lastBottom = BTN_UNMOUNT1;
	SetStatus("");
	// why the saved model could not boot, if it could not: said once
	if (!bootNoteShown && machine->bootNote[0])
	{
		SetStatus(machine->bootNote);
		bootNoteShown = true;
	}
	else
		ButtonHint(focusBtn);
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
void Supervisor::DrawTextXY(int x, int y, const char* text, int fg, int bg)
{
	for (int i = 0; text[i] != '\0'; i++)
	{
		BYTE g = (BYTE)text[i];
		if (g >= 'a' && g <= 'z')
			g -= 32;
		if (g < 0x20 || g > 0x5F)
			g = 0x20;
		font.RenderFont(vga, g, x + i * SUP_CELL_W, y, false, fg, bg);
	}
}

void Supervisor::DrawText(int col, int row, const char* text, int fg, int bg)
{
	char clip[SCREENTEXT_X + 1];
	snprintf(clip, sizeof(clip), "%.*s", SCREENTEXT_X - col, text);
	DrawTextXY(ColX(col), RowY(row), clip, fg, bg);
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

// Page field plus the vertical stripe accent in the right margin, alongside
// the list window.
void Supervisor::DrawChrome(int listTop, int listRows)
{
	vga->clear(C_BG);

	int top    = RowY(listTop);
	int bottom = RowY(listTop + listRows);
	int band   = (bottom - top) / 6;
	for (int i = 0; i < 6; i++)
		vga->fillRect(612, top + i * band, 16, band, stripe[i]);
}

// "D1: NAME" in a 20-column half of the disk line; names that do not fit
// end in '~' like DrawRow's.
void Supervisor::DrawDiskSlot(int col, int drive)
{
	char label[4] = { 'D', (char)('1' + drive), ':', '\0' };
	DrawText(col, 1, label, C_GREY, C_BG);

	std::string path = machine->device.GetDiskName(drive);
	if (path.empty())
	{
		DrawText(col + 4, 1, "(EMPTY)", C_GREY, C_BG);
		return;
	}

	const int room = 15;                     // leaves a space before D2
	char name[room + 1];
	const char* base = BaseName(path.c_str());
	if ((int)strlen(base) > room)
	{
		memcpy(name, base, room - 1);
		name[room - 1] = '~';
		name[room] = '\0';
	}
	else
		snprintf(name, sizeof(name), "%s", base);
	DrawText(col + 4, 1, name, C_GREEN, C_BG);
}

// Raised button: light top/left edge, dark bottom/right edge and a 1px drop
// shadow. The focused one takes the selection bar's cyan; an unmount button
// with nothing to unmount has a dimmed label.
void Supervisor::DrawButton(int btn)
{
	const ButtonRect& r = buttonRects[btn];
	bool focused = (btn == focusBtn);
	bool idle = (btn == BTN_UNMOUNT1 && !machine->device.HasFloppy(0)) ||
	            (btn == BTN_UNMOUNT2 && !machine->device.HasFloppy(1));
	int face = focused ? C_CYAN : C_GREY;
	int text = (idle && !focused) ? C_DIM : C_BLACK;

	vga->fillRect(r.x, r.y, r.w, BTN_H, face);
	vga->fillRect(r.x, r.y, r.w, 1, C_WHITE);
	vga->fillRect(r.x, r.y, 1, BTN_H, C_WHITE);
	vga->fillRect(r.x, r.y + BTN_H - 1, r.w, 1, C_BLACK);
	vga->fillRect(r.x + r.w - 1, r.y, 1, BTN_H, C_BLACK);
	vga->fillRect(r.x + 1, r.y + BTN_H, r.w, 1, C_BLACK);
	vga->fillRect(r.x + r.w, r.y + 1, 1, BTN_H, C_BLACK);

	int tw = strlen(r.label) * SUP_CELL_W;
	DrawTextXY(r.x + (r.w - tw) / 2, r.y + 2, r.label, text, face);
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
	int idx = index;
	if (!AtRoot())
	{
		if (idx == 0)
			return C_CYAN;                   // ".." is a directory too
		idx--;
	}
	if (idx < entryCount && entries[idx].isDir)
		return C_CYAN;
	return C_WHITE;                          // disk images
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

		if (mode == PICK_MACHINE)
		{
			if (vk == fabgl::VK_UP && machineCursor > 0)
				machineCursor--;
			else if (vk == fabgl::VK_DOWN && machineCursor < MACHINE_COUNT - 1)
				machineCursor++;
			else if (vk == fabgl::VK_RETURN)
				ChooseMachine(machineCursor);
			else if (vk == fabgl::VK_ESCAPE)
			{
				ButtonHint(focusBtn);
				mode = BROWSE;
			}
			continue;
		}

		if (mode == PICK_KEYBOARD)
		{
			if (vk == fabgl::VK_UP && keyboardCursor > 0)
				keyboardCursor--;
			else if (vk == fabgl::VK_DOWN && keyboardCursor < KEYBOARD_LAYOUT_COUNT - 1)
				keyboardCursor++;
			else if (vk == fabgl::VK_RETURN)
				ChooseKeyboard(keyboardCursor);
			else if (vk == fabgl::VK_ESCAPE)
			{
				ButtonHint(focusBtn);
				mode = BROWSE;
			}
			continue;
		}

		if (mode == RESTARTING)
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
			case fabgl::VK_UP:     MoveFocus(0, -1); break;
			case fabgl::VK_DOWN:   MoveFocus(0, 1);  break;
			case fabgl::VK_LEFT:   MoveFocus(-1, 0); break;
			case fabgl::VK_RIGHT:  MoveFocus(1, 0);  break;
			case fabgl::VK_RETURN:
				if (ListFocused())
					Select();
				else
					Press(focusBtn);
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
	else if (mode == PICK_MACHINE)
		RenderMachines();
	else if (mode == PICK_KEYBOARD)
		RenderKeyboards();
	else if (mode == RESTARTING)
	{
		// the choice is already in NVS; show it long enough to read
		RenderRestarting();
		delay(400);
		Bootloader::Restart();               // back into this app, not the menu
	}
	else
		RenderBrowse();
}

// Title, the D1/D2 line, a stripe, the two button rows, a stripe, then the
// SD browser.
void Supervisor::RenderBrowse()
{
	DrawChrome(SUP_FILES_TOP, SUP_FILES_ROWS);
	DrawBar(0, "               SUPERVISOR", C_WHITE, C_BARBG);

	DrawDiskSlot(0, 0);
	DrawDiskSlot(20, 1);
	DrawRule(2);

	for (int b = 0; b < BTN_COUNT; b++)
		DrawButton(b);
	DrawRule(7);

	char label[SUP_NAME_LEN + 24];
	int count = VirtualCount();
	for (int i = 0; i < SUP_FILES_ROWS; i++)
	{
		int idx = scroll + i;
		if (idx >= count)
			break;
		VirtualLabel(idx, label, sizeof(label));
		int bg;
		int fg = RowColor(idx, ListFocused() && idx == cursor, &bg);
		DrawRow(SUP_FILES_TOP + i, label, fg, bg);
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

	DrawText(2, 4, "MACHINE", C_YELLOW, C_BG);
	DrawText(13, 4, machine->profile.name, C_WHITE, C_BG);
	DrawText(2, 5, "CPU", C_YELLOW, C_BG);
	DrawText(13, 5, machine->profile.cpu == CPU_65C02 ? "65C02" : "MOS 6502", C_WHITE, C_BG);
	DrawText(2, 6, "DISPLAY", C_YELLOW, C_BG);
	DrawText(13, 6, "640X200 VGA / 16 COLORS", C_WHITE, C_BG);
	DrawText(2, 7, "KEYBOARD", C_YELLOW, C_BG);
	DrawText(13, 7, GetKeyboardLayoutProfile(CurrentKeyboardLayoutId())->name, C_WHITE, C_BG);
	DrawText(2, 8, "VERSION", C_YELLOW, C_BG);
#if BUILD_TARGET == BUILD_TARGET_BOOTLOADER
	DrawText(13, 8, FW_VERSION_STR " (SD BOOTLOADER)", C_WHITE, C_BG);
#else
	DrawText(13, 8, FW_VERSION_STR, C_WHITE, C_BG);
#endif
	DrawText(2, 9, "BUILT", C_YELLOW, C_BG);
	DrawText(13, 9, FW_BUILD_DATE, C_WHITE, C_BG);

	DrawText(2, 11, "CREDITS", C_YELLOW, C_BG);
	DrawText(2, 12, "REINALDO TORRES / COCO BYTE CLUB", C_WHITE, C_BG);
	DrawText(2, 13, "BASED ON CODESAFE", C_GREY, C_BG);
	DrawText(4, 14, "ESP32-VGA_APPLEII_EMULATOR", C_GREY, C_BG);
	DrawText(2, 15, "FABGL BY FABRIZIO DI VITTORIO", C_GREY, C_BG);
	DrawText(2, 16, "CO-DEVELOPED WITH CLAUDE CODE", C_GREY, C_BG);
	DrawText(2, 17, "MIT LICENSE", C_GREY, C_BG);

	DrawText(2, 19, "GITHUB.COM/REYCO2000/", C_DIMCYAN, C_BG);
	DrawText(4, 20, "ESP32-TTGO-VGA_APPLEII_EMULATOR", C_DIMCYAN, C_BG);

	DrawBar(23, "           PRESS ESC TO RETURN", C_GREY, C_BARBG);
}

//////////////////////////////////////////////////////////////////////////
// Virtual list: ".." when not at root, then the scanned entries

int Supervisor::VirtualCount()
{
	return (AtRoot() ? 0 : 1) + entryCount;
}

void Supervisor::VirtualLabel(int index, char* out, int outlen)
{
	int idx = index;
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
				if (DskImage::TypeFromPath(name) != DskImage::IMAGE_NONE)
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
	if (cursor >= scroll + SUP_FILES_ROWS) scroll = cursor - SUP_FILES_ROWS + 1;
}

// Arrow keys in BROWSE mode. LEFT/RIGHT walk a button row; UP/DOWN go
// top row <-> unmount row <-> SD list, each row returning to the button
// last focused on it. In the list LEFT/RIGHT do nothing.
void Supervisor::MoveFocus(int dx, int dy)
{
	if (ListFocused())
	{
		if (dy < 0 && cursor == 0)
			FocusButton(lastBottom);
		else if (dy != 0)
			MoveCursor(dy);
		return;
	}

	int btn = focusBtn;
	if (dx != 0)
	{
		int first = TopRow(btn) ? BTN_RESET : BTN_UNMOUNT1;
		int last  = TopRow(btn) ? BTN_ABOUT : BTN_UNMOUNT2;
		btn += dx;
		if (btn >= first && btn <= last)
			FocusButton(btn);
		return;
	}

	if (dy > 0)
	{
		if (TopRow(btn))
			FocusButton(lastBottom);
		else if (VirtualCount() > 0)
		{
			focusBtn = BTN_NONE;
			SetStatus("");
			if (cursor >= VirtualCount())
				cursor = 0;
			MoveCursor(0);                   // brings the cursor into view
		}
	}
	else if (dy < 0 && !TopRow(btn))
		FocusButton(lastTop);
}

void Supervisor::FocusButton(int btn)
{
	focusBtn = btn;
	if (TopRow(btn))
		lastTop = btn;
	else
		lastBottom = btn;
	ButtonHint(btn);
}

// Status-line text for the focused button; the machine and keyboard in use
// live here since their names do not fit on the buttons.
void Supervisor::ButtonHint(int btn)
{
	char msg[SCREENTEXT_X + 1];
	switch (btn)
	{
		case BTN_RESET:
			SetStatus("RESET THE EMULATED MACHINE");
			break;
		case BTN_MACHINE:
			snprintf(msg, sizeof(msg), "MACHINE: %s", machine->profile.name);
			SetStatus(msg);
			break;
		case BTN_KEYBOARD:
			snprintf(msg, sizeof(msg), "KEYBOARD: %s",
			         GetKeyboardLayoutProfile(CurrentKeyboardLayoutId())->name);
			SetStatus(msg);
			break;
		case BTN_ABOUT:
			SetStatus("VERSION AND CREDITS");
			break;
		case BTN_UNMOUNT1:
		case BTN_UNMOUNT2:
		{
			int drv = btn - BTN_UNMOUNT1;
			std::string path = machine->device.GetDiskName(drv);
			if (path.empty())
				snprintf(msg, sizeof(msg), "DRIVE %d IS EMPTY", drv + 1);
			else
				snprintf(msg, sizeof(msg), "EJECT %s", BaseName(path.c_str()));
			SetStatus(msg);
			break;
		}
	}
}

void Supervisor::Press(int btn)
{
	switch (btn)
	{
		case BTN_RESET:
			machine->Reset();
			Close();                         // close so the boot is visible
			break;
		case BTN_MACHINE:
			OpenMachinePicker();
			break;
		case BTN_KEYBOARD:
			OpenKeyboardPicker();
			break;
		case BTN_ABOUT:
			mode = ABOUT;
			break;
		case BTN_UNMOUNT1:
		case BTN_UNMOUNT2:
		{
			int drv = btn - BTN_UNMOUNT1;
			if (machine->device.HasFloppy(drv))
			{
				machine->Unmount(drv);
				char msg[32];
				snprintf(msg, sizeof(msg), "UNMOUNTED DRIVE %d", drv + 1);
				SetStatus(msg);
			}
			else
				SetStatus(drv == 0 ? "DRIVE 1 EMPTY" : "DRIVE 2 EMPTY");
			break;
		}
	}
}

void Supervisor::Select()
{
	int index = cursor;

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

//////////////////////////////////////////////////////////////////////////
// Machine picker. Switching model saves the choice and the mounted disks in
// NVS and restarts the ESP32, so memory is laid out from scratch for the
// new model; setup() mounts the disks again.

void Supervisor::OpenMachinePicker()
{
	// a model whose ROMs are missing from /roms cannot be picked
	for (int i = 0; i < MACHINE_COUNT; i++)
		machineMissing[i] = RomLoader::FirstMissing(GetMachineProfile(i));
	machineCursor = machine->profile.id;
	SetStatus("");
	mode = PICK_MACHINE;
}

void Supervisor::ChooseMachine(int id)
{
	if (id == machine->profile.id)
	{
		SetStatus("ALREADY RUNNING");
		return;
	}
	if (machineMissing[id])
	{
		char msg[SCREENTEXT_X + 1];
		snprintf(msg, sizeof(msg), "MISSING %s", machineMissing[id]);
		SetStatus(msg);
		return;
	}

	// restarting without the choice saved would only come back as this model
	if (!Settings::SaveMachine(id))
	{
		SetStatus("CANNOT SAVE SETTINGS (NVS)");
		return;
	}
	Settings::SaveDisk(0, machine->device.GetDiskName(0).c_str());
	Settings::SaveDisk(1, machine->device.GetDiskName(1).c_str());
	machineCursor = id;
	mode = RESTARTING;                       // Render() shows it, then restarts
}

void Supervisor::RenderMachines()
{
	DrawChrome();
	DrawBar(0, "                MACHINE", C_WHITE, C_BARBG);
	DrawRow(1, " CHOOSE THE COMPUTER TO EMULATE", C_GREY, C_BG);
	DrawRule(3);

	char line[64];
	for (int i = 0; i < MACHINE_COUNT; i++)
	{
		int row = SUP_LIST_TOP + i * 2;
		bool selected = (i == machineCursor);
		snprintf(line, sizeof(line), " %s%s", GetMachineProfile(i).name,
		         i == machine->profile.id ? "  (RUNNING)" : "");
		int fg = selected ? C_BLACK : (machineMissing[i] ? C_GREY : C_WHITE);
		DrawRow(row, line, fg, selected ? C_CYAN : C_BG);
		if (machineMissing[i])
		{
			snprintf(line, sizeof(line), "   NEEDS %s", machineMissing[i]);
			DrawRow(row + 1, line, C_RED, C_BG);
		}
	}

	vga->fillRect(0, RowY(20) + 3, VGA_WIDTH, 1, C_DIM);
	DrawRow(21, status, C_AMBER, C_BG);
	DrawRow(22, "ROM FILES GO IN /ROMS ON THE SD CARD", C_DIMCYAN, C_BG);
	DrawBar(23, " ARROWS:MOVE  ENTER:SELECT  ESC:BACK", C_GREY, C_BARBG);
}

//////////////////////////////////////////////////////////////////////////
// Keyboard layout picker. A layout only changes how FabGL turns scancodes
// into characters, so unlike a machine switch it takes effect as soon as
// it is chosen; NVS only has to remember it for the next boot.

void Supervisor::OpenKeyboardPicker()
{
	keyboardCursor = CurrentKeyboardLayoutId();
	SetStatus("");
	mode = PICK_KEYBOARD;
}

void Supervisor::ChooseKeyboard(int id)
{
	const KeyboardLayoutProfile* profile = ApplyKeyboardLayout((uint8_t)id, keyboard_ptr);

	char msg[SCREENTEXT_X + 1];
	if (Settings::SaveKeyboard(profile->id))
		snprintf(msg, sizeof(msg), "KEYBOARD: %s", profile->name);
	else
		snprintf(msg, sizeof(msg), "%s, NOT SAVED (NVS)", profile->name);
	SetStatus(msg);
	mode = BROWSE;
}

void Supervisor::RenderKeyboards()
{
	DrawChrome();
	DrawBar(0, "               KEYBOARD", C_WHITE, C_BARBG);
	DrawRow(1, " CHOOSE THE PS/2 KEYBOARD LAYOUT", C_GREY, C_BG);
	DrawRule(3);

	char line[64];
	for (int i = 0; i < KEYBOARD_LAYOUT_COUNT; i++)
	{
		int row = SUP_LIST_TOP + i;
		bool selected = (i == keyboardCursor);
		snprintf(line, sizeof(line), " %s%s", GetKeyboardLayoutProfile(i)->name,
		         i == CurrentKeyboardLayoutId() ? "  (IN USE)" : "");
		DrawRow(row, line, selected ? C_BLACK : C_WHITE, selected ? C_CYAN : C_BG);
	}

	vga->fillRect(0, RowY(20) + 3, VGA_WIDTH, 1, C_DIM);
	DrawRow(21, status, C_AMBER, C_BG);
	DrawRow(22, "ACCENTS AND N-TILDE HAVE NO APPLE KEY", C_DIMCYAN, C_BG);
	DrawBar(23, " ARROWS:MOVE  ENTER:SELECT  ESC:BACK", C_GREY, C_BARBG);
}

void Supervisor::RenderRestarting()
{
	DrawChrome();
	DrawBar(0, "                MACHINE", C_WHITE, C_BARBG);
	char line[64];
	snprintf(line, sizeof(line), " RESTARTING AS %s...", GetMachineProfile(machineCursor).name);
	DrawRow(10, line, C_YELLOW, C_BG);
	DrawBar(23, "", C_GREY, C_BARBG);
}
