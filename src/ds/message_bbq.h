#pragma once
// #define _BBQ_NON_ATOMIC_ENTRY_
//  #define _BBQ_DEQ_LATE_READ_
#define _BATCHED_BBQ_
#include <atomic>
// #define DEBUG
// #define DUMP
#include "bbq.h"

#include <iostream>
#include <limits>
namespace snmalloc
{
  template<typename T, uint32_t block_num, uint32_t block_size>
  class batchedBBQ_MPSC final : public BBQ<T, block_num, block_size>
  {
    static_assert(
      std::is_pointer<T>::value &&
        std::is_same<
          decltype(std::declval<std::remove_pointer_t<T>>().next),
          std::atomic<T>>::value,
      "T must be a pointer type, and *T must have a member `next` of type T");

  public:
    using typename BBQ<T, block_num, block_size>::Queuestatus;

  private:
    using typename BBQ<T, block_num, block_size>::MetaCursor;
    using typename BBQ<T, block_num, block_size>::MetaHead;
    using typename BBQ<T, block_num, block_size>::Blockstate;
    using typename BBQ<T, block_num, block_size>::Block;
    //    struct Block final : public BBQ<T, block_num, block_size>::Block
    //    {
    // #ifdef DEBUG
    //
    //      std::ostream& dump(
    //        std::ostream& out = std::cout,
    //        [[maybe_unused]] size_t groupsize = block_size) const override
    //      {
    //        MetaCursor allocate = this->allocated.load();
    //        MetaCursor commit = this->committed.load();
    //        MetaCursor reserve = this->reserved.load();
    //        MetaCursor consume = this->consumed.load();
    //
    //        out << "allocated: " << allocate.meta.ptr << "[" <<
    //        allocate.meta.ver
    //            << "]\n";
    //        out << "committed: " << commit.meta.ptr << "[" << commit.meta.ver
    //            << "]\n";
    //        out << "reserved: " << reserve.meta.ptr << "[" << reserve.meta.ver
    //            << "]\n";
    //        out << "consumed: " << consume.meta.ptr << "[" << consume.meta.ver
    //            << "]\n";
    //        for (size_t i = 0; i < block_size; ++i)
    //        {
    //          out << this->payload[i]->value;
    //          T cur = this->payload[i]->next;
    //          while (cur != nullptr)
    //          {
    //            out << " -> " << cur->value;
    //            cur = cur->next;
    //          }
    //
    //          out << '\n';
    //        }
    //        return out;
    //      }
    // #endif
    //    };
    T consumed_current{nullptr};
    T next{nullptr};
    T list_head{nullptr};

  public:
    [[deprecated]] Queuestatus getfront(T& data)
    {
      std::atomic_thread_fence(std::memory_order_acquire);
      Queuestatus st{};
      if (list_head == nullptr)
      {
        uint64_t head{0};
        if ((st = this->deq(head)) == Queuestatus::OK)
        {
          // std::cout << head << ' ';
          assert(head != std::numeric_limits<uint64_t>::max());
          list_head = reinterpret_cast<T>(static_cast<uintptr_t>(head));
          // std::cout << list_head << '\n';
          consumed_current = list_head;
        }
        else
        {
          return st;
        }
        // std::cout << "head::::" << list_head->value << '\n';
      }

      if (consumed_current != nullptr)
      {
        std::cout << "consumed-current" << consumed_current << '\n';
        data = consumed_current;
        std::cout << "data" << data << '\n';
        consumed_current =
          consumed_current->next.load(std::memory_order_acquire);
        std::cout << "next-consumed-current" << consumed_current << '\n';
        // std::cout << "head::::" << list_head->value << '\n';
        if (consumed_current == nullptr)
        {
          list_head = nullptr;
        }
        data->next.store(nullptr, std::memory_order_release);
        // std::cout << "data    " << data->value << '\n';
        return Queuestatus::OK;
      }
      __builtin_unreachable();
    }

    Queuestatus front(T& data)
    {
    again:
      Block& blk = this->blocks_[this->chead.load().meta.ptr];
      MetaCursor cursor{};
      switch (reserve_entry(blk, cursor))
      {
        case Blockstate::RESERVED: {
          if (list_head == nullptr)
          {
            std::atomic_thread_fence(std::memory_order_acquire);
            list_head =
              blk.payload[cursor.meta.ptr].load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_acquire);

            consumed_current = list_head;
          }
          next = consumed_current->next.load(std::memory_order_acquire);
          if (next != nullptr)
          {
            T nnext = next->next.load(std::memory_order_acquire);
            consumed_current->next.store(nullptr, std::memory_order_release);
            data = consumed_current;
            consumed_current = next;
            next = nnext;
            return Queuestatus::OK;
          }
          else
          {
            list_head = nullptr;
            data = consumed_current;
            consumed_current = nullptr;
            next = nullptr;
            blk.reserved.fetch_add(1);
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
        }
        case Blockstate::NO_ENTRY:
          return Queuestatus::EMPTY;
        case Blockstate::NOT_AVAILABLE:
          return Queuestatus::BUSY;
        case Blockstate::BLOCK_DONE: {
          MetaCursor cursor{};
          if (this->advance_chead(this->chead, cursor))
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

    Queuestatus emplace(T data)
    {
      // std::cout << "enq " << data << data->value << '\n';

      return this->enq(data);
    }

    // private:
    //   using head = uint64_t;
    //   using cursor = uint64_t;
    //   head peek_head{std::numeric_limits<uint64_t>::max()};
    //   cursor peek_cursor{block_size};

  private:
    Blockstate reserve_entry(Block& blk, MetaCursor& cursor)
    {
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
        {
          cursor = reserved;
          return Blockstate::RESERVED;
        }
      }
      cursor = reserved;
      return Blockstate::BLOCK_DONE;
    }

#ifdef DEBUG
  public:
    using batchedBBQLog = typename BBQ<T, block_num, block_size>::BBQLog;

#endif
    // friend std::ostream& operator<<( std::ostream&, const
    // batchedBBQ_MPSC<T, block_num, block_size>&) = delete;
  };
}