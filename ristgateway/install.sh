#!/bin/bash
# RIST Gateway Installation Script
# Installs the modified librist with rist22rist gateway tool
#
# Usage:
#   GITHUB_TOKEN=xxx ./install.sh          # Set token via env
#   ./install.sh --token ghp_xxxxx         # Set token via arg
#   ./install.sh                           # Uses SSH or prompts

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
# Set GITHUB_TOKEN env var for private repo access, or use SSH
if [ -n "$GITHUB_TOKEN" ]; then
    REPO_URL="https://${GITHUB_TOKEN}@github.com/caritechsolutions/ristSTB.git"
elif [ -n "$USE_SSH" ] || [ -f ~/.ssh/id_ed25519 ] || [ -f ~/.ssh/id_rsa ]; then
    REPO_URL="git@github.com:caritechsolutions/ristSTB.git"
else
    REPO_URL="https://github.com/caritechsolutions/ristSTB.git"
fi

echo "========================================"
echo "RIST Gateway Installation"
echo "========================================"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo ./install.sh)"
    exit 1
fi

echo "Repository: $REPO_URL"
echo "Branch: $BRANCH"

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    echo "Cannot detect OS"
    exit 1
fi

echo "Detected OS: $OS"

# Install dependencies
echo ""
echo "Installing build dependencies..."
case $OS in
    ubuntu|debian)
        apt-get update
        apt-get install -y \
            git \
            build-essential \
            meson \
            ninja-build \
            libmbedtls-dev \
            libcjson-dev \
            libmicrohttpd-dev \
            pkg-config \
            python3 \
            python3-pip \
            python3-venv \
            ffmpeg
        ;;
    centos|rhel|rocky|almalinux)
        dnf install -y epel-release || yum install -y epel-release
        dnf install -y \
            git \
            gcc \
            gcc-c++ \
            meson \
            ninja-build \
            mbedtls-devel \
            cjson-devel \
            libmicrohttpd-devel \
            pkgconfig \
            python3 \
            python3-pip \
            ffmpeg
        ;;
    fedora)
        dnf install -y \
            git \
            gcc \
            gcc-c++ \
            meson \
            ninja-build \
            mbedtls-devel \
            cjson-devel \
            libmicrohttpd-devel \
            pkgconfig \
            python3 \
            python3-pip \
            ffmpeg
        ;;
    *)
        echo "Unsupported OS: $OS"
        echo "Please install manually: git, meson, ninja, mbedtls-dev, cjson-dev, python3"
        exit 1
        ;;
esac

# Create install directory
echo ""
echo "Creating installation directory: $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

# Clone or update repository
if [ -d "$INSTALL_DIR/ristSTB" ]; then
    echo "Repository exists, pulling latest changes..."
    cd "$INSTALL_DIR/ristSTB"
    git fetch origin
    git checkout $BRANCH
    git pull origin $BRANCH
else
    echo "Cloning repository..."
    git clone "$REPO_URL" "$INSTALL_DIR/ristSTB"
    cd "$INSTALL_DIR/ristSTB"
    git checkout $BRANCH
fi

# Build librist
echo ""
echo "Building librist..."
cd "$INSTALL_DIR/ristSTB/librist"

# Clean previous build if exists
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

# Build ristpreview (preview transcoder)
echo ""
echo "Building ristpreview..."
gcc -O2 -Wall -pthread \
    "$INSTALL_DIR/ristSTB/ristgateway/ristpreview.c" \
    -o /usr/local/bin/ristpreview
chmod +x /usr/local/bin/ristpreview
echo "ristpreview installed to /usr/local/bin/ristpreview"

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
    echo "rist22rist installed successfully!"

    # Check if metrics support is enabled
    if $RIST22RIST_BIN --help 2>&1 | grep -q "metrics-http"; then
        echo ""
        echo "[OK] HTTP Metrics support enabled (prometheus/libmicrohttpd)"
    else
        echo ""
        echo "[WARNING] HTTP Metrics support NOT enabled"
        echo "  This may happen if libmicrohttpd was not detected during build."
        echo "  Stats will fall back to journald parsing."
    fi

    echo ""
    $RIST22RIST_BIN --help | head -25
else
    echo "[ERROR] rist22rist not found!"
    exit 1
fi

# Install Web API
echo ""
echo "Installing Gateway Web API..."

# Create directories
mkdir -p /etc/ristgateway
mkdir -p /var/log/ristgateway
mkdir -p /var/run/ristgateway

# Create Python virtual environment
VENV_DIR="$INSTALL_DIR/venv"
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi

# Install Python dependencies
source "$VENV_DIR/bin/activate"
pip install --upgrade pip
pip install -r "$INSTALL_DIR/ristSTB/ristgateway/requirements.txt"
deactivate

# Copy web files
cp -r "$INSTALL_DIR/ristSTB/ristgateway/web" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristgateway/gateway_api.py" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristgateway/stats_collector.sh" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/stats_collector.sh"
cp "$INSTALL_DIR/ristSTB/ristgateway/gateway_config.yaml" /etc/ristgateway/ 2>/dev/null || true

# Create shared memory directories (RAM-backed, no disk I/O)
mkdir -p /dev/shm/ristgateway-stats
mkdir -p /dev/shm/ristgateway-preview

# Create systemd service for stats collector
cat > /etc/systemd/system/ristgateway-stats.service << 'EOF'
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
EOF

# Create systemd service for API
cat > /etc/systemd/system/ristgateway-api.service << 'EOF'
[Unit]
Description=RIST Gateway Web API
After=network.target ristgateway-stats.service

[Service]
Type=simple
User=root
WorkingDirectory=/opt/ristgateway
Environment="GATEWAY_CONFIG=/etc/ristgateway/gateway_config.yaml"
ExecStart=/opt/ristgateway/venv/bin/python /opt/ristgateway/gateway_api.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=ristgateway-api

[Install]
WantedBy=multi-user.target
EOF

# Reload systemd and enable services
systemctl daemon-reload
systemctl enable ristgateway-stats.service
systemctl enable ristgateway-api.service
systemctl start ristgateway-stats.service
systemctl start ristgateway-api.service

echo ""
echo "========================================"
echo "Installation complete!"
echo "========================================"
echo ""
echo "Installed components:"
echo "  - rist22rist  (RIST gateway with multi-peer, NPD, SSRC passthrough)"
echo "  - rist2rist   (original simple RIST relay)"
echo "  - ristsender  (RIST sender)"
echo "  - ristreceiver (RIST receiver)"
echo "  - ristpreview (preview transcoder for web player)"
echo "  - Web API     (http://localhost)"
echo ""
echo "Web Interface:"
echo "  URL: http://$(hostname -I | awk '{print $1}')"
echo "  Default password: admin"
echo ""
echo "Example CLI usage:"
echo "  rist22rist -i 'rist://@:5000' -i 'rist://@:5001' \\"
echo "             -o 'rist://server1:6000' -o 'rist://server2:6001' \\"
echo "             -n -P"
echo ""
echo "Run 'rist22rist --help' for full options"
echo ""
