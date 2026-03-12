# RIST Gateway Development Notes

## Deployment to ristgateway1

Use curl with the GitHub raw URL and token to deploy files directly:

```bash
# Download and deploy gateway_api.py
curl -o /opt/ristgateway/gateway_api.py "https://raw.githubusercontent.com/caritechsolutions/ristSTB/claude/pid-selection-oob-lkWRy/ristgateway/gateway_api.py?token=TOKEN_HERE&$(date +%s)"

# Restart API after deploy
systemctl restart ristgateway-api
```

**Important:** Always add `&$(date +%s)` as cache buster at the end of the URL.

## GitHub Token

Token for raw file access: `[PASTE TOKEN HERE]`

## Testing Stats Endpoint

```bash
# Login
curl -s -c /tmp/cookies.txt -X POST http://localhost/api/login -H 'Content-Type: application/json' -d '{"password":"admin"}'

# Test stats
timeout 5 curl -s -b /tmp/cookies.txt http://localhost/api/gateways/gateway1/stats | python3 -m json.tool
```

## Stats Architecture

- Stats collector service: `ristgateway-stats.service`
- Stats files: `/tmp/ristgateway-stats/ristgateway-{gateway_id}.txt`
- API reads from stats files (no subprocess calls in request handlers)
- Service status checked via cgroup filesystem (`/sys/fs/cgroup/system.slice/`)
