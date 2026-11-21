#!/bin/bash
# Random but reproducible input to the real windows (tests/test_ui.c, the
# /ui/monkey test) on a headless display; see tests/README.md.
#
#   tools/ui-monkey.sh [-b BUILD] [-n STEPS] [-s SEED] [-r RUNS]
#
# BUILD: a configured build folder (default build/; an ASan build finds
# more). STEPS per run (default 1000). SEED: replay one run (default: a new
# one each run, printed). RUNS: how many runs (default 1). Exits 1 when a
# run fails; its log is kept in BUILD/ui-monkey-SEED.log.
set -euo pipefail

main() {
    cd "$(dirname "$0")/.."
    local build=build steps=1000 seed="" runs=1 failed=0
    while getopts "b:n:s:r:h" opt; do
        case $opt in
        b) build=$OPTARG ;;
        n) steps=$OPTARG ;;
        s) seed=$OPTARG ;;
        r) runs=$OPTARG ;;
        *) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
        esac
    done
    ninja -C "$build" tests/test-ui >/dev/null
    export ASAN_OPTIONS=detect_leaks=0 # (GTK's one-time caches)
    for ((i = 0; i < runs; i++)); do
        local s=${seed:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
        local log=$build/ui-monkey-$s.log
        if SFE_MONKEY_STEPS=$steps SFE_MONKEY_SEED=$s "$build"/tests/test-ui -p /ui/monkey >"$log" 2>&1; then
            echo "seed $s: $steps actions, fine"
            rm -f "$log"
        else
            failed=1
            echo "seed $s: FAILED (log: $log)"
            grep -m1 -E "FATAL|ERROR|AddressSanitizer|runtime error" "$log" | sed 's/^/    /' || true
            grep -A6 '\*\*\* monkey' "$log" | sed 's/^/    /' || true
        fi
    done
    exit $failed
}

main "$@"
