#pragma once

#include <Arduino.h>

// Manages a Bluetooth A2DP source (ESP32 classic BT) that streams PCM audio
// to a paired speaker/headphones. Discovered device names are collected while
// the source is scanning so the UI can offer them for pairing.
namespace bt_audio {

constexpr size_t kMaxDevices = 12;

// Starts the A2DP source. If targetDeviceName is non-empty, connects to that
// device as soon as it is discovered; otherwise keeps scanning and collecting
// device names.
void begin(const String& targetDeviceName);

// Stops the A2DP source (keeps memory so it can be restarted).
void end();

bool isStarted();
bool isConnected();

// Selects the device to pair with; disconnects from any other device so the
// scan can find and connect to the new target.
void setTargetDevice(const String& name);

size_t deviceCount();
String deviceName(size_t index);

// Incremented whenever the discovered-device list changes; lets the UI
// redraw only when needed.
uint32_t deviceListVersion();

// Queues stereo 16-bit 44.1 kHz frames (4 bytes per frame) for transmission.
// All-or-nothing: returns frameCount when queued, 0 when the buffer is full.
size_t writeFrames(const uint8_t* data, size_t frameCount);

}  // namespace bt_audio
