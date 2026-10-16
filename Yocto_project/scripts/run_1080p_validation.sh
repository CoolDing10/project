#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
sender="$project_dir/build/adaptive_sender"
receiver="$project_dir/build/adaptive_receiver"
duration=${2:-10}
port=${3:-18900}
output_dir=${1:-/tmp/adaptive-1080p-$(date +%Y%m%d-%H%M%S)}

case "$duration" in *[!0-9]*|'') echo "duration must be an integer" >&2; exit 2;; esac
case "$port" in *[!0-9]*|'') echo "port must be an integer" >&2; exit 2;; esac
[ "$duration" -ge 3 ] || { echo "duration must be at least 3 seconds" >&2; exit 2; }
[ "$port" -ge 1 ] && [ "$port" -le 65535 ] || { echo "invalid port" >&2; exit 2; }
[ -x "$sender" ] && [ -x "$receiver" ] || {
    echo "build/adaptive_sender and build/adaptive_receiver are required" >&2
    exit 1
}
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

"$receiver" --port "$port" --record "$output_dir/received.ts" \
    >"$output_dir/receiver.log" 2>&1 &
receiver_pid=$!
sleep 1

set +e
timeout "${duration}s" "$sender" --host 127.0.0.1 --port "$port" --synthetic \
    --width 1920 --height 1080 --fps 25 --mode mpc \
    --min-bitrate 800000 --max-bitrate 6000000 --initial-bitrate 2500000 \
    --gop 50 --metrics "$output_dir/mpc.csv" >"$output_dir/sender.log" 2>&1
sender_status=$?
set -e
[ "$sender_status" -eq 0 ] || [ "$sender_status" -eq 124 ] || {
    echo "sender failed with status $sender_status" >&2
    exit "$sender_status"
}
cleanup
receiver_pid=

minimum_duration=$((duration - 2))
"$script_dir/summarize_metrics.py" --min-duration-s "$minimum_duration" \
    --max-e2e-p95-ms 130 --max-control-p95-us 20000 --max-stalls-per-min 0 \
    "$output_dir/mpc.csv"

if command -v gst-discoverer-1.0 >/dev/null 2>&1; then
    media_info=$(gst-discoverer-1.0 "$output_dir/received.ts")
    echo "$media_info" | grep -q 'Width: 1920' || { echo "recording is not 1920 wide" >&2; exit 1; }
    echo "$media_info" | grep -q 'Height: 1080' || { echo "recording is not 1080 high" >&2; exit 1; }
fi
echo "1080P validation passed: $output_dir"

