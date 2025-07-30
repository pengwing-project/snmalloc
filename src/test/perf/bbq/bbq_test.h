#define LOGFILE
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <snmalloc/snmalloc.h>
#include <test/opt.h>
#include <test/setup.h>
#include <thread>
#include <unordered_map>

template<
  uint32_t bn,
  uint32_t bs,
  bool isSP,
  bool isSC,
  bool islogging = true,
  uint16_t loglevel = 0,
  uint16_t tp_type = 0>
/*ty_type: 0:all 1:producer only, 2:consumer only*/
class BBQTest
{
  using type = size_t;

private:
  size_t prod;
  size_t cons;
  snmalloc::BBQ<type, bn, bs, isSP, isSC> test_q{};

  static const size_t single_writer_iteration = 1'000'000;
  using status = typename snmalloc::BBQ<type, bn, bs, isSP, isSC>::Queuestatus;

#if defined(DEBUG)
#  define qlog \
    typename snmalloc::BBQ<type, bn, bs, isSP, isSC>::BBQLog("bbq.log")
#else
#  define qlog std::clog
#endif
#ifdef TEST_BBQ_CHECK_ENTRIES
  std::mutex map_mutex;
  std::unordered_map<type, int> enq_item_count;
  std::unordered_map<type, int> deq_item_count;
#endif

  bool ready = false;
  std::mutex m{};
  std::condition_variable cv;

  // zero should not appear
  void consumer_distribution(std::vector<size_t>& single_reader_iteration)
  {
    size_t total = prod * single_writer_iteration;
    if (cons == 1)
    {
      single_reader_iteration[0] = total;
      return;
    }

    assert(total > cons);
    size_t remaining = total - cons;
    std::mt19937 gen(std::random_device{}());

    std::vector<size_t> bars;
    std::uniform_int_distribution<size_t> dist(0, remaining);
    for (size_t i = 0; i < cons - 1; ++i)
    {
      bars.push_back(dist(gen));
    }

    std::sort(bars.begin(), bars.end());
    single_reader_iteration[0] = bars[0] + 1;
    for (size_t i = 1; i < static_cast<size_t>(cons - 1); ++i)
    {
      single_reader_iteration[i] = bars[i] - bars[i - 1] + 1;
    }
    single_reader_iteration[static_cast<size_t>(cons - 1)] =
      remaining - bars.back() + 1;
  }

  void writer(size_t id)
  {
    if constexpr (islogging && loglevel > 0)
    {
      qlog << "writer start";
    }
    for (size_t i = 0; i < single_writer_iteration; ++i)
    {
      status st{};
      type data = id * single_writer_iteration + i;
      while ((st = test_q.enq(static_cast<type>(data))))
      {
        switch (st)
        {
          case status::BUSY:
            if constexpr (islogging && loglevel > 0)
            {
              qlog << "\033[36menq " << data << " enq busy"
                   << "\n\033[0m";
            }
            continue;
          case status::FULL:
            if constexpr (islogging && loglevel > 0)
            {
              qlog << "\033[36menq " << data << " enq full"
                   << "\n\033[0m";
            }
            continue;
          default:
            SNMALLOC_ASSUME(0);
        }
      }
      if constexpr (islogging)
      {
        qlog << "\033[36menq " << data << "\n\033[0m";
      }
#ifdef TEST_BBQ_CHECK_ENTRIES
      std::lock_guard<std::mutex> lk(map_mutex);
      enq_item_count[data]++;
#endif
    }
    if constexpr (islogging && loglevel > 0)
    {
      qlog << "writter end\n";
    }
    if constexpr (tp_type == 2)
    {
      {
        std::lock_guard<std::mutex> l{m};
        ready = true;
      }
      cv.notify_all();
    }
  }

  void reader(size_t iter)
  {
    if constexpr (tp_type == 2)
    {
      std::unique_lock l{m};
      cv.wait(l, [this] { return ready; });
    }
    if constexpr (islogging && loglevel > 0)
    {
      qlog << "reader start";
    }
    type buf;
    for (size_t i = 0; i < iter; ++i)
    {
      status st{};
      while ((st = test_q.deq(buf)))
      {
        switch (st)
        {
          case status::BUSY:
            if constexpr (islogging && loglevel > 0)
            {
              qlog << "\033[32mdeq " << buf << " deq busy"
                   << "\n\033[0m";
            }
            continue;
          case status::EMPTY:
            if constexpr (islogging && loglevel > 0)
            {
              qlog << "\033[32mdeq " << buf << " deq empty"
                   << "\n\033[0m";
            }
            continue;
          default:
            SNMALLOC_ASSUME(0);
        }
      }
      if constexpr (islogging)
      {
        qlog << "\033[32mdeq " << buf << "\n\033[0m";
      }
#ifdef TEST_BBQ_CHECK_ENTRIES
      std::lock_guard<std::mutex> lk(map_mutex);
      deq_item_count[buf]++;
#endif
    }
    if constexpr (islogging && loglevel > 0)
    {
      qlog << "reader end\n";
    }
  }

#ifdef TEST_BBQ_CHECK_ENTRIES
  void check(size_t iterations)
  {
    if (
      enq_item_count.size() != iterations ||
      deq_item_count.size() != iterations)
    {
      qlog << "\033[4;41mERROR\n\033[0m";
      std::cerr << "\033[4;41mCount Fail\n\033[0m";
      abort();
    }
    std::lock_guard<std::mutex> lk(map_mutex);
    for (const auto& [k, v] : enq_item_count)
    {
      if (deq_item_count[k] != v)
      {
        qlog << "\033[4;41mERROR\n\033[0m";
        std::cerr << "\033[4;41mMISMATCH enq/deq at item: " << k
                  << "[enq= " << v << ", deq= " << deq_item_count[k]
                  << "]\n\033[0m";
        abort();
      }
    }
    qlog << "\033[4;42menq/deq matched\n\033[0m";
  }
#endif

public:
  BBQTest(size_t producer, size_t consumer) : prod{producer}, cons{consumer}
  {
    std::thread* pt = new std::thread[prod];
    std::thread* ct = new std::thread[cons];

    std::vector<size_t> single_reader_iteration(cons);
    consumer_distribution(single_reader_iteration);

    std::chrono::steady_clock::time_point begin;

    if constexpr (tp_type == 1)
    {
      begin = std::chrono::steady_clock::now();
      for (size_t i = 0; i < prod; ++i)
      {
        pt[i] = std::thread(&BBQTest::writer, this, i);
      }

      for (size_t i = 0; i < prod; ++i)
      {
        pt[i].join();
      }
    }
    else if constexpr (tp_type == 2)
    {
      for (size_t i = 0; i < prod; ++i)
      {
        pt[i] = std::thread(&BBQTest::writer, this, i);
      }

      for (size_t i = 0; i < prod; ++i)
      {
        pt[i].join();
      }
      begin = std::chrono::steady_clock::now();
      for (size_t i = 0; i < cons; ++i)
      {
        ct[i] = std::thread(&BBQTest::reader, this, single_reader_iteration[i]);
      }

      for (size_t i = 0; i < cons; ++i)
      {
        ct[i].join();
      }
    }
    else // tp_type == 0
    {
      begin = std::chrono::steady_clock::now();
      for (size_t i = 0; i < prod; ++i)
      {
        pt[i] = std::thread(&BBQTest::writer, this, i);
      }

      for (size_t i = 0; i < cons; ++i)
      {
        ct[i] = std::thread(&BBQTest::reader, this, single_reader_iteration[i]);
      }
      for (size_t i = 0; i < prod; ++i)
      {
        pt[i].join();
      }
      for (size_t i = 0; i < cons; ++i)
      {
        ct[i].join();
      }
    }

    auto end = std::chrono::steady_clock::now();

    double time_in_sec =
      std::chrono::duration<double, std::milli>(end - begin).count() / 1000.0;

    uint64_t total_op = single_writer_iteration * prod *
      ((tp_type == 1 || tp_type == 2) ? 1 : 2); // producer's or/and consumer's

#if defined(DEBUG)
    if constexpr (islogging)
    {
      qlog << test_q;
    }
#endif
    delete[] pt;
    delete[] ct;

#ifdef TEST_BBQ_CHECK_ENTRIES
    qlog << "check enq and deq operations' data matching:\n";
    check(single_writer_iteration * prod);
#endif

    std::string queue_mode = test_q.queue_mode();

    double throughput = static_cast<double>(total_op) / time_in_sec / 1e6;

#ifdef BBQ_VARIANT_PERF
    std::ofstream csv("perf-bbq-variant.csv", std::ios::app);
    csv << bn << ',' << bs << ',' << prod << ',' << cons << ',' << queue_mode
        << ',' << throughput;
    if constexpr (tp_type == 1)
    {
      csv << ',' << 'P';
    }
    else if constexpr (tp_type == 2)
    {
      csv << ',' << 'C';
    }
    else
    {
      csv << ',' << 'A';
    }
    // no new line cuz there's run time mode handling in script
    csv.close();
#endif

    qlog << "vanilla bbq's test: \n"
         << "test configuration: producers = " << prod
         << ", consumers = " << cons << '\n'
         << "queue configuration: "
         << "block num: " << bn << ", block size: " << bs
         << ", queue_mode: " << queue_mode << '\n';
#if defined(TEST_BBQ_CHECK_ENTRIES)
    qlog << "[check_entries enabled(turn off to get real throughput)]\n";
#endif
    if constexpr (islogging)
    {
      qlog << "[logging enabled(turn off to get real throughput)]\n";
    }
    qlog << "BBQ: finish writing and reading with throughput = " << throughput
         << " Mop/s\n";
  }
};
