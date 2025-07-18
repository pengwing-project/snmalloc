#include "snmalloc.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <condition_variable>
#include <mutex>
#define DEBUG
#define DUMP
#include <thread>

size_t* p{nullptr};

static bool ready = {false};
std::mutex m{};
std::condition_variable cv{};
int main()
{
  xoroshiro::p128r32 r;
  size_t alloc_size = 16;
  std::thread t1([&]() {
    // snmalloc::ThreadAlloc::get()->message_queue().dump();
    p = (size_t*)snmalloc::ThreadAlloc::get()->alloc(alloc_size);
    // std::cout << "alloced\n\n";
    std::cout << snmalloc::ThreadAlloc::get()->get_id();
    {
      std::lock_guard<std::mutex> l{m};
      ready = true;
    }
    cv.notify_one();
  });
  std::thread t2([&]() {
    std::unique_lock l{m};
    cv.wait(l, [] { return ready; });
    // snmalloc::ThreadAlloc::get()->message_queue().dump();
    snmalloc::ThreadAlloc::get()->dealloc(p, alloc_size);
    // std::cout << "dealloced\n";
    std::cout << snmalloc::ThreadAlloc::get()->get_id();
  });
  t1.join();
  t2.join();

  snmalloc::Stats s;
  snmalloc::current_alloc_pool()->aggregate_stats(s);
  s.print<snmalloc::Alloc>(std::cout);

  usage::print_memory();
  snmalloc::current_alloc_pool()->debug_check_empty();
}
// int main()
//{
//  xoroshiro::p128r32 r;
//  size_t alloc_size = 16;
//  [[maybe_unused]] size_t* ptr =
//    (size_t*)snmalloc::ThreadAlloc::get()->alloc(alloc_size);
//  snmalloc::ThreadAlloc::get()->dealloc(ptr);
//  snmalloc::current_alloc_pool()->debug_check_empty();
//  snmalloc::Stats s;
//  snmalloc::current_alloc_pool()->aggregate_stats(s);
//  s.print<snmalloc::Alloc>(std::cout);
//
//  usage::print_memory();
//}
