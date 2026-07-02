// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace mm {

/// A simple growable byte buffer with a consume cursor, used for both partial
/// inbound reads (append at tail, consume framed messages from head) and
/// partial outbound writes (append frames, consume what the socket accepted).
///
/// Storage is compacted lazily when the consumed prefix grows large, keeping
/// amortized cost low without churning on every consume().
class buffer {
 public:
  [[nodiscard]] std::size_t size() const noexcept {
    return data_.size() - head_;
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  /// Readable region [data(), data()+size()).
  [[nodiscard]] const std::byte* data() const noexcept {
    return data_.data() + head_;
  }
  [[nodiscard]] std::byte* data() noexcept { return data_.data() + head_; }

  void append(const std::byte* p, std::size_t n) {
    data_.insert(data_.end(), p, p + n);
  }
  void append(std::span<const std::byte> s) { append(s.data(), s.size()); }

  /// Reserve `n` bytes of writable tail space and return a pointer to it; the
  /// caller must call commit() with how many were actually filled. Used to
  /// read() directly into the buffer without a bounce copy.
  [[nodiscard]] std::byte* reserve_tail(std::size_t n) {
    const std::size_t end = data_.size();
    data_.resize(end + n);
    return data_.data() + end;
  }
  void commit(std::size_t /*written*/) noexcept { /* tail already resized */ }
  void uncommit(std::size_t unused) noexcept {
    data_.resize(data_.size() - unused);
  }

  /// Drop `n` bytes from the head.
  void consume(std::size_t n) noexcept {
    head_ += n;
    if (head_ == data_.size()) {
      data_.clear();
      head_ = 0;
    } else if (head_ > 4096 && head_ * 2 > data_.size()) {
      // Compact: shift the live tail to the front.
      std::memmove(data_.data(), data_.data() + head_, size());
      data_.resize(size());
      head_ = 0;
    }
  }

  void clear() noexcept {
    data_.clear();
    head_ = 0;
  }

 private:
  std::vector<std::byte> data_;
  std::size_t head_ = 0;
};

}  // namespace mm
