#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "audio_file_source_sd_single_block.h"
#include "audio_output_a2dp.h"
#include "bt_audio.h"
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
bool btEnabled = false;
String btDeviceName;
bool btScanOpen = false;
bool waitingForBtConnection = false;
bool startWhenBtConnected = false;

AudioFileSourceSdSingleBlock* audioFile = nullptr;
AudioOutput* audioOutput = nullptr;
AudioGeneratorMP3* mp3Decoder = nullptr;
SemaphoreHandle_t playbackMutex = nullptr;
TaskHandle_t playbackTaskHandle = nullptr;

constexpr UBaseType_t kPlaybackTaskPriority = 3;
// Keep decoder servicing on the Arduino app core. Core 0 already runs the
// Bluetooth stack and system tasks.
constexpr BaseType_t kPlaybackTaskCore = 1;
// Decoder work used to run on the Arduino loop task with a larger stack.
// Give the dedicated playback task headroom to avoid stack-corruption issues.
constexpr uint32_t kPlaybackTaskStackWords = 8192;
constexpr TickType_t kPlaybackActiveTick = pdMS_TO_TICKS(1);
constexpr TickType_t kPlaybackIdleTick = pdMS_TO_TICKS(2);
volatile uint8_t playbackStatusCode = 0;
constexpr uint8_t kPlaybackStatusNone = 0;
constexpr uint8_t kPlaybackStatusFinished = 1;
constexpr uint8_t kPlaybackStatusError = 2;
constexpr uint8_t kPlaybackStatusWaitingBt = 3;

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

bool servicePlayback();

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

void stopPlaybackLocked() {
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

void stopPlayback() {
  if (playbackMutex == nullptr || xSemaphoreTake(playbackMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  stopPlaybackLocked();
  waitingForBtConnection = false;
  startWhenBtConnected = false;
  xSemaphoreGive(playbackMutex);
}

void startPlayback(const String& path) {
  if (playbackMutex == nullptr || xSemaphoreTake(playbackMutex, portMAX_DELAY) != pdTRUE) {
    setStatusLine("Playback unavailable");
    return;
  }
  if (!SD.exists(path.c_str())) {
    setStatusLine("Track missing: " + displayNameForPath(path));
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    xSemaphoreGive(playbackMutex);
    return;
  }

  if (btEnabled && !bt_audio::isConnected()) {
    stopPlaybackLocked();
    waitingForBtConnection = true;
    startWhenBtConnected = true;
    setStatusLine("Waiting for BT: " + displayNameForPath(path));
    xSemaphoreGive(playbackMutex);
    return;
  }

  stopPlaybackLocked();

  audioFile = new AudioFileSourceSdSingleBlock(path.c_str(), singleBlockReads);
  if (!audioFile->isOpen()) {
    Serial.printf("Unable to open %s\n", path.c_str());
    setStatusLine("Unable to open track");
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    stopPlaybackLocked();
    xSemaphoreGive(playbackMutex);
    return;
  }

  uint8_t header[10];
  size_t headerLength = audioFile->read(header, sizeof(header));
  uint32_t fileSize = audioFile->getSize();
  if (fileSize == 0 || headerLength != std::min(static_cast<uint32_t>(sizeof(header)), fileSize)) {
    Serial.printf("Unable to read MP3 header: %s\n", path.c_str());
    setStatusLine("Unable to read track");
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    stopPlaybackLocked();
    xSemaphoreGive(playbackMutex);
    return;
  }

  uint32_t audioOffset = 0;
  if (id3AudioOffset(header, headerLength, fileSize, audioOffset) == Id3HeaderResult::Invalid) {
    Serial.printf("Invalid ID3 header: %s\n", path.c_str());
    setStatusLine("Invalid MP3 header");
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    stopPlaybackLocked();
    xSemaphoreGive(playbackMutex);
    return;
  }
  if (!audioFile->seek(audioOffset, SEEK_SET)) {
    Serial.printf("Unable to seek to MP3 audio: %s\n", path.c_str());
    setStatusLine("Unable to read track");
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    stopPlaybackLocked();
    xSemaphoreGive(playbackMutex);
    return;
  }

  if (btEnabled) {
    audioOutput = new AudioOutputA2DP();
  } else {
    audioOutput = new AudioOutputI2S(0, 1);
  }
  audioOutput->SetGain(1.0f);
  mp3Decoder = new AudioGeneratorMP3();

  if (!mp3Decoder->begin(audioFile, audioOutput)) {
    Serial.printf("Unable to start decoder: %s\n", path.c_str());
    setStatusLine("Failed to play: " + displayNameForPath(path));
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    stopPlaybackLocked();
    xSemaphoreGive(playbackMutex);
    return;
  }

  waitingForBtConnection = false;
  startWhenBtConnected = false;
  setStatusLine("Playing: " + displayNameForPath(path));
  Serial.printf("Playing %s\n", path.c_str());
  xSemaphoreGive(playbackMutex);
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

  tft.fillRect(kButtonX, kListTop, kButtonWidth, kListHeight, TFT_DARKCYAN);
  tft.drawRect(kButtonX, kButtonUpY, kButtonWidth, kListHeight / 2, TFT_WHITE);
  tft.drawRect(kButtonX, kButtonDownY, kButtonWidth, kListHeight / 2, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("UP", kButtonX + (kButtonWidth / 2), kButtonUpY + (kListHeight / 4), 2);
  tft.drawString("DOWN", kButtonX + (kButtonWidth / 2), kButtonDownY + (kListHeight / 4), 2);
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
  String btStatus = btEnabled ? (bt_audio::isConnected() ? "ON (connected)" : "ON") : "OFF";
  tft.drawString("Bluetooth: " + btStatus, 6, 112, 2);
  tft.drawString("BT device: " + (btDeviceName.length() > 0 ? btDeviceName : String("none")),
                 6, 144, 2);
  tft.drawString("Reads/BT: next track; clock: restart", 6, 180, 2);
}

void drawBtScan() {
  tft.fillRect(0, 0, kDisplayWidth, kDisplayHeight - kFooterHeight, TFT_BLACK);
  tft.fillRect(0, 0, kDisplayWidth, kHeaderHeight, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("Tap a device to pair", 6, kHeaderHeight / 2, 2);
  tft.drawString("BACK", kDisplayWidth - 50, kHeaderHeight / 2, 2);

  size_t count = bt_audio::deviceCount();
  if (count == 0) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Scanning...", 6, kListTop + kRowHeight / 2, 2);
    return;
  }

  for (size_t row = 0; row < kRowsPerPage && row < count; ++row) {
    uint16_t rowY = kListTop + row * kRowHeight;
    String name = bt_audio::deviceName(row);
    bool isSelected = name == btDeviceName;
    tft.setTextColor(isSelected ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
    tft.drawString(name, 6, rowY + (kRowHeight / 2), 2);
    tft.drawFastHLine(0, rowY + kRowHeight - 1, kDisplayWidth, TFT_DARKGREY);
  }
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
  if (btScanOpen) {
    if (y < kHeaderHeight && x >= kDisplayWidth - 60) {
      btScanOpen = false;
      drawSettings();
      return;
    }
    if (y >= kListTop && y < kListTop + kRowsPerPage * kRowHeight) {
      size_t row = (y - kListTop) / kRowHeight;
      if (row < bt_audio::deviceCount()) {
        String name = bt_audio::deviceName(row);
        if (name.length() > 0) {
          btDeviceName = name;
          if (preferences.putString("btDevice", name) == 0) {
            setStatusLine("Unable to save BT device");
          } else {
            setStatusLine("Pairing with " + name);
          }
          bt_audio::setTargetDevice(name);
          drawBtScan();
        }
      }
    }
    return;
  }
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
    } else if (y >= 96 && y < 128) {
      bool value = !btEnabled;
      if (preferences.putBool("btOn", value) == 1) {
        btEnabled = value;
        if (btEnabled) {
          setStatusLine("Starting Bluetooth...");
          bt_audio::begin(btDeviceName);
          setStatusLine("Bluetooth on; applies to next track");
        } else {
          bt_audio::end();
          setStatusLine("Bluetooth off; applies to next track");
        }
      } else {
        setStatusLine("Unable to save setting");
      }
      drawSettings();
    } else if (y >= 128 && y < 160) {
      if (btEnabled) {
        btScanOpen = true;
        drawBtScan();
      } else {
        setStatusLine("Turn Bluetooth on first");
      }
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
  playbackMutex = xSemaphoreCreateMutex();
  if (playbackMutex == nullptr) {
    Serial.println("Unable to create playback mutex.");
    setStatusLine("Playback unavailable");
    return;
  }
  if (preferences.begin("sonic-settings", false)) {
    singleBlockReads = preferences.getBool("singleBlock", true);
    slowSdClock = preferences.getBool("slowClock", false);
    btEnabled = preferences.getBool("btOn", false);
    btDeviceName = preferences.getString("btDevice", "");
  } else {
    Serial.println("Unable to open settings storage.");
  }

  if (btEnabled) {
    bt_audio::begin(btDeviceName);
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

  if (xTaskCreatePinnedToCore(
          [](void*) {
            while (true) {
              if (servicePlayback()) {
                vTaskDelay(kPlaybackActiveTick);
              } else {
                vTaskDelay(kPlaybackIdleTick);
              }
            }
          },
          "playback-loop",
          kPlaybackTaskStackWords,
          nullptr,
          kPlaybackTaskPriority,
          &playbackTaskHandle,
          kPlaybackTaskCore) != pdPASS) {
    Serial.println("Unable to create playback task.");
    setStatusLine("Playback unavailable");
    return;
  }

  if (trackPage.count() == 0) {
    setStatusLine("No MP3 files found");
    return;
  }

  handleTrackSelection(0);
}

bool servicePlayback() {
  if (playbackMutex == nullptr || xSemaphoreTake(playbackMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  bool running = mp3Decoder != nullptr && mp3Decoder->isRunning();
  if (btEnabled && running && !bt_audio::isConnected()) {
    stopPlaybackLocked();
    waitingForBtConnection = true;
    startWhenBtConnected = selectedTrackPath.length() > 0;
    playbackStatusCode = kPlaybackStatusWaitingBt;
    running = false;
  }
  if (mp3Decoder != nullptr && mp3Decoder->isRunning() && !mp3Decoder->loop()) {
    bool reachedEnd = mp3Decoder->isRunning() && audioFile != nullptr &&
                      audioFile->getPos() >= audioFile->getSize();
    Serial.println(reachedEnd ? "Playback finished" : "Playback stopped: decoder error");
    stopPlaybackLocked();
    playbackStatusCode = reachedEnd ? kPlaybackStatusFinished : kPlaybackStatusError;
    waitingForBtConnection = false;
    startWhenBtConnected = false;
    running = false;
  }
  xSemaphoreGive(playbackMutex);
  return running;
}

void loop() {
  if (waitingForBtConnection && bt_audio::isConnected()) {
    waitingForBtConnection = false;
    if (startWhenBtConnected && selectedTrackPath.length() > 0) {
      startPlayback(selectedTrackPath);
    }
  }

  if (playbackStatusCode != kPlaybackStatusNone) {
    uint8_t status = playbackStatusCode;
    playbackStatusCode = kPlaybackStatusNone;
    if (status == kPlaybackStatusFinished) {
      setStatusLine("Playback finished");
    } else if (status == kPlaybackStatusWaitingBt) {
      setStatusLine("Waiting for BT: " + displayNameForPath(selectedTrackPath));
    } else {
      setStatusLine("Playback error");
    }
  }

  if (btScanOpen) {
    static uint32_t lastBtListVersion = 0;
    uint32_t version = bt_audio::deviceListVersion();
    if (version != lastBtListVersion) {
      lastBtListVersion = version;
      drawBtScan();
    }
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