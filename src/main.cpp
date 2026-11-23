#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>

#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "id3_header.h"

#include <algorithm>
#include <vector>

constexpr int kAudioEnablePin = 4;
constexpr int kSdChipSelectPin = 5;
constexpr int kSdMosiPin = 23;
constexpr int kSdMisoPin = 19;
constexpr int kSdClockPin = 18;
constexpr uint32_t kSdSpiFrequency = 1000000;
constexpr int kTouchDataOutPin = 39;
constexpr int kTouchDataInPin = 32;
constexpr int kTouchChipSelectPin = 33;
constexpr int kTouchClockPin = 25;

constexpr uint16_t kDisplayWidth = 320;
constexpr uint16_t kDisplayHeight = 240;
constexpr uint16_t kRowsPerPage = 6;
constexpr uint16_t kHeaderHeight = 24;
constexpr uint16_t kFooterHeight = 24;
constexpr uint16_t kListTop = kHeaderHeight;
constexpr uint16_t kListHeight = kDisplayHeight - kHeaderHeight - kFooterHeight;
constexpr uint16_t kRowHeight = kListHeight / kRowsPerPage;
constexpr uint16_t kListWidth = kDisplayWidth - 48;
constexpr uint16_t kButtonX = kListWidth;
constexpr uint16_t kButtonWidth = kDisplayWidth - kButtonX;
constexpr uint16_t kButtonUpY = kHeaderHeight;
constexpr uint16_t kButtonDownY = kHeaderHeight + (kListHeight / 2);

constexpr uint16_t kTouchXMin = 527;
constexpr uint16_t kTouchXMax = 3552;
constexpr uint16_t kTouchYMin = 683;
constexpr uint16_t kTouchYMax = 3464;

SPIClass sdSpi(VSPI);
TFT_eSPI tft = TFT_eSPI();

std::vector<String> trackPaths;
size_t selectedTrackIndex = 0;
size_t firstVisibleTrackIndex = 0;
String statusLine = "Scanning SD card...";

AudioFileSourceSD* audioFile = nullptr;
AudioOutputI2S* audioOutput = nullptr;
AudioGeneratorMP3* mp3Decoder = nullptr;

bool touchWasDown = false;
unsigned long lastTouchHandledAtMs = 0;

String displayNameForPath(const String& path) {
  int slashIndex = path.lastIndexOf('/');
  if (slashIndex < 0 || slashIndex == static_cast<int>(path.length()) - 1) {
    return path;
  }
  return path.substring(slashIndex + 1);
}

bool hasMp3Extension(const String& fileName) {
  String lowered = fileName;
  lowered.toLowerCase();
  return lowered.endsWith(".mp3");
}

String appendPath(const String& directory, const String& name) {
  if (name.startsWith("/")) {
    return name;
  }
  if (directory == "/") {
    return "/" + name;
  }
  return directory + "/" + name;
}

void drawScanProgress() {
  tft.fillRect(0, 0, kDisplayWidth, kHeaderHeight, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("Found MP3 files: " + String(trackPaths.size()), 6, kHeaderHeight / 2, 2);
}

void scanDirectoryForMp3(const String& directoryPath) {
  File directory = SD.open(directoryPath.c_str());
  if (!directory || !directory.isDirectory()) {
    return;
  }

  while (true) {
    File entry = directory.openNextFile();
    if (!entry) {
      break;
    }

    String entryName = entry.name();
    if (entry.isDirectory()) {
      scanDirectoryForMp3(appendPath(directoryPath, entryName));
    } else if (hasMp3Extension(entryName)) {
      trackPaths.push_back(appendPath(directoryPath, entryName));
      drawScanProgress();
    }

    entry.close();
  }

  directory.close();
}

void setStatusLine(const String& status) {
  statusLine = status;
  tft.fillRect(0, kDisplayHeight - kFooterHeight, kDisplayWidth, kFooterHeight, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(statusLine, 4, kDisplayHeight - (kFooterHeight / 2), 2);
}

void stopPlayback() {
  if (mp3Decoder != nullptr) {
    if (mp3Decoder->isRunning()) {
      mp3Decoder->stop();
    }
    delete mp3Decoder;
    mp3Decoder = nullptr;
  }
  if (audioFile != nullptr) {
    delete audioFile;
    audioFile = nullptr;
  }
  if (audioOutput != nullptr) {
    delete audioOutput;
    audioOutput = nullptr;
  }
}

void startPlayback(const String& path) {
  if (!SD.exists(path.c_str())) {
    setStatusLine("Track missing: " + displayNameForPath(path));
    return;
  }

  stopPlayback();

  audioFile = new AudioFileSourceSD(path.c_str());
  if (!audioFile->isOpen()) {
    Serial.printf("Unable to open %s\n", path.c_str());
    setStatusLine("Unable to open track");
    stopPlayback();
    return;
  }

  uint8_t header[10];
  size_t headerLength = audioFile->read(header, sizeof(header));
  uint32_t fileSize = audioFile->getSize();
  if (fileSize == 0 || headerLength != std::min(static_cast<uint32_t>(sizeof(header)), fileSize)) {
    Serial.printf("Unable to read MP3 header: %s\n", path.c_str());
    setStatusLine("Unable to read track");
    stopPlayback();
    return;
  }

  uint32_t audioOffset = 0;
  if (id3AudioOffset(header, headerLength, fileSize, audioOffset) == Id3HeaderResult::Invalid) {
    Serial.printf("Invalid ID3 header: %s\n", path.c_str());
    setStatusLine("Invalid MP3 header");
    stopPlayback();
    return;
  }
  if (!audioFile->seek(audioOffset, SEEK_SET)) {
    Serial.printf("Unable to seek to MP3 audio: %s\n", path.c_str());
    setStatusLine("Unable to read track");
    stopPlayback();
    return;
  }

  audioOutput = new AudioOutputI2S(0, 1);
  audioOutput->SetGain(1.0f);
  mp3Decoder = new AudioGeneratorMP3();

  if (!mp3Decoder->begin(audioFile, audioOutput)) {
    Serial.printf("Unable to start decoder: %s\n", path.c_str());
    setStatusLine("Failed to play: " + displayNameForPath(path));
    stopPlayback();
    return;
  }

  setStatusLine("Playing: " + displayNameForPath(path));
  Serial.printf("Playing %s\n", path.c_str());
}

void drawTrackList() {
  tft.fillRect(0, 0, kDisplayWidth, kHeaderHeight, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("MP3 files: " + String(trackPaths.size()), 6, kHeaderHeight / 2, 2);

  tft.fillRect(0, kListTop, kListWidth, kListHeight, TFT_BLACK);
  for (uint16_t row = 0; row < kRowsPerPage; ++row) {
    size_t index = firstVisibleTrackIndex + row;
    uint16_t rowY = kListTop + row * kRowHeight;
    bool hasTrack = index < trackPaths.size();
    bool isSelected = hasTrack && (index == selectedTrackIndex);
    uint16_t bgColor = isSelected ? TFT_NAVY : TFT_BLACK;
    uint16_t fgColor = isSelected ? TFT_YELLOW : TFT_WHITE;
    tft.fillRect(0, rowY, kListWidth, kRowHeight, bgColor);
    tft.drawFastHLine(0, rowY + kRowHeight - 1, kListWidth, TFT_DARKGREY);
    if (hasTrack) {
      tft.setTextColor(fgColor, bgColor);
      String label = displayNameForPath(trackPaths[index]);
      tft.drawString(label, 6, rowY + (kRowHeight / 2), 2);
    }
  }

  tft.fillRect(kButtonX, kListTop, kButtonWidth, kListHeight, TFT_DARKCYAN);
  tft.drawRect(kButtonX, kButtonUpY, kButtonWidth, kListHeight / 2, TFT_WHITE);
  tft.drawRect(kButtonX, kButtonDownY, kButtonWidth, kListHeight / 2, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("UP", kButtonX + (kButtonWidth / 2), kButtonUpY + (kListHeight / 4), 2);
  tft.drawString("DOWN", kButtonX + (kButtonWidth / 2), kButtonDownY + (kListHeight / 4), 2);
}

void scanTracks() {
  trackPaths.clear();
  drawScanProgress();
  setStatusLine("Scanning SD card...");
  scanDirectoryForMp3("/");

  std::sort(trackPaths.begin(), trackPaths.end(), [](const String& left, const String& right) {
    String loweredLeft = left;
    String loweredRight = right;
    loweredLeft.toLowerCase();
    loweredRight.toLowerCase();
    return loweredLeft < loweredRight;
  });

  selectedTrackIndex = 0;
  firstVisibleTrackIndex = 0;
}

uint16_t readTouchRaw(uint8_t command) {
  digitalWrite(kTouchChipSelectPin, LOW);
  for (int8_t bit = 7; bit >= 0; --bit) {
    digitalWrite(kTouchDataInPin, ((command >> bit) & 0x01) ? HIGH : LOW);
    digitalWrite(kTouchClockPin, HIGH);
    digitalWrite(kTouchClockPin, LOW);
  }

  uint16_t value = 0;
  for (uint8_t bit = 0; bit < 16; ++bit) {
    digitalWrite(kTouchClockPin, HIGH);
    value <<= 1;
    value |= static_cast<uint16_t>(digitalRead(kTouchDataOutPin));
    digitalWrite(kTouchClockPin, LOW);
  }
  digitalWrite(kTouchChipSelectPin, HIGH);
  return (value >> 3) & 0x0FFF;
}

bool readTouchPoint(uint16_t& x, uint16_t& y) {
  uint16_t z1 = readTouchRaw(0xB0);
  uint16_t z2 = readTouchRaw(0xC0);
  if (z1 < 80 || z2 > 4000 || z1 >= z2) {
    return false;
  }

  uint16_t rawX = readTouchRaw(0xD0);
  uint16_t rawY = readTouchRaw(0x90);

  int16_t calibratedX = map(rawY, kTouchXMin, kTouchXMax, 0, kDisplayWidth - 1);
  int16_t calibratedY = map(rawX, kTouchYMin, kTouchYMax, 0, kDisplayHeight - 1);
  calibratedX = constrain(calibratedX, 0, kDisplayWidth - 1);
  calibratedY = constrain(calibratedY, 0, kDisplayHeight - 1);

  x = static_cast<uint16_t>(calibratedX);
  y = static_cast<uint16_t>(calibratedY);
  return true;
}

void handleTrackSelection(size_t index) {
  if (index >= trackPaths.size()) {
    return;
  }

  selectedTrackIndex = index;
  drawTrackList();
  startPlayback(trackPaths[selectedTrackIndex]);
}

void handleTouch(uint16_t x, uint16_t y) {
  if (y < kListTop || y >= (kListTop + kListHeight)) {
    return;
  }

  if (x >= kButtonX) {
    if (y < kButtonDownY) {
      if (firstVisibleTrackIndex > 0) {
        --firstVisibleTrackIndex;
        drawTrackList();
      }
    } else if (firstVisibleTrackIndex + kRowsPerPage < trackPaths.size()) {
      ++firstVisibleTrackIndex;
      drawTrackList();
    }
    return;
  }

  size_t row = (y - kListTop) / kRowHeight;
  size_t index = firstVisibleTrackIndex + row;
  handleTrackSelection(index);
}

void initTouchController() {
  pinMode(kTouchChipSelectPin, OUTPUT);
  pinMode(kTouchClockPin, OUTPUT);
  pinMode(kTouchDataInPin, OUTPUT);
  pinMode(kTouchDataOutPin, INPUT);
  digitalWrite(kTouchChipSelectPin, HIGH);
  digitalWrite(kTouchClockPin, LOW);
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);

  pinMode(kAudioEnablePin, OUTPUT);
  digitalWrite(kAudioEnablePin, LOW);
  delay(500);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Starting...", 4, 4, 2);

  initTouchController();

  sdSpi.begin(kSdClockPin, kSdMisoPin, kSdMosiPin, kSdChipSelectPin);
  if (!SD.begin(kSdChipSelectPin, sdSpi, kSdSpiFrequency)) {
    Serial.println("Unable to initialize the SD card.");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SD init failed", 8, 8, 2);
    return;
  }

  scanTracks();
  drawTrackList();

  if (trackPaths.empty()) {
    setStatusLine("No MP3 files found");
    return;
  }

  handleTrackSelection(0);
}

void loop() {
  if (mp3Decoder != nullptr && mp3Decoder->isRunning() && !mp3Decoder->loop()) {
    bool reachedEnd = mp3Decoder->isRunning() &&
                      audioFile->getPos() >= audioFile->getSize();
    Serial.println(reachedEnd ? "Playback finished" : "Playback stopped: decoder error");
    stopPlayback();
    setStatusLine(reachedEnd ? "Playback finished" : "Playback error");
  }

  if (trackPaths.empty()) {
    delay(50);
    return;
  }

  static unsigned long lastTouchPollAtMs = 0;
  if (millis() - lastTouchPollAtMs >= 20) {
    lastTouchPollAtMs = millis();
    uint16_t touchX = 0;
    uint16_t touchY = 0;
    bool touchDown = readTouchPoint(touchX, touchY);

    if (touchDown && !touchWasDown && (millis() - lastTouchHandledAtMs > 140)) {
      lastTouchHandledAtMs = millis();
      handleTouch(touchX, touchY);
    }
    touchWasDown = touchDown;
  }
  yield();
}