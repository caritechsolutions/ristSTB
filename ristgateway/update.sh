#!/bin/bash
# RIST Gateway Update Script
# Updates and rebuilds the modified librist with rist22rist gateway tool
# Configuration files are preserved - NOT overwritten
#
# Usage:
#   GITHUB_TOKEN=xxx ./update.sh           # Set token via env
#   ./update.sh --token ghp_xxxxx          # Set token via arg
#   ./update.sh                            # Uses SSH or prompts

set -e

INSTALL_DIR="/opt/ristgateway"
BRANCH="${RIST_BRANCH:-claude/pid-selection-oob-lkWRy}"  # Override with RIST_BRANCH env var

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --token)
            GITHUB_TOKEN="$2"
            shift 2
            ;;
        --branch)
            BRANCH="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

# Repository URL - supports SSH, HTTPS with token, or plain HTTPS
if [ -n "$GITHUB_TOKEN" ]; then
    REPO_URL="https://${GITHUB_TOKEN}@github.com/caritechsolutions/ristSTB.git"
elif [ -n "$USE_SSH" ] || [ -f ~/.ssh/id_ed25519 ] || [ -f ~/.ssh/id_rsa ]; then
    REPO_URL="git@github.com:caritechsolutions/ristSTB.git"
else
    REPO_URL="https://github.com/caritechsolutions/ristSTB.git"
fi

echo "========================================"
echo "RIST Gateway Update"
echo "========================================"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo ./update.sh)"
    exit 1
fi

echo "Branch: $BRANCH"

# Ensure ffmpeg is installed (needed for preview feature)
if ! command -v ffmpeg &> /dev/null; then
    echo "Installing ffmpeg for preview feature..."
    apt-get update && apt-get install -y ffmpeg
fi

# Check if installation exists
if [ ! -d "$INSTALL_DIR/ristSTB" ]; then
    echo "Error: Installation not found at $INSTALL_DIR"
    echo "Please run install.sh first"
    exit 1
fi

# Get current version
cd "$INSTALL_DIR/ristSTB"

# Update remote URL if token provided
if [ -n "$GITHUB_TOKEN" ]; then
    git remote set-url origin "$REPO_URL"
fi

CURRENT_COMMIT=$(git rev-parse --short HEAD)
echo "Current version: $CURRENT_COMMIT"

# Pull latest changes
echo ""
echo "Fetching updates..."
git fetch origin

# Check if updates available
LOCAL=$(git rev-parse HEAD)
REMOTE=$(git rev-parse origin/$BRANCH)

if [ "$LOCAL" = "$REMOTE" ]; then
    echo "Already up to date."
    read -p "Force rebuild anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 0
    fi
fi

# Pull changes
echo ""
echo "Pulling latest changes..."
git checkout $BRANCH
git pull origin $BRANCH

NEW_COMMIT=$(git rev-parse --short HEAD)
echo "Updated to: $NEW_COMMIT"

# Show what changed
echo ""
echo "Changes:"
git log --oneline $CURRENT_COMMIT..$NEW_COMMIT 2>/dev/null || echo "  (new installation)"

# Stop services before updating
echo ""
echo "Stopping services..."

# Track which services were running so we can restart them
API_WAS_RUNNING=false
if systemctl is-active --quiet ristgateway-api.service 2>/dev/null; then
    API_WAS_RUNNING=true
    echo "Stopping Web API..."
    systemctl stop ristgateway-api.service
fi

# Stop running gateways and remember which ones
RUNNING_GATEWAYS=()
for service in /etc/systemd/system/ristgateway-gateway*.service; do
    if [ -f "$service" ]; then
        service_name=$(basename "$service")
        if systemctl is-active --quiet "$service_name" 2>/dev/null; then
            RUNNING_GATEWAYS+=("$service_name")
            echo "Stopping $service_name..."
            systemctl stop "$service_name"
        fi
    fi
done

# Rebuild librist
echo ""
echo "Rebuilding librist..."
cd "$INSTALL_DIR/ristSTB/librist"

# Clean previous build
if [ -d "build" ]; then
    rm -rf build
fi

# Configure with meson
meson setup build \
    --prefix=/usr/local \
    --buildtype=release \
    -Dbuiltin_cjson=false \
    -Dtest=false

# Build
ninja -C build

# Install
echo ""
echo "Installing..."
ninja -C build install

# Update library cache
ldconfig

# Rebuild ristpreview (preview transcoder)
echo ""
echo "Rebuilding ristpreview..."
gcc -O2 -Wall -pthread \
    "$INSTALL_DIR/ristSTB/ristgateway/ristpreview.c" \
    -o /usr/local/bin/ristpreview
chmod +x /usr/local/bin/ristpreview
echo "[OK] ristpreview updated"

# Verify installation
echo ""
echo "Verifying installation..."
RIST22RIST_BIN=""
if command -v rist22rist &> /dev/null; then
    RIST22RIST_BIN="rist22rist"
elif [ -f /usr/local/bin/rist22rist ]; then
    RIST22RIST_BIN="/usr/local/bin/rist22rist"
fi

if [ -n "$RIST22RIST_BIN" ]; then
    echo "[OK] rist22rist updated successfully"

    # Check if metrics support is enabled
    if $RIST22RIST_BIN --help 2>&1 | grep -q "metrics-http"; then
        echo "[OK] HTTP Metrics support enabled"
    else
        echo "[WARNING] HTTP Metrics support NOT enabled"
        echo "  Stats will fall back to journald parsing."
    fi
else
    echo "[ERROR] rist22rist not found!"
    exit 1
fi

# Update Web API
echo ""
echo "Updating Gateway Web API..."

VENV_DIR="$INSTALL_DIR/venv"

# Update Python dependencies if requirements changed
if [ -f "$INSTALL_DIR/ristSTB/ristgateway/requirements.txt" ]; then
    source "$VENV_DIR/bin/activate"
    pip install -r "$INSTALL_DIR/ristSTB/ristgateway/requirements.txt" --quiet
    deactivate
fi

# Update web files
cp -r "$INSTALL_DIR/ristSTB/ristgateway/web" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristgateway/gateway_api.py" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristgateway/stats_collector.sh" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/stats_collector.sh"

# Migrate from /tmp to /dev/shm (shared memory)
rm -rf /tmp/ristgateway-stats
mkdir -p /dev/shm/ristgateway-stats
mkdir -p /dev/shm/ristgateway-preview

# Create/update stats collector service (uses /dev/shm - RAM backed)
cat > /etc/systemd/system/ristgateway-stats.service << 'SVCEOF'
[Unit]
Description=RIST Gateway Stats Collector
After=network.target

[Service]
Type=simple
ExecStart=/opt/ristgateway/stats_collector.sh
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
SVCEOF
systemctl daemon-reload
systemctl enable ristgateway-stats.service

# NOTE: We do NOT copy gateway_config.yaml to preserve existing configuration
echo "[OK] Configuration preserved at /etc/ristgateway/gateway_config.yaml"

# Restart services that were running before update
echo ""
echo "Starting services..."

# Reload systemd to pick up any service file changes
systemctl daemon-reload

# Start stats collector (writes to /dev/shm - no disk I/O)
echo "Starting stats collector..."
systemctl start ristgateway-stats.service

# Start API if it was running
if [ "$API_WAS_RUNNING" = true ]; then
    echo "Starting Web API..."
    systemctl start ristgateway-api.service
fi

# Start gateways that were running
for service_name in "${RUNNING_GATEWAYS[@]}"; do
    echo "Starting $service_name..."
    systemctl start "$service_name"
done

echo ""
echo "========================================"
echo "Update complete!"
echo "========================================"
echo ""
echo "Updated from $CURRENT_COMMIT to $NEW_COMMIT"
echo ""
echo "Summary:"
echo "  - librist/rist22rist rebuilt"
echo "  - ristpreview rebuilt"
echo "  - Web UI updated"
echo "  - API updated"
echo "  - Stats use /dev/shm (RAM, no disk I/O)"
echo "  - Configuration preserved"
echo "  - Running gateways restarted"
echo ""
echo "Web Interface: http://$(hostname -I | awk '{print $1}')"
echo ""
