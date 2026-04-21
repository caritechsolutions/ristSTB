# RIST Gateway Development Notes

## IMPORTANT: Deployment via Scripts Only

**All deployment must happen via `install.sh` or `update.sh`.** These scripts handle everything:
- Building and installing librist/rist22rist
- Setting up the API service
- Setting up the stats collector service
- Creating shared memory directories
- Managing systemd services

**Never manually deploy individual files** - always use the scripts so a clean server install produces a complete working system.

## Fresh Install (Clean Server)

```bash
curl -fsSL -H "Authorization: token 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" \
  "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/install.sh?$(date +%s)" \
  -o /tmp/install.sh && chmod +x /tmp/install.sh && \
  GITHUB_TOKEN=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 /tmp/install.sh
```

## Update Existing Installation

```bash
curl -fsSL -H "Authorization: token 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" \
  "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/update.sh?$(date +%s)" \
  -o /tmp/update.sh && chmod +x /tmp/update.sh && \
  GITHUB_TOKEN=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 /tmp/update.sh
```

## Quick Iterations (Development Only)

For fast development iterations when only specific files changed:

```bash
# API only
curl -fsSL -H "Authorization: token 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" \
  "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/gateway_api.py?$(date +%s)" \
  -o /opt/ristgateway/gateway_api.py && systemctl restart ristgateway-api

# Stats collector only
curl -fsSL -H "Authorization: token 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" \
  "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/stats_collector.sh?$(date +%s)" \
  -o /opt/ristgateway/stats_collector.sh && systemctl restart ristgateway-stats
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

**Shared Memory Stats (no disk I/O):**
- Stats collector service (`ristgateway-stats.service`) writes to `/dev/shm/ristgateway-stats/`
- `/dev/shm` is tmpfs (RAM-backed) - no disk writes
- API reads from shared memory files (fast, scales to 1000s of channels)
- Simple, reliable architecture

**Resource tracking:**
- Service status via cgroup filesystem (`/sys/fs/cgroup/system.slice/`)
- CPU percentage from `cpu.stat` usage_usec deltas
- Memory from `memory.current` in cgroup

## Services

- `ristgateway-api.service` - Web API (port 80)
- `ristgateway-stats.service` - Stats collector (writes to /dev/shm)
- `ristgateway-{gateway_id}.service` - Individual gateway instances
