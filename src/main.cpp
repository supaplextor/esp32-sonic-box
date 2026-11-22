#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

constexpr int kAudioEnablePin = 4;
constexpr int kSdChipSelectPin = 5;
constexpr int kSdMosiPin = 23;
constexpr int kSdMisoPin = 19;
constexpr int kSdClockPin = 18;
constexpr char kTrackPath[] = "/track.mp3";

SPIClass sdSpi(VSPI);
AudioFileSourceSD* audioFile = nullptr;
AudioFileSourceID3* id3Source = nullptr;
AudioOutputI2S* audioOutput = nullptr;
AudioGeneratorMP3* mp3Decoder = nullptr;

void startPlayback(const char* path) {
  if (!SD.exists(path)) {
    Serial.printf("MP3 file not found: %s\n", path);
    return;
  }

  audioFile = new AudioFileSourceSD(path);
  id3Source = new AudioFileSourceID3(audioFile);
  audioOutput = new AudioOutputI2S(0, 1);
  audioOutput->SetGain(1.0);
  mp3Decoder = new AudioGeneratorMP3();

  if (!mp3Decoder->begin(id3Source, audioOutput)) {
    Serial.println("Unable to start MP3 playback.");
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);

  pinMode(kAudioEnablePin, OUTPUT);
  digitalWrite(kAudioEnablePin, LOW);
  delay(500);

  sdSpi.begin(kSdClockPin, kSdMisoPin, kSdMosiPin, kSdChipSelectPin);
  if (!SD.begin(kSdChipSelectPin, sdSpi)) {
    Serial.println("Unable to initialize the SD card.");
    return;
  }

  startPlayback(kTrackPath);
}

void loop() {
  if (mp3Decoder != nullptr && mp3Decoder->isRunning() && !mp3Decoder->loop()) {
    mp3Decoder->stop();
  }
}
