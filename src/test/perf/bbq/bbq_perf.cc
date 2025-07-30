#include "bbq_test.h"

int main(int argc, char** argv)
{
  setup();
  opt::Opt opt(argc, argv);
  uint32_t producers = opt.is<uint32_t>("--producers", 4);
  uint32_t consumers = opt.is<uint32_t>("--consumers", 4);

  static const uint32_t block_num = 8;
  static const uint32_t block_size = 15;
  static const bool isSP = false;
  static const bool isSC = false;

  static const bool islogging = false;
  static const uint16_t loglevel = 0;

  // sync consuming after producing
  static const uint16_t tp_type = 0;

  BBQTest<block_num, block_size, isSP, isSC, islogging, loglevel, tp_type>
    tests(producers, consumers);
}