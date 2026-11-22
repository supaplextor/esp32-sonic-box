# esp32-sonic-box
Freenove ESP32 FNK0103 F 2.8in ILI9341 touch screen MP3 player

## Build and upload with VS Code and PlatformIO

The firmware in `src/main.cpp` plays an MP3 from the FNK0103's SD card through
its onboard DAC and speaker amplifier. The PlatformIO configuration targets the
ESP32 Arduino framework and installs ESP8266Audio v2.0.0.

1. Install Visual Studio Code and its PlatformIO IDE extension.
2. Open this repository as a PlatformIO project. PlatformIO will install the
   ESP32 platform, Arduino framework, and audio library from `platformio.ini`.
3. Copy an MP3 file named `track.mp3` to the root of a FAT-formatted SD card
   and insert it into the display.
4. Connect the FNK0103 over USB. Use the PlatformIO toolbar or run
   `pio run -t upload` to build and upload; run `pio device monitor` to view
   serial output at 115200 baud.

To use a different file, change `kTrackPath` in `src/main.cpp` to its path on
the SD card. The firmware disables Wi-Fi during playback and does not use the
display.
