#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

enum SeekMode { SeekSet, SeekCur, SeekEnd };
constexpr int FILE_READ = 0;

struct FakeSdState {
  std::vector<uint32_t> reads;
  size_t bufferSize = 0;
  size_t position = 0;
  size_t size = 1300;
};

class File {
 public:
  explicit File(FakeSdState* state = nullptr) : state_(state) {}
  explicit operator bool() const { return state_ != nullptr; }
  void setBufferSize(size_t size) { state_->bufferSize = size; }
  size_t read(uint8_t* out, size_t length) {
    state_->reads.push_back(length);
    size_t available = state_->size - state_->position;
    size_t count = length < available ? length : available;
    std::memset(out, 0, count);
    state_->position += count;
    return count;
  }
  bool seek(int32_t, SeekMode) { return true; }
  void close() { state_ = nullptr; }
  size_t size() const { return state_->size; }
  size_t position() const { return state_->position; }

 private:
  FakeSdState* state_;
};

struct FakeSD {
  FakeSdState state;
  File open(const char*, int) { return File(&state); }
};

extern FakeSD SD;
