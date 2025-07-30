#define LOGFILE
#define DUMP
#include "snmalloc/mem/message_bbq.h"

#include "snmalloc/backend/globalconfig.h"
#include "snmalloc/backend_helpers/commonconfig.h"
#include "snmalloc/mem/backend_concept.h"
#include "test/xoroshiro.h"

// #include <barrier>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
// #include <unistd.h>
#include <unordered_map>
#include <vector>

#define ITERS 800000

using namespace snmalloc;
static FreeListKey messagebbq_key{0xab2acada, 0xb2a01234, 0x56789abc};
static constexpr address_t messagebbq_key_tweak = 0xfedc'ba98;
static batchedBBQ_MPSC<8, 15, &messagebbq_key, messagebbq_key_tweak> messageq{};

using Ptr = freelist::QueuePtr;

#ifdef DEBUG
#  define log \
    snmalloc::batchedBBQ_MPSC<8, 15, &messagebbq_key, messagebbq_key_tweak>:: \
      batchedBBQLog("batched-message-bbq.log")
#else
class NullLogger
{
public:
  template<typename T>
  NullLogger& operator<<(const T&)
  {
    return *this;
  }
};

#  define log NullLogger()
#endif

#define log1 std::clog
using status = snmalloc::
  batchedBBQ_MPSC<8, 15, &messagebbq_key, messagebbq_key_tweak>::Queuestatus;

freelist::Object::T<>
  array[ITERS * 2 /*two writers*/ * 2 /*two nodes per writer*/];
static std::mutex map_mutex;
static std::unordered_map<address_t, int> deq_item_count;
static std::unordered_map<address_t, int> enq_item_count;

// static bool ready = false;
// static std::mutex m{};
// static std::condition_variable cv;

#ifdef DEBUG
void signal_handler(int)
{
#  ifdef DUMP
  messageq.dump();
#  else
  std::cout << messageq;
#  endif
}
#endif
auto domesticate_nop = [](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
  return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
};

static int reads_count = 0;

void writer(int id)
{
  // std::cout << "writter start";
  // std::cout << q;
  int offset = (id == 1) ? 0 : 1;
  for (int i = 0; i < ITERS; ++i)
  {
    Ptr p1 = freelist::QueuePtr::unsafe_from(array + i + ITERS * offset * 2);
    Ptr p2 =
      freelist::QueuePtr::unsafe_from(array + i + ITERS + ITERS * offset * 2);
    freelist::Object::atomic_store_next(
      domesticate_nop(p1),
      domesticate_nop(p2),
      messagebbq_key,
      messagebbq_key_tweak);
    freelist::Object::atomic_store_null(
      domesticate_nop(p2), messagebbq_key, messagebbq_key_tweak);
    status st{};
    while (
      (st = messageq.emplace(
         domesticate_nop(p1), domesticate_nop(p2), domesticate_nop)))
    {
      switch (st)
      {
        case status::BUSY:
          // log << "\033[36menq busy" << "\n\033[0m";
          continue;
        case status::FULL:
          // log << "\033[36menq full" << "\n\033[0m";
          continue;
        default:
          SNMALLOC_ASSUME(0);
      }
    }
    {
      std::lock_guard<std::mutex> lk(map_mutex);
#if defined(TEST_BBQ_CHECK_ENTRIES)
      ++enq_item_count[p1.unsafe_uintptr()];
      //++enq_item_count[p2.unsafe_uintptr()];
      ++enq_item_count
        [domesticate_nop(p1)
           ->read_next(messagebbq_key, messagebbq_key_tweak, domesticate_nop)
           .unsafe_uintptr()];
#endif
    }
    // log << "\033[36menq " << p1.unsafe_uintptr() << "   enq "
    //      << domesticate_nop(p1)
    //           ->read_next(key, key_tweak, domesticate_nop)
    //           .unsafe_uintptr()
    //      << "\n\033[0m";
    //   log << q;
  }
  // log << "writter end\n";
  // log << q;

  //{
  //  std::lock_guard<std::mutex> l{m};
  //  ready = true;
  //}
  // cv.notify_all();
  // log << "writer end\n";
}

// LocalEntropy entropy;
static xoroshiro::p128r32 returned_r;

void reader(int id)
{
  // std::unique_lock l{m};
  // cv.wait(l, [] { return ready; });
  // log << "reader start\n";
  auto cb = [&](freelist::HeadPtr ptr) SNMALLOC_FAST_PATH_LAMBDA {
    std::lock_guard<std::mutex> lk(map_mutex);
    ++reads_count;
#if defined(TEST_BBQ_CHECK_ENTRIES)
    ++deq_item_count[ptr.unsafe_uintptr()];
#else
    UNUSED(ptr);
#endif
    bool retval = static_cast<bool>(returned_r.next() & 1);
    // log << "\033[32mdeq " << retval << " : " << ptr.unsafe_uintptr()
    //     << "\n\033[0m";
    return id < 0 ? retval : true;
  };
  if (id < 0)
  {
    status st{};
    while (true)
    {
      st = messageq.front(domesticate_nop, domesticate_nop, cb);

      if (st == status::EMPTY)
      {
        {
          std::lock_guard<std::mutex> lk(map_mutex);
          if (reads_count == -id * 2)
          {
            return;
          }
        }
      }
    }
  }
  else
  {
    for (int i = 1; i <= id; ++i)
    {
      status st{};
      while (
        (st = messageq.template front<
              decltype(domesticate_nop),
              decltype(domesticate_nop),
              decltype(cb),
              true>(domesticate_nop, domesticate_nop, cb)))
      {
        switch (st)
        {
          case status::BUSY:
            // log << "\033[33m deq busy" << "\n\033[0m";
            continue;
          case status::EMPTY:
            // log << "\033[34m deq empty" << "\n\033[0m";
            continue;
          default:
            SNMALLOC_ASSUME(0);
        }
      }
    }
  }
  // std::cout << "end";
  // log << q;
}

#ifdef TEST_BBQ_CHECK_ENTRIES
void check(size_t iterations)
{
  if (
    enq_item_count.size() != iterations ||
    (deq_item_count.size() != iterations))
  {
    log1 << "\033[4;41mERROR\n\033[0m";
    std::cerr << "\033[4;41mCount Fail:  enqs: " << enq_item_count.size()
              << " deqs: " << deq_item_count.size() << "\n\033[0m";
    abort();
  }
  std::lock_guard<std::mutex> lk(map_mutex);
  for (const auto& [k, v] : enq_item_count)
  {
    if (deq_item_count[k] != v)
    {
      log1 << "\033[4;41mERROR\n\033[0m";
      std::cerr << "\033[4;41mMISMATCH enq/deq at item: " << k << "[enq= " << v
                << ", deq= " << deq_item_count[k] << "]\n\033[0m";
      abort();
    }
  }
  log << "\033[4;42menq/deq matched\n\033[0m";
}
#endif

int main()
{
  constexpr int batched = -1;
  using Config = StandardConfigClientMeta<NoClientMetaDataProvider>;
  static_assert(Config::Options.QueueHeadsAreTame);
#if defined(DEBUG) && (defined(__unix__) || defined(__APPLE__))
  std::signal(SIGQUIT, signal_handler);
#endif
  auto begin = std::chrono::steady_clock::now();
  std::thread t3(writer, 1);
  std::thread t1(writer, -1);
  std::thread t2(reader, ITERS * 2 * batched);

  // std::thread t4(reader, ITERS * 2);
  //     std::thread t5(writer, 10);
  //    std::thread t6(reader, ITERS + 20);
  //    std::thread t7(reader, 50);
  //   std::thread t8(writer, 2);
  t1.join();
  t2.join();
  t3.join();
  // t4.join();
  //     t7.join();
  //          t5.join();
  //     t8.join();
  //         t6.join();
  //      log << "\v\v===============================================\n";
  //      std::cout << q;
  auto end = std::chrono::steady_clock::now();
  double time_in_sec =
    std::chrono::duration<double, std::milli>(end - begin).count() / 1000.0;
  uint64_t total_op =
    (batched == -1) ? ITERS * 2 : ITERS * 4; // producer's and consumer's
  log << "BatchedBBQ(MPSC): finish writing and reading with throughput = "

      << static_cast<double>(total_op) / time_in_sec << " op/s."
#if defined(TEST_BBQ_CHECK_ENTRIES)
      << "  [check_entries enabled(turn off to get real throughput)]\n";
#else
      << '\n';
#endif
//    log << q;
// std::cout << "===========\n";
// for (node n : array)
//{
//   std::cout << n.value << ' ' << n.next << '\n';
// }
// log << q;
#if defined(TEST_BBQ_CHECK_ENTRIES)
  log << "check enq and deq operations' data matching:\n";
  check(ITERS * 2 * 2);
#endif
}
