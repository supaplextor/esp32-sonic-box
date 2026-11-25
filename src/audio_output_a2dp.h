#pragma once

#include "AudioOutput.h"
#include "bt_audio.h"

// ESP8266Audio output that forwards decoded PCM to the Bluetooth A2DP source
// instead of the onboard DAC. Samples are batched into small chunks to keep
// stream-buffer overhead low; when the Bluetooth buffer is full (or no
// speaker is connected) ConsumeSample returns false so the decoder retries,
// which gives natural backpressure.
class AudioOutputA2DP : public AudioOutput {
 public:
  AudioOutputA2DP() {
    hertz = 44100;
    bps = 16;
    channels = 2;
    SetGain(1.0f);
  }

  bool SetRate(int hz) override {
    hertz = hz;
    if (hz != 44100) {
      // The A2DP stream runs at 44.1 kHz; other rates play at the wrong pitch.
      Serial.printf("A2DP output: track sample rate %d != 44100\n", hz);
    }
    return true;
  }

  bool begin() override {
    pendingFrames_ = 0;
    return true;
  }

  bool ConsumeSample(int16_t sample[2]) override {
    if (pendingFrames_ == kChunkFrames && !flushChunk()) {
      return false;
    }
    int16_t stereo[2] = {sample[LEFTCHANNEL], sample[RIGHTCHANNEL]};
    MakeSampleStereo16(stereo);
    chunk_[pendingFrames_ * 2] = Amplify(stereo[LEFTCHANNEL]);
    chunk_[pendingFrames_ * 2 + 1] = Amplify(stereo[RIGHTCHANNEL]);
    ++pendingFrames_;
    return true;
  }

  bool stop() override {
    flushChunk();
    pendingFrames_ = 0;
    return true;
  }

 private:
  static constexpr size_t kChunkFrames = 64;

  bool flushChunk() {
    if (pendingFrames_ == 0) {
      return true;
    }
    size_t written =
        bt_audio::writeFrames(reinterpret_cast<const uint8_t*>(chunk_), pendingFrames_);
    if (written == pendingFrames_) {
      pendingFrames_ = 0;
      return true;
    }
    return false;
  }

  int16_t chunk_[kChunkFrames * 2];
  size_t pendingFrames_ = 0;
};
