#pragma once

#include <cstdint>
#include <cstdio>

class AudioFileSource {
 public:
  virtual ~AudioFileSource() = default;
  virtual bool open(const char*) = 0;
  virtual uint32_t read(void*, uint32_t) = 0;
  virtual bool seek(int32_t, int) = 0;
  virtual bool close() = 0;
  virtual bool isOpen() = 0;
  virtual uint32_t getSize() = 0;
  virtual uint32_t getPos() = 0;
};
