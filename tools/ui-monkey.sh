#!/bin/bash
# Random but reproducible input to the real windows (tests/test_ui.c, the
# /ui/monkey test) on a headless display; see tests/README.md.
#
#   tools/ui-monkey.sh [-b BUILD] [-n STEPS] [-s SEED] [-r RUNS] [-j JOBS]
#
# BUILD: a configured build folder (default build/; an ASan build finds
# more). STEPS per run (default 1000). SEED: replay one run (default: a new
# one each run, printed). RUNS: how many runs (default 1). JOBS: runs at the
# same time, each pinned to its own core across all CCDs (default 1).
# Exits 1 when a run fails; its log is kept in BUILD/ui-monkey-SEED.log.
set -euo pipefail

source "$(dirname "$0")/cpus.sh"

# One run; prints one line (and the failure's details).
one_run() {
    local s=$1 cpu=$2
    local log=$build/ui-monkey-$s.log pin=()
    [ -n "$cpu" ] && pin=(taskset -c "$cpu")
    if SFE_MONKEY_STEPS=$steps SFE_MONKEY_SEED=$s "${pin[@]}" "$build"/tests/test-ui -p /ui/monkey >"$log" 2>&1; then
        echo "seed $s: $steps actions, fine"
        rm -f "$log"
        return 0
    fi
    echo "seed $s: FAILED (log: $log)"
    grep -m1 -E "FATAL|ERROR|AddressSanitizer|runtime error|SKIP" "$log" | sed 's/^/    /' || true
    grep -A6 '\*\*\* monkey' "$log" | sed 's/^/    /' || true
    return 1
}

main() {
    cd "$(dirname "$0")/.."
    build=build steps=1000
    local seed="" runs=1 jobs=1 failed=0
    while getopts "b:n:s:r:j:h" opt; do
        case $opt in
        b) build=$OPTARG ;;
        n) steps=$OPTARG ;;
        s) seed=$OPTARG ;;
        r) runs=$OPTARG ;;
        j) jobs=$OPTARG ;;
        *) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
        esac
    done
    ninja -C "$build" tests/test-ui >/dev/null
    export ASAN_OPTIONS=detect_leaks=0 # (GTK's one-time caches)

    local seeds=()
    for ((i = 0; i < runs; i++)); do
        seeds+=("${seed:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}")
    done
    if [ "$jobs" -le 1 ]; then
        for s in "${seeds[@]}"; do
            one_run "$s" "" || failed=1
        done
        exit $failed
    fi

    # JOBS slots, each on its own core; the runs are dealt out to them.
    local order=() pids=() slot
    mapfile -t order < <(cpu_order)
    mkdir -p "$build/ui-monkey-status"
    for ((slot = 0; slot < jobs; slot++)); do
        (
            local mine=0
            for ((i = slot; i < ${#seeds[@]}; i += jobs)); do
                one_run "${seeds[$i]}" "${order[$((slot % ${#order[@]}))]}" || mine=1
            done
            exit $mine
        ) >"$build/ui-monkey-status/$slot" 2>&1 &
        pids+=($!)
    done
    for ((slot = 0; slot < jobs; slot++)); do
        wait "${pids[$slot]}" || failed=1
        cat "$build/ui-monkey-status/$slot"
    done
    rm -rf "$build/ui-monkey-status"
    exit $failed
}

main "$@"
