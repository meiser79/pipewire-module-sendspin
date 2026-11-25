// SPDX-License-Identifier: Apache-2.0
//
// Wait-free single-producer/single-consumer containers used to move data between PipeWire's
// realtime data thread and the non-realtime Sendspin threads. Neither side ever locks or
// allocates after construction.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace pwsendspin {

/// Byte ring. Positions are free-running 64-bit counters; the buffer index is pos % capacity.
class ByteRing {
public:
    explicit ByteRing(size_t capacity)
        : buf_(std::make_unique<uint8_t[]>(capacity)), cap_(capacity) {}

    size_t capacity() const { return cap_; }

    /// Producer: bytes that may be written right now, given a soft fill limit.
    size_t writable(size_t limit) const {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint64_t r = read_.load(std::memory_order_acquire);
        const size_t used = static_cast<size_t>(w - r);
        const size_t lim = std::min(limit, cap_);
        return used >= lim ? 0 : lim - used;
    }

    /// Producer: copies len bytes in (caller checked writable()).
    void write(const uint8_t* src, size_t len) {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const size_t idx = static_cast<size_t>(w % cap_);
        const size_t first = std::min(len, cap_ - idx);
        std::memcpy(buf_.get() + idx, src, first);
        if (len > first) std::memcpy(buf_.get(), src + first, len - first);
        write_.store(w + len, std::memory_order_release);
    }

    /// Consumer: bytes available to read.
    size_t readable() const {
        return static_cast<size_t>(write_.load(std::memory_order_acquire) -
                                   read_.load(std::memory_order_relaxed));
    }

    /// Consumer: copies up to len bytes out, returns the count.
    size_t read(uint8_t* dst, size_t len) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        len = std::min<size_t>(len, static_cast<size_t>(w - r));
        const size_t idx = static_cast<size_t>(r % cap_);
        const size_t first = std::min(len, cap_ - idx);
        std::memcpy(dst, buf_.get() + idx, first);
        if (len > first) std::memcpy(dst + first, buf_.get(), len - first);
        read_.store(r + len, std::memory_order_release);
        return len;
    }

    /// Consumer: drops everything written before `upto` (a write_position() snapshot).
    void discard_until(uint64_t upto) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        if (upto > r) {
            const uint64_t w = write_.load(std::memory_order_acquire);
            read_.store(std::min(upto, w), std::memory_order_release);
        }
    }

    /// Any thread: the producer's current position.
    uint64_t write_position() const { return write_.load(std::memory_order_acquire); }

private:
    std::unique_ptr<uint8_t[]> buf_;
    size_t cap_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

/// Fixed-size queue of trivially copyable items. push() fails (returns false) when full.
template <typename T, size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& item) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) return false;
        items_[h & (N - 1)] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /// Returns a pointer to the oldest item, or nullptr when empty. Valid until pop().
    T* front() {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return nullptr;
        return &items_[t & (N - 1)];
    }

    void pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    /// Producer-side reservation: a slot to fill in place, or nullptr when full. Commit with
    /// commit_push().
    T* reserve() {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) return nullptr;
        return &items_[h & (N - 1)];
    }
    void commit_push() {
        head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

private:
    T items_[N];
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

}  // namespace pwsendspin
