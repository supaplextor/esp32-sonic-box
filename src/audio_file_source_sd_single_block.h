#pragma once

#include <SD.h>

#include "AudioFileSource.h"

// Some SD cards have broken multi-block (CMD18/CMD12) read support over SPI,
// which makes the ESP32 SD driver log "sdReadSectors(): command failed" and
// return no data. FatFS only issues a multi-block read when a single request
// spans two or more full sectors, so pinning the stdio buffer at one sector
// and capping every read at 512 bytes keeps all file reads on the reliable
// single-block (CMD17) path.
class AudioFileSourceSdSingleBlock : public AudioFileSource {
 public:
  static constexpr uint32_t kSectorSize = 512;

  AudioFileSourceSdSingleBlock() = default;

  explicit AudioFileSourceSdSingleBlock(const char* filename) {
    open(filename);
  }

  ~AudioFileSourceSdSingleBlock() override {
    close();
  }

  bool open(const char* filename) override {
    close();
    file_ = SD.open(filename, FILE_READ);
    if (!file_) {
      return false;
    }
    file_.setBufferSize(kSectorSize);
    return true;
  }

  uint32_t read(void* data, uint32_t len) override {
    if (!file_) {
      return 0;
    }
    uint8_t* out = static_cast<uint8_t*>(data);
    uint32_t total = 0;
    while (total < len) {
      uint32_t remaining = len - total;
      uint32_t chunk = remaining < kSectorSize ? remaining : kSectorSize;
      size_t bytesRead = file_.read(out + total, chunk);
      total += bytesRead;
      if (bytesRead < chunk) {
        break;
      }
    }
    return total;
  }

  bool seek(int32_t pos, int dir) override {
    if (!file_) {
      return false;
    }
    SeekMode mode = SeekSet;
    if (dir == SEEK_CUR) {
      mode = SeekCur;
    } else if (dir == SEEK_END) {
      mode = SeekEnd;
    }
    return file_.seek(pos, mode);
  }

  bool close() override {
    if (!file_) {
      return false;
    }
    file_.close();
    return true;
  }

  bool isOpen() override {
    return static_cast<bool>(file_);
  }

  uint32_t getSize() override {
    return file_ ? file_.size() : 0;
  }

  uint32_t getPos() override {
    return file_ ? file_.position() : 0;
  }

 private:
  File file_;
};
