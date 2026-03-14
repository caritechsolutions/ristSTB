#!/bin/bash
# RIST Sender Update Script
# Updates the RIST Sender web management interface
# Configuration files are preserved - NOT overwritten
#
# Usage:
#   GITHUB_TOKEN=xxx ./update.sh           # Set token via env
#   ./update.sh --token ghp_xxxxx          # Set token via arg
#   ./update.sh                            # Uses SSH or prompts

set -e

INSTALL_DIR="/opt/ristsender"
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
echo "RIST Sender Update"
echo "========================================"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo ./update.sh)"
    exit 1
fi

echo "Branch: $BRANCH"

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
    read -p "Force update anyway? (y/N) " -n 1 -r
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
if systemctl is-active --quiet ristsender-api.service 2>/dev/null; then
    API_WAS_RUNNING=true
    echo "Stopping Web API..."
    systemctl stop ristsender-api.service
fi

# Stop running sender channels and remember which ones
RUNNING_CHANNELS=()
for service_file in /etc/systemd/system/ristsender-channel*.service; do
    if [ -f "$service_file" ]; then
        service_name=$(basename "$service_file" .service)
        if systemctl is-active --quiet "$service_name" 2>/dev/null; then
            RUNNING_CHANNELS+=("$service_name")
            echo "Stopping $service_name..."
            systemctl stop "$service_name"
        fi
    fi
done

# Build ristpreview if not present or source changed
echo ""
echo "Checking ristpreview..."
if [ -f "$INSTALL_DIR/ristSTB/ristgateway/ristpreview.c" ]; then
    # Rebuild if source is newer or binary doesn't exist
    if [ ! -f /usr/local/bin/ristpreview ] || \
       [ "$INSTALL_DIR/ristSTB/ristgateway/ristpreview.c" -nt /usr/local/bin/ristpreview ]; then
        echo "Building ristpreview..."
        gcc -O2 -Wall -o /usr/local/bin/ristpreview \
            "$INSTALL_DIR/ristSTB/ristgateway/ristpreview.c" \
            -lpthread
        chmod +x /usr/local/bin/ristpreview
        echo "[OK] ristpreview updated"
    else
        echo "[OK] ristpreview already up to date"
    fi
fi

# Create preview shared memory directory
mkdir -p /dev/shm/ristsender-preview

# Update Web API
echo ""
echo "Updating Sender Web API..."

VENV_DIR="$INSTALL_DIR/venv"

# Update Python dependencies if requirements changed
if [ -f "$INSTALL_DIR/ristSTB/ristsender/requirements.txt" ]; then
    source "$VENV_DIR/bin/activate"
    pip install -r "$INSTALL_DIR/ristSTB/ristsender/requirements.txt" --quiet
    deactivate
fi

# Update web files
cp -r "$INSTALL_DIR/ristSTB/ristsender/web" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristsender/sender_api.py" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristsender/stats_collector.sh" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/stats_collector.sh"

# Ensure shared memory directory exists
mkdir -p /dev/shm/ristsender-stats

# Create/update stats collector service
cat > /etc/systemd/system/ristsender-stats.service << 'SVCEOF'
[Unit]
Description=RIST Sender Stats Collector
After=network.target

[Service]
Type=simple
ExecStart=/opt/ristsender/stats_collector.sh
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
SVCEOF
systemctl daemon-reload
systemctl enable ristsender-stats.service

# NOTE: We do NOT copy sender_config.yaml to preserve existing configuration
echo "[OK] Configuration preserved at /etc/ristsender/sender_config.yaml"

# Restart services that were running before update
echo ""
echo "Starting services..."

# Reload systemd to pick up any service file changes
systemctl daemon-reload

# Start stats collector
echo "Starting stats collector..."
systemctl start ristsender-stats.service

# Start API if it was running
if [ "$API_WAS_RUNNING" = true ]; then
    echo "Starting Web API..."
    systemctl start ristsender-api.service
fi

# Start channels that were running
for service_name in "${RUNNING_CHANNELS[@]}"; do
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
echo "  - Web UI updated"
echo "  - API updated"
echo "  - Stats use /dev/shm (RAM, no disk I/O)"
echo "  - Configuration preserved"
echo "  - Running channels restarted"
echo ""
echo "Web Interface: http://$(hostname -I | awk '{print $1}'):8080"
echo ""
