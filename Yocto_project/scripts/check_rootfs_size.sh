#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ ! -d "$1" ]; then
    echo "Usage: $0 ROOTFS_DIRECTORY" >&2
    exit 2
fi

size_kib=$(du -sk --apparent-size "$1" | awk '{print $1}')
limit_kib=102400
echo "rootfs size: ${size_kib} KiB (limit: ${limit_kib} KiB)"
[ "$size_kib" -lt "$limit_kib" ]
