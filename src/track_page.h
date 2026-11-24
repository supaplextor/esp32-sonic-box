#pragma once

#include <stddef.h>

template <typename Source, typename Path, size_t PageSize>
class TrackPage {
 public:
  explicit TrackPage(Source& source) : source_(source) {}

  bool begin() {
    count_ = 0;
    page_ = 0;
    cursorValid_ = source_.reset();
    return cursorValid_ && readPage();
  }

  bool next() {
    if (!cursorValid_ && !seekToPage(page_ + 1)) {
      return false;
    }
    if (!readPage()) {
      return false;
    }
    ++page_;
    return true;
  }

  bool previous() {
    if (page_ == 0 || !seekToPage(page_ - 1) || !readPage()) {
      return false;
    }
    --page_;
    return true;
  }

  size_t count() const { return count_; }
  size_t page() const { return page_; }
  const Path& operator[](size_t index) const { return paths_[index]; }

 private:
  bool seekToPage(size_t page) {
    cursorValid_ = source_.reset();
    Path ignored;
    for (size_t i = 0; cursorValid_ && i < page * PageSize; ++i) {
      cursorValid_ = source_.next(ignored);
    }
    return cursorValid_;
  }

  bool readPage() {
    Path path;
    if (!source_.next(path)) {
      cursorValid_ = false;
      return false;
    }
    paths_[0] = path;
    count_ = 1;
    while (count_ < PageSize && source_.next(path)) {
      paths_[count_++] = path;
    }
    cursorValid_ = true;
    return true;
  }

  Source& source_;
  Path paths_[PageSize];
  size_t count_ = 0;
  size_t page_ = 0;
  bool cursorValid_ = false;
};
