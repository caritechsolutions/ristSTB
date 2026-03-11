#!/bin/bash
# RIST Gateway Update Script
# Updates and rebuilds the modified librist with rist22rist gateway tool

set -e

INSTALL_DIR="/opt/ristgateway"
BRANCH="${RIST_BRANCH:-claude/pid-selection-oob-lkWRy}"  # Override with RIST_BRANCH env var

echo "========================================"
echo "RIST Gateway Update"
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

# Verify installation
echo ""
echo "Verifying installation..."
if command -v rist22rist &> /dev/null; then
    echo "rist22rist updated successfully!"
elif [ -f /usr/local/bin/rist22rist ]; then
    echo "rist22rist updated at /usr/local/bin/rist22rist"
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

# Restart API service
if systemctl is-active --quiet ristgateway-api.service; then
    echo "Restarting Web API..."
    systemctl restart ristgateway-api.service
fi

echo ""
echo "========================================"
echo "Update complete!"
echo "========================================"
echo "Updated from $CURRENT_COMMIT to $NEW_COMMIT"
echo ""
echo "Web Interface: http://$(hostname -I | awk '{print $1}')"
echo ""
