#!/bin/bash
# Fuzzes sfe's parsers with libFuzzer, AddressSanitizer and UBSan.
#
#   tools/fuzz.sh [-t SECONDS] [-j JOBS] [TARGET...]
#
# TARGET: config cmdline fuzzy keymap filefilter rename portalreq (default:
# all, one after the other). SECONDS per target (default 60). JOBS runs that
# many fuzzer processes per target (default 1).
#
# Builds build-fuzz/ with clang. Each target's corpus grows in
# build-fuzz/corpus/TARGET, seeded from fuzz/corpus/TARGET. A crash or a
# failed invariant is saved as fuzz/crashes/TARGET/crash-* (reproduce:
# build-fuzz/fuzz/fuzz-TARGET FILE). Exits 1 if any target found one.
set -euo pipefail

cd "$(dirname "$0")/.."
seconds=60
jobs=1
while getopts "t:j:h" opt; do
    case $opt in
    t) seconds=$OPTARG ;;
    j) jobs=$OPTARG ;;
    *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(config cmdline fuzzy keymap filefilter rename portalreq)

if [ ! -d build-fuzz ]; then
    CC=clang meson setup build-fuzz -Dfuzzing=true -Dbuildtype=debugoptimized \
        -Db_sanitize=address,undefined -Db_lundef=false
fi
ninja -C build-fuzz $(printf 'fuzz/fuzz-%s ' "${targets[@]}")

# UBSan reports stop the run like a crash.
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export ASAN_OPTIONS=detect_leaks=1

found=0
for t in "${targets[@]}"; do
    corpus=build-fuzz/corpus/$t
    crashes=fuzz/crashes/$t
    mkdir -p "$corpus" "$crashes"
    echo "==> $t (${seconds}s)"
    if ! build-fuzz/fuzz/fuzz-"$t" "$corpus" fuzz/corpus/"$t" \
        -max_total_time="$seconds" -jobs="$jobs" -workers="$jobs" -print_final_stats=1 \
        -artifact_prefix="$crashes/" >build-fuzz/fuzz-"$t".log 2>&1; then
        found=1
        echo "    found a problem: $(ls -t "$crashes" | head -1) (log: build-fuzz/fuzz-$t.log)"
        grep -m3 -E "ERROR: AddressSanitizer|runtime error|invariant failed|SUMMARY" build-fuzz/fuzz-"$t".log | sed 's/^/    /' || true
    else
        grep -E "^stat::number_of_executed_units|^cov:" build-fuzz/fuzz-"$t".log | tail -1 | sed 's/^/    /' || true
        echo "    nothing found; corpus: $(ls "$corpus" | wc -l) inputs"
    fi
done
exit $found
