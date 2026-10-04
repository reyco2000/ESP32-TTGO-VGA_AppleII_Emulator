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

void Supervisor::DiskMenu()
{
}

//////////////////////////////////////////////////////////////////////////
// Setup

void Supervisor::SetupMenu()
{
}
