/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Apple2Device.cpp
 *  Module : Apple II peripherals. Routes slot I/O to the cards
 *           (Disk II: DiskIICard.cpp), PS/2 keyboard and gamepad
 *           input, and the soft switches, including the video
 *           mode flags AppleVideo renders from.
 * ============================================================
*/


#include "Predef.h"
#include "AppleCpu.h"
#include "AppleMem.h"
#include "Apple2Device.h"
#include "AppleFont.h"
#include "../VGA/VGA.h"
#include "fabgl.h"

extern fabgl::Keyboard *keyboard_ptr;

// Serial trace of every key event, of each character latched for the Apple
// and of the program taking it: for chasing keyboard problems (layouts,
// ordering). 0 silences it.
#define KEY_TRACE 0

#if KEY_TRACE
static const char* KeyName(fabgl::VirtualKey vk)
{
#if FABGLIB_HAS_VirtualKeyO_STRING
	return fabgl::Keyboard::virtualKeyToString(vk);
#else
	static char name[12];
	snprintf(name, sizeof(name), "vk%d", (int)vk);
	return name;
#endif
}

static void TraceTaken(BYTE keyboard)
{
	if (keyboard & 0x80)
		Serial.printf("[key] program took $%02X\n", keyboard & 0x7F);
}
#endif

/////////////////////////////////////////////////////////////////////////// 

Apple2Device::Apple2Device()
{
	DEBUG_PRINTLN("Construct Apple2Device");
	// host-side overlay state: deliberately not in Reset(), an emulated
	// machine reset must not switch the user's FPS display off
	fpsOverlay = false;
	fpsValue = 0;
	iie = false;
	for (int slot = 0; slot < 8; slot++)
		slots[slot] = NULL;
	Reset();
}

Apple2Device::~Apple2Device()
{
}

void Apple2Device::Create(CPU* cpu)
{
	this->cpu = cpu;
	video.Create();
	zoomscale = 3;

	// No screen backbuffer: Render() draws into the VGA framebuffer directly.

	//////////////////////////////////////////////////////////////////////////
}

void Apple2Device::Reset()
{
	loaddumpmachine = false;
	dumpMachine = false;
	loadromfile = false;

	resetMachine = false;
	colorMonitor = true;
	keyboard = 0;
	supervisorRequested = false;
	resetRequested = false;
	lastVK = 0;
	keyHead = 0;
	keyCount = 0;
	col80 = false;
	altCharset = false;
	dhires = false;

	// slot cards: controller state only, inserted disks stay
	for (int slot = 1; slot < 8; slot++)
		if (slots[slot])
			slots[slot]->Reset();

	////////////////////////////////////////////////////////////////////////// VIDEO

	textMode = true;
	mixedMode = false;
	videoPage = 1;
	hires_Mode = false;

	video.Reset();
}


BYTE Apple2Device::SoftSwitch(Memory *mem, WORD address, BYTE value, bool WRT)
{
	// $C090-$C0FF: the I/O registers of the cards in slots 1-7
	if (address >= 0xC090 && address < 0xC100)
	{
		Card* card = slots[(address >> 4) & 7];
		return card ? card->Io(address & 0x0F, value, WRT) : 0;
	}

	// IIe: $C000-$C01F are the MMU and video switches and their status
	if (iie && address < 0xC020)
		return IIeSwitch(mem, address, WRT);

	switch (address) 
	{
		// KEYBOARD
		case 0xC000: 
			return(keyboard);
		// KBDSTROBE
		case 0xC010:
#if KEY_TRACE
			TraceTaken(keyboard);
#endif
			keyboard &= 0x7F;
			return(keyboard);

		// TAPEOUT??
		case 0xC020:
			break;

		///////////////////////////////////////////////////////////////////////////////// Speaker

		case 0xC030: // SPEAKER
		//case 0xC033: 
			PlaySound(); 
			break;

		///////////////////////////////////////////////////////////////////////////////// Graphics

		case 0xC050: 
			textMode = false; 
			//printf("Text Mode Off\n");
			break;
		// Text
		case 0xC051: 
			textMode = true;  
			//printf("Text Mode On\n");
			break;

		// Mixed off
		case 0xC052: 
			mixedMode = false; 
			//printf("Mixed Mode Off\n");
			break;

		// Mixed on
		case 0xC053: 
			mixedMode = true;  
			//printf("Mixed Mode On\n");
			break;

		// Page 1
		case 0xC054: 
			videoPage = 1;
			//printf("Video Page 1\n");
			break;
		// Page 2
		case 0xC055: 
			videoPage = 2;
			//printf("Video Page 2\n");
			break;

		// HiRes off
		case 0xC056: 
			hires_Mode = false; 
			//printf("HIRES Mode Off\n");
			break;
		// HiRes on
		case 0xC057: 
			hires_Mode = true;  
			//printf("HIRES Mode On\n");
			break;

		/////////////////////////////////////////////////////////////////////////////////	Joy Paddle ?

/*
	https://apple2.org.za/gswv/a2zine/faqs/csa2pfaq.html

	These are actually the first two game Pushbutton inputs (PB0
	and PB1) which are borrowed by the Open Apple and Closed Apple
	keys. Bit 7 is set (=1) in these locations if the game switch or
	corresponding key is pressed.

	PB2 =      $C063 ;game Pushbutton 2 (read)
	This input has an option to be connected to the shift key on
	the keyboard. (See info on the 'shift key mod'.)

	PADDLE0 =  $C064 ;bit 7 = status of pdl-0 timer (read)
	PADDLE1 =  $C065 ;bit 7 = status of pdl-1 timer (read)
	PADDLE2 =  $C066 ;bit 7 = status of pdl-2 timer (read)
	PADDLE3 =  $C067 ;bit 7 = status of pdl-3 timer (read)
	PDLTRIG =  $C070 ;trigger paddles
	Read this to start paddle countdown, then time the period until
	$C064-$C067 bit 7 becomes set to determine the paddle position.
	This takes up to three milliseconds if the paddle is at its maximum
	extreme (reading of 255 via the standard firmware routine).

	SETIOUDIS= $C07E ;enable DHIRES & disable $C058-5F (W)
	CLRIOUDIS= $C07E ;disable DHIRES & enable $C058-5F (W)

*/
/*
		// Push Button 0
		case 0xC061: 
		{
			if (gamepad.pressbtn2)
				return 0x80;
			else
				return 0;
		}
		// Push Button 1
		case 0xC062: 
		{
			if (gamepad.pressbtn1)
				return 0x80;
			else
				return 0;
		}

// 		// Push Button 2
// 		case 0xC063: 
// 			return 0;

		// Paddle 0
		case 0xC064: 
		{
			BYTE v = readPaddle(0);
			return(v);
		}

		// Paddle 1
		case 0xC065: 
		{
			BYTE v = readPaddle(1);
			return(v);
		}

		// paddle timer RST
		case 0xC070: 
			resetPaddles(); 
			break;
*/
		// $CFFF stops the drive motor, as the original emulator did (on real
		// hardware it releases the slots' $C800 expansion ROMs)
		case 0xCFFF:
			disk6.MotorOff();
			break;

		// Pushbuttons 0 and 1: on the IIe these are the Open Apple and
		// Solid Apple keys, here the PC's left and right Alt
		case 0xC061:
			return ButtonDown(0) ? 0x80 : 0;
		case 0xC062:
			return ButtonDown(1) ? 0x80 : 0;

		// AN3: off ($C05E) gives double hires in 80-column mode
		case 0xC05E:
			dhires = true;
			break;
		case 0xC05F:
			dhires = false;
			break;

		///////////////////////////////////////////////////////////////////////////////// LANGUAGE CARD

		case 0xC080: 
		case 0xC084: 
			mem->LCBank2Enable = 1; 
			mem->LCReadable = 1; 
			mem->LCWritable = 0;
			mem->LCPreWriteFlipflop = 0;    
			break;       // LC2RD

		case 0xC081:
		case 0xC085: 
			mem->LCBank2Enable = 1;
			mem->LCReadable = 0; 
			mem->LCWritable |= mem->LCPreWriteFlipflop; 
			mem->LCPreWriteFlipflop = !WRT; 
			break;       // LC2WR

		case 0xC082:
		case 0xC086: 
			mem->LCBank2Enable = 1;
			mem->LCReadable = 0;
			mem->LCWritable = 0;
			mem->LCPreWriteFlipflop = 0;    
			break;       // ROMONLY2

		case 0xC083:
		case 0xC087: 
			mem->LCBank2Enable = 1;
			mem->LCReadable = 1;
			mem->LCWritable |= mem->LCPreWriteFlipflop; 
			mem->LCPreWriteFlipflop = !WRT; 
			break;       // LC2RW

		case 0xC088:
		case 0xC08C: 
			mem->LCBank2Enable = 0;
			mem->LCReadable = 1; 
			mem->LCWritable = 0;
			mem->LCPreWriteFlipflop = 0;    
			break;       // LC1RD

		case 0xC089:
		case 0xC08D: 
			mem->LCBank2Enable = 0; 
			mem->LCReadable = 0; 
			mem->LCWritable |= mem->LCPreWriteFlipflop; 
			mem->LCPreWriteFlipflop = !WRT; 
			break;       // LC1WR

		case 0xC08A:
		case 0xC08E: 
			mem->LCBank2Enable = 0; 
			mem->LCReadable = 0; 
			mem->LCWritable = 0; 
			mem->LCPreWriteFlipflop = 0;    
			break;       // ROMONLY1

		case 0xC08B:
		case 0xC08F: 
			mem->LCBank2Enable = 0; mem->LCReadable = 1; 
			mem->LCWritable |= mem->LCPreWriteFlipflop; 
			mem->LCPreWriteFlipflop = !WRT; 
			break;       // LC1RW
	}

	// With 80STORE on, PAGE2 (and HIRES) also choose main or aux memory for
	// the display pages, so the MMU keeps a copy of both.
	if (iie && (address & 0xFFFC) == 0xC054)
	{
		mem->page2 = (videoPage == 2);
		mem->hires = hires_Mode;
		if (mem->store80)
			mem->Remap();
	}

	// any access to $C080-$C08F may have flipped Language Card banking
	if ((address & 0xFFF0) == 0xC080)
		mem->RemapLanguageCard();

	// Unhandled switches and empty slots. Real hardware returns the floating
	// bus (video data); this used to be cpu->tick % 256, but tick never
	// advanced, so it was always 0. Now that tick runs, keep returning 0
	// explicitly: a random bit 7 would read as unimplemented pushbuttons
	// ($C061-$C063) being pressed.
	return 0;
}

// $C000-$C01F on the IIe. Writes to $C000-$C00F set the MMU and video
// switches; reads of $C011-$C01F report one of them in bit 7 over the
// keyboard code; $C010 clears the strobe and reports any key still down.
BYTE Apple2Device::IIeSwitch(Memory* mem, WORD address, bool WRT)
{
	if (address < 0xC010)
	{
		if (!WRT)
			return keyboard;
		bool on = address & 1;
		switch (address & 0x0E)
		{
			case 0x00: mem->store80   = on; break;
			case 0x02: mem->ramRd     = on; break;
			case 0x04: mem->ramWrt    = on; break;
			case 0x06: mem->intCxRom  = on; break;
			case 0x08: mem->altZp     = on; break;
			case 0x0A: mem->slotC3Rom = on; break;
			case 0x0C: col80      = on; return 0;
			case 0x0E: altCharset = on; return 0;
		}
		mem->Remap();
		return 0;
	}

	if (WRT || address == 0xC010)
	{
#if KEY_TRACE
		TraceTaken(keyboard);
#endif
		keyboard &= 0x7F;               // any access to $C010, or a write to $C01x
		if (WRT)
			return 0;
		return (AnyKeyDown() ? 0x80 : 0) | keyboard;
	}

	bool flag;
	switch (address)
	{
		case 0xC011: flag = mem->LCBank2Enable; break;
		case 0xC012: flag = mem->LCReadable;    break;
		case 0xC013: flag = mem->ramRd;         break;
		case 0xC014: flag = mem->ramWrt;        break;
		case 0xC015: flag = mem->intCxRom;      break;
		case 0xC016: flag = mem->altZp;         break;
		case 0xC017: flag = mem->slotC3Rom;     break;
		case 0xC018: flag = mem->store80;       break;
		// RDVBLBAR: high while the beam draws the 192 visible lines, low
		// during vertical blank (17030 cycles a frame, 12480 of them visible)
		case 0xC019: flag = (cpu->CurrentTick() % 17030) < 12480; break;
		case 0xC01A: flag = textMode;           break;
		case 0xC01B: flag = mixedMode;          break;
		case 0xC01C: flag = (videoPage == 2);   break;
		case 0xC01D: flag = hires_Mode;         break;
		case 0xC01E: flag = altCharset;         break;
		default:     flag = col80;              break;   // $C01F
	}
	return (flag ? 0x80 : 0) | (keyboard & 0x7F);
}

// Is the key that produced the latched code still held?
bool Apple2Device::AnyKeyDown()
{
	return keyboard_ptr && lastVK != fabgl::VK_NONE &&
	       keyboard_ptr->isVKDown((fabgl::VirtualKey)lastVK);
}

bool Apple2Device::ButtonDown(int button)
{
	return keyboard_ptr && keyboard_ptr->isVKDown(button == 0 ? fabgl::VK_LALT : fabgl::VK_RALT);
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////// Input

void Apple2Device::UpdateInput()
{
	UpdateKeyBoard();
	UpdateGamepad();
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////// 

static bool sound_state = false;
void Apple2Device::PlaySound()
{
	sound_state = !sound_state;
	digitalWrite(25, sound_state ? HIGH : LOW);
	digitalWrite(26, sound_state ? HIGH : LOW);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////// Keyboard

void Apple2Device::UpdateKeyBoard()
{
    if (!keyboard_ptr)
        return;

    // Drain FabGL's queue every frame. Each event carries the character it
    // made with the modifiers held at that moment: translating it later,
    // with the modifiers as they are by then, turned a quickly typed "#3"
    // into "3#". Characters then wait in keyQueue until the program has
    // taken the previous one, so fast typing is neither reordered nor lost.
    fabgl::VirtualKeyItem item;
    while (keyboard_ptr->getNextVirtualKey(&item, 0))
    {
#if KEY_TRACE
        Serial.printf("[key] %s %-16s ascii=%02X shift=%d ctrl=%d lalt=%d ralt=%d caps=%d\n",
                      item.down ? "down" : "up  ", KeyName(item.vk), item.ASCII,
                      item.SHIFT, item.CTRL, item.LALT, item.RALT, item.CAPSLOCK);
#endif
        if (!item.down)
            continue;
        fabgl::VirtualKey vk = item.vk;

        if (vk == fabgl::VK_F1)
        {
            // the supervisor reads the keyboard from here on
            supervisorRequested = true;
            return;
        }
        if (vk == fabgl::VK_F2)
        {
            fpsOverlay = !fpsOverlay;
            // repaint what the overlay covered (or is about to cover)
            InvalidateFpsOverlayRegion();
            continue;
        }
        if (vk == fabgl::VK_F12 && item.CTRL)
        {
            // Ctrl-Reset; Ctrl+Alt+F12 is Open-Apple-Ctrl-Reset
            resetRequested = true;
            continue;
        }

        BYTE code = 0;
        int ascii = item.ASCII;                   // 0: the key makes no character
        if (ascii > 0 && ascii < 0x80)
        {
            // the ][+ keyboard has no lowercase: letters are capitals
            // whatever Shift says, as Applesoft expects
            if (!iie && ascii >= 'a' && ascii <= 'z')
                ascii -= 'a' - 'A';
            code = ascii;
        }
        else
        {
            switch (vk)
            {
                case fabgl::VK_LEFT:      code = 0x08; break;
                case fabgl::VK_RIGHT:     code = 0x15; break;
                case fabgl::VK_UP:        code = 0x0B; break;
                case fabgl::VK_DOWN:      code = 0x0A; break;
                case fabgl::VK_ESCAPE:    code = 0x1B; break;
                case fabgl::VK_RETURN:    code = 0x0D; break;
                case fabgl::VK_BACKSPACE: code = 0x08; break;
                case fabgl::VK_DELETE:    code = 0x7F; break;
                default: break;
            }
        }

        if (code && keyCount < KEY_QUEUE_LEN)
        {
            int tail = (keyHead + keyCount) % KEY_QUEUE_LEN;
            keyQueue[tail] = code;
            keyQueueVK[tail] = vk;
            keyCount++;
        }
#if KEY_TRACE
        else if (code)
            Serial.println("[key] type-ahead full, dropped");
#endif
    }

    // latch the next character once the program has cleared the strobe
    if (keyCount && !(keyboard & 0x80))
    {
#if KEY_TRACE
        BYTE c = keyQueue[keyHead];
        Serial.printf("[key] latched $%02X '%c'\n", c, (c >= 0x20 && c < 0x7F) ? c : '.');
#endif
        keyboard = keyQueue[keyHead] | 0x80;
        lastVK = keyQueueVK[keyHead];
        keyHead = (keyHead + 1) % KEY_QUEUE_LEN;
        keyCount--;
    }
}


// game pad update
void Apple2Device::UpdateGamepad()
{
}


///////////////////////////////////////////////////////////////////////////////////////

void Apple2Device::Dump(FILE* fp)
{

}

void Apple2Device::LoadDump(FILE* fp)
{

}


