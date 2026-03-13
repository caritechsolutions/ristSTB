# RIST Gateway Development Notes

## Deployment to ristgateway1

### Full Update (Preferred)

Run the full update script to update all components (librist, API, web UI, stats collector):

```bash
# Download and run update script
curl -fsSL -H "Authorization: token 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" \
  "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/update.sh?$(date +%s)" \
  -o /tmp/update.sh && chmod +x /tmp/update.sh && \
  GITHUB_TOKEN=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 /tmp/update.sh
```

### Quick API-Only Update

For fast iterations when only gateway_api.py changed:

```bash
curl -fsSL -H "Authorization: token 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" \
  "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/gateway_api.py?$(date +%s)" \
  -o /opt/ristgateway/gateway_api.py && systemctl restart ristgateway-api
```

### Quick Web UI Update

For web file changes only:

```bash
for f in index.html login.html style.css app.js; do
  curl -fsSL -H "Authorization: token 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" \
    "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/web/$f?$(date +%s)" \
    -o /opt/ristgateway/web/$f
done
```

**Important:** Always add `?$(date +%s)` as cache buster at the end of URLs.

## GitHub Token

Token for raw file access: `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`

## Testing Stats Endpoint

```bash
# Login
curl -s -c /tmp/cookies.txt -X POST http://localhost/api/login -H 'Content-Type: application/json' -d '{"password":"admin"}'

# Test stats
timeout 5 curl -s -b /tmp/cookies.txt http://localhost/api/gateways/gateway1/stats | python3 -m json.tool
```

## Stats Architecture

**In-Memory Stats (no disk I/O):**
- Background `stats_poller_thread` runs every 2 seconds in the API
- Polls journalctl for RIST stats, stores in `gateway_stats` dict
- API endpoint reads from memory (instant, scales to 1000s of channels)
- When gateway stops, stats are zeroed out (no stale data)

**Resource tracking:**
- Service status via cgroup filesystem (`/sys/fs/cgroup/system.slice/`)
- CPU percentage from `cpu.stat` usage_usec deltas
- Memory from `memory.current` in cgroup

**Note:** Old `ristgateway-stats.service` and `/tmp/ristgateway-stats/` files are no longer used.
