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
 *           either drive, and .hdv/.2mg/large .po hard disks into
 *           slot 7, via Apple2Machine, and shows an ABOUT page with the
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
#include "../AppleII/SuperSerialCard.h"

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

// Buttons sit between the two stripe rules (rows 1 and 6) in pixel space, off
// the text grid, so each can have a 2px margin around its label. The five
// top-row buttons span the same 40..598 the browser does, with 7px of padding
// either side of a label and about 13px between buttons. The drive row is
// two equal halves, D1 left and D2 right: each shows the disk it holds and
// ejects it when pressed.
#define BTN_H     12
#define BTN_TOP_Y 21
#define BTN_BOT_Y 38
#define BTN_PAD   7    // button edge to its label or icon
#define EJECT_W   14

struct ButtonRect { const char* label; int x, y, w; };
static const ButtonRect buttonRects[BTN_COUNT] =
{
	{ "RESET",      40,  BTN_TOP_Y,  84 },
	{ "MACHINE",    137, BTN_TOP_Y, 112 },
	{ "KEYBOARD",   263, BTN_TOP_Y, 126 },
	{ "SERIAL",     402, BTN_TOP_Y,  98 },
	{ "ABOUT",      514, BTN_TOP_Y,  84 },
	{ "D1:",        40,  BTN_BOT_Y, 272 },
	{ "D2:",        326, BTN_BOT_Y, 272 },
};

static inline bool TopRow(int btn) { return btn <= BTN_ABOUT; }

// Popup panel over the SD browser: 34 text columns inside a raised frame,
// with two 16-column buttons side by side.
#define POP_COL      3
#define POP_COLS     34
#define POP_TOP      10   // first text row covered by the panel
#define POP_ROWS     9
#define POP_BTN_COLS 16
#define POP_BTN_GAP  2

// Serial card picker: the three places the card can be, then the line that
// turns the capture file on and off.
#define SUP_SERIAL_ROWS        4
#define SUP_SERIAL_CAPTURE_ROW 3
static const char* serialRowLabel[SUP_SERIAL_ROWS] =
{
	"NOT INSTALLED",
	"SLOT 1  (PRINTER)",
	"SLOT 2  (TERMINAL / MODEM)",
	"ALSO CAPTURE TO SD CARD",
};

// file name without its directory, for the drive buttons and hints
static const char* BaseName(const char* path)
{
	const char* p = strrchr(path, '/');
	return p ? p + 1 : path;
}

// copy of text cut to room characters, ending in '~' when it did not fit
static void FitName(char* out, int room, const char* text)
{
	if ((int)strlen(text) > room)
	{
		memcpy(out, text, room - 1);
		out[room - 1] = '~';
		out[room] = '\0';
	}
	else
		snprintf(out, room + 1, "%s", text);
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
	serialCursor = 0;
	serialRomMissing = false;
	cursor = 0;
	scroll = 0;
	status[0] = '\0';
	pickPath[0] = '\0';
	popupCursor = 0;
	pickDrive = 0;
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
// Spans x0..x1, the full width unless a screen's margin accent is in the way.
void Supervisor::DrawRule(int row, int x0, int x1)
{
	if (x1 < 0)
		x1 = VGA_WIDTH;
	int y = RowY(row) + 2;
	for (int i = 0; i < 6; i++)
	{
		int b0 = x0 + ((x1 - x0) * i) / 6;
		int b1 = x0 + ((x1 - x0) * (i + 1)) / 6;
		vga->fillRect(b0, y, b1 - b0, 3, stripe[i]);
	}
}

// Text centred on the screen rather than the text grid, so odd lengths
// centre too.
void Supervisor::DrawCentered(int row, const char* text, int fg, int bg)
{
	DrawTextXY((VGA_WIDTH - (int)strlen(text) * SUP_CELL_W) / 2, RowY(row), text, fg, bg);
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

// Raised box: light top/left edge, dark bottom/right edge and a 1px drop
// shadow.
void Supervisor::DrawRaised(int x, int y, int w, int h, int face)
{
	vga->fillRect(x, y, w, h, face);
	vga->fillRect(x, y, w, 1, C_WHITE);
	vga->fillRect(x, y, 1, h, C_WHITE);
	vga->fillRect(x, y + h - 1, w, 1, C_BLACK);
	vga->fillRect(x + w - 1, y, 1, h, C_BLACK);
	vga->fillRect(x + 1, y + h, w, 1, C_BLACK);
	vga->fillRect(x + w, y + 1, 1, h, C_BLACK);
}

// The focused button takes the selection bar's cyan. A drive button names
// the disk it holds, "D1:NAME" with an eject mark at its right edge, since
// pressing it unmounts; an empty drive has a dimmed label and no mark.
void Supervisor::DrawButton(int btn)
{
	const ButtonRect& r = buttonRects[btn];
	bool focused = (btn == focusBtn);
	int face = focused ? C_CYAN : C_GREY;

	DrawRaised(r.x, r.y, r.w, BTN_H, face);

	if (TopRow(btn))
	{
		int tw = strlen(r.label) * SUP_CELL_W;
		DrawTextXY(r.x + (r.w - tw) / 2, r.y + 2, r.label, C_BLACK, face);
		return;
	}

	char label[SCREENTEXT_X + 1];
	std::string path = machine->device.GetDiskName(btn - BTN_UNMOUNT1);
	if (path.empty())
	{
		snprintf(label, sizeof(label), "%s (EMPTY)", r.label);
		int tw = strlen(label) * SUP_CELL_W;
		DrawTextXY(r.x + (r.w - tw) / 2, r.y + 2, label, focused ? C_BLACK : C_DIM, face);
		return;
	}

	// the name gets what is left of the eject mark and the label's "D1:"
	int room = (r.w - 3 * BTN_PAD - EJECT_W) / SUP_CELL_W - (int)strlen(r.label);
	char name[SCREENTEXT_X + 1];
	FitName(name, room, BaseName(path.c_str()));
	snprintf(label, sizeof(label), "%s%s", r.label, name);
	DrawTextXY(r.x + BTN_PAD, r.y + 2, label, C_BLACK, face);

	// eject mark: a triangle over a bar. Pixels are over twice as tall as
	// they are wide here, so four rows make the triangle.
	int ex = r.x + r.w - BTN_PAD - EJECT_W;
	for (int i = 0; i < 4; i++)
		vga->fillRect(ex + 6 - 2 * i, r.y + 2 + i, 2 + 4 * i, 1, C_BLACK);
	vga->fillRect(ex, r.y + 7, EJECT_W, 2, C_BLACK);
}

// Popup frame with up to two lines of heading; the buttons and the hint line
// are the caller's.
void Supervisor::DrawPopup(const char* line1, const char* line2)
{
	DrawRaised(ColX(POP_COL) - 8, RowY(POP_TOP), POP_COLS * SUP_CELL_W + 16,
	           POP_ROWS * FONT_Y, C_BARBG);
	DrawText(POP_COL, POP_TOP + 1, line1, C_WHITE, C_BARBG);
	if (line2)
		DrawText(POP_COL, POP_TOP + 2, line2, C_WHITE, C_BARBG);
}

// One of the popup's two buttons, side 0 left or 1 right, its label on the
// given text row.
void Supervisor::DrawPopupButton(int side, int row, const char* label)
{
	int x = ColX(POP_COL + side * (POP_BTN_COLS + POP_BTN_GAP));
	int y = RowY(row) - 2;
	int w = POP_BTN_COLS * SUP_CELL_W;
	int face = (side == popupCursor) ? C_CYAN : C_GREY;

	DrawRaised(x, y, w, BTN_H, face);
	int tw = strlen(label) * SUP_CELL_W;
	DrawTextXY(x + (w - tw) / 2, y + 2, label, C_BLACK, face);
}

// "Mount to which drive": a button per drive with what it holds underneath.
void Supervisor::RenderDrivePopup()
{
	char name[POP_COLS + 1];
	char line[POP_COLS + 1];
	FitName(name, POP_COLS - 6, BaseName(pickPath));
	snprintf(line, sizeof(line), "MOUNT %s", name);
	DrawPopup(line, NULL);

	DrawPopupButton(0, POP_TOP + 3, "DRIVE 1");
	DrawPopupButton(1, POP_TOP + 3, "DRIVE 2");
	for (int drv = 0; drv < 2; drv++)
	{
		int col = POP_COL + drv * (POP_BTN_COLS + POP_BTN_GAP);
		std::string path = machine->device.GetDiskName(drv);
		if (path.empty())
			DrawText(col, POP_TOP + 5, "(EMPTY)", C_GREY, C_BARBG);
		else
		{
			FitName(name, POP_BTN_COLS, BaseName(path.c_str()));
			DrawText(col, POP_TOP + 5, name, C_GREEN, C_BARBG);
		}
	}

	DrawText(POP_COL, POP_TOP + 7, "ARROWS:CHOOSE ENTER:OK ESC:CANCEL", C_GREY, C_BARBG);
}

// "Replace the disk in there": asked before a mount displaces another image.
void Supervisor::RenderConfirmPopup()
{
	char name[POP_COLS + 1];
	char line1[POP_COLS + 1];
	char line2[POP_COLS + 1];

	if (pickDrive < 0)
	{
		FitName(name, POP_COLS - 14, BaseName(machine->device.hdd7.ImageName()));
		snprintf(line1, sizeof(line1), "HARD DISK HAS %s", name);
	}
	else
	{
		std::string path = machine->device.GetDiskName(pickDrive);
		FitName(name, POP_COLS - 12, BaseName(path.c_str()));
		snprintf(line1, sizeof(line1), "DRIVE %d HAS %s", pickDrive + 1, name);
	}
	FitName(name, POP_COLS - 14, BaseName(pickPath));
	snprintf(line2, sizeof(line2), "REPLACE WITH %s?", name);
	DrawPopup(line1, line2);

	DrawPopupButton(0, POP_TOP + 4, "REPLACE");
	DrawPopupButton(1, POP_TOP + 4, "CANCEL");

	DrawText(POP_COL, POP_TOP + 7, "ARROWS:CHOOSE ENTER:OK ESC:BACK", C_GREY, C_BARBG);
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

		if (mode == PICK_SERIAL)
		{
			if (vk == fabgl::VK_UP && serialCursor > 0)
				serialCursor--;
			else if (vk == fabgl::VK_DOWN && serialCursor < SUP_SERIAL_ROWS - 1)
				serialCursor++;
			else if (vk == fabgl::VK_RETURN)
				ChooseSerial(serialCursor);
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
			if (vk == fabgl::VK_LEFT)
				popupCursor = 0;
			else if (vk == fabgl::VK_RIGHT)
				popupCursor = 1;
			else if (vk == fabgl::VK_RETURN)
				ChooseDrive(popupCursor);
			else if (ascii == '1')
				ChooseDrive(0);
			else if (ascii == '2')
				ChooseDrive(1);
			else if (vk == fabgl::VK_ESCAPE)
				mode = BROWSE;
			continue;
		}

		if (mode == CONFIRM_REPLACE)
		{
			if (vk == fabgl::VK_LEFT)
				popupCursor = 0;
			else if (vk == fabgl::VK_RIGHT)
				popupCursor = 1;
			else if (vk == fabgl::VK_RETURN && popupCursor == 0)
			{
				if (pickDrive < 0)
					MountHardDisk();
				else
					MountTo(pickDrive);
			}
			else if (vk == fabgl::VK_RETURN || vk == fabgl::VK_ESCAPE)
				LeaveConfirm();
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
	else if (mode == PICK_SERIAL)
		RenderSerial();
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

// Title, a stripe, the two button rows, a stripe, then the SD browser.
void Supervisor::RenderBrowse()
{
	DrawChrome(SUP_FILES_TOP, SUP_FILES_ROWS);
	char title[SCREENTEXT_X + 1];
	snprintf(title, sizeof(title), "%s SUPERVISOR", machine->profile.name);
	DrawBar(0, "", C_WHITE, C_BARBG);
	DrawCentered(0, title, C_WHITE, C_BARBG);

	DrawRule(1);
	for (int b = 0; b < BTN_COUNT; b++)
		DrawButton(b);
	DrawRule(6);

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

	vga->fillRect(0, RowY(21) + 3, 320, 1, C_DIM);

	DrawRow(22, status, sdError ? C_RED : C_AMBER, C_BG);
	DrawBar(23, " ARROWS:MOVE  ENTER:SELECT  ESC:EXIT", C_GREY, C_BARBG);

	if (mode == PICK_DRIVE)
		RenderDrivePopup();
	else if (mode == CONFIRM_REPLACE)
		RenderConfirmPopup();
}

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

// Version and credits. The font is uppercase-only, glyphs 0x20-0x5F, so every
// string here stays inside that range.
void Supervisor::RenderAbout()
{
	DrawChrome();
	DrawBar(0, "                 ABOUT", C_WHITE, C_BARBG);

	// 5x2 screen pixels per logo pixel: about its own shape on this 640x200 mode
	const int sx = 5, sy = 2;
	int lx = (VGA_WIDTH - LOGO_W * sx) / 2;
	for (int row = 0; row < LOGO_H; row++)
		for (int col = 0; col < LOGO_W; col++)
			if (logoCocoByte[row * LOGO_ROW_BYTES + (col >> 3)] & (1 << (col & 7)))
				vga->fillRect(lx + col * sx, RowY(2) + row * sy, sx, sy, C_GREEN);

	DrawRow(6, "      APPLE II EMULATOR FOR ESP32", C_WHITE, C_BG);
	DrawRule(7, SUP_ORIGIN_X, ColX(SCREENTEXT_X));   // clear of the margin stripe

	DrawText(2, 8, "MACHINE", C_YELLOW, C_BG);
	DrawText(13, 8, machine->profile.name, C_WHITE, C_BG);
	DrawText(2, 9, "CPU", C_YELLOW, C_BG);
	DrawText(13, 9, machine->profile.cpu == CPU_65C02 ? "65C02" : "MOS 6502", C_WHITE, C_BG);
	DrawText(2, 10, "DISPLAY", C_YELLOW, C_BG);
	DrawText(13, 10, "640X200 VGA / 16 COLORS", C_WHITE, C_BG);
	DrawText(2, 11, "KEYBOARD", C_YELLOW, C_BG);
	DrawText(13, 11, GetKeyboardLayoutProfile(CurrentKeyboardLayoutId())->name, C_WHITE, C_BG);
	DrawText(2, 12, "SERIAL", C_YELLOW, C_BG);
	{
		char line[32];
		int slot = machine->device.SerialSlot();
		if (slot)
			snprintf(line, sizeof(line), "SUPER SERIAL CARD, SLOT %d", slot);
		else
			snprintf(line, sizeof(line), "NONE");
		DrawText(13, 12, line, C_WHITE, C_BG);
	}
	DrawText(2, 13, "VERSION", C_YELLOW, C_BG);
#if BUILD_TARGET == BUILD_TARGET_BOOTLOADER
	DrawText(13, 13, FW_VERSION_STR " (SD BOOTLOADER)", C_WHITE, C_BG);
#else
	DrawText(13, 13, FW_VERSION_STR, C_WHITE, C_BG);
#endif
	DrawText(2, 14, "BUILT", C_YELLOW, C_BG);
	DrawText(13, 14, FW_BUILD_DATE, C_WHITE, C_BG);

	DrawCentered(16, "CREDITS", C_YELLOW, C_BG);
	DrawCentered(17, "REINALDO TORRES & CLAUDE CODE", C_WHITE, C_BG);
	DrawCentered(18, "COCOBYTE CLUB", C_WHITE, C_BG);
	DrawCentered(19, "CODESAFE ESP32-VGA_APPLEII_EMULATOR", C_GREY, C_BG);

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
			SetStatus("CHOOSE THE MACHINE MODEL");
			break;
		case BTN_KEYBOARD:
			snprintf(msg, sizeof(msg), "KEYBOARD: %s",
			         GetKeyboardLayoutProfile(CurrentKeyboardLayoutId())->name);
			SetStatus(msg);
			break;
		case BTN_SERIAL:
		{
			int slot = machine->device.SerialSlot();
			if (slot)
				snprintf(msg, sizeof(msg), "SUPER SERIAL CARD IN SLOT %d", slot);
			else
				snprintf(msg, sizeof(msg), "SUPER SERIAL CARD: NOT INSTALLED");
			SetStatus(msg);
			break;
		}
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
		case BTN_SERIAL:
			OpenSerialPicker();
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

		// a hard disk has one place to go, slot 7: no drive to pick
		if (HardDiskCard::IsHardDiskImage(pickPath))
		{
			if (strcmp(machine->device.hdd7.ImageName(), pickPath) == 0)
			{
				machine->UnmountHardDisk();
				SetStatus("HARD DISK EJECTED");
			}
			else if (machine->device.hdd7.HasImage())
			{
				pickDrive = -1;
				popupCursor = 1;             // CANCEL: Enter twice must not replace
				mode = CONFIRM_REPLACE;
			}
			else
				MountHardDisk();
			return;
		}
		popupCursor = 0;
		mode = PICK_DRIVE;
	}
}

// A drive that already holds a disk asks before giving it up.
void Supervisor::ChooseDrive(int drive)
{
	if (machine->device.HasFloppy(drive))
	{
		pickDrive = drive;
		popupCursor = 1;                     // CANCEL: Enter twice must not replace
		mode = CONFIRM_REPLACE;
	}
	else
		MountTo(drive);
}

// Back to the drive popup, where the other drive can still be picked; a hard
// disk had no popup to go back to.
void Supervisor::LeaveConfirm()
{
	if (pickDrive < 0)
		mode = BROWSE;
	else
	{
		popupCursor = pickDrive;
		mode = PICK_DRIVE;
	}
}

void Supervisor::MountHardDisk()
{
	mode = BROWSE;
	if (machine->MountHardDisk(pickPath))
		SetStatus("HARD DISK MOUNTED (PICK AGAIN TO EJECT)");
	else
		SetStatus("LOAD FAILED");
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
	Settings::SaveHardDisk(machine->device.hdd7.ImageName());
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

//////////////////////////////////////////////////////////////////////////
// Super Serial Card picker. The card's line is the ESP32's USB serial port,
// so what it sends reaches whatever terminal is on the other end of the
// programming cable; with capture on it is also written to the SD card.
// Installing or removing the card takes effect at once, like mounting a
// disk, and the Apple only sees the change at its next PR#/IN#.

void Supervisor::OpenSerialPicker()
{
	serialRomMissing = (RomLoader::Check(SSC_ROM, SSC_ROM_SIZE) != ROM_OK);
	int slot = machine->device.SerialSlot();
	serialCursor = (slot >= 1 && slot <= 2) ? slot : 0;
	SetStatus("");
	mode = PICK_SERIAL;
}

void Supervisor::ChooseSerial(int row)
{
	char msg[SCREENTEXT_X + 1];
	bool capture = Settings::LoadPrintCapture(false);

	if (row == SUP_SERIAL_CAPTURE_ROW)
	{
		capture = !capture;
		if (!Settings::SavePrintCapture(capture))
		{
			SetStatus("CANNOT SAVE SETTINGS (NVS)");
			return;
		}
		machine->SetSerialSlot(machine->device.SerialSlot(), capture);
		SetStatus(capture ? "CAPTURING TO /PRINTER ON THE SD CARD"
		                  : "CAPTURE OFF, USB ONLY");
		return;
	}

	int slot = row;                          // rows 0-2 are slots 0-2
	if (slot && serialRomMissing)
	{
		snprintf(msg, sizeof(msg), "MISSING %s", SSC_ROM);
		SetStatus(msg);
		return;
	}
	if (!machine->SetSerialSlot(slot, capture))
	{
		SetStatus("THAT SLOT IS TAKEN");
		return;
	}

	if (!Settings::SaveSerial((uint8_t)slot))
		SetStatus("CANNOT SAVE SETTINGS (NVS)");
	else if (slot)
	{
		snprintf(msg, sizeof(msg), "SERIAL CARD IN SLOT %d - PR#%d / IN#%d", slot, slot, slot);
		SetStatus(msg);
	}
	else
		SetStatus("SERIAL CARD REMOVED");
	mode = BROWSE;
}

void Supervisor::RenderSerial()
{
	DrawChrome();
	DrawBar(0, "             SERIAL CARD", C_WHITE, C_BARBG);
	DrawRow(1, " SUPER SERIAL CARD OVER THE USB PORT", C_GREY, C_BG);
	DrawRule(3);

	int slot = machine->device.SerialSlot();
	bool capture = Settings::LoadPrintCapture(false);

	char line[64];
	for (int i = 0; i < SUP_SERIAL_ROWS; i++)
	{
		int row = SUP_LIST_TOP + i + (i == SUP_SERIAL_CAPTURE_ROW ? 1 : 0);
		bool selected = (i == serialCursor);
		if (i == SUP_SERIAL_CAPTURE_ROW)
			snprintf(line, sizeof(line), " %s  [%s]", serialRowLabel[i], capture ? "ON" : "OFF");
		else
			snprintf(line, sizeof(line), " %s%s", serialRowLabel[i], i == slot ? "  (IN USE)" : "");
		int fg = selected ? C_BLACK : ((i >= 1 && i <= 2 && serialRomMissing) ? C_GREY : C_WHITE);
		DrawRow(row, line, fg, selected ? C_CYAN : C_BG);
	}

	if (serialRomMissing)
		DrawRow(SUP_LIST_TOP + 6, "   NEEDS " SSC_ROM, C_RED, C_BG);

	DrawRow(SUP_LIST_TOP + 8,  " 115200 BAUD 8N1, NO HANDSHAKE.", C_DIMCYAN, C_BG);
	DrawRow(SUP_LIST_TOP + 9,  " THE FIRMWARE LOG STOPS WHILE THE", C_DIMCYAN, C_BG);
	DrawRow(SUP_LIST_TOP + 10, " CARD IS INSTALLED.", C_DIMCYAN, C_BG);

	vga->fillRect(0, RowY(20) + 3, VGA_WIDTH, 1, C_DIM);
	DrawRow(21, status, C_AMBER, C_BG);
	DrawRow(22, "ROM FILES GO IN /ROMS ON THE SD CARD", C_DIMCYAN, C_BG);
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
