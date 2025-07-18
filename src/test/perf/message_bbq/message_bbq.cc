
#include <unordered_map>
#ifndef DEBUG
#  define DEBUG
#  define DUMP
#  define LOGFILE
#endif

#include "../src/ds/message_bbq.h"

#include <barrier>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>
#define ITERS 40000
using namespace snmalloc;
#define type float

struct node
{
  type value{0};
  std::atomic<node*> next{nullptr};

  void set(type v)
  {
    value = v;
  }
};

class nodeList
{
public:
  node head{};
  node* last;

  nodeList()
  {
    last = &head;
  }

  bool empty()
  {
    return last == &head;
  }
};

batchedBBQ_MPSC<node*, 2, 20> q{};

#define log batchedBBQ_MPSC<node*, 2, 20>::batchedBBQLog()
#define log1 std::cout
using status = batchedBBQ_MPSC<node*, 2, 20>::Queuestatus;

node array[ITERS * 2 /*two writers*/ * 2 /*two nodes per writer*/];
std::mutex map_mutex;
std::unordered_map<type, int> deq_item_count;
// static bool ready = false;
//
// static std::mutex m{};
// static std::condition_variable cv;

#ifdef DEBUG
void signal_handler(int)
{
#  ifdef DUMP
  q.dump();
#  else
  std::cout << q;
#  endif
}
#endif

void writer(int id)
{
  // std::cout << "writter start";
  // std::cout << q;
  int offset = (id == 1) ? 0 : 1;
  for (int i = 0; i < ITERS; ++i)
  {
    nodeList l;
    node* n1 = (node*)(array + i + ITERS * offset * 2);
    n1->set((i + 1) * id);
    l.head.next.store(n1, std::memory_order_release);
    l.last = n1;
    std::atomic_thread_fence(std::memory_order_release);
    n1->next.store(nullptr, std::memory_order_release);

    node* n2 = (node*)(array + i + ITERS + ITERS * offset * 2);
    n2->set((i + 1 + 0.1) * id);
    n2->next.store(nullptr, std::memory_order_release);
    l.last->next.store(n2, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    l.last = n2;
    std::atomic_thread_fence(std::memory_order_release);
    status st{};
    while ((st = q.emplace(l.head.next)))
    {
      switch (st)
      {
        case status::BUSY:
          // log << "\033[36menq " << i * id << " enq busy" << "\n\033[0m";
          continue;
        case status::FULL:
          // log << "\033[36menq " << i * id << " enq full" << "\n\033[0m";
          continue;
        default:
          __builtin_unreachable();
      }
    }
    // log1 << "\033[36menq " << l.head.next << "   enq " <<
    // l.head.next.load()->next << "\n\033[0m";
    //  std::cout << static_cast<type>(l.head.next->value) << ' '
    //            << static_cast<type>(l.head.next->next->value) << ' '
    //            << l.head.next->next->next << '\n';
    //     log << q;
    //        std::cout << q;
  }
  // log << "writter end\n";
  //  q.dump();

  //{
  //  std::lock_guard<std::mutex> l{m};
  //  ready = true;
  //}
  // cv.notify_all();
  // std::cout << "writer end";
}

void reader(int id)
{
  // std::unique_lock l{m};
  // cv.wait(l, [] { return ready; });
  //// std::cout << "reader start";
  //// std::cout << q;
  node* buf = nullptr;
  for (int i = 1; i <= id * 2 /*each write will push two nodes*/; ++i)
  {
    // std::cout << "turn===============" << i << '\n';
    status st{};
    while ((st = q.front(buf)))
    {
      switch (st)
      {
        case status::BUSY:
          // log << "\033[32mdeq " << buf << " deq busy" << "\n\033[0m";
          continue;
        case status::EMPTY:
          // log << "\033[32mdeq " << buf << " deq empty" << "\n\033[0m";
          continue;
        default:
          __builtin_unreachable();
      }
    }
    // q.dump();
    //  std::cout << static_cast<type>(buf->value) << '\n';
    //  std::cout << "reader" << buf->value << '\n';
    std::lock_guard<std::mutex> lk(map_mutex);
    deq_item_count[buf->value]++;
    // log1 << "\033[32mdeq " << buf << "\n\033[0m";
  }
  // std::cout << "end";
  // q.dump();
  //   log << "reader end\n";
}

void check(size_t iterations)
{
  if (deq_item_count.size() != iterations)
  {
    std::clog << "\033[4;41mERROR\n\033[0m";
    std::cerr << "\033[4;41mCount Fail:  deqs: " << deq_item_count.size()
              << "\n\033[0m";
    abort();
  }
  // for (int i = 1; i <= ITERS; ++i)
  //{
  //   if (!deq_item_count.contains(static_cast<type>(i)))
  //   {
  //     std::clog << "\033[4;41mERROR\n\033[0m";
  //     std::cerr << "\033[4;41mMISMATCH enq/deq item of" <<
  //     static_cast<type>(i)
  //               << "\n\033[0m";
  //     abort();
  //   }
  //   if (!deq_item_count.contains(i + 0.1))
  //   {
  //     std::clog << "\033[4;41mERROR\n\033[0m";
  //     std::cerr << "\033[4;41mMISMATCH enq/deq item of" << i + 0.1
  //               << "\n\033[0m";
  //     abort();
  //   }
  //   if (!deq_item_count.contains(static_cast<type>(-i)))
  //   {
  //     std::clog << "\033[4;41mERROR\n\033[0m";
  //     std::cerr << "\033[4;41mMISMATCH enq/deq item of" <<
  //     static_cast<type>(-i)
  //               << "\n\033[0m";
  //     abort();
  //   }
  //   if (!deq_item_count.contains(-(i + 0.1)))
  //   {
  //     std::clog << "\033[4;41mERROR\n\033[0m";
  //     std::cerr << "\033[4;41mMISMATCH enq/deq item of" << -(i + 0.1)
  //               << "\n\033[0m";
  //     abort();
  //   }
  // }
  for (const auto& [k, v] : deq_item_count)
  {
    if (deq_item_count[k] != 1)
    {
      std::clog << "\033[4;41mERROR\n\033[0m";
      std::cerr << "\033[4;41mMISMATCH enq/deq at item: " << k
                << " [deq= " << deq_item_count[k] << "]\n\033[0m";
      // for (int i = 0; i < ITERS * 2 * 2; ++i)
      // {
      //   std::clog << &array[i] << ' ' << array[i].value << ' '
      //             << array[i].next.load() << '\n';
      // }
      abort();
    }
  }
  std::clog << "\033[4;42menq/deq matched\n\033[0m";
}

int main()
{
  // std::cout << q;
#ifdef DEBUG
  std::signal(SIGQUIT, signal_handler);
#  ifdef LOGFILE
  q.init_log_file("bbq.log");
#  endif
#endif
  auto begin = std::chrono::steady_clock::now();
  std::thread t3(writer, 1);
  std::thread t1(writer, -1);
  std::thread t2(reader, ITERS * 2);

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
  float time_in_sec =
    std::chrono::duration<double, std::milli>(end - begin).count() / 1000.0;
  uint64_t total_op = ITERS * 3; // producer's and consumer's
  std::cout << "BatchedBBQ(MPSC): finish writing and reading with throughput = "
            << total_op / time_in_sec << " op/s.\n";
  //    log << q;
  // std::cout << "===========\n";
  // for (node n : array)
  //{
  //   std::cout << n.value << ' ' << n.next << '\n';
  // }
  // log << q;
  check(ITERS * 2 * 2);
}
