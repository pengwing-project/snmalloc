#pragma once

#include <atomic>

namespace snmalloc
{
  namespace stl
  {
    template<typename T>
    using Atomic = std::atomic<T>;

    constexpr auto memory_order_relaxed = std::memory_order_relaxed;
    constexpr auto memory_order_consume = std::memory_order_consume;
    constexpr auto memory_order_acquire = std::memory_order_acquire;
    constexpr auto memory_order_release = std::memory_order_release;
    constexpr auto memory_order_acq_rel = std::memory_order_acq_rel;
    constexpr auto memory_order_seq_cst = std::memory_order_seq_cst;

    using AtomicBool = std::atomic<bool>;
    using MemoryOrder = std::memory_order;

    template<typename T>
    using is_always_lock_free = typename std::atomic<T>::is_always_lock_free;

    inline void (*atomic_thread_fence)(MemoryOrder) = &std::atomic_thread_fence;
  } // namespace stl
} // namespace snmalloc
