#pragma once
// #define _BBQ_NON_ATOMIC_ENTRY_
//  #define _BBQ_DEQ_LATE_READ_
#include "../ds_core/ds_core.h"
#include "snmalloc/mem/metadata.h"
#include "snmalloc/stl/type_traits.h"
#define _BATCHED_BBQ_
#include "bbq.h"
#include "freelist.h"

#include <iostream>

namespace snmalloc
{
  template<
    uint32_t block_num,
    uint32_t block_size,
    FreeListKey* Key, // noticing that `Key` is passed by pointer here
    address_t Key_tweak,
    class T> // T is void*, see forward declaration commentary in bbq
  class alignas(REMOTE_MIN_ALIGN) batchedBBQ_MPSC final
  : public BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>
  {
    // static_assert(
    //   std::is_pointer<T>::value &&
    //     std::is_same<
    //       decltype(std::declval<std::remove_pointer_t<T>>().next),
    //       std::atomic<T>>::value,
    //   "T must be a pointer type, and *T must have a member `next` of type
    //   T");

  public:
    using
      typename BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>::
        Queuestatus;

    SNMALLOC_FAST_PATH void invariant()
    {
      SNMALLOC_ASSERT(pointer_align_up(this, REMOTE_MIN_ALIGN) == this);
    }

    constexpr batchedBBQ_MPSC() = default;

  private:
    using
      typename BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>::
        MetaCursor;
    using
      typename BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>::
        MetaHead;
    using
      typename BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>::
        Blockstate;
    using
      typename BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>::
        Block;

  private:
    stl::conditional_t<stl::is_same_v<T, void*>, freelist::HeadPtr, T>
      consumed_current{nullptr};

    stl::conditional_t<stl::is_same_v<T, void*>, freelist::QueuePtr, T>
      list_head{nullptr};

    T next{nullptr};

  public:
    /**
      * dequeue opertaion of batched bbq (mpsc) as an alternative queue usage to
      freelist_queue based on freelist

      * `IsSingleList` is provided in case to dequeue the current list in
      current payload slot when setting true, otherwise, the queueu will keep
      dequeuing until the callback returns false

      *element is fed to the callback in turn, the callback may return false
      early but still have been processed, poiters read from head or queue will
      be fed to domestication callback*/
    template<
      class Domesticator_head,
      class Domesticator_queue,
      class CB,
      bool IsSingleList = false>
    Queuestatus front(
      Domesticator_head domesticate_head,
      Domesticator_queue domesticate_queue,
      CB cb)
    {
    again:
      invariant();
      Block& blk = this->blocks_[this->chead.load().meta.ptr];
      MetaCursor cursor{};
      switch (reserve_entry(blk, cursor))
      {
        case Blockstate::RESERVED:
        {
          if (list_head == nullptr)
          {
            list_head =
              blk.payload[cursor.meta.ptr].load(stl::memory_order_acquire);
            consumed_current = domesticate_head(list_head);
          }
          while (consumed_current != nullptr)
          {
            freelist::HeadPtr next_ptr = consumed_current->atomic_read_next(
              *Key, Key_tweak, domesticate_queue);
            Aal::prefetch(next_ptr.unsafe_ptr());
            if (SNMALLOC_UNLIKELY(!cb(consumed_current)))
            {
              if (next_ptr != nullptr)
              {
                blk.payload[cursor.meta.ptr].store(
                  capptr_rewild(next_ptr), stl::memory_order_release);
                consumed_current = next_ptr;
                return Queuestatus::OK;
              }
              else
              {
                break;
              }
            }
            consumed_current = next_ptr;
          }
          list_head = nullptr;
          blk.reserved.fetch_add(1);
#if defined(RETRY_NEW)
          blk.consumed.fetch_add(1);
          if constexpr (IsSingleList)
          {
            return Queuestatus::OK;
          }
          else
          {
            goto again;
          }

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
        case Blockstate::BLOCK_DONE:
        {
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
          SNMALLOC_ASSUME(0);
      }
    }

    /**
     * enqueue opertaion of batched bbq (mpsc), elements from first to last
       should be linked together before this operation
     * the last element of the list is terminated by setting it's next nullptr
     to preserve the invariant of the batched bbq (mpmc)

     * The Domesticator here is used only on pointers read from the head.
     */
    template<class Domesticator_head>
    Queuestatus emplace(
      freelist::HeadPtr head,
      freelist::HeadPtr end,
      [[maybe_unused]] Domesticator_head domesticate_head)
    {
      stl::atomic_thread_fence(stl::memory_order_release);
      snmalloc::UNUSED(domesticate_head);

      invariant();
      freelist::Object::atomic_store_null(end, *Key, Key_tweak);

      /*perventfork is disabled*/
      // PreventFork pf;
      // snmalloc::UNUSED(pf);

      return this->enq(capptr_rewild(head));
    }

    /**
     * we leave the is_empty here without tagging it deprecated to make the
     * usage an option, while it's not encourage to call the function since
     * it'll introduce extra memory access overhead*/
    bool is_empty()
    {
      MetaHead head = this->chead.load();
      Block& blk = this->blocks_[head.meta.ptr];
      MetaCursor reserved = blk.reserved.load();
      MetaCursor committed = blk.committed.load();
      if (
        reserved.meta.ptr < block_size &&
        reserved.meta.ptr != committed.meta.ptr)
      {
        return false;
      }

      Block& nextblock = this->blocks_[(head.meta.ptr + 1) % block_num];

      MetaCursor n_committed = nextblock.committed.load();
#if defined(RETRY_NEW)
      if (n_committed.meta.ver != head.meta.ver + 1)
      {
        return true;
      }
#elif defined(DROP_OLD)
      if (
        n_committed.meta.ver <
        reserved.meta.ver + static_cast<uint64_t>(head.meta.ver == 0))
      {
        return true;
      }
#endif
      return false;
    }

  public:
#ifndef _BBQ_DEQ_LATE_READ_
    /**
     * deq operaion for batched bbq (mpsc), calling base deq() directly is ok
     * since it's has already record the the state of current slot in the
     * block*/
    [[deprecated]] Queuestatus getfront(T& data)
    {
      stl::atomic_thread_fence(stl::memory_order_acquire);
      Queuestatus st{};
      if (list_head == nullptr)
      {
        uint64_t head{0};
        if ((st = this->deq(head)) == Queuestatus::OK)
        {
          list_head = reinterpret_cast<T>(static_cast<uintptr_t>(head));
          consumed_current = list_head;
        }
        else
        {
          return st;
        }
      }

      if (consumed_current != nullptr)
      {
        data = consumed_current;
        consumed_current =
          consumed_current->next.load(stl::memory_order_acquire);
        if (consumed_current == nullptr)
        {
          list_head = nullptr;
        }
        data->next.store(nullptr, stl::memory_order_release);
        return Queuestatus::OK;
      }
      SNMALLOC_ASSUME(0);
    }
#endif

    /**
     * deq operation for batched bbq (mpsc), more intuitive version*/
    [[deprecated]] Queuestatus Deq(T& data)
    {
    again:
      Block& blk = this->blocks_[this->chead.load().meta.ptr];
      MetaCursor cursor{};
      switch (reserve_entry(blk, cursor))
      {
        case Blockstate::RESERVED:
        {
          if (list_head == nullptr)
          {
            stl::atomic_thread_fence(stl::memory_order_acquire);
            list_head =
              blk.payload[cursor.meta.ptr].load(stl::memory_order_acquire);
            stl::atomic_thread_fence(stl::memory_order_acquire);

            consumed_current = list_head;
          }
          next = consumed_current->next.load(stl::memory_order_acquire);
          if (next != nullptr)
          {
            T nnext = next->next.load(stl::memory_order_acquire);
            consumed_current->next.store(nullptr, stl::memory_order_release);
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
        case Blockstate::BLOCK_DONE:
        {
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
          SNMALLOC_ASSUME(0);
      }
    }

    /**
     * enq operation*/
    [[deprecated]] Queuestatus Enq(T data)
    {
      return this->enq(data);
    }

    // private:
    //   using head = uint64_t;
    //   using cursor = uint64_t;
    //   head peek_head{std::numeric_limits<uint64_t>::max()};
    //   cursor peek_cursor{block_size};

  private:
    /*overloaded */
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
    using batchedBBQLog =
      typename BBQ<freelist::QueuePtr, block_num, block_size, Key, Key_tweak>::
        BBQLog;

#endif
    // friend std::ostream& operator<<(
    //   std::ostream&,
    //   const batchedBBQ_MPSC<block_num, block_size, Key, Key_tweak, T>&) =
    //   delete;
  };

} // namespace snmalloc