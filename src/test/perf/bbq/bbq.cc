
#ifndef DEBUG
#  define DEBUG
#  define DUMP
#  define LOGFILE
#endif

#include "../src/ds/bbq.h"

#include <barrier>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#define ITERS 80000
using namespace snmalloc;
#define type int
BBQ<type, 8, 15> q{};
#define log BBQ<type, 8, 15>::BBQLog()
#define log1 std::clog
using status = BBQ<type, 8, 15>::Queuestatus;

std::mutex map_mutex;
std::unordered_map<type, int> enq_item_count;
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
  for (int i = 2; i <= ITERS + 1; ++i)
  {
    status st{};
    while ((st = q.enq(static_cast<type>(i * id))))
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
    // log << q;
    log << "\033[36menq " << i * id << "\n\033[0m";
    //      std::cout << q;
    std::lock_guard<std::mutex> lk(map_mutex);
    enq_item_count[i * id]++;
  }
  // log << "writter end\n";
  //    {
  //      std::lock_guard<std::mutex> l{m};
  //      ready = true;
  //    }
  //    cv.notify_all();
}

void reader(int id)
{
  // std::unique_lock l{m};
  // cv.wait(l, [] { return ready; });
  // std::cout << "reader start";
  // std::cout << q;
  type buf;
  for (int i = 0; i < id; ++i)
  {
    status st{};
    while ((st = q.deq(buf)))
    {
      switch (st)
      {
        case status::BUSY:
          // log << "\033[32mdeq " << buf << " deq busy" << "\n\033[0m";
          continue;
        case status::EMPTY:
          // log << "\033[32mdeq " << buf << " deq empty" << "\n\033[0m";
          //    log << q;
          continue;
        default:
          __builtin_unreachable();
      }
    }
    log << "\033[32mdeq " << buf << "\n\033[0m";
    std::lock_guard<std::mutex> lk(map_mutex);
    deq_item_count[buf]++;
  }
  // log << "reader end\n";
}

void check(size_t iterations)
{
  if (
    enq_item_count.size() != iterations || deq_item_count.size() != iterations)
  {
    log << "\033[4;41mERROR\n\033[0m";
    std::cerr << "\033[4;41mCount Fail\n\033[0m";
  }
  std::lock_guard<std::mutex> lk(map_mutex);
  for (const auto& [k, v] : enq_item_count)
  {
    if (deq_item_count[k] != v)
    {
      log << "\033[4;41mERROR\n\033[0m";
      std::cerr << "\033[4;41mMISMATCH enq/deq at item: " << k << "[enq= " << v
                << ", deq= " << deq_item_count[k] << "]\n\033[0m";
      abort();
    }
  }
  log << "\033[4;42menq/deq matched\n\033[0m";
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
  std::thread t3(writer, -1);
  std::thread t1(writer, 1);
  std::thread t2(reader, ITERS);
  std::thread t4(reader, ITERS);
  //     std::thread t5(writer, 10);
  //    std::thread t6(reader, ITERS + 20);
  //    std::thread t7(reader, 50);
  //   std::thread t8(writer, 2);
  t1.join();
  t2.join();
  t3.join();
  t4.join();
  //  t7.join();
  //       t5.join();
  //  t8.join();
  //      t6.join();
  //   log << "\v\v===============================================\n";
  //   std::cout << q;
  auto end = std::chrono::steady_clock::now();
  float time_in_sec =
    std::chrono::duration<double, std::milli>(end - begin).count() / 1000.0;
  uint64_t total_op = ITERS * 4; // producer's and consumer's
  log << "BBQ: finish writing and reading with throughput = "
      << total_op / time_in_sec << " op/s.\n";
  log << q;
  log << "check enq and deq operations' data matching\n";
  check(ITERS * 2);
}
