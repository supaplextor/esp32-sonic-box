#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <Preferences.h>

#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "audio_file_source_sd_single_block.h"
#include "id3_header.h"
#include "track_page.h"

#include <algorithm>
#include <vector>

constexpr int kAudioEnablePin = 4;
constexpr int kSdChipSelectPin = 5;
constexpr int kSdMosiPin = 23;
constexpr int kSdMisoPin = 19;
constexpr int kSdClockPin = 18;
constexpr uint32_t kSdSpiFrequency = 4000000;
constexpr uint32_t kSlowSdSpiFrequency = 1000000;
constexpr int kTouchDataOutPin = 39;
constexpr int kTouchDataInPin = 32;
constexpr int kTouchChipSelectPin = 33;
constexpr int kTouchClockPin = 25;

constexpr uint16_t kDisplayWidth = 320;
constexpr uint16_t kDisplayHeight = 240;
constexpr uint16_t kRowsPerPage = 7;
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

String selectedTrackPath;
String statusLine = "Loading MP3 files...";
Preferences preferences;
bool singleBlockReads = true;
bool slowSdClock = false;
bool settingsOpen = false;
bool sdReady = false;

AudioFileSourceSdSingleBlock* audioFile = nullptr;
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

void servicePlayback();

class SdTrackSource {
 public:
  bool reset() {
    directories_.clear();
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
      return false;
    }
    directories_.push_back({root, "/"});
    return true;
  }

  bool next(String& path) {
    while (!directories_.empty()) {
      servicePlayback();
      File entry = directories_.back().directory.openNextFile();
      if (!entry) {
        directories_.pop_back();
        continue;
      }

      String entryName = entry.name();
      String entryPath = appendPath(directories_.back().path, entryName);
      if (entry.isDirectory()) {
        directories_.push_back({entry, entryPath});
      } else {
        entry.close();
        if (hasMp3Extension(entryName)) {
          path = entryPath;
          return true;
        }
      }
    }
    return false;
  }

 private:
  struct Directory {
    File directory;
    String path;
  };
  std::vector<Directory> directories_;
};

SdTrackSource trackSource;
TrackPage<SdTrackSource, String, kRowsPerPage> trackPage(trackSource);

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

  audioFile = new AudioFileSourceSdSingleBlock(path.c_str(), singleBlockReads);
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
  tft.drawString("MP3 page: " + String(trackPage.page() + 1), 6, kHeaderHeight / 2, 2);
  tft.drawCircle(kDisplayWidth - 12, 12, 6, TFT_WHITE);
  tft.drawCircle(kDisplayWidth - 12, 12, 2, TFT_WHITE);
  for (int i = 0; i < 8; ++i) {
    const int8_t dx[] = {0, 5, 7, 5, 0, -5, -7, -5};
    const int8_t dy[] = {-7, -5, 0, 5, 7, 5, 0, -5};
    tft.drawLine(kDisplayWidth - 12 + dx[i], 12 + dy[i],
                 kDisplayWidth - 12 + dx[i] * 9 / 7, 12 + dy[i] * 9 / 7, TFT_WHITE);
  }

  tft.fillRect(0, kListTop, kListWidth, kListHeight, TFT_BLACK);
  for (uint16_t row = 0; row < kRowsPerPage; ++row) {
    uint16_t rowY = kListTop + row * kRowHeight;
    bool hasTrack = row < trackPage.count();
    bool isSelected = hasTrack && (trackPage[row] == selectedTrackPath);
    uint16_t bgColor = isSelected ? TFT_NAVY : TFT_BLACK;
    uint16_t fgColor = isSelected ? TFT_YELLOW : TFT_WHITE;
    tft.fillRect(0, rowY, kListWidth, kRowHeight, bgColor);
    tft.drawFastHLine(0, rowY + kRowHeight - 1, kListWidth, TFT_DARKGREY);
    if (hasTrack) {
      tft.setTextColor(fgColor, bgColor);
      String label = displayNameForPath(trackPage[row]);
      tft.drawString(label, 6, rowY + (kRowHeight / 2), 2);
    }
  }

  void drawSettings() {
    tft.fillRect(0, 0, kDisplayWidth, kDisplayHeight - kFooterHeight, TFT_BLACK);
    tft.fillRect(0, 0, kDisplayWidth, kHeaderHeight, TFT_DARKGREY);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawString("Settings", 6, kHeaderHeight / 2, 2);
    tft.drawString("BACK", kDisplayWidth - 50, kHeaderHeight / 2, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String("Single-sector reads: ") + (singleBlockReads ? "ON" : "OFF"), 6, 48, 2);
    tft.drawString(String("Slow SD clock: ") + (slowSdClock ? "ON" : "OFF"), 6, 80, 2);
    tft.drawString("Reads: next track; clock: restart", 6, 116, 2);
  }

  tft.fillRect(kButtonX, kListTop, kButtonWidth, kListHeight, TFT_DARKCYAN);
  tft.drawRect(kButtonX, kButtonUpY, kButtonWidth, kListHeight / 2, TFT_WHITE);
  tft.drawRect(kButtonX, kButtonDownY, kButtonWidth, kListHeight / 2, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("UP", kButtonX + (kButtonWidth / 2), kButtonUpY + (kListHeight / 4), 2);
  tft.drawString("DOWN", kButtonX + (kButtonWidth / 2), kButtonDownY + (kListHeight / 4), 2);
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
  if (index >= trackPage.count()) {
    return;
  }

  selectedTrackPath = trackPage[index];
  drawTrackList();
  startPlayback(selectedTrackPath);
}

void handleTouch(uint16_t x, uint16_t y) {
  if (settingsOpen) {
    if (y < kHeaderHeight && x >= kDisplayWidth - 60) {
      settingsOpen = false;
      drawTrackList();
      return;
    }
    if (y >= 32 && y < 96) {
      bool& setting = y < 64 ? singleBlockReads : slowSdClock;
      const char* key = y < 64 ? "singleBlock" : "slowClock";
      bool value = !setting;
      if (preferences.putBool(key, value) == 1) {
        setting = value;
        setStatusLine(y < 64 ? "Saved; applies to next track" : "Saved; restart to apply");
      } else {
        setStatusLine("Unable to save setting");
      }
      drawSettings();
    }
    return;
  }
  if (y < kHeaderHeight && x >= kDisplayWidth - 48) {
    settingsOpen = true;
    drawSettings();
    return;
  }
  if (!sdReady) {
    return;
  }
  if (y < kListTop || y >= (kListTop + kListHeight)) {
    return;
  }

  if (x >= kButtonX) {
    if (y < kButtonDownY) {
      if (trackPage.previous()) {
        drawTrackList();
      }
    } else if (trackPage.next()) {
      drawTrackList();
    }
    return;
  }

  size_t row = (y - kListTop) / kRowHeight;
  handleTrackSelection(row);
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

  if (preferences.begin("sonic-settings", false)) {
    singleBlockReads = preferences.getBool("singleBlock", true);
    slowSdClock = preferences.getBool("slowClock", false);
  } else {
    Serial.println("Unable to open settings storage.");
  }

  sdSpi.begin(kSdClockPin, kSdMisoPin, kSdMosiPin, kSdChipSelectPin);
  if (!SD.begin(kSdChipSelectPin, sdSpi, slowSdClock ? kSlowSdSpiFrequency : kSdSpiFrequency)) {
    Serial.println("Unable to initialize the SD card.");
    drawTrackList();
    setStatusLine("SD init failed");
    return;
  }
  sdReady = true;

  setStatusLine("Loading MP3 files...");
  trackPage.begin();
  drawTrackList();

  if (trackPage.count() == 0) {
    setStatusLine("No MP3 files found");
    return;
  }

  handleTrackSelection(0);
}

void servicePlayback() {
  if (mp3Decoder != nullptr && mp3Decoder->isRunning() && !mp3Decoder->loop()) {
    bool reachedEnd = mp3Decoder->isRunning() &&
                      audioFile->getPos() >= audioFile->getSize();
    Serial.println(reachedEnd ? "Playback finished" : "Playback stopped: decoder error");
    stopPlayback();
    setStatusLine(reachedEnd ? "Playback finished" : "Playback error");
  }
}

void loop() {
  servicePlayback();

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