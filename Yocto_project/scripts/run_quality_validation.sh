#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
sender="$project_dir/build/adaptive_sender"
receiver="$project_dir/build/adaptive_receiver"
duration=${2:-10}
base_port=${3:-18920}
output_dir=${1:-/tmp/adaptive-quality-$(date +%Y%m%d-%H%M%S)}

case "$duration" in *[!0-9]*|'') echo "duration must be an integer" >&2; exit 2;; esac
case "$base_port" in *[!0-9]*|'') echo "port must be an integer" >&2; exit 2;; esac
[ "$duration" -ge 3 ] || { echo "duration must be at least 3 seconds" >&2; exit 2; }
[ "$base_port" -ge 1 ] && [ "$base_port" -le 65533 ] || { echo "invalid port" >&2; exit 2; }
[ -x "$sender" ] && [ -x "$receiver" ] || {
    echo "build/adaptive_sender and build/adaptive_receiver are required" >&2
    exit 1
}
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg executable is required" >&2; exit 1; }
[ ! -e "$output_dir" ] || { echo "output directory already exists: $output_dir" >&2; exit 1; }
mkdir -p "$output_dir"

receiver_pid=
cleanup() {
    if [ -n "$receiver_pid" ] && kill -0 "$receiver_pid" 2>/dev/null; then
        kill -TERM "$receiver_pid" 2>/dev/null || true
        wait "$receiver_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

run_case() {
    name=$1
    port=$2
    mode=$3
    minimum=$4
    maximum=$5
    initial=$6
    "$receiver" --port "$port" --record "$output_dir/$name.ts" \
        >"$output_dir/$name-receiver.log" 2>&1 &
    receiver_pid=$!
    sleep 1
    set +e
    timeout "${duration}s" "$sender" --host 127.0.0.1 --port "$port" --synthetic \
        --width 1920 --height 1080 --fps 25 --mode "$mode" \
        --min-bitrate "$minimum" --max-bitrate "$maximum" \
        --initial-bitrate "$initial" --gop 50 --metrics "$output_dir/$name.csv" \
        >"$output_dir/$name-sender.log" 2>&1
    sender_status=$?
    set -e
    [ "$sender_status" -eq 0 ] || [ "$sender_status" -eq 124 ] || return "$sender_status"
    cleanup
    receiver_pid=
}

# All three runs use the deterministic synthetic source from frame zero.
# The 12 Mbps fixed run is the high-quality reference for the two candidates.
run_case reference "$base_port" fixed 12000000 12000000 12000000
run_case fixed "$((base_port + 1))" fixed 2500000 2500000 2500000
run_case mpc "$((base_port + 2))" mpc 800000 6000000 2500000

"$script_dir/summarize_metrics.py" "$output_dir/fixed.csv" "$output_dir/mpc.csv"
"$script_dir/compare_quality.py" "$output_dir/reference.ts" \
    "$output_dir/fixed.ts" "$output_dir/mpc.ts" --required-improvement 35
echo "1080P quality validation passed: $output_dir"

