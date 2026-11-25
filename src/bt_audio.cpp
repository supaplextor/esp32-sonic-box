#include "bt_audio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BluetoothA2DPSource.h"

namespace bt_audio {
namespace {

// ~93 ms of 44.1 kHz stereo 16-bit PCM between the MP3 decoder (loop task)
// and the Bluetooth stack's data callback (BT task).
constexpr size_t kPcmBufferBytes = 16 * 1024;
constexpr size_t kBytesPerFrame = 4;

BluetoothA2DPSource a2dpSource;
uint8_t pcmBuffer[kPcmBufferBytes];
size_t pcmReadPos = 0;
size_t pcmWritePos = 0;
size_t pcmCount = 0;
portMUX_TYPE pcmBufferMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t devicesMutex = nullptr;
String devices[kMaxDevices];
size_t devicesFound = 0;
volatile uint32_t listVersion = 0;
String targetName;
bool started = false;

// Runs in the Bluetooth task: feed buffered PCM, pad with silence on
// underrun so the stream keeps running.
int32_t provideAudioFrames(Frame* frames, int32_t frameCount) {
  size_t wanted = static_cast<size_t>(frameCount) * kBytesPerFrame;
  uint8_t* out = reinterpret_cast<uint8_t*>(frames);
  size_t got = 0;

  portENTER_CRITICAL(&pcmBufferMux);
  size_t toRead = pcmCount < wanted ? pcmCount : wanted;
  size_t firstChunk = toRead;
  if (pcmReadPos + firstChunk > kPcmBufferBytes) {
    firstChunk = kPcmBufferBytes - pcmReadPos;
  }
  if (firstChunk > 0) {
    memcpy(out, pcmBuffer + pcmReadPos, firstChunk);
    pcmReadPos = (pcmReadPos + firstChunk) % kPcmBufferBytes;
  }
  size_t secondChunk = toRead - firstChunk;
  if (secondChunk > 0) {
    memcpy(out + firstChunk, pcmBuffer + pcmReadPos, secondChunk);
    pcmReadPos = (pcmReadPos + secondChunk) % kPcmBufferBytes;
  }
  pcmCount -= toRead;
  got = toRead;
  portEXIT_CRITICAL(&pcmBufferMux);

  if (got < wanted) {
    memset(reinterpret_cast<uint8_t*>(frames) + got, 0, wanted - got);
  }
  return frameCount;
}

// Runs in the Bluetooth task during discovery. Returning true tells the
// library to connect to this device.
bool onDeviceFound(const char* ssid, esp_bd_addr_t address, int rssi) {
  (void)address;
  (void)rssi;
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }

  bool match = false;
  if (xSemaphoreTake(devicesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    bool known = false;
    for (size_t i = 0; i < devicesFound; ++i) {
      if (devices[i] == ssid) {
        known = true;
        break;
      }
    }
    if (!known && devicesFound < kMaxDevices) {
      devices[devicesFound++] = ssid;
      ++listVersion;
    }
    if (targetName.length() > 0) {
      String foundName = ssid;
      String wantedName = targetName;
      foundName.trim();
      wantedName.trim();
      foundName.toLowerCase();
      wantedName.toLowerCase();
      match = foundName == wantedName ||
              foundName.startsWith(wantedName) ||
              wantedName.startsWith(foundName);
      if (match) {
        Serial.printf("BT audio: matched device \"%s\"\n", ssid);
      }
    }
    xSemaphoreGive(devicesMutex);
  }
  return match;
}

}  // namespace

void begin(const String& targetDeviceName) {
  if (started) {
    return;
  }
  if (devicesMutex == nullptr) {
    devicesMutex = xSemaphoreCreateMutex();
  }
  if (devicesMutex == nullptr) {
    Serial.println("BT audio: unable to allocate buffers");
    return;
  }

  portENTER_CRITICAL(&pcmBufferMux);
  pcmReadPos = 0;
  pcmWritePos = 0;
  pcmCount = 0;
  portEXIT_CRITICAL(&pcmBufferMux);

  targetName = targetDeviceName;
  a2dpSource.set_local_name("ESP32 Sonic Box");
  // Name-based connect via the ssid callback keeps behavior predictable when
  // the user switches devices, so skip address-based auto reconnect.
  a2dpSource.set_auto_reconnect(false);
  a2dpSource.set_data_callback_in_frames(provideAudioFrames);
  if (targetName.length() > 0) {
    a2dpSource.set_ssid_callback(nullptr);
    a2dpSource.start(targetName.c_str());
  } else {
    a2dpSource.set_ssid_callback(onDeviceFound);
    a2dpSource.start();
  }
  started = true;
}

void end() {
  if (!started) {
    return;
  }
  a2dpSource.end(false);
  started = false;
}

bool isStarted() {
  return started;
}

bool isConnected() {
  return started && a2dpSource.is_connected();
}

void setTargetDevice(const String& name) {
  if (devicesMutex != nullptr && xSemaphoreTake(devicesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    targetName = name;
    xSemaphoreGive(devicesMutex);
  } else {
    targetName = name;
  }
  if (started && a2dpSource.is_connected()) {
    a2dpSource.disconnect();
  }
}

size_t deviceCount() {
  size_t count = 0;
  if (devicesMutex != nullptr && xSemaphoreTake(devicesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    count = devicesFound;
    xSemaphoreGive(devicesMutex);
  }
  return count;
}

String deviceName(size_t index) {
  String name;
  if (devicesMutex != nullptr && xSemaphoreTake(devicesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (index < devicesFound) {
      name = devices[index];
    }
    xSemaphoreGive(devicesMutex);
  }
  return name;
}

uint32_t deviceListVersion() {
  return listVersion;
}

size_t writeFrames(const uint8_t* data, size_t frameCount) {
  if (!started) {
    return 0;
  }
  size_t bytes = frameCount * kBytesPerFrame;

  portENTER_CRITICAL(&pcmBufferMux);
  size_t space = kPcmBufferBytes - pcmCount;
  if (space < bytes) {
    portEXIT_CRITICAL(&pcmBufferMux);
    return 0;
  }

  size_t firstChunk = bytes;
  if (pcmWritePos + firstChunk > kPcmBufferBytes) {
    firstChunk = kPcmBufferBytes - pcmWritePos;
  }
  if (firstChunk > 0) {
    memcpy(pcmBuffer + pcmWritePos, data, firstChunk);
    pcmWritePos = (pcmWritePos + firstChunk) % kPcmBufferBytes;
  }
  size_t secondChunk = bytes - firstChunk;
  if (secondChunk > 0) {
    memcpy(pcmBuffer + pcmWritePos, data + firstChunk, secondChunk);
    pcmWritePos = (pcmWritePos + secondChunk) % kPcmBufferBytes;
  }
  pcmCount += bytes;
  portEXIT_CRITICAL(&pcmBufferMux);
  return frameCount;
}

}  // namespace bt_audio
