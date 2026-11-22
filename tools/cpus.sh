# Sourced by tools/fuzz.sh and tools/ui-monkey.sh: cpu_order prints the
# CPUs to pin parallel jobs to, one per line.

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
