/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : ESP32-VGA_AppleII_Emulator.ino
 *  Module : Arduino sketch entry point. setup() brings up PSRAM,
 *           the SD card, FabGL's VGA DisplayController and the
 *           PS/2 keyboard, then constructs Apple2Machine, VGA and
 *           Supervisor. loop() runs one video frame's worth of
 *           6502 cycles, renders and presents.
 * ============================================================
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "fabgl.h"
#include "src/AppleII/Apple2Machine.h"
#include "src/AppleII/RomLoader.h"
#include "src/Tools/Settings.h"
#include "src/Tools/KeyboardLayouts.h"
#include "src/VGA/VGA.h"
#include "src/Tools/Log.h"
#include "src/Supervisor/Supervisor.h"

// Global pointer to keyboard for Apple2Device to read from
fabgl::Keyboard *keyboard_ptr = nullptr;
fabgl::PS2Controller PS2Controller;
AppleVGAController DisplayController;       // see src/VGA/VGA.h
fabgl::Canvas Canvas(&DisplayController);

VGA *vga;
Apple2Machine *machine;
Supervisor *supervisor;

void listDir(fs::FS &fs, const char * dirname, uint8_t levels)
{
    Serial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels - 1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

static void EmulationTask(void*);
// set when there was no internal RAM for EmulationTask's stack
static bool emulationInLoop = false;

void setup()
{
    Serial.begin(115200);
    
    // Initialize audio pins
    pinMode(25, OUTPUT);
    pinMode(26, OUTPUT);
    // LilyGO TTGO VGA32 v1.4 PSRAM detection
    if(psramInit()) {
        Serial.println("\nPSRAM is correctly initialized");
        Serial.printf("PSRam Size: %d bytes\n", ESP.getPsramSize());
    } else {
        Serial.println("PSRAM not available");
    }

    // Initialize SPI for SD Card (TTGO VGA32 V1.4 specific pinout)
    SPI.begin(14, 2, 12, 13); // SCK, MISO, MOSI, SS/CS
    if(!SD.begin(13, SPI, 4000000))
    {
        Serial.println("SD Card Mount Failed");
        uint8_t cardType = SD.cardType();
        Serial.printf("SD Card Type Detected: %d\n", cardType);
        if(cardType == CARD_NONE){
            Serial.println("No SD card attached or pins wrong.");
        } else if(cardType == CARD_MMC){
            Serial.println("Card Type: MMC");
        } else if(cardType == CARD_SD){
            Serial.println("Card Type: SDSC");
        } else if(cardType == CARD_SDHC){
            Serial.println("Card Type: SDHC");
        } else {
            Serial.println("Card Type: UNKNOWN");
        }
    } else {
        Serial.println("SD Card Mount Success");
        listDir(SD, "/", 0);
    }

    // Initialize FabGL VGA controller
    DisplayController.begin();
    // 640x200 in 16 colours (from 64): wide enough for the IIe's 560-dot
    // 80-column text and double hi-res. The mode is 640x240 line-doubled,
    // i.e. standard 640x480@60Hz timing; 640x200@70Hz is not accepted by
    // every monitor. FabGL centres the 200-line viewport in the 240 lines.
    // At 4 bits per pixel the framebuffer is the same 64K of internal RAM
    // the old 320x200 8-bit one took.
    DisplayController.setResolution(VGA_640x240_60Hz, 640, 200);

    // Print VGA timing and resolution diagnostics
    Serial.println("\n=== FabGL Video Mode Diagnostics ===");
    Serial.printf("Physical Screen Width: %d pixels\n", DisplayController.getScreenWidth());
    Serial.printf("Physical Screen Height: %d pixels\n", DisplayController.getScreenHeight());
    Serial.printf("Active Viewport Width: %d pixels\n", DisplayController.getViewPortWidth());
    Serial.printf("Active Viewport Height: %d pixels\n", DisplayController.getViewPortHeight());
    auto timings = DisplayController.getResolutionTimings();
    if (timings) {
        Serial.printf("Signal Clock Frequency: %.2f MHz\n", timings->frequency / 1000000.0);
    }
    Serial.println("====================================\n");


    // Initialize FabGL PS2 controller
    PS2Controller.begin(PS2Preset::KeyboardPort0);
    keyboard_ptr = PS2Controller.keyboard();

    // Keyboard layout from NVS. FabGL starts on US, so this only has to run
    // when something else was chosen, but applying it unconditionally keeps
    // the boot log honest about which layout is live.
    const KeyboardLayoutProfile* kbd =
        ApplyKeyboardLayout(Settings::LoadKeyboard(KEYBOARD_LAYOUT_US), keyboard_ptr);
    Serial.printf("[kbd] layout: %s\n", kbd->name);

    // Machine model from NVS. A model with no built-in ROM can only boot when
    // its ROMs are on the card; otherwise fall back to the ][+ and say why.
    const MachineProfile* profile = &GetMachineProfile(Settings::LoadMachine(MACHINE_APPLE2PLUS));
    const char* bootNote = "";
    const char* missing = RomLoader::FirstMissing(*profile);
    if (missing)
    {
        Serial.printf("[rom] %s needs %s/%s - booting the ][+\n", profile->name, ROM_DIR, missing);
        bootNote = "ROMS MISSING - BOOTED APPLE ][+";
        profile = &GetMachineProfile(MACHINE_APPLE2PLUS);
    }
    Serial.printf("Machine: %s\n", profile->name);

    // Create objects
    machine = new Apple2Machine(*profile);
    machine->bootNote = bootNote;
    vga = new VGA();
    vga->setCanvas(&Canvas);

    DEBUG_PRINTLN("===> INIT Machine");
    machine->InitMachine();
    supervisor = new Supervisor(machine);

    // Disks that were mounted when the machine was switched: the switch
    // restarts the ESP32, so mount them again, once, and boot from them.
    bool remounted = false;
    for (int drive = 0; drive < 2; drive++)
    {
        String path = Settings::LoadDisk(drive);
        if (path.length() == 0)
            continue;
        Settings::SaveDisk(drive, "");
        remounted |= machine->Mount(path.c_str(), drive);
    }
    if (remounted)
        machine->Reset();

    // a model that could not boot says why
    if (bootNote[0])
        supervisor->Open();

    // VGA16Controller converts every scanline in an ISR pinned to core 1,
    // where the Arduino loop runs; sharing that core cost the emulator about
    // a third of its speed. Core 0 has nothing else to do (no WiFi or BT),
    // so the emulator gets a task of its own there. The task never yields,
    // so core 0's idle-task watchdog has to go. The stack is 8K, as the
    // Arduino loop task that used to run all this had; it comes from
    // internal RAM, of which a IIe leaves little.
    Serial.printf("[mem] internal free %u, largest block %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    disableCore0WDT();
    if (xTaskCreatePinnedToCore(EmulationTask, "emulation", 8192, NULL, 1, NULL, 0) != pdPASS)
    {
        // slower, sharing core 1 with the VGA interrupt, but running
        Serial.println("[emu] no RAM for the emulation task: running in loop() on core 1");
        emulationInLoop = true;
    }
}

int frame = 0;
int fpscount = 0;
unsigned long fpsMillis = 0;
unsigned long heapCheckMillis = 0;

// One video frame: emulate (or run the supervisor), render, count FPS.
// Runs in EmulationTask on core 0, see setup().
static void RunFrame()
{
    if (supervisor->IsActive())
    {
        supervisor->Update();
        if (supervisor->IsActive())      // may have closed itself on ESC
            supervisor->Render(vga);
    }
    else
    {
        long long cycles = 17050 * 4;
        machine->Run(cycles);
        if (machine->device.supervisorRequested)
        {
            machine->device.supervisorRequested = false;
            supervisor->Open();
        }
        machine->Render(vga, frame);
    }
    vga->show();

    if (frame++ > TARGET_FRAME) 
        frame = 0;

    if(millis() - heapCheckMillis > 15000)
    {
        heapCheckMillis = millis();
        Serial.printf("Heap : %d / %d\n", ESP.getFreeHeap(), ESP.getHeapSize());
        Serial.printf("PSRam : %d / %d\n", ESP.getFreePsram(), ESP.getPsramSize());
    }

    fpscount++;
    if(millis() - fpsMillis > 1000)
    {
        fpsMillis = millis();
        Serial.printf("FPS : %d\n", fpscount);
        // feeds the F2 on-screen counter (drawn by Apple2Device::Render)
        machine->device.fpsValue = fpscount;
        fpscount = 0;
    }
}

static void EmulationTask(void*)
{
    for (;;)
        RunFrame();
}

void loop()
{
    if (emulationInLoop)
    {
        RunFrame();
        return;
    }
    // everything runs in EmulationTask; the Arduino loop task has no work
    vTaskDelete(NULL);
}
