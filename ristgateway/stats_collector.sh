#!/bin/bash
# RIST Gateway Stats Collector
# Runs as a separate service, writes journal stats to temp files
# The API reads these files instead of calling subprocess

STATS_DIR="/tmp/ristgateway-stats"
mkdir -p "$STATS_DIR"

while true; do
    for service_file in /etc/systemd/system/ristgateway-gateway*.service; do
        if [ -f "$service_file" ]; then
            name=$(basename "$service_file" .service)
            # Write last 20 lines of journal to temp file
            journalctl -u "$name" -n 20 --no-pager -o cat > "${STATS_DIR}/${name}.txt" 2>/dev/null
        fi
    done
    sleep 2
done
