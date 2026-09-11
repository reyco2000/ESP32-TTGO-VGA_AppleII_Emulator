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

/////////////////////////////////////////////////////////////////////////// 

Apple2Device::Apple2Device()
{
	DEBUG_PRINTLN("Construct Apple2Device");
	// host-side overlay state: deliberately not in Reset(), an emulated
	// machine reset must not switch the user's FPS display off
	fpsOverlay = false;
	fpsValue = 0;
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

	switch (address) 
	{
		// KEYBOARD
		case 0xC000: 
			return(keyboard);
		// KBDSTROBE
		case 0xC010: 
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
    if (keyboard_ptr && keyboard_ptr->virtualKeyAvailable())
    {
        bool keyDown = false;
        fabgl::VirtualKey vk = keyboard_ptr->getNextVirtualKey(&keyDown, 0);
        if (vk != fabgl::VK_NONE && keyDown)
        {
            if (vk == fabgl::VK_F1)
            {
                supervisorRequested = true;
                return;
            }
            if (vk == fabgl::VK_F2)
            {
                fpsOverlay = !fpsOverlay;
                // repaint what the overlay covered (or is about to cover)
                InvalidateFpsOverlayRegion();
                return;
            }
            char ascii = keyboard_ptr->virtualKeyToASCII(vk);
            if (ascii != 0)
            {
                if (keyboard_ptr->isVKDown(fabgl::VK_LCTRL) || keyboard_ptr->isVKDown(fabgl::VK_RCTRL))
                {
                    if (ascii >= 'a' && ascii <= 'z') {
                        ascii = ascii - 'a' + 1;
                    } else if (ascii >= 'A' && ascii <= 'Z') {
                        ascii = ascii - 'A' + 1;
                    }
                }
                keyboard = (BYTE)ascii | 0x80;
            }
            else
            {
                BYTE appleKey = 0;
                switch (vk)
                {
                    case fabgl::VK_LEFT:   appleKey = 0x08; break;
                    case fabgl::VK_RIGHT:  appleKey = 0x15; break;
                    case fabgl::VK_UP:     appleKey = 0x0B; break;
                    case fabgl::VK_DOWN:   appleKey = 0x0A; break;
                    case fabgl::VK_ESCAPE: appleKey = 0x1B; break;
                    case fabgl::VK_RETURN: appleKey = 0x0D; break;
                    case fabgl::VK_BACKSPACE: appleKey = 0x08; break;
                    default: break;
                }
                if (appleKey != 0)
                {
                    keyboard = appleKey | 0x80;
                }
            }
        }
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


