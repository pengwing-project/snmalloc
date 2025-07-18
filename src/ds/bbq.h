#pragma once
#include "mem/allocconfig.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <endian.h>
#include <type_traits>
#ifdef DEBUG
#  include <bitset>
#  include <chrono>
#  include <fstream>
#  include <iomanip>
#  include <iostream>
#  include <mutex>
#  include <sstream>
#  include <thread>
#  include <unistd.h>
#endif
#define RETRY_NEW
// #define DROP_OLD

namespace snmalloc
{
#ifdef _BBQ_NON_ATOMIC_ENTRY_
  template<class T>
  using maybe_atomic = T;
#elif __cpp_concepts
  template<class T>
  concept AtomicInstantiable =
    std::is_trivially_copyable_v<T> && std::is_copy_constructible_v<T> &&
    std::is_move_constructible_v<T> && std::is_copy_assignable_v<T> &&
    std::is_move_assignable_v<T> && std::is_same_v<T, std::remove_cv_t<T>>;
  template<class T>
  using maybe_atomic =
    std::conditional_t<AtomicInstantiable<T>, std::atomic<T>, T>;
#else
  template<class T>
  struct is_atomic_instantiable
  : std::integral_constant<
      bool,
      std::is_trivially_copyable<T>::value &&
        std::is_copy_constructible<T>::value &&
        std::is_move_constructible<T>::value &&
        std::is_copy_assignable<T>::value &&
        std::is_move_assignable<T>::value &&
        std::is_same<T, typename std::remove_cv<T>::type>::value>
  {};
  template<class T>
  using maybe_atomic =
    std::conditional_t<is_atomic_instantiable<T>::value, std::atomic<T>, T>;
#endif

  template<typename T, uint32_t block_num = 8, uint32_t block_size = 15>
  class batchedBBQ_MPSC;

  template<class T, uint32_t block_num = 8, uint32_t block_size = 15>
  class BBQ
  {
    friend class batchedBBQ_MPSC<T, block_num, block_size>;

  public:
    enum Queuestatus
    {
      OK = 0,
      FULL,
      EMPTY,
      BUSY
    };

  protected:
    enum class Blockstate
    {
      SUCCESS,
      ALLOCATED,
      NO_ENTRY,
      NOT_AVAILABLE,
      RESERVED,
      BLOCK_DONE
    };

    constexpr static inline uint32_t log2_inner(unsigned int v)
    {
      unsigned int r{};
      unsigned int shift{};

      r = static_cast<unsigned int>(v > 0xFFFF) << 4;
      v >>= r;
      shift = static_cast<unsigned int>(v > 0xFF) << 3;
      v >>= shift;
      r |= shift;
      shift = static_cast<unsigned int>(v > 0xF) << 2;
      v >>= shift;
      r |= shift;
      shift = static_cast<unsigned int>(v > 0x3) << 1;
      v >>= shift;
      r |= shift;
      r |= (v >> 1);
      return r;
    }

    constexpr static inline unsigned int log2(unsigned int v)
    {
      return log2_inner(v) + (v & (v - 1) ? 1 : 0);
    }

    static constexpr unsigned offset_bits = log2(block_size) + 2;
    static constexpr unsigned index_bits = log2(block_num);
    static constexpr unsigned version_bits{64 - offset_bits};

    template<uint64_t pointer_bits>
    union MetaCtrl final
    {
      struct final
      {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint64_t ptr : pointer_bits;
        uint64_t ver : version_bits;
#else
        uint64_t ver : version_bits;
        uint64_t ptr : pointer_bits;
#endif
      } meta;

      uint64_t val{0};

      MetaCtrl() : val{0} {}

      // MetaCtrl& operator=(const MetaCtrl c)
      //{
      //   if (this == &c)
      //   {
      //     return *this;
      //   }
      //   val = c.val;
      //   return *this;
      // }

      MetaCtrl(uint64_t raw) : val{raw} {}
    };

    template<uint64_t pointer_bits>
    class alignas(CACHELINE_SIZE) Ctrl final
    {
    public:
      std::atomic<uint64_t> raw{0};

      Ctrl() = default;

      MetaCtrl<pointer_bits> inline load(
        std::memory_order order = std::memory_order_acquire) const noexcept
      {
        return MetaCtrl<pointer_bits>{raw.load(order)};
      }

      inline void store(
        uint64_t v,
        std::memory_order order = std::memory_order_release) noexcept
      {
        raw.store(v, order);
      }

      inline MetaCtrl<pointer_bits> fetch_add(
        uint64_t v,
        std::memory_order order = std::memory_order_acq_rel) noexcept
      {
        return MetaCtrl<pointer_bits>{raw.fetch_add(v, order)};
      }

      Ctrl(const Ctrl&) = delete;
      Ctrl& operator=(const Ctrl&) = delete;
      Ctrl(Ctrl&&) = delete;
      Ctrl& operator=(Ctrl&&) = delete;
    };

  protected:
    using offset = uint64_t;
    using index = uint64_t;
    using version = uint64_t;
    using Raw = uint64_t;
    using Cursor = Ctrl<offset_bits>;
    using Head = Ctrl<index_bits>;
    using MetaCursor = MetaCtrl<offset_bits>;
    using MetaHead = MetaCtrl<index_bits>;

    struct alignas(CACHELINE_SIZE) Block
    {
      using PayloadEntry = maybe_atomic<T>;
      alignas(CACHELINE_SIZE) PayloadEntry payload[block_size]{};

      Cursor allocated{}, committed{};
      Cursor reserved{}, consumed{};

      void init() noexcept
      {
        allocated.store(block_size);
        committed.store(block_size);
        reserved.store(block_size);
        consumed.store(block_size);
      }

#ifndef DEBUG
      Block() = default;
#else
      Block()
      {
        //   for (uint32_t i = 0; i < block_size; i++)
        //   {
        //     if constexpr (std::is_same_v<maybe_atomic<T>, std::atomic<T>>)
        //       // payload[i].store(static_cast<T>(1),
        //       std::memory_order_seq_cst); else
        //       {
        //         // payload[i] = static_cast<T>(1);
        //         std::atomic_thread_fence(std::memory_order_release);
        //       }
        //   }
      }
#endif

      Block(const Block&) = delete;
      Block& operator=(const Block&) = delete;

#ifdef DEBUG
      // virtual
      std::ostream& dump(
        std::ostream& out = std::cout,
        [[maybe_unused]] size_t groupsize = block_size) const
      {
        MetaCursor allocate = allocated.load();
        MetaCursor commit = committed.load();
        MetaCursor reserve = reserved.load();
        MetaCursor consume = consumed.load();

        out << "allocated: " << allocate.meta.ptr << "[" << allocate.meta.ver
            << "]\n";
        out << "committed: " << commit.meta.ptr << "[" << commit.meta.ver
            << "]\n";
        out << "reserved: " << reserve.meta.ptr << "[" << reserve.meta.ver
            << "]\n";
        out << "consumed: " << consume.meta.ptr << "[" << consume.meta.ver
            << "]\n";
#  ifndef _BATCHED_BBQ_
        for (size_t i = 0; i < block_size; ++i)
        {
          out << payload[i] << ' ';
          if ((i + 1) % groupsize == 0)
          {
            out << '\n';
          }
        }
// #  else
//         for (size_t i = 0; i < block_size; ++i)
//         {
//           auto first = this->payload[i];
//           if (first == nullptr)
//           {
//             out << "nullptr\n";
//             continue;
//           }
//           out << first->value;
//           T cur = first->next;
//           while (cur != nullptr)
//           {
//             out << " -> " << cur->value;
//             cur = cur->next;
//           }
//
//           out << '\n';
//         }
#  endif
        return out;
      }
#endif
    };

    alignas(CACHELINE_SIZE) Head phead{};
    alignas(CACHELINE_SIZE) Head chead{};
    alignas(CACHELINE_SIZE) Block blocks_[block_num]{};

  public:
    BBQ()
    {
      if constexpr (std::is_same_v<maybe_atomic<T>, std::atomic<T>>)
      {
        static_assert(std::atomic<T>::is_always_lock_free);
      }
      static_assert(
        block_num > 1 && block_size > 1,
        "block number and block size should be greater than 1");
      static_assert(
        block_num <= block_size,
        "assume block num is not greater than block_size");
      static_assert(
        !(block_num & (block_num - 1)), "block_num should be power of 2");
      // first block cursor is zero, remains are block_size
      for (size_t i = 1; i < block_num; ++i)
      {
        blocks_[i].init();
      }
    }

    Queuestatus enq([[maybe_unused]] T data)
    {
    again:
      Block& blk = blocks_[phead.load().meta.ptr];
      MetaCursor cursor{};
      switch (allocate_entry(blk, cursor))
      {
        case (Blockstate::ALLOCATED):
          if constexpr (std::is_same_v<maybe_atomic<T>, std::atomic<T>>)
            blk.payload[cursor.meta.ptr].store(data, std::memory_order_release);
          else
          {
            blk.payload[cursor.meta.ptr] = data;
            std::atomic_thread_fence(std::memory_order_release);
          }
          blk.committed.fetch_add(1);
          return Queuestatus::OK;
        case (Blockstate::BLOCK_DONE):
          switch (advance_phead(phead))
          {
            case Blockstate::NO_ENTRY:
              return Queuestatus::FULL;
            case Blockstate::NOT_AVAILABLE:
              return Queuestatus::BUSY;
            case Blockstate::SUCCESS:
              goto again;
            default:
              __builtin_unreachable();
          }
        default:
          __builtin_unreachable();
      }
    }

    Queuestatus deq([[maybe_unused]] T& data)
    {
    again:
      Block& blk = blocks_[chead.load().meta.ptr];
      MetaCursor cursor{};
      switch (reserve_entry(blk, cursor))
      {
        case Blockstate::RESERVED: {
#ifndef _BBQ_DEQ_LATE_READ_
          if constexpr (std::is_same_v<maybe_atomic<T>, std::atomic<T>>)
            data = blk.payload[cursor.meta.ptr].load(std::memory_order_acquire);
          else
          {
            std::atomic_thread_fence(std::memory_order_acquire);
            data = blk.payload[cursor.meta.ptr];
          }
#endif
#ifdef DEBUG
          // std::atomic_thread_fence(std::memory_order_seq_cst);
          // if constexpr (std::is_same_v<maybe_atomic<T>, std::atomic<T>>)
          //   blk.payload[cursor.meta.ptr].store(
          //     static_cast<T>(-1), std::memory_order_seq_cst);
          // else
          //{
          //   // blk.payload[cursor.meta.ptr] = static_cast<T>(-1);
          //   std::atomic_thread_fence(std::memory_order_release);
          // }
#endif
#if defined(RETRY_NEW)
          blk.consumed.fetch_add(1);
          return Queuestatus::OK;
#elif defined(DROP_OLD)
          MetaCursor allocated = blk.allocated.load();
          if (allocated.meta.ver != cursor.meta.ver)
          {
            goto again;
          }
#endif
        }
        case Blockstate::NO_ENTRY:
          return Queuestatus::EMPTY;
        case Blockstate::NOT_AVAILABLE:
          return Queuestatus::BUSY;
        case Blockstate::BLOCK_DONE: {
          MetaCursor cursor{};
          if (advance_chead(chead, cursor))
          {
            goto again;
          }
          else
          {
            return Queuestatus::EMPTY;
          }
        }
        default:
          __builtin_unreachable();
      }
    }

#ifdef DEBUG
#  ifdef DUMP
  public:
#  else
  private:
#  endif
    std::ostream& dump(std::ostream& out = std::cout) const
    {
      MetaHead p = phead.load();
      MetaHead c = chead.load();
      out << "\033[31mphead: " << p.meta.ptr << "[" << p.meta.ver << "]\n"
          << "chead: " << c.meta.ptr << "[" << c.meta.ver << "]\n\033[0m";

      for (size_t i = 0; i < block_num; ++i)
      {
        out << "\033[0;36mBlock " << i << "=========" << "\n\033[0m";
        blocks_[i].dump(out);
      }
      return out;
    }
#endif

  private:
    inline uint64_t fetch_max(std::atomic<uint64_t>& old, uint64_t val)
    {
#if defined(__ARM_ARCH_8A) && defined(__aarch64__)
      // memory_order: acq_rel || seq_cst
      uint64_t prev;
      __asm__ __volatile__("ldumaxal %0,%2,[%1]"
                           : "=&r"(prev)
                           : "r"(&old), "r"(val)
                           :);
#else
      while (true)
      {
        uint64_t cur = old.load(std::memory_order_relaxed);
        if (cur >= val)
        {
          return cur;
        }
        if (old.compare_exchange_weak(
              cur, val, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
          return cur;
        }
      }
#endif
    }

    inline Raw cursorVal(version version = 0, offset offset = 0)
    {
      return (version << offset_bits) | offset;
    }

    inline Blockstate allocate_entry(Block& blk, MetaCursor& cursor)
    {
      MetaCursor cur = blk.allocated.load();
      if (cur.meta.ptr >= block_size)
      {
        return Blockstate::BLOCK_DONE;
      }
      MetaCursor old = blk.allocated.fetch_add(1);
      if (old.meta.ptr >= block_size)
      {
        return Blockstate::BLOCK_DONE;
      }
      cursor = old;
      return Blockstate::ALLOCATED;
    }

    inline Blockstate reserve_entry(Block& blk, MetaCursor& cursor)
    {
    again:
      MetaCursor reserved = blk.reserved.load();
      if (reserved.meta.ptr < block_size)
      {
        MetaCursor committed = blk.committed.load();
        if (reserved.meta.ptr == committed.meta.ptr)
        {
          return Blockstate::NO_ENTRY;
        }
        if (committed.meta.ptr != block_size)
        {
          MetaCursor allocated = blk.allocated.load();
          if (allocated.meta.ptr != committed.meta.ptr)
          {
            return Blockstate::NOT_AVAILABLE;
          }
        }
        if (fetch_max(blk.reserved.raw, reserved.val + 1) == reserved.val)
        {
          cursor = reserved;
          return Blockstate::RESERVED;
        }
        else
        {
          goto again;
        }
      }
      cursor = reserved;
      return Blockstate::BLOCK_DONE;
    }

    inline Blockstate advance_phead(const Head& ph)
    {
      MetaHead head = ph.load();
      // there's should be a pre-check before advancing phead
      if (blocks_[head.meta.ptr].allocated.load().meta.ptr < block_size)
      {
        return Blockstate::SUCCESS;
      }
      Block& nextblock = blocks_[(head.meta.ptr + 1) % block_num];
#if defined(RETRY_NEW)
      MetaCursor consumed = nextblock.consumed.load();
      if (
        consumed.meta.ver < head.meta.ver ||
        (consumed.meta.ver == head.meta.ver && consumed.meta.ptr != block_size))
      {
        MetaCursor reserved = nextblock.reserved.load();
        if (reserved.meta.ptr == consumed.meta.ptr)
        {
          return Blockstate::NO_ENTRY;
        }
        else
        {
          return Blockstate::NOT_AVAILABLE;
        }
      }
#elif defined(DROP_OLD)
      MetaCursor committed = nextblock.committed.load();
      if (
        committed.meta.ver == head.meta.ver && committed.meta.ptr != block_size)
      {
        return Blockstate::NOT_AVAILABLE;
      }
#endif

      Raw reset = cursorVal(head.meta.ver + 1);
      fetch_max(nextblock.committed.raw, reset);
      fetch_max(nextblock.allocated.raw, reset);
      fetch_max(phead.raw, head.val + 1);
      return Blockstate::SUCCESS;
    }

    inline bool
    advance_chead(const Head& ch, [[maybe_unused]] const MetaCursor& cursor)
    {
      MetaHead head = ch.load();
      // there's should be a pre-check before advancing chead
      if (blocks_[head.meta.ptr].reserved.load().meta.ptr < block_size)
      {
        return true;
      }
      Block& nextblock = blocks_[(head.meta.ptr + 1) % block_num];
      MetaCursor committed = nextblock.committed.load();
#if defined(RETRY_NEW)
      if (committed.meta.ver != head.meta.ver + 1)
      {
        return false;
      }
      Raw reset = cursorVal(head.meta.ver + 1);
      fetch_max(nextblock.consumed.raw, reset);
      fetch_max(nextblock.reserved.raw, reset);
#elif defined(DROP_OLD)
      if (
        committed.meta.ver <
        cursor.meta.ver + static_cast<uint64_t>(head.meta.ver == 0))
      {
        return false;
      }
      Raw reset = cursorVal(committed.meta.ver);
      fetch_max(nextblock.reserved.raw, reset);
#endif
      fetch_max(chead.raw, head.val + 1);
      return true;
    }

#ifdef DEBUG
  private:
#  ifdef LOGFILE
    static inline std::ofstream log_file;
#  endif
    static inline std::mutex log_mutex;
    std::ostringstream ss{};

    static std::string timestamp()
    {
      auto now = std::chrono::system_clock::now();
      auto t = std::chrono::system_clock::to_time_t(now);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
        1000;

      std::ostringstream ts;
      // this is not Msan safe, use Msan without `DEBUG`
      ts << std::put_time(std::localtime(&t), "%H:%M:%S") << '.' << std::setw(3)
         << std::setfill('0') << ms.count();
      return ts.str();
    }

  public:
#  ifdef LOGFILE

    void init_log_file(const std::string& filename)
    {
      {
        std::lock_guard<std::mutex> lk{log_mutex};
        log_file.open(filename, std::ios::out | std::ios::trunc);
        if (!log_file.is_open())
        {
          std::cerr << "failed to open logfile\n";
        }
      }
    }
#  endif
    class BBQLog final
    {
    private:
      std::ostringstream ss;

    public:
      template<class Message>
      BBQLog& operator<<(const Message& msg)
      {
        ss << msg;
        return *this;
      }

      ~BBQLog()
      {
        std::lock_guard<std::mutex> lk(log_mutex);
        std::string time = timestamp();
        std::cout << "\033[34m[" << time << "]\033[32m[PID " << ::getpid()
                  << "]\033[35m "
                  << "[TID " << std::this_thread::get_id() << "]\033[0m "
                  << ss.str();
        std::cout.flush();
#  ifdef LOGFILE
        if (log_file.is_open())
        {
          log_file << "[" << time << "][PID " << ::getpid() << "] "
                   << "[TID " << std::this_thread::get_id() << "]"

                   << ss.str();
          log_file.flush();
        }
#  endif
      }
    };

    friend std::ostream&
    operator<<(std::ostream& out, const BBQ<T, block_num, block_size>& q)
    {
      out << "Queue status:\n";
      std::lock_guard<std::mutex> lk(log_mutex);
      q.dump(out);
      out.flush();
      return out;
    }

    friend std::ostream& operator<<(
      std::ostream& out, const typename BBQ<T, block_num, block_size>::Block b)
    {
      std::lock_guard<std::mutex> lk(log_mutex);
      b.dump(out);
      out.flush();
      return out;
    }

#endif
  };
}