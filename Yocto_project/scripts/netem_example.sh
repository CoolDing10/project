#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 apply|clear INTERFACE [RATE] [DELAY] [LOSS]" >&2
    echo "Example: sudo $0 apply eth0 4mbit 40ms 2%" >&2
    exit 2
fi

action=$1
interface=$2
case "$action" in
    apply)
        rate=${3:-4mbit}
        delay=${4:-40ms}
        loss=${5:-2%}
        tc qdisc replace dev "$interface" root netem rate "$rate" delay "$delay" loss "$loss"
        tc qdisc show dev "$interface"
        ;;
    clear)
        tc qdisc del dev "$interface" root 2>/dev/null || true
        ;;
    *)
        echo "action must be apply or clear" >&2
        exit 2
        ;;
esac
