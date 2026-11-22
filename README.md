# esp32-sonic-box
Freenove ESP32 FNK0103 F 2.8in ILI9341 touch screen MP3 player

## Play an MP3 from the SD card

The Arduino sketch in `Sketch_07.1_Play_MP3_SD_by_DAC/` plays an MP3 from
the FNK0103's SD card through its onboard DAC and speaker amplifier. It uses
the ESP8266Audio library (v2.0.0, as used by the Freenove tutorial).

1. Install the ESP32 Arduino board support and the ESP8266Audio v2.0.0 library
   in the Arduino IDE.
2. Copy an MP3 file named `track.mp3` to the root of a FAT-formatted SD card
   and insert it into the display.
3. Open `Sketch_07.1_Play_MP3_SD_by_DAC/Sketch_07.1_Play_MP3_SD_by_DAC.ino`,
   select the board and serial port for the FNK0103, then upload.

To use a different file, change `kTrackPath` in the sketch to its path on the
SD card. The sketch disables Wi-Fi during playback and does not use the display.
