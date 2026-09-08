#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "fabgl.h"
#include "src/AppleII/Apple2Machine.h"
#include "src/VGA/VGA.h"
#include "src/Tools/Log.h"
#include "src/Supervisor/Supervisor.h"

// Global pointer to keyboard for Apple2Device to read from
fabgl::Keyboard *keyboard_ptr = nullptr;
fabgl::PS2Controller PS2Controller;
fabgl::VGAController DisplayController;
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
    DisplayController.setResolution(VGA_640x480_60Hz, 320, 200);

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

    // Create objects
    machine = new Apple2Machine();
    vga = new VGA();
    vga->setCanvas(&Canvas);

    DEBUG_PRINTLN("===> INIT Machine");
    machine->InitMachine();
    supervisor = new Supervisor(machine);
}

int frame = 0;
int fpscount = 0;
unsigned long fpsMillis = 0;
unsigned long heapCheckMillis = 0;

void loop()
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
        fpscount = 0;
    }
}
