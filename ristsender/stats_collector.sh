#!/bin/bash
# RIST Sender Stats Collector
# Runs as a separate service, writes journal stats to shared memory
# The API reads these files instead of calling subprocess
# Using /dev/shm (tmpfs) - RAM backed, no disk I/O

STATS_DIR="/dev/shm/ristsender-stats"
mkdir -p "$STATS_DIR"

while true; do
    for service_file in /etc/systemd/system/ristsender-*.service; do
        if [ -f "$service_file" ]; then
            name=$(basename "$service_file" .service)
            # Skip non-channel services (api, stats)
            case "$name" in
                ristsender-api|ristsender-stats) continue ;;
            esac
            # Write last 50 lines - need enough for all output peer stats
            journalctl -u "$name" -n 50 --no-pager -o cat > "${STATS_DIR}/${name}.txt" 2>/dev/null
        fi
    done
    sleep 2
done
