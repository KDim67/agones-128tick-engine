#!/usr/bin/env bash
# netem impairment for one interface; on lo every packet crosses it once per direction
set -euo pipefail

usage() {
    echo "usage: impair.sh apply <dev> <delay_ms> <jitter_ms> <loss_pct> [queue_limit_pkts]" >&2
    echo "       impair.sh clear <dev>" >&2
    echo "       impair.sh show <dev>" >&2
    exit 64
}

is_number() {
    [[ $1 =~ ^[0-9]+([.][0-9]+)?$ ]]
}

[[ $# -ge 2 ]] || usage
action=$1
dev=$2

case "$action" in
    apply)
        [[ $# -ge 5 ]] || usage
        delay=$3
        jitter=$4
        loss=$5
        limit=${6:-100000}
        for value in "$delay" "$jitter" "$loss" "$limit"; do
            is_number "$value" || { echo "impair.sh: '$value' is not a number" >&2; exit 64; }
        done

        args=(delay "${delay}ms")
        # Normal jitter reorders packets like a real path; tc rejects a distribution without a jitter term
        if [[ $jitter != 0 ]]; then
            args+=("${jitter}ms" distribution normal)
        fi
        args+=(loss "${loss}%")
        # 60 clients at 128Hz keep ~1k packets in flight at 60 ms; the 1000-packet default would tail-drop
        args+=(limit "$limit")

        # Only UDP game traffic is impaired; TCP metrics scrapes take the clean band
        tc qdisc del dev "$dev" root 2>/dev/null || true
        tc qdisc add dev "$dev" root handle 1: prio bands 2 priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
        tc qdisc add dev "$dev" parent 1:2 handle 20: netem "${args[@]}"
        tc filter add dev "$dev" parent 1: protocol ip prio 1 u32 match ip protocol 17 0xff flowid 1:2
        echo "netem on $dev (udp only): ${args[*]}"
        ;;
    clear)
        tc qdisc del dev "$dev" root 2>/dev/null || true
        ;;
    show)
        tc -s qdisc show dev "$dev"
        tc filter show dev "$dev"
        ;;
    *)
        usage
        ;;
esac
