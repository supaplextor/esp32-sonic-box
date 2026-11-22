# esp32-sonic-box
Freenove ESP32 FNK0103 F 2.8in ILI9341 touch screen MP3 player

## Build and upload with VS Code and PlatformIO

The firmware in `src/main.cpp` scans the SD card for `.mp3` files, shows them
on the ILI9341 touch display, and starts playback through the onboard DAC and
speaker amplifier when you tap a track. The PlatformIO configuration targets
the ESP32 Arduino framework and installs ESP8266Audio and TFT_eSPI.

1. Install Visual Studio Code and its PlatformIO IDE extension.
2. Open this repository as a PlatformIO project. PlatformIO will install the
   ESP32 platform, Arduino framework, and audio library from `platformio.ini`.
3. Copy one or more MP3 files to a FAT-formatted SD card (root or
   subdirectories) and insert it into the display.
4. Connect the FNK0103 over USB. Use the PlatformIO toolbar or run
   `pio run -t upload` to build and upload; run `pio device monitor` to view
   serial output at 115200 baud.
5. Tap a track to start playback. Use the on-screen UP and DOWN buttons to
   scroll the file list.

Touch calibration constants are in `src/main.cpp` (`kTouchXMin`, `kTouchXMax`,
`kTouchYMin`, `kTouchYMax`). If touches are offset on your display, adjust
those values using Freenove's touch calibration sketch.
