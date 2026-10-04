#!/usr/bin/env bash
# N game servers on loopback, netem on lo, load_generator verdict as the exit code
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
bin_dir=${BIN_DIR:-$script_dir/../../build}
impair=${IMPAIR:-$script_dir/../netem/impair.sh}
[[ -x $impair ]] || impair=$script_dir/impair.sh

profile=degraded
servers=6
clients=""
warmup=5
duration=60
drain=4
redundancy=4
aim=mixed
fire_interval=500
base_port=7777
base_metrics_port=9090
report_dir=${REPORT_DIR:-/tmp/bench-reports}
label=""
delay=""
jitter=""
loss=""
dev=lo

usage() {
    cat >&2 <<EOF
usage: run_benchmark.sh [--profile baseline|degraded|spec] [--servers N] [--clients N]
                        [--duration S] [--warmup S] [--redundancy 1-4] [--aim head|torso|mixed]
                        [--fire-interval MS] [--delay MS --jitter MS --loss PCT] [--label NAME] [--report-dir DIR]
profiles (UDP only, per direction on lo):
  baseline  no impairment
  degraded  60 ms +/- 15 ms normal, 5% loss   -> ~120 ms RTT
  spec      25 ms +/- 5 ms normal, 5% loss    -> ~50 ms RTT
EOF
    exit 64
}

while [[ $# -gt 0 ]]; do
    [[ $# -ge 2 ]] || usage
    case "$1" in
        --profile) profile=$2 ;;
        --servers) servers=$2 ;;
        --clients) clients=$2 ;;
        --duration) duration=$2 ;;
        --warmup) warmup=$2 ;;
        --redundancy) redundancy=$2 ;;
        --aim) aim=$2 ;;
        --fire-interval) fire_interval=$2 ;;
        --delay) delay=$2 ;;
        --jitter) jitter=$2 ;;
        --loss) loss=$2 ;;
        --label) label=$2 ;;
        --report-dir) report_dir=$2 ;;
        *) usage ;;
    esac
    shift 2
done

case "$profile" in
    baseline) : "${delay:=0}" "${jitter:=0}" "${loss:=0}" ;;
    degraded) : "${delay:=60}" "${jitter:=15}" "${loss:=5}" ;;
    spec)     : "${delay:=25}" "${jitter:=5}" "${loss:=5}" ;;
    *) usage ;;
esac
label=${label:-$profile-r$redundancy}
clients=${clients:-$((servers * 10))}

for value in "$servers" "$clients" "$duration" "$fire_interval"; do
    [[ $value =~ ^[1-9][0-9]*$ ]] || usage
done
[[ $warmup =~ ^[0-9]+$ && $redundancy =~ ^[1-4]$ && $label =~ ^[A-Za-z0-9._-]+$ ]] || usage

mkdir -p "$report_dir"
server_pids=()
loadgen_pid=""
netem_applied=0

cleanup() {
    if [[ $netem_applied == 1 ]]; then
        "$impair" clear "$dev"
        netem_applied=0
    fi
    if [[ -n $loadgen_pid ]]; then
        kill -TERM "$loadgen_pid" 2>/dev/null || true
        wait "$loadgen_pid" 2>/dev/null || true
        loadgen_pid=""
    fi
    for pid in "${server_pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${server_pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    server_pids=()
}
trap cleanup EXIT
# As container PID 1 the default signal actions are ignored; exit explicitly so the EXIT trap tears down
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_port() {
    local port=$1
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "metrics endpoint 127.0.0.1:$port never came up" >&2
    return 1
}

report_field() {
    awk -F': ' -v key="$1" '$1 ~ key {print $2; exit}' "$2" 2>/dev/null || true
}

loadgen_args=()
for ((i = 0; i < servers; i++)); do
    port=$((base_port + i))
    metrics_port=$((base_metrics_port + i))
    "$bin_dir/game_server" --port "$port" --metrics-port "$metrics_port" > "$report_dir/$label-server-$i.log" 2>&1 &
    server_pids+=($!)
    loadgen_args+=(--server "127.0.0.1:$port" --metrics "127.0.0.1:$metrics_port")
done
for ((i = 0; i < servers; i++)); do
    wait_for_port $((base_metrics_port + i))
done

if [[ $delay != 0 || $jitter != 0 || $loss != 0 ]]; then
    "$impair" apply "$dev" "$delay" "$jitter" "$loss"
    netem_applied=1
fi

# Background plus wait keeps the shell responsive to signals during the run
"$bin_dir/load_generator" "${loadgen_args[@]}" \
    --clients "$clients" --warmup "$warmup" --duration "$duration" --drain "$drain" \
    --redundancy "$redundancy" --aim "$aim" --fire-interval "$fire_interval" \
    --label "$label" --report "$report_dir/$label.json" &
loadgen_pid=$!
verdict=0
wait "$loadgen_pid" || verdict=$?
loadgen_pid=""

cleanup

echo
echo " Server execution reports ($label)"
for ((i = 0; i < servers; i++)); do
    log="$report_dir/$label-server-$i.log"
    peak=$(report_field 'Peak Tick Exec Time' "$log")
    jitter_peak=$(report_field 'Peak Tick Jitter' "$log")
    overruns=$(report_field 'Total Tick Overruns' "$log")
    echo "   server $i: peak exec ${peak:-n/a}, peak start jitter ${jitter_peak:-n/a}, deadline misses ${overruns:-n/a}"
done
echo " Reports: $report_dir"
exit "$verdict"
