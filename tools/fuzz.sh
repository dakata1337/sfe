#!/bin/bash
# Fuzzes sfe's parsers with libFuzzer, AddressSanitizer and UBSan.
#
#   tools/fuzz.sh [-t SECONDS] [-j JOBS] [-p] [TARGET...]
#
# TARGET: config cmdline fuzzy keymap filefilter rename portalreq bookmarks
# trashinfo thumbnailer portalconf (default: all). SECONDS per target
# (default 60). JOBS fuzzer processes per target (default 1).
# -p runs the targets at the same time, each pinned to its own cores:
# spread over every L3 cache (CCD) and over physical cores before their
# SMT siblings, as the scheduler kept them all on one CCD.
#
# Builds build-fuzz/ with clang. Each target's corpus grows in
# build-fuzz/corpus/TARGET, seeded from tests/fuzz/corpus/TARGET. A crash or
# a failed invariant is saved as tests/fuzz/crashes/TARGET/crash-* (reproduce:
# build-fuzz/tests/fuzz/fuzz-TARGET FILE). Exits 1 if any target found one.
set -euo pipefail

# CPUs in the order to hand them out: alternating between L3 caches, first
# one thread of every core, then the SMT siblings.
cpu_order() {
    local first=() second=() groups=() cpu c l3 i
    for c in /sys/devices/system/cpu/cpu[0-9]*; do
        cpu=${c##*cpu}
        [ -r "$c/cache/index3/shared_cpu_list" ] && l3=$(cat "$c/cache/index3/shared_cpu_list") || l3=all
        [[ " ${groups[*]-} " == *" $l3 "* ]] || groups+=("$l3")
        if [ "$(cut -d, -f1 <"$c/topology/thread_siblings_list" | cut -d- -f1)" = "$cpu" ]; then
            first+=("$l3:$cpu")
        else
            second+=("$l3:$cpu")
        fi
    done
    for list in first second; do
        local -n cpus=$list
        local per=()
        for l3 in "${groups[@]}"; do # one queue per L3 cache, in CPU order
            per+=("$(printf '%s\n' "${cpus[@]-}" | grep "^$l3:" | cut -d: -f2 | sort -n | tr '\n' ' ')")
        done
        for ((i = 0; ; i++)) do # round robin over the queues
            local any=0
            for q in "${per[@]}"; do
                read -ra qa <<<"$q"
                [ "$i" -lt "${#qa[@]}" ] && { echo "${qa[$i]}"; any=1; }
            done
            [ $any = 1 ] || break
        done
    done
}

# Runs one target; prints its summary. $2: CPU list for taskset, or empty.
run_target() {
    local t=$1 cpus=$2
    local corpus=build-fuzz/corpus/$t crashes=tests/fuzz/crashes/$t work=build-fuzz/work/$t
    mkdir -p "$corpus" "$crashes" "$work"
    local parallel=() pin=()
    [ "$jobs" -gt 1 ] && parallel=(-jobs="$jobs" -workers="$jobs")
    [ -n "$cpus" ] && pin=(taskset -c "$cpus")
    local out="==> $t (${seconds}s${cpus:+, CPUs $cpus})"
    # libFuzzer writes each job's log as fuzz-N.log in the current folder:
    # a folder per target, so targets running at the same time do not mix.
    if ! (cd "$work" && "${pin[@]}" "$root"/build-fuzz/tests/fuzz/fuzz-"$t" "$root/$corpus" "$root"/tests/fuzz/corpus/"$t" \
        -max_total_time="$seconds" "${parallel[@]}" -print_final_stats=1 \
        -artifact_prefix="$root/$crashes/") >build-fuzz/fuzz-"$t".log 2>&1; then
        out+=$'\n'"    found a problem: $(ls -t "$crashes" | head -1) (log: build-fuzz/fuzz-$t.log)"
        out+=$'\n'"$(grep -m3 -E "ERROR: AddressSanitizer|runtime error|invariant failed|SUMMARY" build-fuzz/fuzz-"$t".log | sed 's/^/    /')"
        echo "$out"
        return 1
    fi
    out+=$'\n'"$(grep -h "^stat::number_of_executed_units" build-fuzz/fuzz-"$t".log "$work"/fuzz-*.log 2>/dev/null | sed 's/^/    /')"
    out+=$'\n'"    nothing found; corpus: $(ls "$corpus" | wc -l) inputs"
    echo "$out"
}

main() {
    cd "$(dirname "$0")/.."
    root=$PWD
    seconds=60
    jobs=1
    local spread=0
    while getopts "t:j:ph" opt; do
        case $opt in
        t) seconds=$OPTARG ;;
        j) jobs=$OPTARG ;;
        p) spread=1 ;;
        *) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
        esac
    done
    shift $((OPTIND - 1))
    local targets=("$@")
    [ ${#targets[@]} -gt 0 ] || targets=(config cmdline fuzzy keymap filefilter rename portalreq bookmarks trashinfo thumbnailer portalconf)

    if [ ! -d build-fuzz ]; then
        CC=clang meson setup build-fuzz -Dfuzzing=true -Dbuildtype=debugoptimized \
            -Db_sanitize=address,undefined -Db_lundef=false
    fi
    ninja -C build-fuzz $(printf 'tests/fuzz/fuzz-%s ' "${targets[@]}")

    # UBSan reports stop the run like a crash.
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    export ASAN_OPTIONS=detect_leaks=1

    local found=0 t
    if [ $spread = 0 ]; then
        for t in "${targets[@]}"; do
            run_target "$t" "" || found=1
        done
        exit $found
    fi

    local order=() pids=() next=0 k
    mapfile -t order < <(cpu_order)
    if [ $((${#targets[@]} * jobs)) -gt ${#order[@]} ]; then
        echo "note: ${#targets[@]} targets x $jobs jobs is more than ${#order[@]} CPUs; some share" >&2
    fi
    mkdir -p build-fuzz/status
    for t in "${targets[@]}"; do
        local cpus=()
        for ((k = 0; k < jobs; k++)); do
            cpus+=("${order[$((next % ${#order[@]}))]}")
            next=$((next + 1))
        done
        cpus=$(IFS=,; echo "${cpus[*]}")
        (run_target "$t" "$cpus" >build-fuzz/status/"$t" && echo ok >>build-fuzz/status/"$t") &
        pids+=($!)
    done
    wait "${pids[@]}" || true
    for t in "${targets[@]}"; do
        grep -v '^ok$' build-fuzz/status/"$t"
        tail -1 build-fuzz/status/"$t" | grep -qx ok || found=1
    done
    exit $found
}

main "$@"
