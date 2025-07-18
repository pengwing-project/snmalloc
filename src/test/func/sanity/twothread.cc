
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <condition_variable>
#include <mutex>
#include <snmalloc/snmalloc.h>
#define DEBUG
#define DUMP
#include <thread>

int main()
{
  static bool ready = {false};
  std::mutex m{};
  std::condition_variable cv{};
  size_t* p{nullptr};
  xoroshiro::p128r32 r;
  size_t alloc_size = 16;
  std::thread t1([&]() {
    p = static_cast<size_t*>(snmalloc::alloc(alloc_size));
    {
      std::lock_guard<std::mutex> l{m};
      ready = true;
    }
    cv.notify_one();
  });
  std::thread t2([&]() {
    std::unique_lock l{m};
    cv.wait(l, [] { return ready; });
    snmalloc::dealloc(p);
  });
  t1.join();
  t2.join();

  size_t* p1 = static_cast<size_t*>(snmalloc::alloc(alloc_size));
  snmalloc::dealloc(p1);

#ifndef NDEBUG
  snmalloc::debug_check_empty();
#endif
#ifdef USE_SNMALLOC_STATS
  Stats s;
  current_alloc_pool()->aggregate_stats(s);
  s.print<Alloc>(std::cout);
#endif

  usage::print_memory();
}
