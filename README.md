# esp32-sonic-box
Freenove ESP32 FNK0103 F 2.8in ILI9341 touch screen MP3 player

## Build and upload with VS Code and PlatformIO

The firmware in `src/main.cpp` browses `.mp3` files on the SD card, shows them
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
   navigate seven-track pages.

Tracks are shown in SD directory order, including subdirectories, rather than
sorted alphabetically. Only the current page is retained in memory; the first
page is loaded at startup, and later pages are read on demand. Going back
rescans from the root, so it can take longer on large cards.

Playback errors and file paths are reported on the serial monitor. MP3 files
with ID3v2 tags are played without displaying their embedded metadata.

The SD SPI clock is set to 1 MHz in `src/main.cpp` to improve reliability with
cards or wiring sensitive to the default 4 MHz clock. This can slow scanning
and may not sustain every high-bitrate file. If `sdReadSectors(): command
failed` persists, check the card contacts, wiring and power supply, and test
the card and filesystem on a computer or try another card. A FAT32 partition
alone does not rule out an SD communication or card fault.

Touch calibration constants are in `src/main.cpp` (`kTouchXMin`, `kTouchXMax`,
`kTouchYMin`, `kTouchYMax`). If touches are offset on your display, adjust
those values using Freenove's touch calibration sketch.
