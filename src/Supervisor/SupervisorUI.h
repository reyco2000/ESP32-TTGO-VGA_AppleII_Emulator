/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : SupervisorUI.h
 *  Module : Look and feel of the supervisor menu (interface): the
 *           theme colours, the screen layout, and the frame, row,
 *           list, popup, icon and key-input primitives every
 *           screen is built from. Knows nothing about the Apple.
 * ============================================================
*/

#ifndef SUPERVISOR_UI_H
#define SUPERVISOR_UI_H

#include "fabgl.h"
#include "SupervisorLogic.h"

// Theme. The framebuffer holds indices into the Apple's 16 lores colours and
// the menu leaves that palette alone, so the Apple picture around the box
// keeps its colours. These are entries of applePalette (AppleVideo.cpp),
// by value, which FabGL maps back to the same index.
#define T_BG      fabgl::RGB888(0, 0, 0)         // 0  black: box, lists
#define T_TEXT    fabgl::RGB888(255, 255, 255)   // 15 white: text, tiles, popups
#define T_ACCENT  fabgl::RGB888(28, 116, 205)    // 2  dark blue: selection, borders
#define T_GREY    fabgl::RGB888(158, 151, 143)   // 10
#define T_DKGREY  fabgl::RGB888(137, 130, 122)   // 5
#define T_GREEN   fabgl::RGB888(144, 192, 49)    // 12
#define T_YELLOW  fabgl::RGB888(255, 253, 166)   // 13
#define T_ORANGE  fabgl::RGB888(234, 108, 21)    // 9
#define T_RED     fabgl::RGB888(226, 57, 86)     // 1
#define T_VIOLET  fabgl::RGB888(126, 110, 173)   // 3
#define T_BLUE    fabgl::RGB888(86, 168, 228)    // 6

// Layout, in framebuffer pixels: 640x200, each pixel 2.4 times taller than
// wide. Text is drawn at double width, 16 pixels a character (12 for hints).
#define SUP_SCREEN_W 640
#define SUP_SCREEN_H 200
#define BOX_X      48
#define BOX_Y      6
#define BOX_W      544
#define BOX_H      188
#define STRIPE_H   6
#define TITLE_Y    (BOX_Y + 12)
#define HEAD_Y     (BOX_Y + 30)            // first line under the title
#define HINT_Y     (BOX_Y + BOX_H - 20)    // key hints, above the bottom stripes
#define MENU_ROW_H 16                      // menus: 8x14 font
#define LIST_ROW_H 9                       // long file lists: 8x8 font
#define LIST_X     (BOX_X + 8)
#define LIST_W     (BOX_W - 28)
#define POPUP_W    464

namespace SupUI
{

struct Row
{
	char label[40];
	char value[32];
	bool dim;                // a choice that cannot be taken: grey label
};
typedef void (*RowFn)(int idx, Row& row);
typedef int  (*IconFn)(int idx);
typedef void (*HeaderFn)(fabgl::Canvas& cv);

enum Icon
{
	ICON_NONE, ICON_DISK, ICON_HDD, ICON_SETUP, ICON_RESET, ICON_ABOUT,
	ICON_RESUME, ICON_MACHINE, ICON_KEYBOARD, ICON_SERIAL, ICON_CAPTURE,
	ICON_SPEED, ICON_SPIDER
};

extern bool debugOn;         // gates Trace and the serial keys
extern bool serialKeys;      // false while the Super Serial Card owns the port
extern bool closeAll;        // F1 pressed: leave every menu level

void   Begin(fabgl::Keyboard* keyboard);     // on opening the menu
SupKey WaitKey();                            // blocks until a key the menu knows
void   Trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

bool Same(fabgl::RGB888 a, fabgl::RGB888 b);
void FillBox(fabgl::Canvas& cv, fabgl::RGB888 c, int x1, int y1, int x2, int y2);
// pen and brush are the caller's; the glyphs fill their background
void Text(fabgl::Canvas& cv, const fabgl::FontInfo* font, int x, int y, const char* s);
int  TextW(const fabgl::FontInfo* font, const char* s);
void TextCentered(fabgl::Canvas& cv, const fabgl::FontInfo* font, int x, int w, int y, const char* s);

void DrawFrame(fabgl::Canvas& cv, const char* title, const char* hint);
void DrawRowAt(fabgl::Canvas& cv, int x, int y, int w, int textOff, bool compact,
               const Row& r, bool hl, fabgl::RGB888 bg);
void DrawScrollbar(fabgl::Canvas& cv, int y, int h, int top, int visible, int count);

// Icons are drawn from boxes, ellipses and lines on a grid whose cells are
// two pixels wide: 16x12 cells small, 32x26 big. bg is what they sit on and
// fg the text colour there; grey draws the "nothing here" version.
void DrawSmallIcon(fabgl::Canvas& cv, int icon, int x, int y,
                   fabgl::RGB888 fg, fabgl::RGB888 bg, bool grey = false);
void DrawBigIcon(fabgl::Canvas& cv, int icon, int x, int y,
                 fabgl::RGB888 fg, fabgl::RGB888 bg, bool grey = false);

// Small window over the current screen: up to two message lines, then the
// options. Returns the chosen option, or -1 on Esc/F1. The caller redraws.
int  Popup(const char* title, const char* msg1, const char* msg2, int count, RowFn rowFn, int selected);
bool Confirm(const char* title, const char* msg1, const char* msg2);   // No first
void Notice(const char* title, const char* msg1, const char* msg2);    // one OK row

// One menu level with an icon on every row. Returns the chosen index, or -1
// on Esc; F1 also returns -1 and sets closeAll. header, if given, draws above
// a list that starts at listY; without it a short list is centred vertically.
int  RunList(const char* title, int count, RowFn rowFn, IconFn iconFn, int selected,
             HeaderFn header = nullptr, int listY = HEAD_Y + 2);

}

#endif
