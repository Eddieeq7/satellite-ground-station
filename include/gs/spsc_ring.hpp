#pragma once
// Bounded lock-free single-producer / single-consumer ring buffer.
//
// - Capacity is a power of two so wrap-around is a mask, not a modulo.
// - head_ is written only by the consumer, tail_ only by the producer; each side
//   reads the other's index with acquire and publishes its own with release, which
//   is the only synchronization needed for SPSC.
// - Each side keeps a cached copy of the other's index and only reloads it when the
//   ring looks full/empty, which keeps the shared cache line from ping-ponging.
// - Indices live on separate cache lines to avoid false sharing.
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace gs {

#ifdef __cpp_lib_hardware_interference_size
inline constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr size_t kCacheLine = 64;
#endif

template <typename T>
class SpscRing {
    static_assert(std::is_nothrow_move_assignable_v<T>);

public:
    explicit SpscRing(size_t capacity_pow2) : mask_(capacity_pow2 - 1), slots_(capacity_pow2) {
        if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0)
            throw std::invalid_argument("SpscRing capacity must be a power of two >= 2");
    }
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only.
    bool try_push(T&& v) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ > mask_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ > mask_) return false;  // full
        }
        slots_[tail & mask_] = std::move(v);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    std::optional<T> try_pop() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return std::nullopt;  // empty
        }
        T v = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return v;
    }

    size_t capacity() const noexcept { return mask_ + 1; }
    // Approximate; exact only when both sides are quiescent.
    size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    const size_t mask_;
    std::vector<T> slots_;
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    alignas(kCacheLine) size_t tail_cache_ = 0;  // consumer-local
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    alignas(kCacheLine) size_t head_cache_ = 0;  // producer-local
};

}  // namespace gs
