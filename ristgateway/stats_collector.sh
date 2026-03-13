#!/bin/bash
# RIST Gateway Stats Collector
# Runs as a separate service, writes journal stats to shared memory
# The API reads these files instead of calling subprocess
# Using /dev/shm (tmpfs) - RAM backed, no disk I/O

STATS_DIR="/dev/shm/ristgateway-stats"
mkdir -p "$STATS_DIR"

while true; do
    for service_file in /etc/systemd/system/ristgateway-*.service; do
        if [ -f "$service_file" ]; then
            name=$(basename "$service_file" .service)
            # Skip non-gateway services (api, stats)
            case "$name" in
                ristgateway-api|ristgateway-stats) continue ;;
            esac
            # Write last 50 lines - need enough for receiver + all sender-stats (one per output peer)
            journalctl -u "$name" -n 50 --no-pager -o cat > "${STATS_DIR}/${name}.txt" 2>/dev/null
        fi
    done
    sleep 2
done
