#!/bin/bash

if [[ "$(basename "$(pwd)")" != "perf_bbq_variant" ]]; then
	echo "do not run this script from the build directory"
	exit 1
fi

set -euo pipefail

#reconfig the build system
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release -DTEST_BBQ_CHECK_ENTRIES=OFF
chmod +x *.sh

SRC_FILE="bbq_perf.cc"
CSV_FILE="perf-bbq-variant.csv"

rm -f "$CSV_FILE"

# enable the result generation
grep -q '#define BBQ_VARIANT_PERF' "$SRC_FILE" || sed -i '1i#define BBQ_VARIANT_PERF' "$SRC_FILE"

echo "bn,bs,prod,cons,mode,throughput,tp_type,scenario" >"$CSV_FILE"

# generate queue config:
# `mode` is the real config of a queue
# `scenario` is the run-time observation
gen_queue_config() {
	local bn=$1
	local bs=$2
	local mode=$3
	local iter=$4
	local sync=$5

	local args=""
	case "$mode" in
	mpmc) args="--mp --mc" ;;
	mpsc) args="--mp --sc" ;;
	spmc) args="--sp --mc" ;;
	spsc) args="--sp --sc" ;;
	*)
		echo "Unknown mode $mode"
		exit 1
		;;
	esac

	./bbq_perf.sh --nolog --bn "$bn" --bs "$bs" $args --iteration "$iter" --sync "$sync"
	ninja -C .. bbq_variant_perf
}

# Function to run test and append result
test_variant() {
	local cons=$1
	local prod=$2
	local scenario=""

	if [ "$prod" -eq 1 ]; then
		scenario="sp"
	elif [ "$prod" -gt 1 ]; then
		scenario="mp"
	fi
	if [ "$cons" -eq 1 ]; then
		scenario+="sc"
	elif [ "$cons" -gt 1 ]; then
		scenario+="mc"
	fi

	../bbq_variant_perf --producers "$prod" --consumers "$cons"

	sed -i '$ s/$/,'"$scenario"'/' "$CSV_FILE"
	echo >>"$CSV_FILE"

}

declare -A tested_modes=()

perf() {

	local bn=$1
	local bs=$2
	local mode=$3
	local cons=$4
	local prod=$5
	local sync=$6

	local iter=1000000
	if [ "$sync" -gt 0 ]; then
		iter=$((bn * bs / prod))
	fi

	# Force mode based on prod/cons
	if [ "$prod" -gt 1 ]; then
		mode="mp${mode:2:2}"
	fi
	if [ "$cons" -gt 1 ]; then
		mode="${mode:0:2}mc"
	fi

	key="${bn}_${bs}_${mode}_${prod}_${cons}_${sync}"

	if [[ -z "${tested_modes[${key}]+set}" ]]; then
		tested_modes[${key}]=1
		gen_queue_config "$bn" "$bs" "$mode" "$iter" "$sync"
		test_variant "$cons" "$prod"
	fi
}

#FOR N..1,2,4,7,14,28,56,112

Ns=(1 2 4 7 14 28 56 112)
bns=(8 16 32)
bss=(64 100 1024 4096)
modes=(mpmc mpsc spmc spsc)

for M in "${Ns[@]}"; do
	for N in "${Ns[@]}"; do
		for bn in "${bns[@]}"; do
			for bs in "${bss[@]}"; do
				for mode in "${modes[@]}"; do
					#only producer tpt, no consumer's running
					perf "$bn" "$bs" "$mode" "$M" "$N" 1
					#only consumer tpt, produce before consuming
					perf "$bn" "$bs" "$mode" "$M" "$N" 2
					#real tpt, no synchronization
					perf "$bn" "$bs" "$mode" "$M" "$N" 0
				done
			done
		done
	done
done

echo "done"
