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
