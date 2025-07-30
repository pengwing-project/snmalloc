#!/bin/bash
ISSP=false
ISSC=false
BLOCKNUM=8
BLOCKSIZE=15
PRODUCER=4
CONS=4
LOG=true
LEVEL=0
SYNC=0
ITER=1'000'000

while [[ $# -gt 0 ]]; do
  case "$1" in
  --sp)
    ISSP=true
    ;;
  --sc)
    ISSC=true
    ;;
  --mp)
    ISSP=false
    ;;
  --mc)
    ISSC=false
    ;;
  --bn)
    BLOCKNUM="$2"
    shift
    ;;
  --bs)
    BLOCKSIZE="$2"
    shift
    ;;
  --producer)
    PRODUCER="$2"
    shift
    ;;
  --consumer)
    CONS="$2"
    shift
    ;;
  --iteration)
    ITERS="$2"
    shift
    ;;
  --log)
    LOG=true
    ;;
  --nolog)
    LOG=false
    ;;
  --loglevel)
    LEVEL="$2"
    shift
    ;;
  --sync)
    SYNC="$2"
    shift
    ;;
  --nosync)
    SYNC=false
    ;;
  *)
    echo "unknown options: $1"
    exit 1
    ;;
  esac
  shift
done

if [[ "$(basename "$(pwd)")" != "perf_bbq_variant" ]]; then
  echo "do not run this script from the build directory"
  exit 1
fi

sed -i "s/static const uint32_t block_num = .*/static const uint32_t block_num = ${BLOCKNUM};/" bbq_perf.cc
sed -i "s/static const uint32_t block_size = .*/static const uint32_t block_size = ${BLOCKSIZE};/" bbq_perf.cc

sed -i "s/static const bool isSP = .*/static const bool isSP = ${ISSP};/" bbq_perf.cc
sed -i "s/static const bool isSC = .*/static const bool isSC = ${ISSC};/" bbq_perf.cc
sed -i "s/static const bool islogging = .*/static const bool islogging = ${LOG};/" bbq_perf.cc
sed -i "s/static const uint16_t loglevel = .*/static const uint16_t loglevel = ${LEVEL};/" bbq_perf.cc

sed -i "s/static const uint16_t tp_type = .*/static const uint16_t tp_type = ${SYNC};/" bbq_perf.cc

sed -i "s/static const size_t single_writer_iteration = .*/static const size_t single_writer_iteration = ${ITERS};/" bbq_test.h
